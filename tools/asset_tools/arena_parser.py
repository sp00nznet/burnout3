"""
Criterion Arena (.rws) parser for Burnout 3: Takedown.

Arena files use Criterion's custom chunk-based format built on the
RenderWare binary stream foundation. The format wraps track/scene
data in a container with a structured header.

File structure:
  crARENA (0x080D) - top-level container
    crARENA_HEADER (0x080E) - structured header with:
      - Track/scene name
      - Segment descriptors
      - Configuration metadata
      - Named sub-sections (GenCrash, SloCrash, etc.)
    crARENA_DATA (0x080F) - packed/compressed scene data
      - Geometry, materials, textures
      - Data appears to be a dense binary blob (compressed or custom-packed)

Audio-type files (.awd, .hwd, .lwd) use chunk type 0x0809 with
a similar but simpler structure.
"""

import struct
from . import formats


def parse_arena(data, verbose=False):
    """Parse a Criterion arena (.rws) file.

    Returns dict with chunk tree, header analysis, and metadata.
    """
    if len(data) < 12:
        raise ValueError("File too small")

    chunks = _parse_chunk_tree(data, 0, len(data), depth=0, max_depth=3)

    # Extract header info if this is an arena
    header_info = None
    arena_size = 0
    data_size = 0

    for chunk in chunks:
        if chunk["type"] == 0x080D:
            arena_size = chunk["size"]
            for child in chunk.get("children", []):
                if child["type"] == 0x080E:
                    header_info = _analyze_arena_header(data, child)
                elif child["type"] == 0x080F:
                    data_size = child["size"]

    result = {
        "format": "criterion_arena",
        "file_size": len(data),
        "chunks": chunks,
        "arena_size": arena_size,
        "data_size": data_size,
        "header": header_info,
    }

    if verbose:
        _print_arena_summary(result)

    return result


def parse_audio_stream(data, verbose=False):
    """Parse a Criterion audio file (.awd, .hwd, .lwd).

    These start with chunk type 0x0809 (crAUDIO).
    """
    if len(data) < 12:
        raise ValueError("File too small")

    chunks = _parse_chunk_tree(data, 0, len(data), depth=0, max_depth=2)

    # Extract sound names from the raw data
    sound_names = _extract_strings(data, min_length=3)

    result = {
        "format": "criterion_audio",
        "file_size": len(data),
        "chunks": chunks,
        "sound_names": sound_names,
    }

    if verbose:
        _print_audio_summary(result)

    return result


def _parse_chunk_tree(data, start, end, depth, max_depth):
    """Recursively parse RenderWare/Criterion chunk headers."""
    chunks = []
    offset = start

    while offset + 12 <= end:
        chunk_type, chunk_size, version = struct.unpack_from("<III", data, offset)

        # Validate chunk type (reasonable range)
        if chunk_type > 0x00FFFFFF and chunk_type not in formats.CRITERION_CHUNKS:
            break

        # Validate size
        if chunk_size > end - offset - 12:
            # Might be the top-level chunk spanning the whole file
            if offset == start and chunk_size <= len(data):
                pass  # Allow it
            else:
                break

        chunk_name = formats.CRITERION_CHUNKS.get(chunk_type, f"unk_0x{chunk_type:04x}")
        ver_str = formats.rw_version_str(version)

        chunk = {
            "offset": offset,
            "type": chunk_type,
            "type_name": chunk_name,
            "size": chunk_size,
            "version": ver_str,
        }

        # Recurse into container chunks
        if chunk_type in formats.CONTAINER_CHUNKS and depth < max_depth:
            child_start = offset + 12
            child_end = offset + 12 + chunk_size
            children = _parse_chunk_tree(data, child_start, child_end, depth + 1, max_depth)
            if children:
                chunk["children"] = children

        chunks.append(chunk)
        offset += 12 + chunk_size

        if offset <= start:
            break  # Prevent infinite loops

    return chunks


def _analyze_arena_header(data, header_chunk):
    """Extract metadata from a crARENA_HEADER chunk."""
    hdr_start = header_chunk["offset"] + 12
    hdr_size = header_chunk["size"]

    if hdr_start + hdr_size > len(data):
        return None

    hdr_data = data[hdr_start : hdr_start + hdr_size]

    # Extract structured fields from header
    info = {
        "size": hdr_size,
    }

    # Parse initial fields
    if len(hdr_data) >= 0x40:
        num_fields = min(len(hdr_data) // 4, 16)
        fields = struct.unpack_from(f"<{num_fields}I", hdr_data, 0)
        info["field_count"] = fields[0]
        if num_fields > 9:
            info["data_size_1"] = fields[9]
        if num_fields > 11:
            info["data_size_2"] = fields[11]

    # Extract all printable strings from header
    strings = _extract_strings(hdr_data, min_length=3)

    # Classify strings
    info["scene_name"] = None
    info["segments"] = []
    info["sub_scenes"] = []
    info["metadata_fields"] = []

    for s in strings:
        text = s["text"]
        if text.startswith("crash") or text.startswith("_fe"):
            info["scene_name"] = text
        elif text.startswith("Segment"):
            info["segments"].append(text)
        elif text.startswith("Gen") or text.startswith("Slo"):
            info["sub_scenes"].append(text)
        elif text in ("true", "false", "name", "id", "data", "uint", "notes"):
            info["metadata_fields"].append(text)
        elif len(text) >= 4 and text.isascii():
            info["sub_scenes"].append(text)

    return info


def _extract_strings(data, min_length=3):
    """Extract printable ASCII strings from binary data."""
    strings = []
    i = 0
    while i < len(data):
        if 32 <= data[i] < 127:
            j = i
            while j < len(data) and 32 <= data[j] < 127:
                j += 1
            if j - i >= min_length:
                text = data[i:j].decode("ascii", errors="replace").strip()
                if text:
                    strings.append({"offset": i, "text": text})
            i = j + 1
        else:
            i += 1
    return strings


def _print_arena_summary(result):
    """Print formatted arena analysis."""
    print(f"Arena file: {result['file_size']} bytes ({result['file_size']/1024/1024:.1f} MB)")
    print(f"Arena data: {result['data_size']} bytes ({result['data_size']/1024/1024:.1f} MB)")

    # Print chunk tree
    print("\nChunk tree:")
    _print_chunk_tree(result["chunks"], indent=0)

    # Print header info
    hdr = result.get("header")
    if hdr:
        print(f"\nHeader ({hdr['size']} bytes):")
        if hdr.get("scene_name"):
            print(f"  Scene: {hdr['scene_name']}")
        if hdr.get("segments"):
            print(f"  Segments: {', '.join(hdr['segments'])}")
        if hdr.get("sub_scenes"):
            print(f"  Sub-scenes: {', '.join(hdr['sub_scenes'])}")


def _print_audio_summary(result):
    """Print formatted audio analysis."""
    print(f"Audio file: {result['file_size']} bytes ({result['file_size']/1024:.1f} KB)")

    print("\nChunk tree:")
    _print_chunk_tree(result["chunks"], indent=0)

    names = result.get("sound_names", [])
    if names:
        # Filter to likely sound names (alphabetic, reasonable length)
        sound_names = [s["text"] for s in names
                       if 3 <= len(s["text"]) <= 32
                       and s["text"][0].isalpha()
                       and s["text"].replace("_", "").replace(" ", "").isalnum()]
        if sound_names:
            print(f"\nSound names ({len(sound_names)}):")
            for name in sound_names[:30]:
                print(f"  {name}")
            if len(sound_names) > 30:
                print(f"  ... and {len(sound_names) - 30} more")


def _print_chunk_tree(chunks, indent=0):
    """Print chunk tree with indentation."""
    prefix = "  " * indent
    for chunk in chunks:
        size_str = f"{chunk['size']:,}"
        print(f"{prefix}[0x{chunk['offset']:06x}] {chunk['type_name']} "
              f"size={size_str}  {chunk['version']}")
        if "children" in chunk:
            _print_chunk_tree(chunk["children"], indent + 1)
