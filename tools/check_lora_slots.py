#!/usr/bin/env python3
"""Regression check: every TX slot must leave a usable uplink window.

Bug this pins (found on hardware, 2026-08-29): the backend handed a node slot 7. With
LORA_LINK_SLOT0_S=10 and LORA_LINK_SLOT_GAP_S=7 that puts my_sec at 59, and the uplink
window is (my_sec, 60 - LORA_LINK_GUARD_S) -- empty. The beacon still fired (it triggers
exactly at my_sec), but SMOSB batches and Meshtastic frames never transmitted: the queued
batch stayed pending forever and every later batch was skipped silently.

The constants are read from the REAL src/lora_config.h so the check follows the firmware.

Exit 0 = PASS, 1 = a slot has no usable uplink window, 2 = constants not found.
"""
import os
import re
import sys

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
HEADER = os.path.join(REPO, "src", "lora_config.h")
MIN_WINDOW_S = 5           # a slot needs at least this many seconds to get a frame out
BE_SLOTS_TO_TEST = 16      # the backend field is a byte; test the plausible range


def const(name, text):
    m = re.search(rf"^#define\s+{name}\s+(\d+)", text, re.M)
    if not m:
        print(f"missing {name} in {HEADER}", file=sys.stderr)
        sys.exit(2)
    return int(m.group(1))


def main():
    text = open(HEADER, encoding="utf-8", errors="replace").read()
    slot0 = const("LORA_LINK_SLOT0_S", text)
    gap = const("LORA_LINK_SLOT_GAP_S", text)
    guard = const("LORA_LINK_GUARD_S", text)
    slots = const("LORA_LINK_SLOTS", text)

    print(f"SLOT0={slot0}s GAP={gap}s GUARD={guard}s SLOTS={slots}")
    upper = 60 - guard
    bad = []
    for be_slot in range(BE_SLOTS_TO_TEST):
        slot_eff = be_slot % slots
        my_sec = slot0 + slot_eff * gap
        window = upper - my_sec - 1          # seconds strictly between my_sec and upper
        ok = window >= MIN_WINDOW_S and my_sec < 60
        print(f"  BE slot {be_slot:2d} -> eff {slot_eff} my_sec {my_sec:2d} "
              f"uplink window {max(window,0):2d}s {'OK' if ok else 'FAIL'}")
        if not ok:
            bad.append(be_slot)

    if bad:
        print(f"{len(bad)} slot(s) with no usable uplink window: {bad}")
        return 1
    print("All assertions PASSED")
    return 0


if __name__ == "__main__":
    sys.exit(main())
