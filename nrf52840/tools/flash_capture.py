#!/usr/bin/env python3
# flash_capture.py — flash the XIAO nRF52840 over serial DFU and/or capture its serial
# output from boot. Ports are resolved by USB VID:PID on every invocation (they
# renumber between app and bootloader) — never hardcode a COM number.
#
#   py -3 tools/flash_capture.py --flash [--seconds 20] [--out FILE]
#   py -3 tools/flash_capture.py --seconds 180 --out soak.txt     (capture only, no reset)
#   py -3 tools/flash_capture.py --reboot --seconds 30            (reboot via DFU re-flash, capture boot)
#
# The XIAO's app CDC is 2886:8044 (Seeed) — 239A:810B was the factory Meshtastic image.
# Bootloader CDC is 2886:0045. Reset path: 1200bps touch -> bootloader -> DFU package.
import sys, os, time, argparse, subprocess

try:
    import serial
    import serial.tools.list_ports as lp
except ImportError:
    print("pyserial required: py -3 -m pip install pyserial"); sys.exit(2)

APP  = [(0x2886, 0x8044), (0x239A, 0x810B)]
BOOT = [(0x2886, 0x0045)]
NRFUTIL = os.path.expanduser(
    "~/.platformio/packages/framework-arduinoadafruitnrf52/tools/adafruit-nrfutil/win32/adafruit-nrfutil.exe")
PKG = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
                   ".pio", "build", "xiao_nrf52840", "firmware.zip")

def find_port(pairs):
    for p in lp.comports():
        if p.vid is not None and (p.vid, p.pid) in pairs:
            return p.device
    return None

def wait_port(pairs, seconds):
    end = time.time() + seconds
    while time.time() < end:
        d = find_port(pairs)
        if d: return d
        time.sleep(0.25)
    return None

def touch_to_bootloader():
    boot = find_port(BOOT)
    if boot: return boot
    app = find_port(APP)
    if not app:
        print("[fc] no app or bootloader port found"); return None
    try:
        s = serial.Serial(app, 1200); s.dtr = True; s.rts = True
        time.sleep(0.3); s.close()
    except Exception as e:
        print("[fc] touch exception (often benign):", e)
    return wait_port(BOOT, 30)

def dfu_flash(boot):
    r = subprocess.run([NRFUTIL, "dfu", "serial", "-pkg", PKG, "-p", boot,
                        "-b", "115200", "--singlebank"],
                       capture_output=True, text=True, timeout=180)
    tail = (r.stdout or "") + (r.stderr or "")
    print("[fc] nrfutil:", tail.strip().splitlines()[-1] if tail.strip() else "(no output)")
    return r.returncode == 0

def capture(seconds, out_path):
    port = wait_port(APP, 30)
    if not port:
        print("[fc] app port never appeared"); return 1
    # Open ASAP so the firmware's 3s Serial-wait window catches us and boot lines land here.
    ser = None
    for _ in range(40):
        try:
            ser = serial.Serial(port, 115200, timeout=0.5); break
        except Exception:
            time.sleep(0.25)
    if not ser:
        print("[fc] could not open", port); return 1
    print("[fc] capturing %ds from %s" % (seconds, port))
    out = open(out_path, "w", encoding="utf-8") if out_path else None
    end = time.time() + seconds
    buf = b""
    try:
        while time.time() < end:
            chunk = ser.read(512)
            if not chunk: continue
            buf += chunk
            while b"\n" in buf:
                raw, buf = buf.split(b"\n", 1)
                line = raw.decode("utf-8", "replace").rstrip("\r")
                print(line)
                if out: out.write(line + "\n"); out.flush()
    finally:
        ser.close()
        if out: out.close()
    return 0

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--flash", action="store_true", help="touch->bootloader->DFU flash first")
    ap.add_argument("--reboot", action="store_true", help="reboot via DFU re-flash (same image)")
    ap.add_argument("--seconds", type=int, default=20)
    ap.add_argument("--out", default=None)
    a = ap.parse_args()
    if a.flash or a.reboot:
        boot = touch_to_bootloader()
        if not boot:
            print("[fc] FAIL: bootloader port not found"); sys.exit(1)
        print("[fc] bootloader at", boot)
        if not dfu_flash(boot):
            print("[fc] FAIL: DFU flash failed"); sys.exit(1)
    sys.exit(capture(a.seconds, a.out))

if __name__ == "__main__":
    main()
