"""
Burnout 3: Takedown - x86 → C Static Recompiler

Usage:
    py -3 -m tools.recomp <xbe_path> [options]

Options:
    -o, --output-dir DIR    Output directory (default: tools/recomp/output)
    -f, --function ADDR     Translate a single function (hex address)
    -c, --category CAT      Translate functions of a specific category
    --game-only             Only translate game_engine + game_vtable + unknown functions
    --all                   Translate all functions (including RW, CRT, XDK)
    -n, --max-funcs N       Maximum number of functions to translate
    -v, --verbose           Verbose output
    --list-categories       List available function categories and counts
    --header                Generate C header with forward declarations
"""

import argparse
import json
import os
import sys
import time

from .translator import BatchTranslator
from .output import write_summary, print_stats, generate_header


def find_data_files():
    """Locate the disasm/func_id output files."""
    base = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

    paths = {
        "functions": os.path.join(base, "disasm", "output", "functions.json"),
        "labels": os.path.join(base, "disasm", "output", "labels.json"),
        "identified": os.path.join(base, "func_id", "output", "identified_functions.json"),
    }

    for key, path in paths.items():
        if not os.path.exists(path):
            print(f"WARNING: {key} not found at {path}", file=sys.stderr)
            paths[key] = None

    return paths


def list_categories(translator):
    """Print category breakdown."""
    cats = {}
    for addr, func_info in sorted(translator.func_db.items()):
        cls = translator.classification_db.get(addr, {})
        cat = cls.get("category", "unknown")
        cats[cat] = cats.get(cat, 0) + 1

    print(f"\nFunction categories ({len(translator.func_db)} total):")
    print(f"{'Category':<30} {'Count':>8} {'Pct':>8}")
    print("-" * 48)
    for cat, count in sorted(cats.items(), key=lambda x: -x[1]):
        pct = count / len(translator.func_db) * 100
        print(f"{cat:<30} {count:>8} {pct:>7.1f}%")


def main():
    parser = argparse.ArgumentParser(
        description="Burnout 3: x86 → C Static Recompiler")
    parser.add_argument("xbe_path", help="Path to default.xbe")
    parser.add_argument("-o", "--output-dir",
                        help="Output directory")
    parser.add_argument("-f", "--function",
                        help="Translate single function (hex address)")
    parser.add_argument("-c", "--category",
                        help="Translate functions of a category")
    parser.add_argument("--game-only", action="store_true",
                        help="Only game functions (game_engine, game_vtable, unknown)")
    parser.add_argument("--all", action="store_true",
                        help="Translate all functions")
    parser.add_argument("-n", "--max-funcs", type=int,
                        help="Max functions to translate")
    parser.add_argument("-v", "--verbose", action="store_true")
    parser.add_argument("--list-categories", action="store_true",
                        help="List categories and exit")
    parser.add_argument("--header", action="store_true",
                        help="Generate C header file")

    args = parser.parse_args()

    # Find data files
    data_files = find_data_files()
    if not data_files["functions"]:
        print("ERROR: functions.json not found. Run the disassembler first.",
              file=sys.stderr)
        sys.exit(1)

    print(f"Loading data files...", file=sys.stderr)
    t0 = time.time()

    translator = BatchTranslator(
        xbe_path=args.xbe_path,
        func_json_path=data_files["functions"],
        labels_json_path=data_files.get("labels"),
        identified_json_path=data_files.get("identified"),
        output_dir=args.output_dir,
    )

    t_load = time.time() - t0
    print(f"Loaded {len(translator.func_db)} functions, "
          f"{len(translator.label_db)} labels, "
          f"{len(translator.classification_db)} classifications "
          f"in {t_load:.1f}s", file=sys.stderr)

    # List categories mode
    if args.list_categories:
        list_categories(translator)
        return

    # Single function mode
    if args.function:
        addr = int(args.function, 16)
        code = translator.translate_single(addr)
        if code:
            print(code)
        else:
            print(f"ERROR: Could not translate function at 0x{addr:08X}",
                  file=sys.stderr)
            sys.exit(1)
        return

    # Generate header mode
    if args.header:
        output_dir = args.output_dir or os.path.join(
            os.path.dirname(__file__), "output")
        os.makedirs(output_dir, exist_ok=True)

        if args.game_only:
            funcs = translator.get_functions_by_category(
                categories={"game_engine", "game_vtable", "unknown"})
        elif args.category:
            funcs = translator.get_functions_by_category(
                categories={args.category})
        else:
            funcs = translator.get_functions_by_category()

        header_path = os.path.join(output_dir, "recomp_functions.h")
        generate_header(funcs, header_path)
        print(f"Generated header: {header_path} ({len(funcs)} declarations)")
        return

    # Batch translation
    t0 = time.time()

    if args.category:
        categories = {args.category}
        funcs = translator.get_functions_by_category(categories=categories)
    elif args.game_only:
        # Game-specific functions only
        categories = {"game_engine", "game_vtable", "unknown"}
        funcs = translator.get_functions_by_category(categories=categories)
    elif args.all:
        funcs = translator.get_functions_by_category()
    else:
        # Default: game functions only
        categories = {"game_engine", "game_vtable", "unknown"}
        funcs = translator.get_functions_by_category(categories=categories)

    print(f"\nTranslating {len(funcs)} functions...", file=sys.stderr)

    stats = translator.translate_batch(
        funcs,
        max_funcs=args.max_funcs,
        verbose=args.verbose,
    )

    t_translate = time.time() - t0

    print(f"\n=== Translation Complete ({t_translate:.1f}s) ===",
          file=sys.stderr)
    print_stats(stats)

    # Write summary
    output_dir = args.output_dir or os.path.join(
        os.path.dirname(__file__), "output")
    summary_path = write_summary(stats, output_dir)
    print(f"\nSummary: {summary_path}", file=sys.stderr)


if __name__ == "__main__":
    main()
