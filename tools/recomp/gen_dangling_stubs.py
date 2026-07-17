#!/usr/bin/env python3
"""
Emit empty stubs for call targets the recompiler never defined.

The generated code calls addresses that the disassembler never identified as
function starts (branch/tail-call targets landing mid-function). Nothing
defines them, so the link fails with `undefined reference to sub_XXXXXXXX`.

This is a backstop, not a fix. The real fix is to feed these addresses back to
the disassembler via --seed-functions so they become real, lifted functions;
each address stubbed here is a function whose body silently does nothing.
The count is printed so the gap stays visible instead of quietly rotting.

Usage:
    py -3 tools/recomp/gen_dangling_stubs.py [--gen-dir DIR] [--manual FILE]
"""
import argparse
import glob
import os
import re

CALL_RE = re.compile(r"\b([A-Za-z_]\w*)\(\);")
DEF_RE = re.compile(r"^void (\w+)\(void\)\s*$", re.M)
DECL_RE = re.compile(r"^void (\w+)\(void\);", re.M)


def read(p):
    return open(p, encoding="utf-8", errors="replace").read()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--gen-dir", default="src/game/recomp/gen")
    ap.add_argument("--manual", default="src/game/recomp/recomp_manual.c")
    ap.add_argument("--out", default=None)
    a = ap.parse_args()

    out = a.out or os.path.join(a.gen_dir, "recomp_dangling_stubs.c")

    defined, called = set(), set()
    for p in glob.glob(os.path.join(a.gen_dir, "*.c")):
        if os.path.basename(p) == os.path.basename(out):
            continue
        t = read(p)
        defined |= set(DEF_RE.findall(t))
        called |= set(CALL_RE.findall(t))

    hdr = os.path.join(a.gen_dir, "recomp_funcs.h")
    if os.path.exists(hdr):
        defined |= set(DECL_RE.findall(read(hdr)))
    if os.path.exists(a.manual):
        defined |= set(DEF_RE.findall(read(a.manual)))

    # Only sub_XXXXXXXX names are safe to stub: a named XDK function that is
    # missing means something upstream is wrong, and a silent empty stub would
    # hide it.
    missing = sorted(c for c in called - defined
                     if re.fullmatch(r"sub_[0-9A-Fa-f]{8}", c))
    suspicious = sorted(c for c in called - defined
                        if not re.fullmatch(r"sub_[0-9A-Fa-f]{8}", c))

    with open(out, "w", encoding="utf-8") as f:
        f.write("/**\n * recomp_dangling_stubs.c -- GENERATED, do not edit.\n"
                " *\n"
                " * Empty stubs for call targets the disassembler never found.\n"
                " * Each one is a function that silently does nothing; feed these\n"
                " * addresses back through --seed-functions to make them real.\n"
                " *\n"
                f" * Regenerate: py -3 tools/recomp/gen_dangling_stubs.py\n"
                " */\n\n"
                '#define RECOMP_GENERATED_CODE\n'
                '#include "recomp_funcs.h"\n\n')
        for n in missing:
            f.write(f"void {n}(void) {{ }}\n")

    print(f"defined: {len(defined)}   called: {len(called)}")
    print(f"wrote {len(missing)} dangling stubs -> {out}")
    if suspicious:
        print(f"WARNING: {len(suspicious)} non-sub_ calls are undefined; these "
              f"are NOT stubbed because a missing named function means "
              f"something upstream is broken:")
        for s in suspicious[:10]:
            print(f"   {s}")


if __name__ == "__main__":
    main()
