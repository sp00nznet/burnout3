"""
RenderWare module identification.

Uses the 67 RW source file ID strings embedded in .rdata to classify
functions as belonging to specific RenderWare modules.

Strategy:
1. Parse RW ID strings and their .rdata addresses
2. Define tight .rdata zones around RW strings (capped) for initial seeds
3. Bootstrap: use seed functions' .rdata references to define a broader
   RW data region, then find all functions referencing that region
4. Use .text code region density to fill remaining gaps
"""

import re
from collections import defaultdict

from . import config

# Maximum zone size for initial tight zones around RW ID strings
MAX_ZONE_SIZE = 0x400

# Padding around the inferred RW data region boundaries
RW_DATA_REGION_PADDING = 0x200


def identify_rw_functions(strings, imm_refs, functions, verbose=False):
    """
    Identify RenderWare functions using bootstrapped region analysis.

    Args:
        strings: List of string dicts from strings.json.
        imm_refs: Dict of rdata_addr -> [func_start_addr, ...] from imm_scanner.
        functions: List of function dicts.
        verbose: Print progress info.

    Returns:
        tuple: (rw_results, rw_modules)
    """
    # Step 1: Parse RW ID strings
    rw_strings = _parse_rw_strings(strings)
    if verbose:
        print(f"  Found {len(rw_strings)} RenderWare ID strings")

    # Step 2: Classify each RW string into a subcategory
    for rw in rw_strings:
        rw["category"] = _classify_rw_path(rw["path"])

    # Step 3: Initialize module tracking
    rw_results = {}
    rw_modules = {}

    rw_string_addrs = {rw["address"]: rw for rw in rw_strings}
    for rw in rw_strings:
        rw_modules[rw["filename"]] = {
            "address": rw["address"],
            "category": rw["category"],
            "path": rw["path"],
            "functions": [],
        }

    # Step 4: Find seed functions via tight .rdata zones
    zones = _build_rdata_zones(rw_strings)
    if verbose:
        print(f"  Built {len(zones)} tight .rdata zones (max 0x{MAX_ZONE_SIZE:X})")

    # Direct string references
    for rw in rw_strings:
        addr = rw["address"]
        if addr in imm_refs:
            for func_addr in imm_refs[addr]:
                _add_rw_result(rw_results, rw_modules, func_addr,
                               rw["category"], rw["filename"], rw["path"],
                               config.CONFIDENCE_RW_STRING_REF, "rw_string_ref")

    # Zone-based seeds
    for rdata_addr, func_addrs in imm_refs.items():
        if rdata_addr in rw_string_addrs:
            continue
        zone = _find_zone(rdata_addr, zones)
        if zone is not None:
            for func_addr in func_addrs:
                if func_addr not in rw_results:
                    _add_rw_result(rw_results, rw_modules, func_addr,
                                   zone["category"], zone["filename"], zone["path"],
                                   config.CONFIDENCE_RW_ZONE, "rw_zone")

    seed_count = len(rw_results)
    if verbose:
        print(f"  Seed functions: {seed_count}")

    # Step 5: Define RW data region from the RW string cluster range
    # The RW ID strings form a known cluster in .rdata. Data between
    # them and slightly beyond is RW-related. We use this fixed range
    # rather than bootstrapping from referenced addresses (which can
    # expand to cover all of .rdata).
    func_to_rdata = defaultdict(set)
    for rdata_addr, func_addrs in imm_refs.items():
        for fa in func_addrs:
            func_to_rdata[fa].add(rdata_addr)

    rw_string_addrs_list = sorted(rw_string_addrs.keys())
    if rw_string_addrs_list:
        # The RW data region spans from before the first RW string to
        # after the last, with generous padding for nearby data tables
        rw_data_lo = rw_string_addrs_list[0] - 0x1000
        rw_data_hi = rw_string_addrs_list[-1] + 0x1000
        rw_data_lo = max(rw_data_lo, config.RDATA_VA_START)
        rw_data_hi = min(rw_data_hi, config.RDATA_VA_END)
    else:
        rw_data_lo = rw_data_hi = 0

    if verbose:
        print(f"  RW data region: 0x{rw_data_lo:08X} - 0x{rw_data_hi:08X} "
              f"({(rw_data_hi - rw_data_lo) // 1024}KB)")

    # Step 6: Find functions referencing the RW data region
    # Require at least 2 references to .rdata within the RW region
    # (single refs could be coincidental)
    region_count = 0
    for rdata_addr, func_addrs in imm_refs.items():
        if not (rw_data_lo <= rdata_addr < rw_data_hi):
            continue
        if rdata_addr in rw_string_addrs:
            continue
        for func_addr in func_addrs:
            if func_addr in rw_results:
                continue
            # Check if this function has multiple refs into the RW data region
            all_refs = func_to_rdata.get(func_addr, set())
            rw_region_refs = sum(1 for r in all_refs if rw_data_lo <= r < rw_data_hi)
            if rw_region_refs >= 2:
                # Assign to nearest RW module by .rdata zone
                best_zone = _find_nearest_zone(rdata_addr, zones)
                cat = best_zone["category"] if best_zone else "rw_core"
                mod = best_zone["filename"] if best_zone else "unknown"
                path = best_zone["path"] if best_zone else ""
                _add_rw_result(rw_results, rw_modules, func_addr,
                               cat, mod, path, 0.80, "rw_data_region")
                region_count += 1

    if verbose:
        print(f"  RW data region identifications: {region_count} functions")

    # Step 7: Expand into single-ref functions that are in the RW code range
    rw_func_addrs = sorted(rw_results.keys())
    if len(rw_func_addrs) >= 10:
        rw_code_lo = min(rw_func_addrs)
        rw_code_hi = max(rw_func_addrs)
        if verbose:
            print(f"  RW code region: 0x{rw_code_lo:08X} - 0x{rw_code_hi:08X}")

        # Functions in the RW code range that reference the RW data region
        # even with just 1 reference
        code_region_count = 0
        for rdata_addr, func_addrs in imm_refs.items():
            if not (rw_data_lo <= rdata_addr < rw_data_hi):
                continue
            for func_addr in func_addrs:
                if func_addr in rw_results:
                    continue
                if rw_code_lo <= func_addr <= rw_code_hi:
                    best_zone = _find_nearest_zone(rdata_addr, zones)
                    cat = best_zone["category"] if best_zone else "rw_core"
                    mod = best_zone["filename"] if best_zone else "unknown"
                    path = best_zone["path"] if best_zone else ""
                    _add_rw_result(rw_results, rw_modules, func_addr,
                                   cat, mod, path, 0.75, "rw_code_region")
                    code_region_count += 1

        if verbose:
            print(f"  RW code+data region identifications: {code_region_count} functions")

    if verbose:
        print(f"  Total RW-identified functions: {len(rw_results)}")

    # Deduplicate function lists in modules
    for mod in rw_modules.values():
        mod["functions"] = sorted(set(mod["functions"]))

    return rw_results, rw_modules


def _add_rw_result(rw_results, rw_modules, func_addr, category, module,
                   source_file, confidence, method):
    """Add a function to the RW results."""
    rw_results[func_addr] = {
        "category": category,
        "module": module,
        "source_file": source_file,
        "confidence": confidence,
        "method": method,
    }
    if module in rw_modules:
        rw_modules[module]["functions"].append(func_addr)


def _parse_rw_strings(strings):
    """Extract RenderWare ID strings and parse their paths."""
    rw_strings = []
    rw_pattern = re.compile(
        r'@@?\(?#\)?\$Id:\s*//RenderWare/RW36Active/rwsdk/(.+?)#\d+\s*\$'
    )

    for s in strings:
        text = s["string"]
        idx = text.find("@@")
        if idx < 0:
            continue
        clean = text[idx:]

        m = rw_pattern.search(clean)
        if m:
            path = m.group(1).strip()
            filename = path.rsplit("/", 1)[-1] if "/" in path else path
            rw_strings.append({
                "address": int(s["address"], 16),
                "path": path,
                "filename": filename,
                "category": None,
            })

    rw_strings.sort(key=lambda x: x["address"])
    return rw_strings


def _classify_rw_path(path):
    """Classify an RW source path into a subcategory."""
    for prefix, category in config.RW_CATEGORIES.items():
        if path.startswith(prefix):
            return category
    return "rw_unknown"


def _build_rdata_zones(rw_strings):
    """
    Build .rdata zones around RW ID strings with capped size.
    """
    zones = []
    for i, rw in enumerate(rw_strings):
        zone_start = rw["address"]
        if i + 1 < len(rw_strings):
            natural_end = rw_strings[i + 1]["address"]
        else:
            natural_end = config.RDATA_VA_END

        zone_end = min(natural_end, zone_start + MAX_ZONE_SIZE)

        zones.append({
            "start": zone_start,
            "end": zone_end,
            "filename": rw["filename"],
            "path": rw["path"],
            "category": rw["category"],
        })

    return zones


def _find_zone(rdata_addr, zones):
    """Binary search for the zone containing rdata_addr."""
    lo, hi = 0, len(zones) - 1
    while lo <= hi:
        mid = (lo + hi) // 2
        if zones[mid]["start"] <= rdata_addr < zones[mid]["end"]:
            return zones[mid]
        elif rdata_addr < zones[mid]["start"]:
            hi = mid - 1
        else:
            lo = mid + 1
    return None


def _find_nearest_zone(rdata_addr, zones):
    """Find the nearest zone to a given .rdata address."""
    best = None
    best_dist = float("inf")
    for z in zones:
        mid = (z["start"] + z["end"]) // 2
        dist = abs(rdata_addr - mid)
        if dist < best_dist:
            best_dist = dist
            best = z
    return best
