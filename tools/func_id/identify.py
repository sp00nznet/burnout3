"""
Main orchestrator for function identification.

Loads all input data, runs identification phases in order,
and produces enriched output.
"""

import json
import os
import time
from collections import defaultdict

from . import config
from .imm_scanner import scan_immediate_refs
from .rw_identifier import identify_rw_functions
from .crt_identifier import identify_crt_functions
from .clustering import propagate_labels
from .output import write_results


def run(xbe_path, functions_path=None, strings_path=None, xrefs_path=None,
        output_dir=None, verbose=False):
    """
    Run the full function identification pipeline.

    Args:
        xbe_path: Path to the XBE file.
        functions_path: Path to functions.json (or default).
        strings_path: Path to strings.json (or default).
        xrefs_path: Path to xrefs.json (or default).
        output_dir: Output directory (or default).
        verbose: Print progress info.

    Returns:
        dict: Summary statistics.
    """
    functions_path = functions_path or config.DEFAULT_FUNCTIONS_JSON
    strings_path = strings_path or config.DEFAULT_STRINGS_JSON
    xrefs_path = xrefs_path or config.DEFAULT_XREFS_JSON
    output_dir = output_dir or config.DEFAULT_OUTPUT_DIR

    t_start = time.time()

    # ── Phase 0: Load inputs ──────────────────────────────────
    if verbose:
        print("Phase 0: Loading inputs...")

    xbe_data = _load_binary(xbe_path)
    functions = _load_json(functions_path)
    strings = _load_json(strings_path)
    xrefs = _load_json(xrefs_path)

    if verbose:
        print(f"  XBE: {len(xbe_data):,} bytes")
        print(f"  Functions: {len(functions):,}")
        print(f"  Strings: {len(strings):,}")
        print(f"  Xrefs: {len(xrefs):,}")

    # ── Phase 1: Immediate operand scan + xref merge ─────────
    if verbose:
        print("\nPhase 1: Scanning immediate operands + merging xrefs...")
    t1 = time.time()
    imm_refs = scan_immediate_refs(xbe_data, functions, verbose=verbose)

    # Merge data_read xrefs that target .rdata/.data into imm_refs
    xref_merge_count = _merge_xref_data_reads(xrefs, functions, imm_refs, verbose)

    if verbose:
        print(f"  Merged {xref_merge_count:,} data_read xrefs")
        print(f"  Total unique data addresses after merge: {len(imm_refs):,}")
        print(f"  Done in {time.time() - t1:.1f}s")

    # ── Phase 2: RenderWare identification ────────────────────
    if verbose:
        print("\nPhase 2: RenderWare identification...")
    t2 = time.time()
    rw_results, rw_modules = identify_rw_functions(
        strings, imm_refs, functions, verbose=verbose
    )
    if verbose:
        print(f"  Done in {time.time() - t2:.1f}s")

    # ── Phase 3: CRT identification ──────────────────────────
    if verbose:
        print("\nPhase 3: CRT identification...")
    t3 = time.time()
    crt_results = identify_crt_functions(xbe_data, functions, verbose=verbose)
    if verbose:
        print(f"  Done in {time.time() - t3:.1f}s")

    # Remove any CRT matches that overlap with RW (RW wins)
    for addr in list(crt_results.keys()):
        if addr in rw_results:
            del crt_results[addr]

    # ── Phase 4: Label propagation ───────────────────────────
    if verbose:
        print("\nPhase 4: Label propagation...")
    t4 = time.time()
    propagated = propagate_labels(
        functions, rw_results, crt_results, imm_refs, strings,
        verbose=verbose
    )
    if verbose:
        print(f"  Done in {time.time() - t4:.1f}s")

    # ── Phase 5: Write output ────────────────────────────────
    if verbose:
        print("\nPhase 5: Writing output...")

    summary = write_results(
        functions, rw_results, crt_results, propagated, rw_modules,
        output_dir, verbose=verbose
    )

    if verbose:
        print(f"\nTotal time: {time.time() - t_start:.1f}s")
        print(f"Output written to: {output_dir}/")

    return summary


def _merge_xref_data_reads(xrefs, functions, imm_refs, verbose):
    """
    Merge data_read xrefs into the imm_refs map.

    The xref database has 77K+ data_read entries (from mov [addr] style
    instructions) that the imm_scanner misses. Merging these gives a
    much more complete picture of data references.

    Returns the count of new entries added.
    """
    # Build sorted function start list for binary search
    func_starts = sorted(int(f["start"], 16) for f in functions)

    rdata_lo = config.RDATA_VA_START
    rdata_hi = config.RDATA_VA_END
    data_lo = config.DATA_VA_START
    data_hi = config.DATA_VA_END

    count = 0
    for xref in xrefs:
        if xref["type"] != "data_read":
            continue

        target = int(xref["to"], 16)
        if not ((rdata_lo <= target < rdata_hi) or (data_lo <= target < data_hi)):
            continue

        source = int(xref["from"], 16)

        # Find containing function
        lo, hi = 0, len(func_starts) - 1
        while lo <= hi:
            mid = (lo + hi) // 2
            if func_starts[mid] <= source:
                lo = mid + 1
            else:
                hi = mid - 1
        func_addr = func_starts[hi] if hi >= 0 else None

        if func_addr is not None:
            if target not in imm_refs:
                imm_refs[target] = []
                count += 1
            if func_addr not in imm_refs[target]:
                imm_refs[target].append(func_addr)

    return count


def _load_binary(path):
    """Load a binary file."""
    if not os.path.exists(path):
        raise FileNotFoundError(f"XBE file not found: {path}")
    with open(path, "rb") as f:
        return f.read()


def _load_json(path):
    """Load a JSON file."""
    if not os.path.exists(path):
        raise FileNotFoundError(f"JSON file not found: {path}")
    with open(path, "r", encoding="utf-8") as f:
        return json.load(f)
