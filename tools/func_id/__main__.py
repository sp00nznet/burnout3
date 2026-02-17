"""
CLI entry point for the function identification tool.

Usage:
    py -3 -m tools.func_id "Burnout 3 Takedown/default.xbe" [-v]
    py -3 -m tools.func_id path/to/default.xbe --functions path/to/functions.json -v
"""

import argparse
import sys

from .identify import run


def main():
    parser = argparse.ArgumentParser(
        description="Identify RenderWare, CRT, and game functions in Burnout 3 XBE"
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
        "--strings",
        help="Path to strings.json (default: tools/disasm/output/strings.json)"
    )
    parser.add_argument(
        "--xrefs",
        help="Path to xrefs.json (default: tools/disasm/output/xrefs.json)"
    )
    parser.add_argument(
        "--output", "-o",
        help="Output directory (default: tools/func_id/output)"
    )
    parser.add_argument(
        "--verbose", "-v",
        action="store_true",
        help="Print detailed progress"
    )

    args = parser.parse_args()

    try:
        summary = run(
            xbe_path=args.xbe_path,
            functions_path=args.functions,
            strings_path=args.strings,
            xrefs_path=args.xrefs,
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
