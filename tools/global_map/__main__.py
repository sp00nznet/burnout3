"""
CLI entry point for the global variable mapper.

Usage:
    py -3 -m tools.global_map "Burnout 3 Takedown/default.xbe" [-v]
"""

import argparse
import sys

from .mapper import run


def main():
    parser = argparse.ArgumentParser(
        description="Map global variables and data structures in Burnout 3 XBE"
    )
    parser.add_argument(
        "xbe_path",
        help="Path to default.xbe"
    )
    parser.add_argument(
        "--functions",
        help="Path to functions.json (default: tools/disasm/output/functions.json)"
    )
    parser.add_argument(
        "--xrefs",
        help="Path to xrefs.json (default: tools/disasm/output/xrefs.json)"
    )
    parser.add_argument(
        "--identified",
        help="Path to identified_functions.json (default: tools/func_id/output/identified_functions.json)"
    )
    parser.add_argument(
        "--strings",
        help="Path to strings.json (default: tools/disasm/output/strings.json)"
    )
    parser.add_argument(
        "--output", "-o",
        help="Output directory (default: tools/global_map/output)"
    )
    parser.add_argument(
        "--verbose", "-v",
        action="store_true",
        help="Print detailed progress"
    )

    args = parser.parse_args()

    try:
        run(
            xbe_path=args.xbe_path,
            functions_path=args.functions,
            xrefs_path=args.xrefs,
            identified_path=args.identified,
            strings_path=args.strings,
            output_dir=args.output,
            verbose=args.verbose,
        )
    except FileNotFoundError as e:
        print(f"Error: {e}", file=sys.stderr)
        sys.exit(1)
    except Exception as e:
        print(f"Error: {e}", file=sys.stderr)
        raise


if __name__ == "__main__":
    main()
