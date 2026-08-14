//------------------------------------------------------------------------------------------------------------------------------------------
// PsyDoom Xbox: the network diagnostic relay. See 'XboxLog.h' for what it is for and the rule it is built around.
//------------------------------------------------------------------------------------------------------------------------------------------
#include "XboxLog.h"

// Note: guarded on nxdk's own macro, not the CMake variable. PLATFORM_XBOX exists only in the build files and is never
// handed to the compiler, so guarding on it compiled this entire file away - a 628 byte object and a relay that
// silently did not exist. 'Main_Xbox.cpp' uses __XBOX__ for the same reason.
#if defined(__XBOX__)

#include <atomic>
#include <cstdio>
#include <cstring>

#include <SDL.h>

#include <lwip/sockets.h>
#include <lwip/inet.h>
#include <nxdk/net.h>

namespace XboxLog {

//------------------------------------------------------------------------------------------------------------------------------------------
// Where the records go.
//
// Nothing is built in. The relay is a development tool: it sends a running commentary of what the console is doing to a
// listener on another machine, and it only does anything at all if you tell it where to send by creating
// 'E:\Apps\PsyDoomX\logserver.txt' with a single line of 'address:port' in it, for example:
//
//     192.168.0.5:9909
//
// Without that file the relay does not start, no socket is opened and nothing leaves the console. That is the right
// default for something anyone might run: an address compiled into a binary is one that gets shipped to strangers, and
// a console quietly talking to a machine that is not theirs is not a diagnostic, it is a surprise.
//------------------------------------------------------------------------------------------------------------------------------------------
static constexpr uint16_t       DEFAULT_SERVER_PORT = 9909;
static constexpr const char*    CONFIG_PATH         = "E:\\Apps\\PsyDoomX\\logserver.txt";

//------------------------------------------------------------------------------------------------------------------------------------------
// The ring.
//
// Fixed slots, no allocation, written by any thread and drained by one. 1024 records of 192 bytes is 192 KB, which is
// nothing against 64 MB and is enough to ride out several seconds of a listener that has stalled.
//------------------------------------------------------------------------------------------------------------------------------------------
static constexpr uint32_t NUM_RECORDS  = 1024;
static constexpr uint32_t RECORD_BYTES = 192;

struct Record {
    // Stamped once the text is fully written, and read before the text is. That ordering is what makes a record safe to
    // read without a lock: the sequence number appearing is the promise that everything else in the slot is complete.
    std::atomic<uint64_t>   readySeq;
    uint64_t                timeMicros;
    Sys                     sys;
    Sev                     sev;
    uint16_t                textLen;
    char                    text[RECORD_BYTES];
};

static Record                   gRecords[NUM_RECORDS];
static std::atomic<uint64_t>    gWriteSeq{0};       // Next sequence to hand out. Only ever incremented.
static std::atomic<uint64_t>    gSentSeq{0};        // How far the sender has actually got. See the note on dropping.
static std::atomic<uint32_t>    gDropped{0};
static std::atomic<bool>        gbRunning{false};
static SDL_Thread*              gpSenderThread = nullptr;
static std::atomic<uint64_t>    gHeartbeatSeq{0};       // Bumped by the game thread; watched by the sender
static std::atomic<uint64_t>    gHeartbeatMicros{0};
static const char*              gpHeartbeatWhere = "start";
static uint64_t                 gStartCounter = 0;
static double                   gCounterToMicros = 0.0;

//------------------------------------------------------------------------------------------------------------------------------------------
// Timing
//------------------------------------------------------------------------------------------------------------------------------------------
uint64_t nowMicros() noexcept {
    if (gCounterToMicros == 0.0)
        return 0;

    return (uint64_t)((double)(SDL_GetPerformanceCounter() - gStartCounter) * gCounterToMicros);
}

//------------------------------------------------------------------------------------------------------------------------------------------
// Writing a record.
//
// This is the part that runs on the game thread and on the audio callback, so it is the part that must stay cheap. One
// atomic increment to claim a slot, a vsnprintf into it, one atomic store to publish it. Nothing that can block.
//
// Claiming a slot cannot fail. If the sender has fallen far enough behind that this slot still holds an unsent record,
// that record is overwritten and counted as dropped - losing old diagnostics is always better than delaying the game.
//------------------------------------------------------------------------------------------------------------------------------------------
void vlogf(const Sys sys, const Sev sev, const char* const format, std::va_list args) noexcept {
    if (!gbRunning.load(std::memory_order_relaxed))
        return;

    const uint64_t seq = gWriteSeq.fetch_add(1, std::memory_order_relaxed);
    Record& rec = gRecords[seq % NUM_RECORDS];

    // Is this about to overwrite something the sender has not taken yet?
    //
    // This has to be asked of where the sender has got to, not of the slot. Asking the slot cannot work: the previous
    // occupant of slot 'seq % NUM_RECORDS' always has sequence 'seq - NUM_RECORDS', so any test of its stamp is a test
    // of whether the ring has wrapped, which after the first thousand records it always has. The first version of this
    // did exactly that and so counted every record as a drop - and since a drop was reported by logging, each report
    // was another record and another drop. 149,071 warnings against 3,227 real lines, a 13.8MB log of almost nothing,
    // and needless work on the game thread polluting the very timings this exists to measure.
    if (seq >= gSentSeq.load(std::memory_order_relaxed) + NUM_RECORDS) {
        gDropped.fetch_add(1, std::memory_order_relaxed);
    }

    rec.timeMicros = nowMicros();
    rec.sys = sys;
    rec.sev = sev;

    const int written = std::vsnprintf(rec.text, RECORD_BYTES, format, args);
    rec.textLen = (uint16_t) ((written < 0) ? 0 : ((written >= (int) RECORD_BYTES) ? RECORD_BYTES - 1 : written));

    // Publish last, with release ordering, so a reader that sees this sequence sees a complete record
    rec.readySeq.store(seq + 1, std::memory_order_release);
}

void logf(const Sys sys, const Sev sev, const char* const format, ...) noexcept {
    std::va_list args;
    va_start(args, format);
    vlogf(sys, sev, format, args);
    va_end(args);
}

void heartbeat(const char* const where) noexcept {
    gpHeartbeatWhere = where;
    gHeartbeatMicros.store(nowMicros(), std::memory_order_relaxed);
    gHeartbeatSeq.fetch_add(1, std::memory_order_relaxed);
}

uint32_t droppedCount() noexcept {
    return gDropped.load(std::memory_order_relaxed);
}

//------------------------------------------------------------------------------------------------------------------------------------------
// Formatting a record for the wire.
//
// Plain text, one line each, so it can be read with a terminal and searched with grep. The volume this produces is far
// below what a hundred megabit link will carry, so there is nothing to gain by making it binary and plenty to lose.
//------------------------------------------------------------------------------------------------------------------------------------------
static const char* sysName(const Sys sys) noexcept {
    switch (sys) {
        case Sys::Video:    return "video";
        case Sys::Audio:    return "audio";
        case Sys::CdAudio:  return "cdaudio";
        case Sys::Input:    return "input";
        case Sys::Disc:     return "disc";
        case Sys::Mem:      return "mem";
        case Sys::Net:      return "net";
        case Sys::Split:    return "split";
        default:            return "general";
    }
}

static char sevChar(const Sev sev) noexcept {
    switch (sev) {
        case Sev::Info:  return 'I';
        case Sev::Warn:  return 'W';
        case Sev::Error: return 'E';
        default:         return 'T';
    }
}

//------------------------------------------------------------------------------------------------------------------------------------------
// Where to send, if anywhere. Returns false when no destination has been configured, which is the normal case.
//------------------------------------------------------------------------------------------------------------------------------------------
static bool readServerConfig(char* const ipOut, const size_t ipOutSize, uint16_t& portOut) noexcept {
    ipOut[0] = 0;
    portOut = DEFAULT_SERVER_PORT;

    std::FILE* const pFile = std::fopen(CONFIG_PATH, "r");

    if (!pFile)
        return false;

    char line[64] = {};

    if (std::fgets(line, sizeof(line), pFile)) {
        char ip[48] = {};
        unsigned int port = 0;

        if (std::sscanf(line, "%47[^:]:%u", ip, &port) == 2) {
            std::strncpy(ipOut, ip, ipOutSize - 1);
            ipOut[ipOutSize - 1] = 0;
            portOut = (uint16_t) port;
        }
    }

    std::fclose(pFile);
    return (ipOut[0] != 0);
}

//------------------------------------------------------------------------------------------------------------------------------------------
// The sender.
//
// Brings the network up, connects, and drains the ring. Everything slow lives here and nowhere else: the network stack
// coming up can take seconds while a console waits for DHCP, and the game must not wait for any of it.
//------------------------------------------------------------------------------------------------------------------------------------------
static int senderThreadMain([[maybe_unused]] void* pUserData) noexcept {
    // Bringing up the network is the slow part, which is exactly why it happens here rather than in 'init'
    nxNetInit(nullptr);

    char serverIp[48] = {};
    uint16_t serverPort = 0;
    readServerConfig(serverIp, sizeof(serverIp), serverPort);

    int sock = -1;
    uint64_t nextSeq = 0;
    uint32_t lastReportedDrops = 0;
    uint64_t lastDropReportMicros = 0;

    while (gbRunning.load(std::memory_order_relaxed)) {
        // Not connected: try, but do not spin at it
        if (sock < 0) {
            sock = lwip_socket(AF_INET, SOCK_STREAM, 0);

            if (sock >= 0) {
                sockaddr_in addr = {};
                addr.sin_family = AF_INET;
                addr.sin_port = lwip_htons(serverPort);
                addr.sin_addr.s_addr = inet_addr(serverIp);

                if (lwip_connect(sock, (sockaddr*) &addr, sizeof(addr)) != 0) {
                    lwip_close(sock);
                    sock = -1;
                }
            }

            if (sock < 0) {
                SDL_Delay(2000);        // Nothing listening yet. The game does not care either way.
                continue;
            }

            // Start from whatever is in the ring now rather than replaying a session's backlog at a listener that has
            // only just appeared
            nextSeq = gWriteSeq.load(std::memory_order_acquire);
            nextSeq = (nextSeq > NUM_RECORDS) ? (nextSeq - NUM_RECORDS) : 0;
        }

        // Drain whatever is ready
        char sendBuf[2048];
        int sendLen = 0;
        const uint64_t writeSeq = gWriteSeq.load(std::memory_order_acquire);

        // If the writer has lapped us entirely, skip ahead: those records are already overwritten
        if (writeSeq > nextSeq + NUM_RECORDS) {
            nextSeq = writeSeq - NUM_RECORDS;
        }

        while ((nextSeq < writeSeq) && (sendLen < (int)(sizeof(sendBuf)) - 320)) {
            Record& rec = gRecords[nextSeq % NUM_RECORDS];

            // Acquire, to pair with the release in 'vlogf': if the record is not yet published, stop and come back
            if (rec.readySeq.load(std::memory_order_acquire) != nextSeq + 1)
                break;

            const int n = std::snprintf(
                sendBuf + sendLen,
                sizeof(sendBuf) - (size_t) sendLen,
                "%llu.%06llu %c %-7s %s\n",
                (unsigned long long)(rec.timeMicros / 1000000),
                (unsigned long long)(rec.timeMicros % 1000000),
                sevChar(rec.sev),
                sysName(rec.sys),
                rec.text
            );

            if (n <= 0)
                break;

            sendLen += n;
            nextSeq++;
        }

        gSentSeq.store(nextSeq, std::memory_order_relaxed);

        if (sendLen > 0) {
            int sent = 0;

            while (sent < sendLen) {
                const int n = lwip_send(sock, sendBuf + sent, (size_t)(sendLen - sent), 0);

                if (n <= 0) {
                    lwip_close(sock);       // Listener has gone. Reconnect and carry on.
                    sock = -1;
                    break;
                }

                sent += n;
            }
        } else {
            SDL_Delay(15);      // Nothing waiting. Roughly a frame.
        }

        // Has the game thread stopped?
        //
        // This thread is the only thing that keeps running when it does, so this is the only place a freeze can be
        // reported from. Reported once per stall rather than repeatedly - a hung game would otherwise fill the log
        // with the news that it is still hung.
        {
            static uint64_t sLastSeenSeq = 0;
            static uint64_t sLastSeenMicros = 0;
            static bool sbStallReported = false;

            const uint64_t seq = gHeartbeatSeq.load(std::memory_order_relaxed);
            const uint64_t now = nowMicros();

            if (seq != sLastSeenSeq) {
                sLastSeenSeq = seq;
                sLastSeenMicros = now;
                sbStallReported = false;
            }
            else if ((!sbStallReported) && (sLastSeenMicros > 0) && (now - sLastSeenMicros > 3000000)) {
                sbStallReported = true;
                logf(
                    Sys::General,
                    Sev::Error,
                    "game thread has not moved for %llums - last seen in '%s' at frame mark %llu",
                    (unsigned long long)((now - sLastSeenMicros) / 1000),
                    gpHeartbeatWhere,
                    (unsigned long long) seq
                );
            }
        }

        // Say so if records are being lost, since it means these numbers are a sample and not the whole picture.
        //
        // At most once every few seconds. Reporting on every change is what turned a handful of genuine drops into a
        // hundred and fifty thousand warnings: the report is itself a record, so reporting a drop caused the next one.
        constexpr uint64_t DROP_REPORT_INTERVAL_MICROS = 5000000;
        const uint32_t drops = gDropped.load(std::memory_order_relaxed);
        const uint64_t now = nowMicros();

        if ((drops != lastReportedDrops) && (sock >= 0) && (now - lastDropReportMicros >= DROP_REPORT_INTERVAL_MICROS)) {
            const uint32_t sinceLast = drops - lastReportedDrops;
            lastReportedDrops = drops;
            lastDropReportMicros = now;
            logf(Sys::Net, Sev::Warn, "relay dropped %u records (%u total) - the log is a sample here", sinceLast, drops);
        }
    }

    if (sock >= 0) {
        lwip_close(sock);
    }

    return 0;
}

//------------------------------------------------------------------------------------------------------------------------------------------
// Startup and shutdown
//------------------------------------------------------------------------------------------------------------------------------------------
void init() noexcept {
    if (gbRunning.load(std::memory_order_relaxed))
        return;

    // No destination configured means no relay. Checked here rather than in the sender thread so that nothing is
    // started, no socket is opened and no network stack is brought up on a console that never asked for any of it.
    {
        char ip[48] = {};
        uint16_t port = 0;

        if (!readServerConfig(ip, sizeof(ip), port))
            return;
    }

    const uint64_t frequency = SDL_GetPerformanceFrequency();
    gStartCounter = SDL_GetPerformanceCounter();
    gCounterToMicros = (frequency > 0) ? (1000000.0 / (double) frequency) : 0.0;

    std::memset(gRecords, 0, sizeof(gRecords));
    gWriteSeq.store(0, std::memory_order_relaxed);
    gDropped.store(0, std::memory_order_relaxed);
    gbRunning.store(true, std::memory_order_release);

    gpSenderThread = SDL_CreateThread(senderThreadMain, "XboxLogSender", nullptr);

    if (!gpSenderThread) {
        gbRunning.store(false, std::memory_order_release);
        return;
    }

    // Below the game and well below audio. This is the least important thing running.
    SDL_DetachThread(gpSenderThread);

    logf(Sys::General, Sev::Info, "PsyDoom Xbox diagnostic relay up");
}

void stop() noexcept {
    if (!gbRunning.load(std::memory_order_relaxed))
        return;

    logf(Sys::General, Sev::Info, "relay shutting down, %u records dropped over the session", droppedCount());
    SDL_Delay(120);     // A moment for the sender to get the last of it out. Not a wait worth caring about.
    gbRunning.store(false, std::memory_order_release);
    gpSenderThread = nullptr;
}

}   // namespace XboxLog

#endif  // #if defined(__XBOX__)
