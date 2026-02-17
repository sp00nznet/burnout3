"""
Asset catalog tool for Burnout 3: Takedown.

Scans the game directory and produces a comprehensive inventory of all
asset files, classified by type with format details.
"""

import os
import struct
import json
from . import formats
from .txd_parser import parse_txd
from .arena_parser import parse_arena, parse_audio_stream


# File extension to category mapping
EXTENSION_CATEGORIES = {
    ".rws": "scene",
    ".txd": "texture",
    ".awd": "audio",
    ".hwd": "vehicle_hd",
    ".lwd": "vehicle_ld",
    ".xwb": "wave_bank",
    ".xmv": "video",
    ".bum": "burnout_model",
    ".bgv": "vehicle_geometry",
    ".bgd": "game_data",
    ".dat": "track_data",
    ".bin": "binary_data",
    ".kfs": "keyframe",
    ".btv": "vehicle_texture",
    ".xbe": "executable",
}


def catalog_assets(game_dir, verbose=False):
    """Scan game directory and catalog all asset files.

    Args:
        game_dir: Path to "Burnout 3 Takedown/" directory.
        verbose: Print progress info.

    Returns:
        dict with categorized asset inventory.
    """
    if not os.path.isdir(game_dir):
        raise ValueError(f"Game directory not found: {game_dir}")

    assets = []
    categories = {}
    total_size = 0

    for dirpath, dirnames, filenames in os.walk(game_dir):
        for filename in sorted(filenames):
            filepath = os.path.join(dirpath, filename)
            relpath = os.path.relpath(filepath, game_dir)
            ext = os.path.splitext(filename)[1].lower()

            try:
                file_size = os.path.getsize(filepath)
            except OSError:
                continue

            category = EXTENSION_CATEGORIES.get(ext, "other")

            asset = {
                "path": relpath.replace("\\", "/"),
                "name": filename,
                "ext": ext,
                "category": category,
                "size": file_size,
            }

            # Read file header for identification
            try:
                with open(filepath, "rb") as f:
                    header = f.read(16)
                asset["format"] = formats.identify_file(header)
            except (OSError, IOError):
                asset["format"] = "unreadable"

            # Deep parse specific formats
            if verbose and ext == ".txd":
                try:
                    with open(filepath, "rb") as f:
                        txd_data = f.read()
                    txd_info = parse_txd(txd_data, verbose=False)
                    asset["texture_count"] = len(txd_info["textures"])
                    asset["textures"] = [
                        {"name": t["name"], "width": t["width"], "height": t["height"],
                         "format": t["format_name"]}
                        for t in txd_info["textures"]
                    ]
                except Exception as e:
                    asset["parse_error"] = str(e)

            assets.append(asset)
            total_size += file_size

            # Track category stats
            if category not in categories:
                categories[category] = {"count": 0, "total_size": 0, "extensions": set()}
            categories[category]["count"] += 1
            categories[category]["total_size"] += file_size
            categories[category]["extensions"].add(ext)

    # Convert sets to lists for JSON
    for cat in categories.values():
        cat["extensions"] = sorted(cat["extensions"])

    result = {
        "game_dir": game_dir,
        "total_files": len(assets),
        "total_size": total_size,
        "total_size_mb": round(total_size / (1024 * 1024), 1),
        "categories": categories,
        "assets": assets,
    }

    if verbose:
        _print_catalog_summary(result)

    return result


def _print_catalog_summary(result):
    """Print formatted catalog summary."""
    print(f"Game directory: {result['game_dir']}")
    print(f"Total files: {result['total_files']}")
    print(f"Total size: {result['total_size_mb']} MB")

    print(f"\n{'Category':<20} {'Count':>6} {'Size (MB)':>10} {'Extensions'}")
    print("-" * 65)
    for cat_name, cat_info in sorted(result["categories"].items(),
                                      key=lambda x: -x[1]["total_size"]):
        size_mb = cat_info["total_size"] / (1024 * 1024)
        exts = ", ".join(cat_info["extensions"])
        print(f"{cat_name:<20} {cat_info['count']:6d} {size_mb:10.1f} {exts}")

    # Print texture details if available
    txd_assets = [a for a in result["assets"] if a.get("textures")]
    if txd_assets:
        print(f"\nTexture Dictionaries:")
        for a in txd_assets:
            print(f"  {a['path']}: {a['texture_count']} textures")
            for tex in a.get("textures", [])[:10]:
                print(f"    {tex['name']:<24} {tex['width']}x{tex['height']} {tex['format']}")
            if len(a.get("textures", [])) > 10:
                print(f"    ... and {len(a['textures']) - 10} more")
