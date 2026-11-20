"""Print the knob's log for a number of seconds.

Usage: python tools/serial_log.py [seconds] [--no-reset] [--send CMD ...] [--port PORT]

By default the knob is reset first, so the log starts at boot. --no-reset
listens without disturbing it. Each --send types a console command (bench,
shots, frame <name>, slide <ms>, np, knob <n>, ...), 0.8 s apart. A --send of
"wait N" just pauses N seconds before the next one.

Opening the port with DTR low resets the ESP32-S3 (measured 26 Oct 2026), so
--no-reset opens it with DTR high, which leaves the knob running. Never
reconnects after the port drops: reconnecting in a loop kept resetting it.
Needs pyserial: run with the ESP-IDF Python (. ~/esp/esp-idf/export.sh).
"""
import argparse
import glob
import sys
import time

import serial

ap = argparse.ArgumentParser()
ap.add_argument("seconds", nargs="?", type=float, default=25)
ap.add_argument("--no-reset", action="store_true")
ap.add_argument("--send", action="append", default=[])
ap.add_argument("--port", default=(glob.glob("/dev/cu.usbmodem*") or [None])[0])
args = ap.parse_args()
if not args.port:
    sys.exit("knob not found on USB")


def open_port(keep_running):
    s = serial.Serial()
    s.port = args.port
    s.baudrate = 115200
    s.timeout = 0.2
    s.dtr = keep_running  # DTR high on open: no reset
    s.rts = False
    s.open()
    return s


s = open_port(args.no_reset)
if not args.no_reset:
    s.rts = True  # USB Serial/JTAG hard reset, as esptool does
    time.sleep(0.1)
    s.rts = False
pending = list(args.send)
next_send = time.time() + (0.3 if args.no_reset else 3.0)

end = time.time() + args.seconds
buf = b""
while time.time() < end:
    if pending and time.time() >= next_send:
        cmd = pending.pop(0)
        if cmd.startswith("wait "):
            next_send = time.time() + float(cmd.split()[1])
        else:
            s.write((cmd + "\n").encode())
            next_send = time.time() + 0.8
    try:
        chunk = s.read(4096)
    except serial.SerialException:
        if args.no_reset:
            sys.exit("port dropped; stopping rather than reconnecting")
        time.sleep(0.3)  # after our own reset the port comes back once
        try:
            s.close()
            s = open_port(False)
        except serial.SerialException:
            pass
        continue
    if chunk:
        buf += chunk
        *lines, buf = buf.split(b"\n")
        for line in lines:
            print(line.decode("utf-8", "replace").rstrip(), flush=True)
