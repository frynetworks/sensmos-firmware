#!/usr/bin/env python3
"""check_fw_chip.py — regression check for chip-identity strings.

Verifies that FWD_CHIP (src/fw_digest.h) and OTA_CHIP (src/ota.cpp) resolve to the
correct, chip-family-aware string for a matrix of (chip, LORA_ENABLED) combinations.
This exists because of a real bug: ota.cpp used to test `#if LORA_ENABLED` BEFORE any
CONFIG_IDF_TARGET_* macro, so ANY chip family compiled with -DLORA_ENABLED=1 would claim
OTA_CHIP "esp32s3-lora", even a plain ESP32 (T-Beam) build. See
docs/upstream-reports/webflasher-esp32-lora-manifest.md and the fix commit for context.

This does NOT hardcode the expected strings and diff them against a fixed table —
that would happily pass even if the source regressed to always returning the same
string. Instead it extracts the ACTUAL #if/#elif/#else/#endif chain that defines
FWD_CHIP / OTA_CHIP out of the real source files and evaluates it against synthetic
compiler-macro environments, exactly like the real preprocessor would.

Two evaluation backends:
  1. "real cpp" — if a C preprocessor is found on PATH (cpp, gcc, or an ESP32 Arduino
     core xtensa-*-elf-gcc under %LOCALAPPDATA%\\Arduino15), the extracted block is fed
     to it with -D flags and the substituted output is read back. This is authoritative.
  2. "python fallback" — a small conditional-directive evaluator implemented in this
     file, used when no preprocessor is available (e.g. this sandbox has neither
     arduino-cli nor gcc/cpp installed — verified 2026-08-27, `which cpp/gcc/
     xtensa-esp32-elf-gcc` all fail). It still parses the REAL file content; it does not
     use a hand-written model of what the file "should" say.

Usage:
    python check_fw_chip.py [--fw-digest PATH] [--ota PATH]

Exit code 0 = all assertions passed, 1 = at least one failed (prints FAIL lines).
"""
import argparse
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent

# ── directive-chain extraction (works on raw source text) ──────────────────────

DIRECTIVE_RE = re.compile(r'^\s*#\s*(if|ifdef|ifndef|elif|else|endif)\b\s*(.*)$')


def find_chain_start(lines, anchor_re):
    """Return the index of the first line matching anchor_re (a #if/#ifdef directive)."""
    for i, l in enumerate(lines):
        if anchor_re.match(l.strip()):
            return i
    raise RuntimeError(f"anchor not found: {anchor_re.pattern}")


def parse_if_chain(lines, start_idx):
    """Parse a #if/#ifdef chain starting at lines[start_idx].
    Returns (branches, endif_idx). branches = [(cond_str_or_None, body_lines), ...].
    cond_str is a Python-evaluable-after-substitution string; None means #else (always true
    if reached).
    """
    m = DIRECTIVE_RE.match(lines[start_idx].strip())
    directive, rest = m.group(1), m.group(2).strip()
    if directive == 'ifdef':
        cond = f'defined({rest})'
    elif directive == 'ifndef':
        cond = f'not defined({rest})'
    elif directive == 'if':
        cond = rest
    else:
        raise RuntimeError(f"expected #if/#ifdef/#ifndef at line {start_idx}: {lines[start_idx]!r}")

    branches = []
    cur_cond = cond
    cur_body_start = start_idx + 1
    depth = 0
    i = cur_body_start
    while i < len(lines):
        s = lines[i].strip()
        d = DIRECTIVE_RE.match(s)
        if d:
            kw = d.group(1)
            if kw in ('if', 'ifdef', 'ifndef'):
                depth += 1
            elif kw == 'endif':
                if depth == 0:
                    branches.append((cur_cond, lines[cur_body_start:i]))
                    return branches, i
                depth -= 1
            elif kw == 'elif' and depth == 0:
                branches.append((cur_cond, lines[cur_body_start:i]))
                cur_cond = d.group(2).strip()
                cur_body_start = i + 1
            elif kw == 'else' and depth == 0:
                branches.append((cur_cond, lines[cur_body_start:i]))
                cur_cond = None
                cur_body_start = i + 1
        i += 1
    raise RuntimeError("unterminated #if chain (no matching #endif)")


def eval_cond(cond, env):
    if cond is None:
        return True
    expr = cond
    # defined(NAME) -> True/False, evaluated first so bare-identifier substitution
    # below doesn't also try to rewrite NAME independently.
    expr = re.sub(
        r'defined\s*\(\s*(\w+)\s*\)',
        lambda m: str(env.get(m.group(1)) is not None),
        expr,
    )

    def repl_ident(m):
        name = m.group(0)
        if name in ('True', 'False', 'and', 'or', 'not'):
            return name
        v = env.get(name, 0)
        if v is None:
            v = 0
        return str(v)

    expr = re.sub(r'\b[A-Za-z_]\w*\b', repl_ident, expr)
    # eval() is safe here: expr is our OWN regex-substituted preprocessor condition
    # (already reduced to Python bool/int literals + and/or/not/comparators by the two
    # substitution passes above), builtins are stripped, and the source text it derives
    # from is this repo's own tracked C++ headers, not untrusted/external input.
    return bool(eval(expr, {'__builtins__': {}}, {}))


def resolve_define_python(lines, start_idx, env, target_name):
    """Python-fallback: walk the real #if chain at lines[start_idx], pick the branch
    that matches env, recurse into nested chains, and return the string value assigned
    to `#define target_name "..."` in the reached branch. None if not found/no match.
    """
    branches, _ = parse_if_chain(lines, start_idx)
    for cond, body in branches:
        if not eval_cond(cond, env):
            continue
        # does this branch's body itself open with a nested #if/#ifdef chain?
        nested_at = None
        for j, l in enumerate(body):
            s = l.strip()
            if not s or s.startswith('//'):
                continue
            if DIRECTIVE_RE.match(s) and DIRECTIVE_RE.match(s).group(1) in ('if', 'ifdef', 'ifndef'):
                nested_at = j
            break  # only look at the first non-blank/non-comment line
        if nested_at is not None:
            return resolve_define_python(body, nested_at, env, target_name)
        for l in body:
            dm = re.match(rf'\s*#\s*define\s+{re.escape(target_name)}\s+"([^"]*)"', l)
            if dm:
                return dm.group(1)
        return None
    return None


# ── real-preprocessor backend ───────────────────────────────────────────────────

def find_real_cpp():
    for cand in ('cpp', 'gcc', 'xtensa-esp32-elf-gcc', 'xtensa-esp32s3-elf-gcc'):
        p = shutil.which(cand)
        if p:
            return p
    import os
    localappdata = os.environ.get('LOCALAPPDATA', '')
    if localappdata:
        base = Path(localappdata) / 'Arduino15' / 'packages' / 'esp32' / 'tools'
        if base.exists():
            for hit in base.rglob('*gcc.exe'):
                return str(hit)
            for hit in base.rglob('*gcc'):
                return str(hit)
    return None


def resolve_define_realcpp(cpp_bin, chain_lines, env, target_name):
    defs = []
    for name in ('CONFIG_IDF_TARGET_ESP32', 'CONFIG_IDF_TARGET_ESP32S3',
                 'CONFIG_IDF_TARGET_ESP32C3', 'CONFIG_IDF_TARGET_ESP32S2',
                 'CONFIG_IDF_TARGET_ESP32C6', 'LORA_ENABLED'):
        defs.append(f'-D{name}={1 if env.get(name) else 0}')
    if env.get('CONFIG_SPIRAM_MODE_OCT') is not None:
        defs.append('-DCONFIG_SPIRAM_MODE_OCT=1')

    harness = '\n'.join(chain_lines) + f'\nRESULT_MARKER: {target_name}\n'
    with tempfile.NamedTemporaryFile('w', suffix='.c', delete=False) as f:
        f.write(harness)
        path = f.name
    try:
        is_gcc = 'gcc' in Path(cpp_bin).name
        cmd = [cpp_bin, '-E', '-P'] + defs + [path] if not is_gcc else \
              [cpp_bin, '-E', '-P'] + defs + [path]
        out = subprocess.run(cmd, capture_output=True, text=True, timeout=20)
        m = re.search(r'RESULT_MARKER:\s*"([^"]*)"', out.stdout)
        return m.group(1) if m else None
    finally:
        Path(path).unlink(missing_ok=True)


# ── matrix + assertions ─────────────────────────────────────────────────────────

MATRIX = [
    # (label, env-without-LORA_ENABLED)
    ('esp32', {'CONFIG_IDF_TARGET_ESP32': 1}),
    ('esp32s3', {'CONFIG_IDF_TARGET_ESP32S3': 1}),
    ('esp32s3-n16r8(OCT)', {'CONFIG_IDF_TARGET_ESP32S3': 1, 'CONFIG_SPIRAM_MODE_OCT': 1}),
    ('esp32c3', {'CONFIG_IDF_TARGET_ESP32C3': 1}),
]

EXPECTED = {
    # (chip_label, lora) -> expected chip string (None = "not asserted / don't care")
    ('esp32', 0): 'esp32',
    ('esp32', 1): 'esp32-lora',
    ('esp32s3', 0): 'esp32s3',
    ('esp32s3', 1): 'esp32s3-lora',
    ('esp32s3-n16r8(OCT)', 0): 'esp32s3-n16r8',
    ('esp32s3-n16r8(OCT)', 1): 'esp32s3-n16r8',  # OCT branch wins per fw_digest.h/ota.cpp comment
    ('esp32c3', 0): 'esp32c3',
    ('esp32c3', 1): 'esp32c3',  # LoRa not wired for c3 — LORA_ENABLED has no effect here
}


def load_lines(path):
    return Path(path).read_text(encoding='utf-8').splitlines()


def run(fw_digest_path, ota_path):
    cpp_bin = find_real_cpp()
    backend = 'real-cpp' if cpp_bin else 'python-fallback'
    print(f"[check_fw_chip] backend: {backend}"
          + (f" ({cpp_bin})" if cpp_bin else " — no cpp/gcc/xtensa-*-gcc found on PATH or under %LOCALAPPDATA%\\Arduino15"))

    fwd_lines = load_lines(fw_digest_path)
    ota_lines = load_lines(ota_path)
    fwd_start = find_chain_start(fwd_lines, re.compile(r'#\s*if\s+CONFIG_IDF_TARGET_ESP32\b'))
    ota_start = find_chain_start(ota_lines, re.compile(r'#\s*if(?:def)?\b'))
    # ota.cpp pre-fix has `#if LORA_ENABLED` as the FIRST directive (the bug); post-fix
    # it's `#if CONFIG_IDF_TARGET_ESP32`. Either way this is the FWD/OTA chip-resolution
    # chain's start — there is exactly one such top-level chain before OTA_CHIP is used.
    ota_branches, ota_end = parse_if_chain(ota_lines, ota_start)
    fwd_branches, fwd_end = parse_if_chain(fwd_lines, fwd_start)
    ota_chain = ota_lines[ota_start:ota_end + 1]
    fwd_chain = fwd_lines[fwd_start:fwd_end + 1]

    failures = []
    results = []
    for label, chip_env in MATRIX:
        for lora in (0, 1):
            env = dict(chip_env)
            env['LORA_ENABLED'] = lora
            if cpp_bin:
                fwd_val = resolve_define_realcpp(cpp_bin, fwd_chain, env, 'FWD_CHIP')
                ota_val = resolve_define_realcpp(cpp_bin, ota_chain, env, 'OTA_CHIP')
            else:
                fwd_val = resolve_define_python(fwd_lines, fwd_start, env, 'FWD_CHIP')
                ota_val = resolve_define_python(ota_lines, ota_start, env, 'OTA_CHIP')

            exp = EXPECTED.get((label, lora))
            results.append((label, lora, fwd_val, ota_val, exp))
            if exp is None:
                continue
            if fwd_val != exp:
                failures.append(f"FAIL  {label} LORA={lora}: FWD_CHIP={fwd_val!r} want {exp!r}")
            if ota_val != exp:
                failures.append(f"FAIL  {label} LORA={lora}: OTA_CHIP={ota_val!r} want {exp!r}")

    print(f"{'chip':<20} {'LORA':<5} {'FWD_CHIP':<18} {'OTA_CHIP':<18} expected")
    for label, lora, fwd_val, ota_val, exp in results:
        mark = '' if exp is None else ('OK' if (fwd_val == exp and ota_val == exp) else 'FAIL')
        print(f"{label:<20} {lora:<5} {str(fwd_val):<18} {str(ota_val):<18} {str(exp):<10} {mark}")

    if failures:
        print()
        for f in failures:
            print(f)
        print(f"\n{len(failures)} assertion(s) FAILED")
        return 1
    print("\nAll assertions PASSED")
    return 0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--fw-digest', default=str(REPO_ROOT / 'src' / 'fw_digest.h'))
    ap.add_argument('--ota', default=str(REPO_ROOT / 'src' / 'ota.cpp'))
    args = ap.parse_args()
    sys.exit(run(args.fw_digest, args.ota))


if __name__ == '__main__':
    main()
