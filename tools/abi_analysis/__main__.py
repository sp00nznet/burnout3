"""
CLI entry point for ABI analysis.

Usage:
    py -3 -m tools.abi_analysis "Burnout 3 Takedown/default.xbe" [-v]
"""

import argparse
import sys

from .analyzer import run


def main():
    parser = argparse.ArgumentParser(
        description="Analyze calling conventions and ABI in Burnout 3 XBE"
    )
    parser.add_argument(
        "xbe_path",
        help="Path to default.xbe"
    )
    parser.add_argument(
        "--functions",
        help="Path to functions.json"
    )
    parser.add_argument(
        "--identified",
        help="Path to identified_functions.json"
    )
    parser.add_argument(
        "--output", "-o",
        help="Output directory (default: tools/abi_analysis/output)"
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
            identified_path=args.identified,
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
