"""
Global variable and data structure mapper.

Analyzes xref data to identify, classify, and group global variables
in the .data and .rdata sections. Cross-references with function
identification results to determine variable purpose.

Output: a database of global variables with access patterns,
inferred types, and structure groupings.
"""

import json
import os
import struct
import time
from bisect import bisect_right
from collections import Counter, defaultdict

# Section ranges from XBE analysis
TEXT_VA_START = 0x00011000
TEXT_VA_SIZE = 2863616
TEXT_VA_END = TEXT_VA_START + TEXT_VA_SIZE

RDATA_VA_START = 0x0036B7C0
RDATA_VA_SIZE = 289684
RDATA_VA_END = RDATA_VA_START + RDATA_VA_SIZE
RDATA_RAW_ADDR = 0x0035C000

DATA_VA_START = 0x003B2360
DATA_VA_SIZE = 3904988
DATA_VA_END = DATA_VA_START + DATA_VA_SIZE
DATA_RAW_ADDR = 0x003A3000

# Default paths
DEFAULT_FUNCTIONS_JSON = "tools/disasm/output/functions.json"
DEFAULT_XREFS_JSON = "tools/disasm/output/xrefs.json"
DEFAULT_IDENTIFIED_JSON = "tools/func_id/output/identified_functions.json"
DEFAULT_STRINGS_JSON = "tools/disasm/output/strings.json"
DEFAULT_OUTPUT_DIR = "tools/global_map/output"

# Structure detection: max gap between fields of the same struct
MAX_STRUCT_FIELD_GAP = 256
# Minimum fields to consider a group as a structure
MIN_STRUCT_FIELDS = 3


def run(xbe_path, functions_path=None, xrefs_path=None, identified_path=None,
        strings_path=None, output_dir=None, verbose=False):
    """Run the global variable mapping pipeline."""
    functions_path = functions_path or DEFAULT_FUNCTIONS_JSON
    xrefs_path = xrefs_path or DEFAULT_XREFS_JSON
    identified_path = identified_path or DEFAULT_IDENTIFIED_JSON
    strings_path = strings_path or DEFAULT_STRINGS_JSON
    output_dir = output_dir or DEFAULT_OUTPUT_DIR

    t_start = time.time()

    # Load inputs
    if verbose:
        print("Phase 0: Loading inputs...")

    xbe_data = _load_binary(xbe_path)
    functions = _load_json(functions_path)
    xrefs = _load_json(xrefs_path)
    identified = _load_json(identified_path)
    strings = _load_json(strings_path)

    if verbose:
        print(f"  XBE: {len(xbe_data):,} bytes")
        print(f"  Functions: {len(functions):,}")
        print(f"  Xrefs: {len(xrefs):,}")
        print(f"  Identified functions: {len(identified):,}")

    # Phase 1: Build function lookup and map xrefs to functions
    if verbose:
        print("\nPhase 1: Building accessor maps...")
    t1 = time.time()

    func_starts, func_categories = _build_function_lookups(functions, identified)
    globals_db = _build_globals_from_xrefs(xrefs, func_starts, func_categories)

    if verbose:
        print(f"  Global variables found: {len(globals_db):,}")
        data_globals = sum(1 for g in globals_db.values() if g["section"] == ".data")
        rdata_globals = sum(1 for g in globals_db.values() if g["section"] == ".rdata")
        print(f"    .data:  {data_globals:,}")
        print(f"    .rdata: {rdata_globals:,}")
        print(f"  Done in {time.time() - t1:.1f}s")

    # Phase 2: Infer variable sizes and initial values
    if verbose:
        print("\nPhase 2: Inferring variable properties...")
    t2 = time.time()

    _infer_sizes(globals_db)
    _read_initial_values(globals_db, xbe_data)

    if verbose:
        size_counts = Counter(g["inferred_size"] for g in globals_db.values())
        print(f"  Size distribution: {dict(size_counts.most_common(10))}")
        print(f"  Done in {time.time() - t2:.1f}s")

    # Phase 3: Cross-reference with strings
    if verbose:
        print("\nPhase 3: String cross-reference...")
    t3 = time.time()

    _cross_reference_strings(globals_db, strings)

    str_globals = sum(1 for g in globals_db.values() if g.get("string_ref"))
    if verbose:
        print(f"  Globals near strings: {str_globals:,}")
        print(f"  Done in {time.time() - t3:.1f}s")

    # Phase 4: Detect structures (groups of globals accessed together)
    if verbose:
        print("\nPhase 4: Structure detection...")
    t4 = time.time()

    structures = _detect_structures(globals_db)

    if verbose:
        total_fields = sum(len(s["fields"]) for s in structures)
        print(f"  Structures found: {len(structures):,}")
        print(f"  Total fields in structures: {total_fields:,}")
        print(f"  Done in {time.time() - t4:.1f}s")

    # Phase 5: Classify globals by accessor categories
    if verbose:
        print("\nPhase 5: Classification...")
    t5 = time.time()

    _classify_globals(globals_db)

    cat_counts = Counter(g["classification"] for g in globals_db.values())
    if verbose:
        print(f"  Classification distribution:")
        for cat, count in cat_counts.most_common(15):
            print(f"    {cat:25s}: {count:6,}")
        print(f"  Done in {time.time() - t5:.1f}s")

    # Phase 6: Write output
    if verbose:
        print("\nPhase 6: Writing output...")

    os.makedirs(output_dir, exist_ok=True)
    summary = _write_output(globals_db, structures, output_dir, verbose)

    if verbose:
        print(f"\nTotal time: {time.time() - t_start:.1f}s")
        print(f"Output written to: {output_dir}/")

    return summary


def _build_function_lookups(functions, identified):
    """Build sorted function starts and category lookup."""
    func_starts = sorted(int(f["start"], 16) for f in functions)

    # Map function start address -> category
    func_categories = {}
    for f in identified:
        addr = int(f["start"], 16)
        func_categories[addr] = f.get("category", "unknown")

    return func_starts, func_categories


def _find_containing_function(addr, func_starts):
    """Binary search to find the function containing an address."""
    idx = bisect_right(func_starts, addr) - 1
    if idx >= 0:
        return func_starts[idx]
    return None


def _build_globals_from_xrefs(xrefs, func_starts, func_categories):
    """
    Build global variable database from xref data.

    For each unique .data/.rdata address referenced, record:
    - Read count, write count
    - Which functions access it and their categories
    - Section membership
    """
    globals_db = {}

    for xref in xrefs:
        xref_type = xref["type"]
        if xref_type not in ("data_read", "data_write"):
            continue

        target = int(xref["to"], 16)

        # Only .data and .rdata globals
        if DATA_VA_START <= target < DATA_VA_END:
            section = ".data"
        elif RDATA_VA_START <= target < RDATA_VA_END:
            section = ".rdata"
        else:
            continue

        source = int(xref["from"], 16)
        func_addr = _find_containing_function(source, func_starts)
        func_cat = func_categories.get(func_addr, "unknown") if func_addr else "unknown"

        if target not in globals_db:
            globals_db[target] = {
                "address": target,
                "section": section,
                "read_count": 0,
                "write_count": 0,
                "accessor_functions": set(),
                "accessor_categories": Counter(),
                "inferred_size": 4,
                "initial_value": None,
                "classification": "unknown",
            }

        entry = globals_db[target]
        if xref_type == "data_read":
            entry["read_count"] += 1
        else:
            entry["write_count"] += 1

        if func_addr:
            entry["accessor_functions"].add(func_addr)
            entry["accessor_categories"][func_cat] += 1

    return globals_db


def _infer_sizes(globals_db):
    """
    Infer variable sizes from address gaps between consecutive globals.

    Heuristic: if the next global is N bytes away and N is a common
    data size (1, 2, 4, 8, 16, etc.), infer that as the size.
    """
    sorted_addrs = sorted(globals_db.keys())

    for i in range(len(sorted_addrs)):
        addr = sorted_addrs[i]

        if i + 1 < len(sorted_addrs):
            gap = sorted_addrs[i + 1] - addr
        else:
            gap = 256  # last variable, default

        # Infer size from gap (round down to power of 2 or common size)
        if gap <= 1:
            size = 1
        elif gap <= 2:
            size = 2
        elif gap <= 4:
            size = 4
        elif gap <= 8:
            size = gap if gap in (5, 6, 7, 8) else 4
        elif gap <= 16:
            size = 8  # likely a double or 64-bit value
        else:
            size = 4  # default for large gaps (standalone variable)

        # Alignment check: if address isn't aligned to inferred size, reduce
        if size > 1 and addr % size != 0:
            if addr % 4 == 0:
                size = 4
            elif addr % 2 == 0:
                size = 2
            else:
                size = 1

        globals_db[addr]["inferred_size"] = size


def _read_initial_values(globals_db, xbe_data):
    """Read initial values from XBE for .data globals (not BSS)."""
    for addr, entry in globals_db.items():
        raw_offset = _va_to_raw(addr, entry["section"])
        if raw_offset is None:
            continue

        size = entry["inferred_size"]
        if raw_offset + size > len(xbe_data):
            continue

        data = xbe_data[raw_offset:raw_offset + size]

        if size == 1:
            entry["initial_value"] = struct.unpack_from('<B', data)[0]
        elif size == 2:
            entry["initial_value"] = struct.unpack_from('<H', data)[0]
        elif size == 4:
            entry["initial_value"] = struct.unpack_from('<I', data)[0]
        elif size == 8:
            entry["initial_value"] = struct.unpack_from('<Q', data)[0]


def _va_to_raw(va, section):
    """Convert VA to raw file offset."""
    if section == ".data":
        if DATA_VA_START <= va < DATA_VA_END:
            offset = va - DATA_VA_START + DATA_RAW_ADDR
            # BSS region: raw data may not exist in file
            return offset if offset < DATA_RAW_ADDR + DATA_VA_SIZE else None
        return None
    elif section == ".rdata":
        if RDATA_VA_START <= va < RDATA_VA_END:
            return va - RDATA_VA_START + RDATA_RAW_ADDR
        return None
    return None


def _cross_reference_strings(globals_db, strings):
    """Find globals that are near known strings."""
    string_addrs = {}
    for s in strings:
        addr = int(s["address"], 16)
        string_addrs[addr] = s["string"]

    for addr, entry in globals_db.items():
        # Check if this global IS a string reference or is near one
        if addr in string_addrs:
            entry["string_ref"] = string_addrs[addr]
        else:
            # Check within ±64 bytes for nearby strings
            for offset in range(-64, 65, 4):
                nearby = addr + offset
                if nearby in string_addrs:
                    entry["nearby_string"] = {
                        "address": f"0x{nearby:08X}",
                        "offset": offset,
                        "text": string_addrs[nearby],
                    }
                    break


def _detect_structures(globals_db):
    """
    Detect structure-like groupings of globals.

    Groups globals accessed by the same set of functions into
    candidate structures when they are contiguous in memory.
    """
    # Group globals by their primary accessor function
    by_accessor = defaultdict(list)
    for addr, entry in globals_db.items():
        if entry["section"] != ".data":
            continue
        for func_addr in entry["accessor_functions"]:
            by_accessor[func_addr].append(addr)

    # Find groups of globals accessed by the same function
    # that are close together in memory
    structures = []
    seen = set()

    for func_addr, addrs in sorted(by_accessor.items(), key=lambda x: -len(x[1])):
        if len(addrs) < MIN_STRUCT_FIELDS:
            continue

        sorted_addrs = sorted(addrs)

        # Split into contiguous groups
        groups = []
        current_group = [sorted_addrs[0]]

        for i in range(1, len(sorted_addrs)):
            gap = sorted_addrs[i] - sorted_addrs[i - 1]
            if gap <= MAX_STRUCT_FIELD_GAP:
                current_group.append(sorted_addrs[i])
            else:
                if len(current_group) >= MIN_STRUCT_FIELDS:
                    groups.append(current_group)
                current_group = [sorted_addrs[i]]

        if len(current_group) >= MIN_STRUCT_FIELDS:
            groups.append(current_group)

        for group in groups:
            base = group[0]
            # Skip if we've already identified a structure starting here
            key = (base, len(group))
            if key in seen:
                continue
            seen.add(key)

            fields = []
            for field_addr in group:
                g = globals_db[field_addr]
                fields.append({
                    "offset": field_addr - base,
                    "address": f"0x{field_addr:08X}",
                    "size": g["inferred_size"],
                    "read_count": g["read_count"],
                })

            total_size = group[-1] - base + globals_db[group[-1]]["inferred_size"]
            structures.append({
                "base_address": f"0x{base:08X}",
                "total_size": total_size,
                "num_fields": len(fields),
                "fields": fields,
                "primary_accessor": f"0x{func_addr:08X}",
            })

    # Sort by size descending
    structures.sort(key=lambda s: -s["total_size"])
    return structures


def _classify_globals(globals_db):
    """
    Classify each global variable based on its accessor categories
    and access patterns.
    """
    for addr, entry in globals_db.items():
        cats = entry["accessor_categories"]
        if not cats:
            entry["classification"] = "unreferenced"
            continue

        # Determine primary accessor category
        primary_cat = cats.most_common(1)[0][0]

        # Map to global classification
        if primary_cat.startswith("rw_"):
            entry["classification"] = "rw_internal"
        elif primary_cat == "data_init":
            entry["classification"] = "game_parameter"
        elif primary_cat == "game_engine":
            entry["classification"] = "engine_state"
        elif primary_cat == "game_vtable":
            entry["classification"] = "object_data"
        elif primary_cat == "game_vehicle":
            entry["classification"] = "vehicle_data"
        elif primary_cat == "game_audio":
            entry["classification"] = "audio_data"
        elif primary_cat == "game_render":
            entry["classification"] = "render_data"
        elif primary_cat == "game_physics":
            entry["classification"] = "physics_data"
        elif primary_cat == "game_ui":
            entry["classification"] = "ui_data"
        elif primary_cat == "game_network":
            entry["classification"] = "network_data"
        elif primary_cat == "game_camera":
            entry["classification"] = "camera_data"
        elif primary_cat == "game_io":
            entry["classification"] = "io_data"
        elif primary_cat == "game_input":
            entry["classification"] = "input_data"
        elif primary_cat == "game_video":
            entry["classification"] = "video_data"
        elif primary_cat == "crt":
            entry["classification"] = "crt_internal"
        elif primary_cat == "unknown":
            # Use access pattern to guess
            if entry["read_count"] > 50:
                entry["classification"] = "game_constant"
            else:
                entry["classification"] = "game_data"
        else:
            entry["classification"] = "game_data"

        # Special: .rdata globals are always constants
        if entry["section"] == ".rdata":
            entry["classification"] = entry["classification"].replace("_data", "_const")
            if entry["classification"] == "game_parameter":
                entry["classification"] = "game_const"

        # High read count with multiple accessors = important global
        if entry["read_count"] >= 100 and len(entry["accessor_functions"]) >= 10:
            entry["importance"] = "high"
        elif entry["read_count"] >= 20 or len(entry["accessor_functions"]) >= 5:
            entry["importance"] = "medium"
        else:
            entry["importance"] = "low"


def _write_output(globals_db, structures, output_dir, verbose):
    """Write all output files."""
    # Serialize globals database
    globals_list = []
    for addr in sorted(globals_db.keys()):
        entry = globals_db[addr]
        record = {
            "address": f"0x{addr:08X}",
            "section": entry["section"],
            "classification": entry["classification"],
            "importance": entry.get("importance", "low"),
            "inferred_size": entry["inferred_size"],
            "read_count": entry["read_count"],
            "write_count": entry["write_count"],
            "num_accessors": len(entry["accessor_functions"]),
            "initial_value": entry["initial_value"],
        }
        if entry.get("string_ref"):
            record["string_ref"] = entry["string_ref"]
        if entry.get("nearby_string"):
            record["nearby_string"] = entry["nearby_string"]
        globals_list.append(record)

    _write_json(os.path.join(output_dir, "globals.json"), globals_list)

    # Write structures
    _write_json(os.path.join(output_dir, "structures.json"), structures)

    # Build summary
    total = len(globals_list)
    by_section = Counter(g["section"] for g in globals_list)
    by_class = Counter(g["classification"] for g in globals_list)
    by_importance = Counter(g["importance"] for g in globals_list)

    # BSS detection: .data globals with initial value of 0
    bss_count = sum(1 for g in globals_list
                    if g["section"] == ".data" and g["initial_value"] == 0)

    summary = {
        "total_globals": total,
        "by_section": dict(by_section),
        "by_classification": {k: v for k, v in sorted(by_class.items(), key=lambda x: -x[1])},
        "by_importance": dict(by_importance),
        "structures_found": len(structures),
        "total_struct_fields": sum(len(s["fields"]) for s in structures),
        "bss_globals": bss_count,
        "high_importance": [
            g for g in globals_list if g["importance"] == "high"
        ][:50],  # Top 50 important globals
    }

    _write_json(os.path.join(output_dir, "summary.json"), summary)

    if verbose:
        _print_summary(summary)

    return summary


def _print_summary(summary):
    """Print summary to stdout."""
    print("\n" + "=" * 60)
    print("GLOBAL VARIABLE MAPPING SUMMARY")
    print("=" * 60)
    print(f"  Total globals:      {summary['total_globals']:,}")
    for sec, count in summary["by_section"].items():
        print(f"    {sec}:  {count:,}")
    print(f"  BSS globals (init=0): {summary['bss_globals']:,}")
    print(f"  Structures found:   {summary['structures_found']:,}")
    print(f"  Struct fields:      {summary['total_struct_fields']:,}")

    print(f"\n  By importance:")
    for imp, count in summary["by_importance"].items():
        print(f"    {imp:10s}: {count:6,}")

    print(f"\n  By classification:")
    for cls, count in summary["by_classification"].items():
        print(f"    {cls:25s}: {count:6,}")

    hi = summary["high_importance"]
    if hi:
        print(f"\n  Top high-importance globals:")
        for g in hi[:20]:
            extra = ""
            if g.get("string_ref"):
                extra = f'  str: "{g["string_ref"][:30]}"'
            print(f"    {g['address']}  reads={g['read_count']:5d}  "
                  f"accessors={g['num_accessors']:3d}  {g['classification']}{extra}")

    print("=" * 60)


def _write_json(path, data):
    """Write data as formatted JSON."""
    with open(path, "w", encoding="utf-8") as f:
        json.dump(data, f, indent=2, ensure_ascii=False)


def _load_binary(path):
    """Load a binary file."""
    if not os.path.exists(path):
        raise FileNotFoundError(f"File not found: {path}")
    with open(path, "rb") as f:
        return f.read()


def _load_json(path):
    """Load a JSON file."""
    if not os.path.exists(path):
        raise FileNotFoundError(f"JSON file not found: {path}")
    with open(path, "r", encoding="utf-8") as f:
        return json.load(f)
