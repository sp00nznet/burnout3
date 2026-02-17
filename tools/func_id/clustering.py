"""
Label propagation for function classification.

Uses the call graph, address proximity, and string references to
propagate RW/CRT/game labels to unclassified functions.

Strategies:
- Forward:   if a majority of callers of F are RW → F is likely RW
- Backward:  if F calls mostly RW functions → F is likely RW
- Proximity: unlabeled functions between same-category functions inherit label
"""

from collections import defaultdict

from . import config


def propagate_labels(functions, rw_results, crt_results, imm_refs, strings,
                     verbose=False):
    """
    Propagate category labels through the call graph and by proximity.

    Args:
        functions: List of function dicts (with calls_to, called_by).
        rw_results: Dict func_addr -> RW identification info.
        crt_results: Dict func_addr -> CRT identification info.
        imm_refs: Dict rdata_addr -> [func_addr, ...].
        strings: List of string dicts from strings.json.
        verbose: Print progress info.

    Returns:
        dict: func_addr (int) -> {
            "category": str,
            "subcategory": str or None,
            "confidence": float,
            "method": str
        }
        This includes new classifications found by propagation only.
    """
    # Build lookup structures
    func_by_addr = {}
    for f in functions:
        addr = int(f["start"], 16)
        func_by_addr[addr] = f

    # Build call graph
    callees = defaultdict(set)
    callers = defaultdict(set)
    for f in functions:
        addr = int(f["start"], 16)
        for target in f.get("calls_to", []):
            t = int(target, 16)
            callees[addr].add(t)
            callers[t].add(addr)

    # Current labels: merge RW and CRT results
    labels = {}
    for addr, info in rw_results.items():
        labels[addr] = info["category"]
    for addr, info in crt_results.items():
        labels[addr] = "crt"

    # Build string ref map for game sub-classification
    string_refs = _build_string_ref_map(imm_refs, strings)

    propagated = {}
    sorted_addrs = sorted(func_by_addr.keys())

    # Iterative propagation with majority voting
    for iteration in range(config.MAX_CLUSTER_ITERATIONS):
        new_labels = 0

        # Forward: if >= 2/3 of callers have the same RW label → propagate
        for addr in sorted_addrs:
            if addr in labels:
                continue
            caller_set = callers.get(addr, set())
            if len(caller_set) < 2:
                continue

            rw_count = 0
            rw_subcounts = defaultdict(int)
            for c in caller_set:
                if c in labels and labels[c].startswith("rw_"):
                    rw_count += 1
                    rw_subcounts[labels[c]] += 1

            # Require >= 2/3 of callers to be RW, minimum 2 RW callers
            if rw_count >= 2 and rw_count / len(caller_set) >= 0.67:
                best = max(rw_subcounts, key=rw_subcounts.get)
                labels[addr] = best
                propagated[addr] = {
                    "category": best,
                    "subcategory": None,
                    "confidence": config.CONFIDENCE_CLUSTER_CALL,
                    "method": "cluster_forward",
                }
                new_labels += 1

        # Backward: if >= 2/3 of callees are RW → propagate
        for addr in sorted_addrs:
            if addr in labels:
                continue
            callee_set = callees.get(addr, set())
            if len(callee_set) < 2:
                continue

            rw_count = 0
            rw_subcounts = defaultdict(int)
            for c in callee_set:
                if c in labels and labels[c].startswith("rw_"):
                    rw_count += 1
                    rw_subcounts[labels[c]] += 1

            if rw_count >= 2 and rw_count / len(callee_set) >= 0.67:
                best = max(rw_subcounts, key=rw_subcounts.get)
                labels[addr] = best
                propagated[addr] = {
                    "category": best,
                    "subcategory": None,
                    "confidence": config.CONFIDENCE_CLUSTER_CALL,
                    "method": "cluster_backward",
                }
                new_labels += 1

        if verbose:
            print(f"  Iteration {iteration + 1}: {new_labels} new labels")

        if new_labels == 0:
            break

    # RW region call-graph propagation: within the RW code region,
    # propagate RW labels more aggressively (any connection suffices).
    # The linker places RW code together, so functions in this region
    # connected to known RW functions are very likely RW.
    rw_region_count = _rw_region_propagation(
        sorted_addrs, labels, propagated, callees, callers
    )
    if verbose:
        print(f"  RW region propagation: {rw_region_count} new labels")

    # Iterative proximity propagation - each pass can fill one more layer
    total_prox = 0
    for prox_pass in range(20):
        prox_count = _proximity_propagation(sorted_addrs, labels, propagated)
        total_prox += prox_count
        if prox_count == 0:
            break
    if verbose:
        print(f"  Proximity propagation: {total_prox} new labels ({prox_pass + 1} passes)")

    # Game sub-classification via string references
    game_sub_count = _classify_game_subcategories(
        sorted_addrs, labels, propagated, string_refs
    )
    if verbose:
        print(f"  Game sub-classification: {game_sub_count} functions categorized")

    return propagated


def _rw_region_propagation(sorted_addrs, labels, propagated, callees, callers):
    """
    Within the RW code region, propagate RW labels aggressively.

    If a function is inside the RW code region and has ANY call-graph
    connection to a known RW function, classify it as RW. This is more
    aggressive than global propagation because the linker places RW
    object code contiguously.

    Iterates until convergence.
    """
    # Determine RW code region from current labels
    rw_addrs = [a for a in sorted_addrs if a in labels and labels[a].startswith("rw_")]
    if len(rw_addrs) < 10:
        return 0

    rw_code_lo = min(rw_addrs)
    rw_code_hi = max(rw_addrs)

    # Build set of addresses in the RW region
    region_addrs = [a for a in sorted_addrs if rw_code_lo <= a <= rw_code_hi]

    total_count = 0
    for iteration in range(20):
        count = 0
        for addr in region_addrs:
            if addr in labels:
                continue

            # Check if this function has any connection to a known RW function
            connected_rw = False
            rw_subcounts = defaultdict(int)

            for c in callers.get(addr, set()):
                if c in labels and labels[c].startswith("rw_"):
                    connected_rw = True
                    rw_subcounts[labels[c]] += 1
            for c in callees.get(addr, set()):
                if c in labels and labels[c].startswith("rw_"):
                    connected_rw = True
                    rw_subcounts[labels[c]] += 1

            if connected_rw:
                best = max(rw_subcounts, key=rw_subcounts.get) if rw_subcounts else "rw_core"
                labels[addr] = best
                propagated[addr] = {
                    "category": best,
                    "subcategory": None,
                    "confidence": 0.70,
                    "method": "rw_region_propagation",
                }
                count += 1

        total_count += count
        if count == 0:
            break

    return total_count


def _proximity_propagation(sorted_addrs, labels, propagated):
    """
    If an unlabeled function is between two functions with the same
    RW category and the gap is small, inherit that label.
    """
    count = 0
    for i in range(1, len(sorted_addrs) - 1):
        addr = sorted_addrs[i]
        if addr in labels:
            continue

        prev_addr = sorted_addrs[i - 1]
        next_addr = sorted_addrs[i + 1]

        if (addr - prev_addr) > config.PROXIMITY_GAP:
            continue
        if (next_addr - addr) > config.PROXIMITY_GAP:
            continue

        prev_lbl = labels.get(prev_addr)
        next_lbl = labels.get(next_addr)

        if prev_lbl and next_lbl:
            if prev_lbl.startswith("rw_") and next_lbl.startswith("rw_"):
                labels[addr] = prev_lbl
                propagated[addr] = {
                    "category": prev_lbl,
                    "subcategory": None,
                    "confidence": config.CONFIDENCE_CLUSTER_PROXIMITY,
                    "method": "cluster_proximity",
                }
                count += 1

    return count


def _build_string_ref_map(imm_refs, strings):
    """Build map of func_addr -> list of referenced string texts."""
    str_by_addr = {}
    for s in strings:
        addr = int(s["address"], 16)
        str_by_addr[addr] = s["string"].lower()

    func_strings = defaultdict(list)
    for rdata_addr, func_addrs in imm_refs.items():
        if rdata_addr in str_by_addr:
            text = str_by_addr[rdata_addr]
            for fa in func_addrs:
                func_strings[fa].append(text)

    return func_strings


def _classify_game_subcategories(sorted_addrs, labels, propagated, string_refs):
    """
    For unlabeled functions, try to sub-classify as game categories
    based on referenced string content.
    """
    count = 0
    for addr in sorted_addrs:
        if addr in labels:
            continue

        refs = string_refs.get(addr, [])
        if not refs:
            continue

        best_cat = None
        best_score = 0
        combined_text = " ".join(refs)

        for cat, keywords in config.GAME_SUBCATEGORIES.items():
            score = sum(1 for kw in keywords if kw in combined_text)
            if score > best_score:
                best_score = score
                best_cat = cat

        if best_cat and best_score >= 1:
            labels[addr] = f"game_{best_cat}"
            propagated[addr] = {
                "category": f"game_{best_cat}",
                "subcategory": best_cat,
                "confidence": 0.60,
                "method": "string_keyword",
            }
            count += 1

    return count
