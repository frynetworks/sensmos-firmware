#!/usr/bin/env python3
# Heap gate for the nRF52840 port. Captures the node's serial output for N seconds and
# asserts: steady state reached ("ready — heap"), [HEAP] free= never below the floor,
# no crash markers, and heap drift start→end within tolerance. The floor is a CLI
# argument — never hardcode it into the firmware or loosen assertions to pass.
#
#   py -3 tools/gate_heap_nrf.py [--seconds 180] [--floor 60000] [--drift-max 1024] [--out FILE]
#
# Exit 0 = PASS, 1 = FAIL. Port resolved by USB VID:PID (2886:8044 app CDC), never a
# hardcoded COM number (it renumbers around DFU). Does NOT reset the board — call
# tools/flash_capture.py --reboot first if a fresh-boot window is needed.
import sys, time, re, argparse

try:
    import serial
    import serial.tools.list_ports as lp
except ImportError:
    print("pyserial required: py -3 -m pip install pyserial"); sys.exit(2)

APP = [(0x2886, 0x8044), (0x239A, 0x810B)]
CRASH = ("HardFault", "Exception", "assert failed", "panic", "CRASH", "[ERROR]",
         "Guru Meditation", "wdt reset")
HEAP_RE  = re.compile(r"\[HEAP\] free=(\d+) max_block=(\d+) uptime=(\d+)s")
READY_RE = re.compile(r"ready . heap (\d+)k free, blk (\d+)k")

def find_port():
    for p in lp.comports():
        if p.vid is not None and (p.vid, p.pid) in APP:
            return p.device
    return None

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--seconds", type=int, default=180)
    ap.add_argument("--floor", type=int, default=60000)
    ap.add_argument("--drift-max", type=int, default=1024)
    ap.add_argument("--out", default=None)
    a = ap.parse_args()

    port = find_port()
    if not port:
        print("[gate] FAIL: no app CDC port (2886:8044) found"); sys.exit(1)
    ser = serial.Serial(port, 115200, timeout=0.5)
    out = open(a.out, "w", encoding="utf-8") if a.out else None
    print("[gate] capturing %ds from %s, floor=%d drift_max=%d"
          % (a.seconds, port, a.floor, a.drift_max))

    start = time.time()
    buf = b""
    ready = False
    first_free = None; last_free = None; min_free = None
    samples = 0; crash = None
    try:
        while time.time() - start < a.seconds:
            chunk = ser.read(512)
            if not chunk: continue
            buf += chunk
            while b"\n" in buf:
                raw, buf = buf.split(b"\n", 1)
                line = raw.decode("utf-8", "replace").rstrip("\r")
                if out: out.write(line + "\n"); out.flush()
                for c in CRASH:
                    if c in line: crash = line
                if crash: break
                if READY_RE.search(line): ready = True
                m = HEAP_RE.search(line)
                if m:
                    free = int(m.group(1)); samples += 1
                    if first_free is None: first_free = free
                    last_free = free
                    if min_free is None or free < min_free: min_free = free
            if crash: break
    finally:
        ser.close()
        if out: out.close()

    if crash:
        print("[gate] FAIL crash marker: %s" % crash); sys.exit(1)
    if not ready:
        print("[gate] FAIL: steady-state 'ready' line never seen"); sys.exit(1)
    if samples < 2:
        print("[gate] FAIL: only %d [HEAP] samples" % samples); sys.exit(1)
    if min_free < a.floor:
        print("[gate] FAIL: min free %d < floor %d" % (min_free, a.floor)); sys.exit(1)
    drift = abs(last_free - first_free)
    if drift > a.drift_max:
        print("[gate] FAIL: heap drift %dB (first %d, last %d) > %d"
              % (drift, first_free, last_free, a.drift_max)); sys.exit(1)
    print("[gate] PASS: samples=%d first=%d last=%d min=%d drift=%dB floor=%d, no crash in %ds"
          % (samples, first_free, last_free, min_free, drift, a.floor, a.seconds))
    sys.exit(0)

if __name__ == "__main__":
    main()
