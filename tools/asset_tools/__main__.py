"""
Burnout 3: Takedown - Asset Analysis Tool

Usage:
    py -3 -m tools.asset_tools catalog "Burnout 3 Takedown/" [-v]
    py -3 -m tools.asset_tools txd "Burnout 3 Takedown/Data/Global.txd" [-v]
    py -3 -m tools.asset_tools extract "Burnout 3 Takedown/Data/Global.txd" [-v] [--name PATTERN]
    py -3 -m tools.asset_tools arena "Burnout 3 Takedown/Tracks/crash1.rws" [-v]
    py -3 -m tools.asset_tools audio "Burnout 3 Takedown/sound/Fe.awd" [-v]
"""

import sys
import os
import json
import time

# Add project root to path
sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))))

from tools.asset_tools.txd_parser import parse_txd, extract_texture_data
from tools.asset_tools.arena_parser import parse_arena, parse_audio_stream
from tools.asset_tools.catalog import catalog_assets
from tools.asset_tools.texture_convert import (
    decode_dxt_texture, decode_argb_texture, decode_p8_texture, save_png
)


def main():
    if len(sys.argv) < 3:
        print(__doc__)
        sys.exit(1)

    command = sys.argv[1]
    target = sys.argv[2]
    verbose = "-v" in sys.argv

    # Ensure output directory exists
    output_dir = os.path.join(os.path.dirname(__file__), "output")
    os.makedirs(output_dir, exist_ok=True)

    start_time = time.time()

    if command == "catalog":
        result = catalog_assets(target, verbose=verbose)
        output_file = os.path.join(output_dir, "asset_catalog.json")
        # Don't include full asset list in output if too large
        output = {k: v for k, v in result.items() if k != "assets"}
        output["asset_count"] = len(result["assets"])
        # Include per-category file lists
        output["files_by_category"] = {}
        for asset in result["assets"]:
            cat = asset["category"]
            if cat not in output["files_by_category"]:
                output["files_by_category"][cat] = []
            entry = {"path": asset["path"], "size": asset["size"]}
            if asset.get("textures"):
                entry["texture_count"] = asset["texture_count"]
            output["files_by_category"][cat].append(entry)

        _write_json(output_file, output)
        print(f"\nCatalog written to {output_file}")

    elif command == "txd":
        with open(target, "rb") as f:
            data = f.read()
        result = parse_txd(data, verbose=verbose)
        basename = os.path.splitext(os.path.basename(target))[0]
        output_file = os.path.join(output_dir, f"txd_{basename}.json")
        _write_json(output_file, result)
        print(f"\nTXD analysis written to {output_file}")

    elif command == "extract":
        # Extract textures from a TXD to PNG
        name_filter = None
        if "--name" in sys.argv:
            idx = sys.argv.index("--name")
            if idx + 1 < len(sys.argv):
                name_filter = sys.argv[idx + 1].lower()

        with open(target, "rb") as f:
            data = f.read()
        result = parse_txd(data, verbose=verbose)

        basename = os.path.splitext(os.path.basename(target))[0]
        tex_dir = os.path.join(output_dir, f"textures_{basename}")
        os.makedirs(tex_dir, exist_ok=True)

        extracted = 0
        skipped = 0
        for tex in result["textures"]:
            if name_filter and name_filter not in tex["name"].lower():
                continue

            fmt = tex["format_code"]
            w, h = tex["width"], tex["height"]
            raw_data = extract_texture_data(data, tex)

            try:
                if fmt in (0x0C, 0x0E, 0x0F):
                    pixels = decode_dxt_texture(raw_data, w, h, fmt)
                elif fmt == 0x0B:
                    pixels = decode_p8_texture(raw_data, w, h)
                elif fmt in (0x06, 0x07, 0x05, 0x00, 0x12, 0x1E, 0x11):
                    pixels = decode_argb_texture(raw_data, w, h, fmt)
                else:
                    if verbose:
                        print(f"  Skipping {tex['name']}: unsupported format 0x{fmt:02X}")
                    skipped += 1
                    continue

                out_path = os.path.join(tex_dir, f"{tex['name']}.png")
                save_png(pixels, w, h, out_path)
                extracted += 1
                if verbose:
                    print(f"  Extracted {tex['name']} ({w}x{h} {tex['format_name']})")
            except Exception as e:
                print(f"  Error extracting {tex['name']}: {e}")
                skipped += 1

        print(f"\nExtracted {extracted} textures to {tex_dir}")
        if skipped:
            print(f"Skipped {skipped} textures (unsupported format)")

    elif command == "arena":
        with open(target, "rb") as f:
            data = f.read()
        result = parse_arena(data, verbose=verbose)
        basename = os.path.splitext(os.path.basename(target))[0]
        output_file = os.path.join(output_dir, f"arena_{basename}.json")
        _write_json(output_file, result)
        print(f"\nArena analysis written to {output_file}")

    elif command == "audio":
        with open(target, "rb") as f:
            data = f.read()
        result = parse_audio_stream(data, verbose=verbose)
        basename = os.path.splitext(os.path.basename(target))[0]
        output_file = os.path.join(output_dir, f"audio_{basename}.json")
        _write_json(output_file, result)
        print(f"\nAudio analysis written to {output_file}")

    else:
        print(f"Unknown command: {command}")
        print(__doc__)
        sys.exit(1)

    elapsed = time.time() - start_time
    print(f"Completed in {elapsed:.1f}s")


def _write_json(path, data):
    """Write JSON output with proper formatting."""
    with open(path, "w", encoding="utf-8") as f:
        json.dump(data, f, indent=2, default=str)


if __name__ == "__main__":
    main()
