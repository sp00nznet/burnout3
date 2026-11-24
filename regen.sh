#!/usr/bin/env bash
# Regenerate the recompiled C from default.xbe with the xboxrecomp toolkit
# (../xboxrecomp), in the one order that works. Same shape as hl2/regen.sh.
#
#   xbe_parser    section layout -> build/xr/analysis.json
#   disasm        ALL code sections, never --text-only: without the XDK
#                 sections (D3D, DSOUND, XNET, XPP...) there are no function
#                 boundaries there and each function is glued to the next.
#   func_id       library-function identification (CRT, XDK).
#   abi_analysis  calling convention / params / return type.
#   recomp        emits the C into src/game/recomp/gen (gitignored).
#
# Nothing here is committed output: build/ and gen/ are regenerable from the
# user's own default.xbe.
#
# Usage: ./regen.sh [--disasm]

set -uo pipefail

BO3="$(cd "$(dirname "$0")" && pwd)"
RECOMP="${XBOXRECOMP_DIR:-$BO3/../xboxrecomp}"   # as CMake's XBOXRECOMP_DIR
XBE="$BO3/Burnout 3 Takedown/default.xbe"
OUT="$BO3/build/xr"

if [[ ! -f "$XBE" ]]; then
    echo "missing $XBE -- extract it from your own disc image"
    exit 1
fi
mkdir -p "$OUT"
# recomp runs from the toolkit dir, so a relative trace list must be absolutised.
if [[ -n "${TRACE_FUNCS:-}" && "$TRACE_FUNCS" != /* ]]; then
    TRACE_FUNCS="$BO3/$TRACE_FUNCS"
fi

SEEDS=()
[[ -f "$BO3/config/seed_functions.json" ]] && \
    SEEDS=(--seed-functions "$BO3/config/seed_functions.json")

if [[ "${1:-}" == "--disasm" || ! -f "$OUT/disasm/functions.json" ]]; then
    echo "==> xbe_parser"
    (cd "$RECOMP" && py -3 -m tools.xbe_parser "$XBE" \
        --json "$OUT/analysis.json" --quiet) || exit 1

    echo "==> disasm"
    (cd "$RECOMP" && py -3 -m tools.disasm "$XBE" \
        --analysis-json "$OUT/analysis.json" "${SEEDS[@]}" \
        -o "$OUT/disasm" -v) > "$OUT/disasm.log" 2>&1 || { tail -20 "$OUT/disasm.log"; exit 1; }
    tr '\r' '\n' < "$OUT/disasm.log" | grep -E "Total functions|Reachable|Seeded" || true
fi

echo "==> func_id"
(cd "$RECOMP" && py -3 -m tools.func_id "$XBE" \
    --functions "$OUT/disasm/functions.json" \
    --strings   "$OUT/disasm/strings.json" \
    --xrefs     "$OUT/disasm/xrefs.json" \
    -o "$OUT/func_id" | tail -1) || exit 1

echo "==> abi_analysis"
(cd "$RECOMP" && py -3 -m tools.abi_analysis "$XBE" \
    --functions  "$OUT/disasm/functions.json" \
    --identified "$OUT/func_id/identified_functions.json" \
    --output-dir "$OUT/abi" | tail -1) || exit 1

echo "==> recomp"
(cd "$RECOMP" && py -3 -m tools.recomp "$XBE" --all --split 1000 \
    --disasm-dir  "$OUT/disasm" \
    --func-id-dir "$OUT/func_id" \
    --abi-dir     "$OUT/abi" \
    --exclude-manual "$BO3/src/game/recomp/recomp_manual.c" \
    ${TRACE_FUNCS:+--trace-functions "$TRACE_FUNCS"} \
    --gen-dir     "$BO3/src/game/recomp/gen" \
    -o "$OUT/recomp"
) > "$OUT/recomp.log" 2>&1
st=$?
grep -aE "unresolved|functions \(|Complete" "$OUT/recomp.log" || true
if [ $st -ne 0 ]; then
    echo "!! recomp failed (status $st) -- gen/ is now STALE" >&2
    tail -25 "$OUT/recomp.log" >&2
    exit $st
fi
echo "==> done. Now: cmake --build build-xr --config Release"
