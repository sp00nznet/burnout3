"""
CLI entry point for the disassembly tool.

Usage:
    py -3 -m tools.disasm <path_to_xbe> [options]

Examples:
    py -3 -m tools.disasm "Burnout 3 Takedown/default.xbe" --text-only --stats-only -v
    py -3 -m tools.disasm "Burnout 3 Takedown/default.xbe" --text-only
    py -3 -m tools.disasm "Burnout 3 Takedown/default.xbe" -o output/
"""

import argparse
import sys

from .disasm import Disassembler


def main():
    parser = argparse.ArgumentParser(
        prog="tools.disasm",
        description="Burnout 3 XBE Disassembly Tool - "
                    "Static analysis and function detection for Xbox executables",
    )

    parser.add_argument(
        "xbe_path",
        help="Path to the Xbox XBE executable file",
    )
    parser.add_argument(
        "-o", "--output",
        dest="output_dir",
        default=None,
        help="Output directory for JSON databases and ASM listings "
             "(default: tools/disasm/output/)",
    )
    parser.add_argument(
        "--analysis-json",
        default=None,
        help="Path to burnout3_analysis.json (auto-detected if not specified)",
    )
    parser.add_argument(
        "--text-only",
        action="store_true",
        help="Only disassemble the .text section (faster, ~2.73 MB)",
    )
    parser.add_argument(
        "--stats-only",
        action="store_true",
        help="Print statistics only, don't write output files",
    )
    parser.add_argument(
        "-v", "--verbose",
        action="store_true",
        help="Verbose output with progress information",
    )
    parser.add_argument(
        "--force",
        action="store_true",
        help="Force re-analysis even if cache is valid",
    )

    args = parser.parse_args()

    try:
        disassembler = Disassembler(
            xbe_path=args.xbe_path,
            analysis_json=args.analysis_json,
            output_dir=args.output_dir,
            text_only=args.text_only,
            stats_only=args.stats_only,
            verbose=args.verbose,
            force=args.force,
        )
        success = disassembler.run()
        sys.exit(0 if success else 1)

    except FileNotFoundError as e:
        print(f"Error: {e}", file=sys.stderr)
        sys.exit(1)
    except ValueError as e:
        print(f"Error: {e}", file=sys.stderr)
        sys.exit(1)
    except KeyboardInterrupt:
        print("\nInterrupted.", file=sys.stderr)
        sys.exit(130)
    except Exception as e:
        print(f"Unexpected error: {e}", file=sys.stderr)
        import traceback
        traceback.print_exc()
        sys.exit(2)


if __name__ == "__main__":
    main()
