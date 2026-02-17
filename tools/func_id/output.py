"""
Output writers for function identification results.

Produces JSON files and a human-readable summary.
"""

import json
import os
from collections import Counter


def write_results(functions, rw_results, crt_results, propagated,
                  rw_modules, output_dir, verbose=False):
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
    """
    os.makedirs(output_dir, exist_ok=True)

    # Build the enriched function database
    enriched = _build_enriched_db(functions, rw_results, crt_results, propagated)

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


def _build_enriched_db(functions, rw_results, crt_results, propagated):
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

    # Group RW subcategories
    rw_total = sum(v for k, v in cat_counts.items() if k.startswith("rw_"))
    game_total = sum(v for k, v in cat_counts.items() if k.startswith("game_"))
    crt_total = cat_counts.get("crt", 0)
    unknown_total = cat_counts.get("unknown", 0)

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
            "game_classified": game_total,
            "unknown": unknown_total,
        },
        "percentages": {
            "renderware": round(rw_total / total * 100, 1) if total else 0,
            "crt": round(crt_total / total * 100, 1) if total else 0,
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
    print(f"  Game (classified):  {cls['game_classified']:,}  ({pct['game_classified']}%)")
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
