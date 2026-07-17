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
import re
import sys
import time

from .translator import BatchTranslator
from .output import write_summary, print_stats, generate_header

# "void sub_001AA100(void)" with no trailing ';' -- a definition, not a decl.
_MANUAL_DEF_RE = re.compile(r"^void (sub_([0-9A-Fa-f]{8}))\(void\)\s*$", re.M)


def _manual_definitions(path):
    """Addresses of functions DEFINED (not merely declared) in a manual file.

    Emitting gen code for these collides with the hand-written version at link
    time, which is why every regen previously needed '#if 0' re-applied around
    each one by hand.
    """
    if not os.path.exists(path):
        print(f"WARNING: {path} not found; excluding nothing.", file=sys.stderr)
        return set()
    src = open(path, encoding="utf-8", errors="replace").read()
    return {int(m.group(2), 16) for m in _MANUAL_DEF_RE.finditer(src)}


def _func_addr(entry):
    """get_functions_by_category yields (addr, func_info) tuples."""
    return entry[0] if isinstance(entry, tuple) else entry


def find_data_files():
    """Locate the disasm/func_id output files."""
    base = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

    paths = {
        "functions": os.path.join(base, "disasm", "output", "functions.json"),
        "labels": os.path.join(base, "disasm", "output", "labels.json"),
        "identified": os.path.join(base, "func_id", "output", "identified_functions.json"),
        "abi": os.path.join(base, "abi_analysis", "output", "abi_functions.json"),
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
    parser.add_argument("--split", type=int, metavar="N",
                        help="Split output into files of N functions each")
    parser.add_argument("--gen-dir",
                        help="Output dir for split generated files "
                             "(default: src/game/recomp/gen)")
    parser.add_argument("--exclude-manual", metavar="FILE",
                        nargs="?", const="src/game/recomp/recomp_manual.c",
                        help="Skip functions that FILE already defines, so the "
                             "generated code does not collide with hand-written "
                             "overrides. Defaults to recomp_manual.c. This "
                             "replaces hand-editing '#if 0' around every "
                             "overridden function after each regen.")

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
        abi_json_path=data_files.get("abi"),
        output_dir=args.output_dir,
    )

    t_load = time.time() - t0
    print(f"Loaded {len(translator.func_db)} functions, "
          f"{len(translator.label_db)} labels, "
          f"{len(translator.classification_db)} classifications, "
          f"{len(translator.abi_db)} ABI entries "
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
        generate_header(funcs, header_path, abi_db=translator.abi_db)
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

    declare_only = set()
    if args.exclude_manual:
        excluded = _manual_definitions(args.exclude_manual)
        if excluded:
            # Declare but do not define: emitting a body collides with the
            # hand-written one at link, while dropping the function outright
            # would also drop the declaration that recomp_manual.c's own
            # dispatch table needs.
            declare_only = {a for a in excluded if a in translator.func_db}
            # The manual file defines these as sub_XXXXXXXX. If a naming pass
            # has since renamed them in func_db, generated call sites would
            # emit the new name and fail to link against the hand-written
            # definition -- so pin excluded functions back to the sub_ name.
            renamed = 0
            for addr in declare_only:
                info = translator.func_db[addr]
                plain = f"sub_{addr:08X}"
                if info.get("name") != plain:
                    info["name"] = plain
                    renamed += 1
            print(f"Declaring (not defining) {len(declare_only)} functions "
                  f"already defined in {args.exclude_manual}"
                  + (f" ({renamed} pinned back to sub_ names)" if renamed else ""),
                  file=sys.stderr)

    print(f"\nTranslating {len(funcs)} functions...", file=sys.stderr)

    if args.split:
        # Split output mode: multiple .c files + header + dispatch table
        gen_dir = args.gen_dir or os.path.join(
            os.path.dirname(os.path.dirname(os.path.dirname(__file__))),
            "src", "game", "recomp", "gen")

        if args.max_funcs:
            funcs = funcs[:args.max_funcs]

        stats = translator.translate_batch_split(
            funcs,
            output_dir=gen_dir,
            chunk_size=args.split,
            verbose=args.verbose,
            declare_only=declare_only,
        )

        t_translate = time.time() - t0
        print(f"\n=== Split Translation Complete ({t_translate:.1f}s) ===",
              file=sys.stderr)
        print(f"{stats['translated']}/{stats['total']} functions "
              f"({stats['failed']} failed), "
              f"{stats['total_lines']} lines of C, "
              f"{stats['num_chunks']} source files",
              file=sys.stderr)
        for f_path in stats.get("files", []):
            print(f"  {f_path}", file=sys.stderr)
    else:
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
