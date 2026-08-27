#!/usr/bin/env python3
# Per-sub-band duty-cycle gate for the ESP32 dual-stack build.
#
# Adapted from nrf52840/tools/gate_duty_bands.py — SAME three assertions and the same log
# regexes, because the ESP32 port emits the same log shapes on purpose. Two differences,
# both about how you reach the node:
#   · the port is a CLI argument, not a USB VID:PID lookup. The nRF gate can resolve its
#     board by VID:PID (2886:8044) because that pair identifies the XIAO uniquely; an ESP32
#     LoRa board is whatever USB-serial bridge the vendor soldered on (CP2102, CH340, native
#     CDC...), so the same trick would match unrelated hardware on the operator's machine.
#   · the node is driven over WiFi-less operation OR the LoRa fallback, so the gate does not
#     assume a WS connection exists.
#
# EU868 does not have one duty-cycle budget, it has one PER SUB-BAND:
#   g1  868.0-868.6  MHz -> 1%  =  36 000 ms/h   (SENSMOS beacons + SMOSB uplinks)
#   g4  869.4-869.65 MHz -> 10% = 360 000 ms/h   (Meshtastic)
# Charging both protocols to a single 1% counter makes them starve each other: Meshtastic
# SF11 airtime eats the SMOS budget, and around 36 s/h BOTH protocols go silent even though
# g4 still has ~90% of its allowance unused.
#
# This gate drives real batches on the device and asserts the split holds:
#   1. mesh keeps transmitting after the node has spent more than the old shared cap
#   2. no cross-contamination — Meshtastic airtime must not land in the g1 counter
#   3. neither band ever exceeds its own regulatory limit
#
#   py -3 tools/gate_duty_bands_esp32.py COM7 [--minutes 10] [--out FILE]
#
# Exit 0 = PASS, 1 = FAIL, 2 = environment problem. Counters are read from the node's own
# TX / budget-spent log lines rather than from get_info, so the gate depends only on what
# the radio path prints.
#
# Runs against the EU868 default plan ONLY. Other regions use different band names and
# different (or absent) duty limits, so the constants below would not describe them.
import sys, time, re, argparse

try:
    import serial
except ImportError:
    print("pyserial required: py -3 -m pip install pyserial"); sys.exit(2)

SHARED_CAP_MS = 36000          # the old single-counter budget = where mesh used to die
G1_LIMIT_MS   = 36000
G4_LIMIT_MS   = 360000
# A node at the old ceiling never reports a number ABOVE it — it refuses the next
# transmission and pins just under. The reference point is therefore "within one small
# frame of the cap", not "past it".
CAP_REACHED_MS = SHARED_CAP_MS - 2000

MESH_SENT_RE = re.compile(r"\[mesh\] packet (\d+)/(\d+) sent")
PAUSE_RE     = re.compile(r"budget spent|TX paused|paused —|skipped —")
# Airtime of a single transmission, however the sender wraps it.
AIR_RE       = re.compile(r"(\d+)ms air")
# Running counter, both the pre-split form ("duty 35988ms/h") and the split form
# ("duty 4502/36000ms/h g1" / "... (35867/36000ms per h ...)").
DUTY_RE      = re.compile(r"duty (?:cycle )?(?:budget spent \()?(\d+)(?:/(\d+))?ms(?:/| per )h")
PAUSE_NUM_RE = re.compile(r"\((\d+)/(\d+)ms per h")


def band_of(line):
    """Which sub-band a log line is talking about: mesh lines are g4, lora lines g1."""
    if line.startswith("[mesh]"):
        return "g4"
    if line.startswith("[lora]"):
        return "g1"
    return None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("port", help="serial port of the node, e.g. COM7 or /dev/ttyUSB0")
    ap.add_argument("--minutes", type=float, default=10.0)
    ap.add_argument("--out", default=None)
    a = ap.parse_args()

    try:
        ser = serial.Serial(a.port, 115200, timeout=0.5)
    except Exception as e:
        print("[duty] FAIL: cannot open %s (%s)" % (a.port, e)); sys.exit(2)
    out = open(a.out, "w", encoding="utf-8") if a.out else None
    print("[duty] driving %s for %.1f min (shared-cap reference %d ms)"
          % (a.port, a.minutes, SHARED_CAP_MS))

    deadline = time.time() + a.minutes * 60
    last_trigger = 0.0
    counters = {"g1": 0, "g4": 0}      # latest counter each band reported
    limits   = {"g1": None, "g4": None}
    mesh_after_cap = mesh_before_cap = mesh_seen = 0
    mesh_paused_after_cap = False
    cap_crossed = False
    over_limit = None

    try:
        while time.time() < deadline:
            # A batch every ~62 s: the firmware's own minimum send interval is 60 s,
            # so this is the fastest legitimate way to accumulate airtime.
            if time.time() - last_trigger > 62:
                last_trigger = time.time()
                ser.write(b'{"cmd":"send_now"}\n'); ser.flush()

            line = ser.readline().decode("utf-8", "replace").rstrip()
            if not line or line.startswith(("[HEAP]", "ready")):
                continue
            if out:
                out.write(line + "\n"); out.flush()

            b = band_of(line)
            if b:
                m = PAUSE_NUM_RE.search(line) or DUTY_RE.search(line)
                if m:
                    counters[b] = int(m.group(1))
                    if m.lastindex and m.group(2):
                        limits[b] = int(m.group(2))
                    total = counters["g1"] + counters["g4"]
                    if total >= CAP_REACHED_MS:
                        cap_crossed = True
                    if limits[b] and counters[b] > limits[b]:
                        over_limit = "%s reported %d over its own limit %d" % (
                            b, counters[b], limits[b])
                    lim = G1_LIMIT_MS if b == "g1" else G4_LIMIT_MS
                    if counters[b] > lim:
                        over_limit = "%s counter %d exceeds the regulatory limit %d" % (
                            b, counters[b], lim)

            if MESH_SENT_RE.search(line):
                mesh_seen += 1
                if cap_crossed:
                    mesh_after_cap += 1
                else:
                    mesh_before_cap += 1
            elif PAUSE_RE.search(line):
                if cap_crossed and line.startswith("[mesh]"):
                    mesh_paused_after_cap = True
                if out:
                    out.write("[gate] pause seen: g1=%d g4=%d crossed=%s\n"
                              % (counters["g1"], counters["g4"], cap_crossed)); out.flush()
    finally:
        ser.close()
        if out: out.close()

    total = counters["g1"] + counters["g4"]
    print("[duty] counters g1=%d/%d g4=%d/%d (sum %d) | mesh packets %d "
          "(before cap %d, after cap %d)"
          % (counters["g1"], G1_LIMIT_MS, counters["g4"], G4_LIMIT_MS, total,
             mesh_seen, mesh_before_cap, mesh_after_cap))

    # 3 — regulatory: neither band over its own limit
    if over_limit:
        print("[duty] FAIL: %s" % over_limit); sys.exit(1)

    # 2 — cross-contamination: with a split counter, mesh airtime lands in g4 and
    #     g1 stays at SMOS-only airtime. A firmware that never reports g4 while mesh
    #     is transmitting is still running one shared budget.
    if mesh_seen > 0 and counters["g4"] == 0:
        print("[duty] FAIL: %d Meshtastic packets transmitted but the g4 counter never "
              "moved — mesh airtime is being charged to the SMOS band (single shared "
              "budget)" % mesh_seen); sys.exit(1)

    # 1 — the actual regression: mesh must survive past the old shared cap
    if not cap_crossed:
        print("[duty] FAIL: run too short — node has only spent %dms this hour, never "
              "reached the %dms reference point. Counters carry over, so a follow-up run "
              "continues from here." % (total, CAP_REACHED_MS)); sys.exit(1)
    if mesh_paused_after_cap and mesh_after_cap == 0:
        print("[duty] FAIL: mesh TX stopped once total airtime passed %dms while its own "
              "sub-band still had headroom (g4 limit %dms) — duty budget is not split"
              % (SHARED_CAP_MS, G4_LIMIT_MS)); sys.exit(1)
    if mesh_after_cap == 0:
        print("[duty] FAIL: no Meshtastic packet after the node passed %dms of total "
              "airtime" % SHARED_CAP_MS); sys.exit(1)

    print("[duty] PASS: %d mesh packets sent after the old %dms shared cap; g1=%d and "
          "g4=%d tracked independently, both within their own limits"
          % (mesh_after_cap, SHARED_CAP_MS, counters["g1"], counters["g4"]))
    sys.exit(0)


if __name__ == "__main__":
    main()
