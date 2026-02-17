"""
Output writers for function identification results.

Produces JSON files and a human-readable summary.
"""

import json
import os
from collections import Counter


def write_results(functions, rw_results, crt_results, propagated,
                  rw_modules, output_dir, verbose=False, stub_results=None):
    """
    Write all output files.

    Args:
        functions: Original function list.
        rw_results: RW identification results.
        crt_results: CRT identification results.
        propagated: Clustering/propagation results.
        rw_modules: RW module -> function mappings.
        output_dir: Directory to write output files.
        verbose: Print progress info.
        stub_results: Stub classification results (optional).
    """
    os.makedirs(output_dir, exist_ok=True)
    stub_results = stub_results or {}

    # Build the enriched function database
    enriched = _build_enriched_db(functions, rw_results, crt_results,
                                  propagated, stub_results)

    # Write files
    _write_json(os.path.join(output_dir, "identified_functions.json"), enriched)
    _write_json(os.path.join(output_dir, "rw_modules.json"),
                _serialize_rw_modules(rw_modules))
    _write_json(os.path.join(output_dir, "crt_functions.json"),
                _serialize_crt(crt_results))

    summary = _build_summary(enriched, rw_results, crt_results, propagated, rw_modules)
    _write_json(os.path.join(output_dir, "summary.json"), summary)

    if verbose:
        _print_summary(summary)

    return summary


def _build_enriched_db(functions, rw_results, crt_results, propagated,
                       stub_results):
    """Build enriched function entries with classification info."""
    enriched = []
    for f in functions:
        addr = int(f["start"], 16)
        entry = {
            "start": f["start"],
            "end": f["end"],
            "size": f["size"],
            "name": f["name"],
            "section": f["section"],
        }

        if addr in crt_results:
            info = crt_results[addr]
            entry["category"] = "crt"
            entry["identified_name"] = info["name"]
            entry["confidence"] = info["confidence"]
            entry["method"] = info["method"]
        elif addr in rw_results:
            info = rw_results[addr]
            entry["category"] = info["category"]
            entry["module"] = info.get("module", "")
            entry["source_file"] = info.get("source_file", "")
            entry["confidence"] = info["confidence"]
            entry["method"] = info["method"]
        elif addr in propagated:
            info = propagated[addr]
            entry["category"] = info["category"]
            entry["subcategory"] = info.get("subcategory")
            entry["confidence"] = info["confidence"]
            entry["method"] = info["method"]
            if "vtable_addr" in info:
                entry["vtable_addr"] = f"0x{info['vtable_addr']:08X}"
                entry["vtable_index"] = info["vtable_index"]
        elif addr in stub_results:
            info = stub_results[addr]
            entry["category"] = info["category"]
            entry["stub_type"] = info.get("stub_type", "")
            entry["confidence"] = info["confidence"]
            entry["method"] = info["method"]
        else:
            entry["category"] = "unknown"
            entry["confidence"] = 0.0
            entry["method"] = "none"

        enriched.append(entry)

    return enriched


def _build_summary(enriched, rw_results, crt_results, propagated, rw_modules):
    """Build summary statistics."""
    total = len(enriched)
    cat_counts = Counter(e["category"] for e in enriched)
    method_counts = Counter(e["method"] for e in enriched)

    # Group subcategories
    rw_total = sum(v for k, v in cat_counts.items() if k.startswith("rw_"))
    game_total = sum(v for k, v in cat_counts.items() if k.startswith("game_"))
    crt_total = cat_counts.get("crt", 0)
    data_init_total = cat_counts.get("data_init", 0)
    unknown_total = cat_counts.get("unknown", 0)

    # Count vtable functions specifically
    vtable_total = sum(1 for e in enriched if e.get("method") in ("vtable_scan", "vtable_ctor"))

    # RW modules with function counts
    rw_module_summary = {}
    for name, mod in rw_modules.items():
        rw_module_summary[name] = {
            "category": mod["category"],
            "path": mod["path"],
            "num_functions": len(mod["functions"]),
        }

    return {
        "total_functions": total,
        "classification": {
            "renderware": rw_total,
            "crt": crt_total,
            "data_init": data_init_total,
            "game_classified": game_total,
            "vtable_methods": vtable_total,
            "unknown": unknown_total,
        },
        "percentages": {
            "renderware": round(rw_total / total * 100, 1) if total else 0,
            "crt": round(crt_total / total * 100, 1) if total else 0,
            "data_init": round(data_init_total / total * 100, 1) if total else 0,
            "game_classified": round(game_total / total * 100, 1) if total else 0,
            "unknown": round(unknown_total / total * 100, 1) if total else 0,
        },
        "by_category": {k: v for k, v in sorted(cat_counts.items())},
        "by_method": {k: v for k, v in sorted(method_counts.items())},
        "rw_modules": rw_module_summary,
        "rw_module_count": len(rw_modules),
    }


def _serialize_rw_modules(rw_modules):
    """Serialize RW modules for JSON output."""
    result = {}
    for name, mod in sorted(rw_modules.items()):
        result[name] = {
            "address": f"0x{mod['address']:08X}",
            "category": mod["category"],
            "path": mod["path"],
            "functions": [f"0x{a:08X}" for a in mod["functions"]],
            "num_functions": len(mod["functions"]),
        }
    return result


def _serialize_crt(crt_results):
    """Serialize CRT results for JSON output."""
    result = []
    for addr in sorted(crt_results):
        info = crt_results[addr]
        result.append({
            "address": f"0x{addr:08X}",
            "name": info["name"],
            "confidence": info["confidence"],
        })
    return result


def _print_summary(summary):
    """Print a human-readable summary to stdout."""
    print("\n" + "=" * 60)
    print("FUNCTION IDENTIFICATION SUMMARY")
    print("=" * 60)
    total = summary["total_functions"]
    cls = summary["classification"]
    pct = summary["percentages"]

    print(f"  Total functions:    {total:,}")
    print(f"  RenderWare:         {cls['renderware']:,}  ({pct['renderware']}%)")
    print(f"  CRT/MSVC:           {cls['crt']:,}  ({pct['crt']}%)")
    print(f"  Data init stubs:    {cls['data_init']:,}  ({pct['data_init']}%)")
    print(f"  Game (classified):  {cls['game_classified']:,}  ({pct['game_classified']}%)")
    vtable_count = cls.get('vtable_methods', 0)
    if vtable_count:
        print(f"    (vtable methods): {vtable_count:,}")
    print(f"  Unknown:            {cls['unknown']:,}  ({pct['unknown']}%)")

    print(f"\n  RW source modules:  {summary['rw_module_count']}")

    print("\n  By category:")
    for cat, count in sorted(summary["by_category"].items(), key=lambda x: -x[1]):
        print(f"    {cat:30s} {count:6,}")

    print("\n  By method:")
    for method, count in sorted(summary["by_method"].items(), key=lambda x: -x[1]):
        print(f"    {method:30s} {count:6,}")
    print("=" * 60)


def _write_json(path, data):
    """Write data as formatted JSON."""
    with open(path, "w", encoding="utf-8") as f:
        json.dump(data, f, indent=2, ensure_ascii=False)
