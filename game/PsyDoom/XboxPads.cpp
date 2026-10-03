//------------------------------------------------------------------------------------------------------------------------------------------
// PsyDoom Xbox: keeping controllers alive, and saying what they are doing. See 'XboxPads.h' for why this exists.
//
// This reaches into nxdk's USB stack and SDL's use of it, which neither offers an interface for. Everything it touches is
// read from the same headers the library was built with, under the same configuration, so the structures cannot be laid
// out differently here from how the library lays them out - the static asserts below are there to keep it that way.
//------------------------------------------------------------------------------------------------------------------------------------------
#include "XboxPads.h"

#if defined(__XBOX__)

#include "XboxLog.h"
#include "XboxPaths.h"

#include <cstdarg>
#include <cstdio>
#include <cstring>

#include <SDL.h>
#include <windows.h>
#include <xboxkrnl/xboxkrnl.h>

// nxdk's USB stack, under the configuration it was compiled with. Without this its headers fall back to a different
// configuration whose interface structure is a different size, and anything built from that would not match the library.
#define USBH_USE_EXTERNAL_CONFIG "usbh_config_xbox.h"
#include <usbh_lib.h>
#include <hub.h>
#include <xid_driver.h>

static_assert(MAX_EP_PER_IFACE == 15, "Not the configuration nxdk's USB library is built with");
static_assert(MAX_ALT_PER_IFACE == 12, "Not the configuration nxdk's USB library is built with");

// The XID driver's own entry points. Global in the library, though no header says so.
extern "C" UDEV_DRV_T xid_driver;

namespace XboxPads {

//------------------------------------------------------------------------------------------------------------------------------------------
// What is known about each pad.
//
// The counters near the top are written from the USB interrupt's deferred routine, which can run between any two
// instructions of the game thread, so they are volatile and only ever incremented there. Everything else is the game
// thread's alone.
//------------------------------------------------------------------------------------------------------------------------------------------
static constexpr int32_t MAX_PADS = CONFIG_XID_MAX_DEV;

struct PadTrack {
    xid_dev_t*          pDev;
    uint32_t            uid;
    uint32_t            connectMs;

    volatile uint32_t   reads;              // Transfers that completed with data
    volatile uint32_t   readErrors;         // Transfers that completed with an error
    volatile uint32_t   changes;            // Reports that differed from the one before
    volatile int32_t    lastErrorStatus;
    volatile uint32_t   minXferLen;
    volatile uint32_t   maxXferLen;
    uint8_t             lastReport[32];

    uint32_t            restarts;           // Times the read pipe was found stopped and started again
    uint32_t            restartFailures;    // Times starting it again was refused
    uint32_t            haltsCleared;       // Times the halt was found set on the controller's side
    uint32_t            deviceHaltClears;   // Times the device itself was asked to clear a stall
    uint32_t            readsAtLastRestart;
    uint32_t            restartsSinceGoodRead;
    uint32_t            lastRestartMs;
    uint32_t            lastDeviceClearMs;
    uint32_t            lastRestartLogMs;
    uint32_t            lastSummaryMs;
    uint32_t            summaryReads;
    uint32_t            summaryErrors;
    uint32_t            summaryRestarts;
    uint32_t            lastNoPipeMs;
    bool                bOpenedBySdl;
    bool                bFirstSummaryFiled;     // The first summary always goes to the file: it shows a working pad's read rate
};

static PadTrack     gPads[MAX_PADS];
static FUNC_UTR_T   gSdlReadCallback = nullptr;     // SDL's own completion routine, which the hook below passes everything on to
static uint32_t     gLastUnboundCheckMs = 0;

// XID interfaces that enumerated but that no driver took, and how many times they have been offered to it again
struct UnboundXid {
    UDEV_T*     pUdev;
    uint8_t     devNum;
    uint8_t     ifNum;
    uint8_t     attempts;
    uint32_t    firstSeenMs;
    uint32_t    lastAttemptMs;
    bool        bGaveUp;
};

static constexpr int32_t MAX_UNBOUND = 8;
static UnboundXid gUnbound[MAX_UNBOUND];

//------------------------------------------------------------------------------------------------------------------------------------------
// Reporting: to the relay, and to 'pads.log' beside the executable.
//
// The relay only helps while a machine is listening, and the boot log is started afresh by each process - so the menu's
// record of a pad is gone by the time the game it launched has misbehaved. This file is only ever added to, and is only
// written when something happens, so a session with nothing wrong writes a handful of lines.
//------------------------------------------------------------------------------------------------------------------------------------------
static constexpr DWORD PAD_LOG_MAX_BYTES = 256 * 1024;

static const char* padLogPath() noexcept {
    static char path[260];
    return XboxPaths::make(path, sizeof(path), "pads.log");
}

static void appendPadLogLine(const char* const line) noexcept {
    HANDLE const h = CreateFileA(padLogPath(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);

    if (h == INVALID_HANDLE_VALUE)
        return;

    // Appending is done by hand: 'FILE_APPEND_DATA' is not honoured on this console (see the boot log writers)
    SetFilePointer(h, 0, nullptr, FILE_END);

    DWORD written = 0;
    WriteFile(h, line, (DWORD) std::strlen(line), &written, nullptr);
    WriteFile(h, "\r\n", 2, &written, nullptr);
    CloseHandle(h);
}

static void padLog(const XboxLog::Sev sev, const bool bToFile, const char* const format, ...) noexcept {
    char text[320];

    va_list args;
    va_start(args, format);
    std::vsnprintf(text, sizeof(text), format, args);
    va_end(args);

    XboxLog::logf(XboxLog::Sys::Input, sev, "%s", text);

    if (bToFile) {
        char line[340];
        std::snprintf(line, sizeof(line), "%8u %s", (unsigned) SDL_GetTicks(), text);
        appendPadLogLine(line);
    }
}

void beginSession(const char* const name) noexcept {
    // Cut the file back rather than let it grow for ever. Started again from empty: a log of old sessions is only worth
    // keeping while it is short enough to read.
    HANDLE const h = CreateFileA(padLogPath(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);

    if (h != INVALID_HANDLE_VALUE) {
        const DWORD size = GetFileSize(h, nullptr);
        CloseHandle(h);

        if ((size != INVALID_FILE_SIZE) && (size > PAD_LOG_MAX_BYTES)) {
            DeleteFileA(padLogPath());
        }
    }

    char line[200];

    #if defined(PSYDOOM_XBOX_BUILD_ID)
        std::snprintf(line, sizeof(line), "==== %s - build %s (%s) - %s", name, PSYDOOM_XBOX_BUILD_ID, PSYDOOM_XBOX_BUILD_TIME, XboxPaths::dir());
    #else
        std::snprintf(line, sizeof(line), "==== %s - %s", name, XboxPaths::dir());
    #endif

    appendPadLogLine(line);
}

static PadTrack* findTrack(const xid_dev_t* const pDev) noexcept {
    for (PadTrack& track : gPads) {
        if (track.pDev == pDev)
            return &track;
    }

    return nullptr;
}

//------------------------------------------------------------------------------------------------------------------------------------------
// Every completed read of a pad passes through here on its way to SDL.
//
// Runs in the USB interrupt's deferred routine. It counts, keeps the last report for comparison, and hands the transfer
// to SDL exactly as it would have arrived. SDL queues the next read itself when this one succeeded; when it failed, SDL
// does nothing, and 'service' notices and starts it again.
//------------------------------------------------------------------------------------------------------------------------------------------
static void padReadHook(UTR_T* const pUtr) noexcept {
    const xid_dev_t* const pDev = (const xid_dev_t*) pUtr->context;
    PadTrack* const pTrack = (pDev) ? findTrack(pDev) : nullptr;     // A null device would match a free slot

    if (pTrack) {
        if (pUtr->status < 0) {
            pTrack->readErrors = pTrack->readErrors + 1;
            pTrack->lastErrorStatus = pUtr->status;
        } else {
            pTrack->reads = pTrack->reads + 1;

            const uint32_t len = pUtr->xfer_len;

            if ((pTrack->minXferLen == 0) || (len < pTrack->minXferLen)) { pTrack->minXferLen = len; }
            if (len > pTrack->maxXferLen) { pTrack->maxXferLen = len; }

            const uint32_t cmpLen = (len < sizeof(pTrack->lastReport)) ? len : (uint32_t) sizeof(pTrack->lastReport);

            if (pUtr->buff && (cmpLen > 0) && (std::memcmp(pTrack->lastReport, pUtr->buff, cmpLen) != 0)) {
                std::memcpy(pTrack->lastReport, pUtr->buff, cmpLen);
                pTrack->changes = pTrack->changes + 1;
            }
        }
    }

    if (gSdlReadCallback) {
        gSdlReadCallback(pUtr);
    }
}

// The hub a device hangs off, as a device of its own, or null for one on a root port
static const UDEV_T* parentDevice(const UDEV_T* const pUdev) noexcept {
    return (pUdev->parent && pUdev->parent->iface) ? pUdev->parent->iface->udev : nullptr;
}

// Which front port a device is on, as the player would count them, and how deep it sits behind hubs.
//
// The same reckoning SDL uses to number its players. Some consoles have a hub inside them between the controller and
// the front ports, so on those the port that counts is the one on that hub, not on the controller underneath it.
static void devicePort(const UDEV_T* const pUdev, int32_t& portOut, int32_t& hubDepthOut) noexcept {
    portOut = 0;
    hubDepthOut = 0;

    for (const UDEV_T* pCur = parentDevice(pUdev); pCur; pCur = parentDevice(pCur)) {
        hubDepthOut++;
    }

    const bool bInternalHub = ((XboxHardwareInfo.Flags & XBOX_HW_FLAG_INTERNAL_USB_HUB) != 0);

    for (const UDEV_T* pCur = pUdev; pCur; pCur = parentDevice(pCur)) {
        const UDEV_T* const pParent = parentDevice(pCur);
        const bool bIsFrontPort = (bInternalHub) ? (pParent && (!pParent->parent)) : (!pCur->parent);

        if (!bIsFrontPort)
            continue;

        // The front ports run 3, 4, 1, 2 from the left
        switch (pCur->port_num) {
            case 3: portOut = 1; break;
            case 4: portOut = 2; break;
            case 1: portOut = 3; break;
            case 2: portOut = 4; break;
            default: portOut = 0; break;
        }

        return;
    }
}

static const char* speedName(const UDEV_T* const pUdev) noexcept {
    switch (pUdev->speed) {
        case SPEED_LOW:     return "low";
        case SPEED_FULL:    return "full";
        case SPEED_HIGH:    return "high";
        default:            return "?";
    }
}

// Everything worth knowing about a pad, said once when it turns up
static void logArrival(const PadTrack& track, xid_dev_t* const pDev, const uint32_t nowMs) noexcept {
    const IFACE_T* const pIface = pDev->iface;
    const UDEV_T* const pUdev = (pIface) ? pIface->udev : nullptr;

    int32_t port = 0, hubDepth = 0;

    if (pUdev) {
        devicePort(pUdev, port, hubDepth);
    }

    padLog(XboxLog::Sev::Info, true,
        "pad arrived: uid=%u vid=%04X pid=%04X bcdDevice=%04X | xid len=%u bcd=%04X type=%02X sub=%02X in=%u out=%u | "
        "port=%d hubs=%d speed=%s addr=%u if=%u | %ums after start",
        (unsigned) track.uid,
        (unsigned) pDev->idVendor, (unsigned) pDev->idProduct,
        (unsigned) ((pUdev) ? pUdev->descriptor.bcdDevice : 0),
        (unsigned) pDev->xid_desc.bLength, (unsigned) pDev->xid_desc.bcdXid,
        (unsigned) pDev->xid_desc.bType, (unsigned) pDev->xid_desc.bSubType,
        (unsigned) pDev->xid_desc.bMaxInputReportSize, (unsigned) pDev->xid_desc.bMaxOutputReportSize,
        (int) port, (int) hubDepth, (pUdev) ? speedName(pUdev) : "?",
        (unsigned) ((pUdev) ? pUdev->dev_num : 0), (unsigned) ((pIface) ? pIface->if_num : 0),
        (unsigned) nowMs
    );

    // The endpoints, which is where third party pads tend to differ from Microsoft's: report sizes and polling rates
    if (pIface && pIface->aif && pIface->aif->ifd) {
        const int32_t numEps = pIface->aif->ifd->bNumEndpoints;

        for (int32_t i = 0; (i < numEps) && (i < MAX_EP_PER_IFACE); ++i) {
            const EP_INFO_T& ep = pIface->aif->ep[i];

            padLog(XboxLog::Sev::Info, true,
                "pad arrived: uid=%u   endpoint %02X attr=%02X maxPacket=%u interval=%ums",
                (unsigned) track.uid, (unsigned) ep.bEndpointAddress, (unsigned) ep.bmAttributes,
                (unsigned) ep.wMaxPacketSize, (unsigned) ep.bInterval
            );
        }
    }

    if (pDev->xid_desc.bType != XID_TYPE_GAMECONTROLLER) {
        padLog(XboxLog::Sev::Warn, true,
            "pad arrived: uid=%u is XID type %02X, which SDL does not offer as a game controller - it will not be usable",
            (unsigned) track.uid, (unsigned) pDev->xid_desc.bType
        );
    }
}

static void logDeparture(const PadTrack& track, const uint32_t nowMs) noexcept {
    padLog(XboxLog::Sev::Info, true,
        "pad left: uid=%u after %ums - reads=%u errors=%u restarts=%u (failed %u) halts=%u stallClears=%u",
        (unsigned) track.uid, (unsigned) (nowMs - track.connectMs),
        (unsigned) track.reads, (unsigned) track.readErrors, (unsigned) track.restarts, (unsigned) track.restartFailures,
        (unsigned) track.haltsCleared, (unsigned) track.deviceHaltClears
    );
}

//------------------------------------------------------------------------------------------------------------------------------------------
// Tell a pad to clear a stall on its own end, and start its data toggle again from the beginning.
//
// Only needed if the endpoint really did stall, which looks no different from here than any other error - so this is
// only tried when restarting on the host side alone keeps failing. It waits for the pad to answer, up to a second, so it
// is rationed.
//------------------------------------------------------------------------------------------------------------------------------------------
static void clearDeviceStall(PadTrack& track, xid_dev_t* const pDev, UTR_T* const pUtr) noexcept {
    UDEV_T* const pUdev = (pDev->iface) ? pDev->iface->udev : nullptr;

    if ((!pUdev) || (!pUtr->ep))
        return;

    const int32_t ret = usbh_clear_halt(pUdev, pUtr->ep->bEndpointAddress);

    // The device starts its toggle again at DATA0 once the stall is cleared, so the controller has to as well
    ED_T* const pEd = (ED_T*) pUtr->ep->hw_pipe;

    if ((ret == USBH_OK) && pEd) {
        volatile uint32_t* const pHeadP = &pEd->HeadP;
        *pHeadP = (*pHeadP) & ~((uint32_t) 0x2);
    }

    track.deviceHaltClears++;

    padLog(XboxLog::Sev::Warn, true,
        "pad uid=%u: restarting alone keeps failing - asked the pad to clear a stall on endpoint %02X, result %d",
        (unsigned) track.uid, (unsigned) pUtr->ep->bEndpointAddress, (int) ret
    );
}

//------------------------------------------------------------------------------------------------------------------------------------------
// Make sure the pad's read goes through the hook, and start it again if an error stopped it
//------------------------------------------------------------------------------------------------------------------------------------------
static void tendReadPipe(PadTrack& track, xid_dev_t* const pDev, const uint32_t nowMs) noexcept {
    // The interrupt IN transfer SDL keeps running for this pad
    UTR_T* pReadUtr = nullptr;

    for (int32_t i = 0; i < XID_MAX_TRANSFER_QUEUE; ++i) {
        UTR_T* const pUtr = pDev->utr_list[i];

        if (pUtr && pUtr->ep && ((pUtr->ep->bEndpointAddress & EP_ADDR_DIR_MASK) == EP_ADDR_DIR_IN)) {
            pReadUtr = pUtr;
            break;
        }
    }

    if (!pReadUtr) {
        // Open, but nothing reading it. SDL starts the read once when it opens a pad and never again, so if that first
        // one could not be queued the pad stays silent for good. Started here instead, through the same routine SDL uses.
        if (pDev->user_data && gSdlReadCallback && ((nowMs - track.lastNoPipeMs) >= 500u)) {
            track.lastNoPipeMs = nowMs;
            const int32_t ret = usbh_xid_read(pDev, 0, (void*) &padReadHook);

            padLog(XboxLog::Sev::Warn, true,
                "pad uid=%u: open but no read running - started one, result %d", (unsigned) track.uid, (int) ret
            );
        }

        return;
    }

    // Route its completions through the hook. SDL's routine is taken from the first transfer seen, and only that
    // routine is ever replaced, so a transfer belonging to anything else is left alone.
    if (pReadUtr->func != &padReadHook) {
        if ((!gSdlReadCallback) && pReadUtr->func) {
            gSdlReadCallback = pReadUtr->func;
        }

        if (pReadUtr->func == gSdlReadCallback) {
            pReadUtr->func = &padReadHook;
        }
    }

    // Stopped by an error, and SDL still has the pad open?
    //
    // An open pad's read only ever sits finished when an error stopped it: a good read is queued again inside the
    // completion itself, before this can see it. A closed pad's read is left finished on purpose, so it is not touched.
    const bool bStoppedByError = (pReadUtr->bIsTransferDone && (pReadUtr->status < 0));

    if ((!bStoppedByError) || (!pDev->user_data))
        return;

    // Back off when restarting does not help. A pad that fails every transfer is most likely being unplugged, and the
    // hub will report that shortly; until then there is no sense in restarting it every frame.
    if (track.reads == track.readsAtLastRestart) {
        track.restartsSinceGoodRead++;
    } else {
        track.restartsSinceGoodRead = 0;
    }

    const uint32_t minGapMs = (track.restartsSinceGoodRead < 20) ? 0u : 1000u;

    if ((nowMs - track.lastRestartMs) < minGapMs)
        return;

    // A real stall needs the pad itself told. Tried on the third restart in a row that got nothing back, and then rarely.
    const bool bTryDeviceClear = (
        (track.restartsSinceGoodRead >= 3) &&
        ((track.deviceHaltClears == 0) || ((nowMs - track.lastDeviceClearMs) >= 5000u))
    );

    if (bTryDeviceClear) {
        track.lastDeviceClearMs = nowMs;
        clearDeviceStall(track, pDev, pReadUtr);
    }

    const int32_t errorStatus = pReadUtr->status;

    // Queue the read again and let the controller carry on.
    //
    // Done with the USB interrupt held off, so its completion routine cannot run while the transfer and the endpoint are
    // half way through being put back together. The halt bit is cleared after the transfer is queued: the controller
    // skips a halted endpoint entirely, so until the bit goes it cannot start on the new transfer early.
    const KIRQL oldIrql = KeRaiseIrqlToDpcLevel();

    ED_T* const pEd = (ED_T*) pReadUtr->ep->hw_pipe;
    const bool bWasHalted = (pEd && ((pEd->HeadP & ED_HEADP_HALT) != 0));

    pReadUtr->xfer_len = 0;
    pReadUtr->bIsTransferDone = 0;
    pReadUtr->func = &padReadHook;

    const int32_t ret = usbh_int_xfer(pReadUtr);

    if (ret == USBH_OK) {
        if (pEd) {
            volatile uint32_t* const pHeadP = &pEd->HeadP;
            *pHeadP = (*pHeadP) & ~((uint32_t) ED_HEADP_HALT);
        }
    } else {
        pReadUtr->bIsTransferDone = 1;      // Still stopped; tried again next time
    }

    KfLowerIrql(oldIrql);

    track.lastRestartMs = nowMs;
    track.readsAtLastRestart = track.reads;

    if (ret == USBH_OK) {
        track.restarts++;
        track.haltsCleared += (bWasHalted) ? 1u : 0u;
    } else {
        track.restartFailures++;
    }

    // Said every time at first, then at most once a second, so a pad stuck in a loop of errors cannot flood the relay
    if ((track.restarts + track.restartFailures <= 10) || ((nowMs - track.lastRestartLogMs) >= 1000u)) {
        track.lastRestartLogMs = nowMs;

        padLog(XboxLog::Sev::Warn, true,
            "pad uid=%u: read stopped by error %d after %u good reads - restarted (endpoint halted=%d, result %d), "
            "restart #%u, %u in a row without data",
            (unsigned) track.uid, (int) errorStatus, (unsigned) track.reads, (int) bWasHalted, (int) ret,
            (unsigned) track.restarts, (unsigned) track.restartsSinceGoodRead
        );
    }
}

// A summary now and then, so a session log shows whether each pad was being read at all
static void summarise(PadTrack& track, const uint32_t nowMs) noexcept {
    if ((nowMs - track.lastSummaryMs) < 5000u)
        return;

    const uint32_t elapsedMs = nowMs - track.lastSummaryMs;
    const uint32_t reads = track.reads;
    const uint32_t errors = track.readErrors;

    const bool bTrouble = ((errors != track.summaryErrors) || (track.restarts != track.summaryRestarts));
    const bool bToFile = (bTrouble || (!track.bFirstSummaryFiled));
    track.bFirstSummaryFiled = true;

    padLog(XboxLog::Sev::Info, bToFile,
        "pad uid=%u: %u reads/s, %u errors in %us (total %u), restarts=%u changes=%u len=%u..%u open=%d",
        (unsigned) track.uid,
        (unsigned) (((reads - track.summaryReads) * 1000u) / ((elapsedMs > 0) ? elapsedMs : 1u)),
        (unsigned) (errors - track.summaryErrors), (unsigned) (elapsedMs / 1000u), (unsigned) errors,
        (unsigned) track.restarts, (unsigned) track.changes,
        (unsigned) track.minXferLen, (unsigned) track.maxXferLen, (int) (track.pDev && track.pDev->user_data)
    );

    track.lastSummaryMs = nowMs;
    track.summaryReads = reads;
    track.summaryErrors = errors;
    track.summaryRestarts = track.restarts;
}

static bool isXidBoundTo(const UDEV_T* const pUdev, const uint8_t ifNum) noexcept {
    for (xid_dev_t* pDev = usbh_xid_get_device_list(); pDev; pDev = pDev->next) {
        if (pDev->iface && (pDev->iface->udev == pUdev) && (pDev->iface->if_num == ifNum))
            return true;
    }

    return false;
}

//------------------------------------------------------------------------------------------------------------------------------------------
// Offer an XID interface to the XID driver again, the way the USB core offers it when a device is first configured.
//
// The driver gives up on a pad that does not answer its request for the XID descriptor, and the core then forgets the
// interface entirely - the pad is enumerated and powered but nothing will ever read it, until it is unplugged. A third
// party pad that is slow to answer straight after being configured fits that exactly. This builds the interface again
// from the configuration descriptor the core kept and lets the driver have another go.
//------------------------------------------------------------------------------------------------------------------------------------------
static bool reprobeXidInterface(UDEV_T* const pUdev, const DESC_IF_T* const pIfDesc, const uint8_t* const pDescEnd) noexcept {
    if (pIfDesc->bNumEndpoints > MAX_EP_PER_IFACE)
        return false;

    IFACE_T* const pIface = (IFACE_T*) usbh_alloc_mem((int) sizeof(IFACE_T));

    if (!pIface)
        return false;

    pIface->udev = pUdev;
    pIface->if_num = pIfDesc->bInterfaceNumber;
    pIface->num_alt = 1;
    pIface->aif = &pIface->alt[0];
    pIface->alt[0].ifd = const_cast<DESC_IF_T*>(pIfDesc);

    // Its endpoints, as the core would have parsed them: every endpoint descriptor up to the next interface
    {
        const uint8_t* p = ((const uint8_t*) pIfDesc) + pIfDesc->bLength;
        int32_t numEps = 0;

        while ((p + 2 <= pDescEnd) && (numEps < pIfDesc->bNumEndpoints)) {
            const uint8_t len = p[0];
            const uint8_t type = p[1];

            if ((len < 2) || (p + len > pDescEnd) || (type == USB_DT_INTERFACE))
                break;

            if ((type == USB_DT_ENDPOINT) && (len >= 7)) {
                const DESC_EP_T* const pEpDesc = (const DESC_EP_T*) p;
                EP_INFO_T& ep = pIface->alt[0].ep[numEps];

                uint16_t maxPacket = 0;
                std::memcpy(&maxPacket, &pEpDesc->wMaxPacketSize, sizeof(maxPacket));

                ep.bEndpointAddress = pEpDesc->bEndpointAddress;
                ep.bmAttributes = pEpDesc->bmAttributes;
                ep.bInterval = pEpDesc->bInterval;
                ep.wMaxPacketSize = (uint16_t)((maxPacket & 0x07FF) * (1 + ((maxPacket >> 11) & 3)));
                ep.hw_pipe = nullptr;
                numEps++;
            }

            p += len;
        }
    }

    if (xid_driver.probe(pIface) != USBH_OK) {
        usbh_free_mem(pIface, (int) sizeof(IFACE_T));
        return false;
    }

    // Taken: give it to the device the way the core does, so unplugging it later tears it down normally
    pIface->driver = &xid_driver;
    pIface->next = pUdev->iface_list;
    pUdev->iface_list = pIface;
    return true;
}

static UnboundXid* findOrAddUnbound(UDEV_T* const pUdev, const uint8_t ifNum, const uint32_t nowMs) noexcept {
    UnboundXid* pFree = nullptr;

    for (UnboundXid& entry : gUnbound) {
        if ((entry.pUdev == pUdev) && (entry.devNum == pUdev->dev_num) && (entry.ifNum == ifNum))
            return &entry;

        if ((!entry.pUdev) && (!pFree)) {
            pFree = &entry;
        }
    }

    if (pFree) {
        *pFree = {};
        pFree->pUdev = pUdev;
        pFree->devNum = pUdev->dev_num;
        pFree->ifNum = ifNum;
        pFree->firstSeenMs = nowMs;
    }

    return pFree;
}

static bool isDeviceStillPresent(const UDEV_T* const pUdev) noexcept {
    for (UDEV_T* pCur = g_udev_list; pCur; pCur = pCur->next) {
        if (pCur == pUdev)
            return true;
    }

    return false;
}

// Find XID interfaces that no driver took, say so, and give each a few more chances
static void checkUnboundXids(const uint32_t nowMs) noexcept {
    if ((nowMs - gLastUnboundCheckMs) < 1000u)
        return;

    gLastUnboundCheckMs = nowMs;

    // Forget devices that have gone
    for (UnboundXid& entry : gUnbound) {
        if (entry.pUdev && (!isDeviceStillPresent(entry.pUdev))) {
            entry = {};
        }
    }

    for (UDEV_T* pUdev = g_udev_list; pUdev; pUdev = pUdev->next) {
        if (!pUdev->cfd_buff)
            continue;

        const DESC_CONF_T* const pConf = (const DESC_CONF_T*) pUdev->cfd_buff;
        uint16_t totalLen = 0;
        std::memcpy(&totalLen, &pConf->wTotalLength, sizeof(totalLen));

        const uint8_t* const pStart = pUdev->cfd_buff;
        const uint8_t* const pEnd = pStart + ((totalLen < MAX_DESC_BUFF_SIZE) ? totalLen : MAX_DESC_BUFF_SIZE);
        const uint8_t* p = pStart + pConf->bLength;

        while (p + 2 <= pEnd) {
            const uint8_t len = p[0];
            const uint8_t type = p[1];

            if ((len < 2) || (p + len > pEnd))
                break;

            if ((type == USB_DT_INTERFACE) && (len >= 9)) {
                const DESC_IF_T* const pIfDesc = (const DESC_IF_T*) p;

                const bool bIsXid = (
                    (pIfDesc->bInterfaceClass == XID_INTERFACE_CLASS) &&
                    (pIfDesc->bInterfaceSubClass == XID_INTERFACE_SUBCLASS) &&
                    (pIfDesc->bAlternateSetting == 0)
                );

                if (bIsXid && (!isXidBoundTo(pUdev, pIfDesc->bInterfaceNumber))) {
                    UnboundXid* const pEntry = findOrAddUnbound(pUdev, pIfDesc->bInterfaceNumber, nowMs);

                    if (pEntry && (!pEntry->bGaveUp) && ((nowMs - pEntry->lastAttemptMs) >= 2000u)) {
                        if (pEntry->attempts == 0) {
                            padLog(XboxLog::Sev::Warn, true,
                                "pad: vid=%04X pid=%04X has an XID interface (%u) that no driver took - "
                                "it did not answer the XID descriptor request. Offering it again.",
                                (unsigned) pUdev->descriptor.idVendor, (unsigned) pUdev->descriptor.idProduct,
                                (unsigned) pIfDesc->bInterfaceNumber
                            );
                        }

                        pEntry->attempts++;
                        pEntry->lastAttemptMs = nowMs;

                        const bool bTaken = reprobeXidInterface(pUdev, pIfDesc, pEnd);

                        padLog(XboxLog::Sev::Warn, true,
                            "pad: vid=%04X pid=%04X re-probe %u: %s",
                            (unsigned) pUdev->descriptor.idVendor, (unsigned) pUdev->descriptor.idProduct,
                            (unsigned) pEntry->attempts, (bTaken) ? "TAKEN - the pad is usable now" : "still no answer"
                        );

                        // Three goes at most. The request waits up to a second for an answer, and a pad that has not
                        // answered three of them is not going to - better than a hitch every two seconds for good.
                        if (bTaken || (pEntry->attempts >= 3)) {
                            pEntry->bGaveUp = (!bTaken);
                            pEntry->pUdev = (bTaken) ? nullptr : pEntry->pUdev;
                        }
                    }
                }
            }

            p += len;
        }
    }
}

void service() noexcept {
    const uint32_t nowMs = SDL_GetTicks();

    // Pads that have gone. Told apart from a new pad that happens to land in the same slot by its unique id.
    for (PadTrack& track : gPads) {
        if (!track.pDev)
            continue;

        bool bStillHere = false;

        for (xid_dev_t* pDev = usbh_xid_get_device_list(); pDev; pDev = pDev->next) {
            if ((pDev == track.pDev) && (pDev->uid == track.uid) && pDev->iface) {
                bStillHere = true;
                break;
            }
        }

        if (!bStillHere) {
            logDeparture(track, nowMs);
            std::memset((void*) &track, 0, sizeof(track));
        }
    }

    // Pads that are here
    for (xid_dev_t* pDev = usbh_xid_get_device_list(); pDev; pDev = pDev->next) {
        if (!pDev->iface)
            continue;

        PadTrack* pTrack = findTrack(pDev);

        if (!pTrack) {
            pTrack = findTrack(nullptr);

            if (!pTrack)
                continue;

            std::memset((void*) pTrack, 0, sizeof(*pTrack));
            pTrack->uid = pDev->uid;
            pTrack->connectMs = nowMs;
            pTrack->lastSummaryMs = nowMs;
            pTrack->pDev = pDev;        // Last, so the completion hook only ever finds a slot that is ready
            logArrival(*pTrack, pDev, nowMs);
        }

        if (pDev->user_data && (!pTrack->bOpenedBySdl)) {
            pTrack->bOpenedBySdl = true;
            padLog(XboxLog::Sev::Info, true, "pad uid=%u: opened by SDL after %ums", (unsigned) pTrack->uid, (unsigned) (nowMs - pTrack->connectMs));
        }

        tendReadPipe(*pTrack, pDev, nowMs);
        summarise(*pTrack, nowMs);
    }

    checkUnboundXids(nowMs);
}

void shutdownForRelaunch() noexcept {
    // Stop the controller processing its transfer lists and raising interrupts: it no longer touches memory after this.
    // 'HcControl' at zero is the reset state, which is also where the next process's driver starts it from.
    if (_ohci) {
        _ohci->HcInterruptDisable = USBH_HcInterruptDisable_MIE_Msk;
        _ohci->HcControl = 0;
    }

    // And take the interrupt handler off the vector, since the code it points at is about to be replaced.
    // Harmless if it was never connected: the kernel checks that first.
    usbh_ohci_irq_deinit();

    padLog(XboxLog::Sev::Info, true, "usb: host controller stopped for relaunch");
}

int32_t describe(char* const out, const size_t outSize) noexcept {
    if ((!out) || (outSize == 0))
        return 0;

    out[0] = '\0';
    size_t used = 0;
    int32_t numPads = 0;

    for (const PadTrack& track : gPads) {
        if ((!track.pDev) || (used + 48 >= outSize))
            continue;

        int32_t port = 0, hubDepth = 0;

        if (track.pDev->iface && track.pDev->iface->udev) {
            devicePort(track.pDev->iface->udev, port, hubDepth);
        }

        used += (size_t) std::snprintf(
            out + used, outSize - used, "%sP%d %04X:%04X rd %u err %u rst %u",
            (numPads > 0) ? "\n" : "", (int) port, (unsigned) track.pDev->idVendor, (unsigned) track.pDev->idProduct,
            (unsigned) track.reads, (unsigned) track.readErrors, (unsigned) track.restarts
        );

        numPads++;
    }

    return numPads;
}

}   // namespace XboxPads

#endif  // #if defined(__XBOX__)
