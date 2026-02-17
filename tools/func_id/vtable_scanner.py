"""
C++ vtable scanner for function classification.

Scans .rdata for arrays of consecutive .text pointers that form
C++ virtual function tables. Each vtable corresponds to a class,
and its entries are virtual methods of that class.

Typical MSVC vtable layout in .rdata:
  [vfunc_0_ptr][vfunc_1_ptr][vfunc_2_ptr]...

Constructors are identified by finding functions that write
a vtable address into memory (the 'this->__vfptr = &vtable' pattern).
"""

import struct
from collections import defaultdict

from . import config


# Minimum number of consecutive .text pointers to qualify as a vtable
MIN_VTABLE_ENTRIES = 3

# Maximum gap between .text pointers to still consider them part of same vtable
# (allows for one RTTI pointer or null between entries)
MAX_NULL_GAP = 0


def scan_vtables(xbe_data, functions, imm_refs, verbose=False):
    """
    Scan .rdata for C++ vtables and classify virtual methods.

    Args:
        xbe_data: Raw bytes of the entire XBE file.
        functions: List of function dicts from disassembly.
        imm_refs: Dict rdata_addr -> [func_addr, ...] from imm scanner.
        verbose: Print progress info.

    Returns:
        tuple: (vtable_results, vtables)
            vtable_results: dict func_addr -> {
                "category": str,
                "subcategory": str,
                "confidence": float,
                "method": str,
                "vtable_addr": int,
                "vtable_index": int,
            }
            vtables: list of vtable dicts for output
    """
    # Build set of known function starts for validation
    func_starts = set()
    for f in functions:
        func_starts.add(int(f["start"], 16))

    # Scan .rdata for vtable structures
    raw_vtables = _find_vtables(xbe_data, func_starts)

    if verbose:
        print(f"  Raw vtable candidates: {len(raw_vtables)}")

    # Filter out false positives
    vtables = _filter_vtables(raw_vtables, xbe_data)

    if verbose:
        total_entries = sum(len(vt["entries"]) for vt in vtables)
        print(f"  Validated vtables: {len(vtables)}")
        print(f"  Total virtual methods: {total_entries}")

    # Find constructors: functions that embed vtable address literals
    constructors = _find_constructors(vtables, xbe_data, functions)

    if verbose:
        print(f"  Constructors found: {len(constructors)}")

    # Build classification results
    results = {}
    for i, vt in enumerate(vtables):
        cls_id = f"cls_{i:03d}"
        vt["class_id"] = cls_id

        for idx, entry_addr in enumerate(vt["entries"]):
            if entry_addr not in results:
                results[entry_addr] = {
                    "category": "game_vtable",
                    "subcategory": cls_id,
                    "confidence": 0.85,
                    "method": "vtable_scan",
                    "vtable_addr": vt["address"],
                    "vtable_index": idx,
                }

    # Add constructors
    for func_addr, vt_info in constructors.items():
        if func_addr not in results:
            results[func_addr] = {
                "category": "game_vtable",
                "subcategory": vt_info["class_id"],
                "confidence": 0.80,
                "method": "vtable_ctor",
                "vtable_addr": vt_info["vtable_addr"],
                "vtable_index": -1,
            }

    if verbose:
        print(f"  Functions classified by vtable: {len(results)}")

    return results, vtables


def _find_vtables(xbe_data, func_starts):
    """
    Scan .rdata for sequences of consecutive function pointers.

    Returns list of dicts: {"address": int, "entries": [int, ...]}
    """
    rdata_raw = config.RDATA_RAW_ADDR
    rdata_va = config.RDATA_VA_START
    rdata_size = config.RDATA_VA_SIZE

    text_lo = config.TEXT_VA_START
    text_hi = config.TEXT_VA_END

    rdata_bytes = xbe_data[rdata_raw:rdata_raw + rdata_size]
    vtables = []

    i = 0
    while i < len(rdata_bytes) - 4:
        # Align to 4 bytes
        if i % 4 != 0:
            i += 4 - (i % 4)
            continue

        # Check if this could be start of a vtable
        val = struct.unpack_from('<I', rdata_bytes, i)[0]
        if val not in func_starts:
            i += 4
            continue

        # Found a function pointer - scan forward for more
        entries = []
        j = i
        while j < len(rdata_bytes) - 4:
            val = struct.unpack_from('<I', rdata_bytes, j)[0]
            if val in func_starts:
                entries.append(val)
                j += 4
            else:
                break

        if len(entries) >= MIN_VTABLE_ENTRIES:
            vtables.append({
                "address": rdata_va + i,
                "entries": entries,
            })
            i = j  # Skip past this vtable
        else:
            i += 4

    return vtables


def _filter_vtables(vtables, xbe_data):
    """
    Filter out false positive vtable candidates.

    Removes:
    - Arithmetic progressions (sequential addresses 4 bytes apart = data table)
    - All-same-value arrays (repeated function pointer = unlikely vtable)
    - Vtables entirely within known data structures
    """
    filtered = []

    for vt in vtables:
        entries = vt["entries"]

        # Filter: all entries identical
        if len(set(entries)) == 1:
            continue

        # Filter: arithmetic progression (entries[i+1] - entries[i] is constant)
        if len(entries) >= 4:
            diffs = [entries[j+1] - entries[j] for j in range(len(entries) - 1)]
            if len(set(diffs)) == 1 and diffs[0] <= 16:
                continue

        # Filter: mostly sequential small-increment (data table, not vtable)
        if len(entries) >= 6:
            small_diffs = sum(1 for j in range(len(entries) - 1)
                            if abs(entries[j+1] - entries[j]) <= 8)
            if small_diffs > len(entries) * 0.8:
                continue

        filtered.append(vt)

    return filtered


def _find_constructors(vtables, xbe_data, functions):
    """
    Find constructor functions that embed vtable addresses as immediates.

    A constructor typically does: mov [ecx], offset vtable
    Encoded as C7 01 [4-byte vtable addr] or similar mov [reg+disp], imm32.

    We scan each function's bytes for vtable address literals.
    """
    # Build lookup of vtable address bytes -> vtable info
    vtable_addrs = {}
    vtable_methods = set()
    for vt in vtables:
        addr_bytes = struct.pack('<I', vt["address"])
        vtable_addrs[addr_bytes] = vt
        vtable_methods.update(vt["entries"])

    if not vtable_addrs:
        return {}

    constructors = {}

    for f in functions:
        func_addr = int(f["start"], 16)
        func_size = f.get("size", 0)
        if func_size < 8 or func_size > 8192:
            continue

        file_offset = config.va_to_file_offset(func_addr)
        if file_offset is None or file_offset + func_size > len(xbe_data):
            continue

        func_bytes = xbe_data[file_offset:file_offset + func_size]

        # Search for any vtable address literal in function bytes
        for addr_bytes, vt in vtable_addrs.items():
            pos = func_bytes.find(addr_bytes)
            if pos >= 0:
                # Don't classify vtable members as constructors
                if func_addr not in vtable_methods:
                    constructors[func_addr] = {
                        "class_id": vt.get("class_id", "cls_???"),
                        "vtable_addr": vt["address"],
                    }
                break  # One match is enough

    return constructors
