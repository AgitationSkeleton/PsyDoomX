#!/usr/bin/env python3
"""
Listener for the PsyDoom Xbox diagnostic relay.

Run this on the development machine before starting the game on the console. It accepts the console's
connection, writes everything it sends to a timestamped session file, and echoes it to the terminal so
a session can be watched as it happens.

    python tools/xbox_log_server.py                 # listen on all interfaces, port 9909
    python tools/xbox_log_server.py --port 9909
    python tools/xbox_log_server.py --only video,audio
    python tools/xbox_log_server.py --quiet         # write the file, do not echo

The console reconnects on its own if this is started late or restarted mid-session, so it can be left
running across many runs of the game.

Wire format is one line per record:

    SECONDS.MICROS  SEVERITY  SUBSYSTEM  MESSAGE

The timestamp is microseconds since the relay started on the console, from a single monotonic clock,
so a slow frame and the disc read that caused it can be laid against each other.
"""

import argparse
import datetime
import os
import socket
import sys
import threading

DEFAULT_PORT = 9909
LOG_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "logs", "xbox")

# Terminal colours, so warnings and errors are findable in a fast-moving trace
COLOURS = {
    "T": "\033[90m",    # trace: grey
    "I": "\033[0m",     # info: normal
    "W": "\033[33m",    # warn: yellow
    "E": "\033[31m",    # error: red
}
RESET = "\033[0m"


def session_path():
    os.makedirs(LOG_DIR, exist_ok=True)
    stamp = datetime.datetime.now().strftime("%Y%m%d-%H%M%S")
    return os.path.join(LOG_DIR, "session-%s.log" % stamp)


def handle_console(conn, addr, args):
    path = session_path()
    print("--- console connected from %s, writing %s ---" % (addr[0], path))

    wanted = set(s.strip() for s in args.only.split(",")) if args.only else None
    line_count = 0
    dropped_seen = 0

    with open(path, "w", encoding="utf-8", buffering=1) as out:
        out.write("# PsyDoom Xbox session, console %s, host time %s\n" % (addr[0], datetime.datetime.now()))
        buf = b""

        while True:
            try:
                chunk = conn.recv(8192)
            except (ConnectionResetError, OSError):
                break

            if not chunk:
                break

            buf += chunk

            while b"\n" in buf:
                raw, buf = buf.split(b"\n", 1)
                line = raw.decode("utf-8", errors="replace").rstrip("\r")

                if not line:
                    continue

                line_count += 1
                out.write(line + "\n")

                # A rising drop count means the relay is saturated and what follows is a sample
                if "relay dropped" in line:
                    dropped_seen += 1

                if args.quiet:
                    continue

                parts = line.split(None, 3)
                sev = parts[1] if len(parts) > 2 else "I"

                if wanted and len(parts) > 2 and parts[2] not in wanted:
                    continue

                colour = COLOURS.get(sev, "")
                sys.stdout.write("%s%s%s\n" % (colour, line, RESET if colour else ""))
                sys.stdout.flush()

    print("--- console %s disconnected: %d lines, %s ---" % (
        addr[0], line_count,
        "%d drop reports" % dropped_seen if dropped_seen else "no drops reported"
    ))
    conn.close()


def main():
    ap = argparse.ArgumentParser(description="Listener for the PsyDoom Xbox diagnostic relay")
    ap.add_argument("--port", type=int, default=DEFAULT_PORT)
    ap.add_argument("--host", default="0.0.0.0", help="interface to listen on")
    ap.add_argument("--only", default="", help="comma separated subsystems to echo, e.g. video,audio")
    ap.add_argument("--quiet", action="store_true", help="write the session file without echoing")
    args = ap.parse_args()

    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind((args.host, args.port))
    srv.listen(4)

    # Worth printing: the console needs this address, and it is the first thing to get wrong
    print("PsyDoom Xbox log server listening on %s:%d" % (args.host, args.port))
    print("Put '<this machine's IP>:%d' in E:\\Apps\\PsyDoomX\\logserver.txt on the console" % args.port)
    print("Sessions are written to %s" % os.path.normpath(LOG_DIR))
    print("Waiting for the console...")

    try:
        while True:
            conn, addr = srv.accept()
            threading.Thread(target=handle_console, args=(conn, addr, args), daemon=True).start()
    except KeyboardInterrupt:
        print("\nstopped")
    finally:
        srv.close()


if __name__ == "__main__":
    main()
