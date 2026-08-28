#!/usr/bin/env python3
"""Regression check: the LoRa pinout table must be selected per chip family.

Bug this pins (found on hardware, 2026-08-28): src/lora_config.h gated its pinout
table on CONFIG_IDF_TARGET_ESP32 without including the header that defines it, so on
a plain ESP32 build the gate was always false -> the six ESP32-S3 rows compiled in and
the T-Beam v1.1 row did not. The probe then drove GPIO 6-11 (the SPI flash lines) and
the board reset in a TG1WDT loop before ever reaching its own row.

The check preprocesses the REAL header with the real compiler for both families and
asserts which board rows survive. It fails if the gate silently stops working again.

Exit 0 = PASS, 1 = assertion failure, 2 = environment problem (no compiler found).
"""
import os
import re
import subprocess
import sys
import tempfile

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
HEADER = os.path.join(REPO, "src", "lora_config.h")

# family -> (macro the core defines, rows that must be present, rows that must be absent)
CASES = [
    ("esp32",   "CONFIG_IDF_TARGET_ESP32",   ["tbeam-v1.1"], ["xiao-s3", "heltec-s3", "lilygo-t3s3", "rak3312"]),
    ("esp32s3", "CONFIG_IDF_TARGET_ESP32S3", ["xiao-s3", "heltec-s3", "lilygo-t3s3", "tbeam-s3-core", "rak3312"], ["tbeam-v1.1"]),
]


def find_cpp():
    import glob
    candidates = ["cpp", "gcc", "clang"]
    # Arduino's own xtensa toolchain is the closest thing to the real build.
    candidates += sorted(glob.glob(os.path.join(
        os.path.expanduser("~"), "AppData", "Local", "Arduino15", "packages", "esp32",
        "tools", "esp-x32", "*", "bin", "xtensa-esp32-elf-gcc.exe")))
    for exe in candidates:
        try:
            subprocess.run([exe, "--version"], capture_output=True, check=True)
            return exe
        except Exception:
            continue
    return None


def expand(cpp, family_macro):
    """Expand LORA_PINOUTS for one chip family.

    The family macro is supplied ONLY by a stub sdkconfig.h, the way the ESP32 core
    supplies it on a real build. It is deliberately NOT defined in the probe: the bug
    this check exists for was lora_config.h testing the macro without including the
    header that defines it, which no test can see if the test defines it itself.
    """
    src = (
        '#define LORA_ENABLED 1\n'
        '#include "lora_config.h"\n'
        'PINOUT_TABLE_IS LORA_PINOUTS\n'
    )
    with tempfile.TemporaryDirectory() as td:
        probe = os.path.join(td, "probe.c")
        with open(probe, "w", encoding="utf-8") as fh:
            fh.write(src)
        stub = os.path.join(td, "sdkconfig.h")
        with open(stub, "w", encoding="utf-8") as fh:
            fh.write(f"#define {family_macro} 1\n")
        cmd = [cpp, "-E", "-P", "-I", os.path.join(REPO, "src"), "-I", td, probe]
        if not cpp.endswith("cpp"):
            cmd.insert(1, "-x")
            cmd.insert(2, "c")
        res = subprocess.run(cmd, capture_output=True, text=True)
        if res.returncode != 0:
            print(res.stderr.strip()[:400])
            return None
        for line in res.stdout.splitlines():
            if "PINOUT_TABLE_IS" in line:
                return line
    return None


def main():
    cpp = find_cpp()
    if cpp is None:
        print("no C preprocessor on PATH (cpp/gcc/clang) — cannot run", file=sys.stderr)
        return 2
    if not os.path.exists(HEADER):
        print(f"missing {HEADER}", file=sys.stderr)
        return 2

    failures = []
    for family, macro, must_have, must_not in CASES:
        table = expand(cpp, macro)
        if table is None:
            print(f"{family}: PREPROCESS FAILED")
            failures.append(family)
            continue
        names = set(re.findall(r'"([a-z0-9.\-@]+)"', table))
        missing = [n for n in must_have if n not in names]
        extra = [n for n in must_not if n in names]
        status = "OK" if not missing and not extra else "FAIL"
        print(f"{family:8s} rows={sorted(names)} {status}")
        if missing:
            print(f"  missing rows: {missing}")
        if extra:
            print(f"  rows that must not be built for this family: {extra}")
        if missing or extra:
            failures.append(family)

    if failures:
        print(f"{len(failures)} assertion(s) FAILED")
        return 1
    print("All assertions PASSED")
    return 0


if __name__ == "__main__":
    sys.exit(main())
