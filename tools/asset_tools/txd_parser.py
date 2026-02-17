"""
Criterion Texture Dictionary (.txd) parser for Burnout 3: Takedown.

The .txd format is Criterion's custom texture dictionary, NOT standard
RenderWare TXD (which uses chunk-based format with type 0x0016).

File structure:
  [0x00] Header (16 bytes)
    - uint32 magic       (0x543C0000)
    - uint32 checksum    (varies)
    - uint32 unk_flags   (0xBF or 0xDA)
    - uint32 unk_count   (0x10)

  [0x10] Table of Contents - array of 16-byte entries:
    - uint32 index       (1-based texture index)
    - uint32 pad         (always 0)
    - uint32 offset      (byte offset to texture entry from file start)
    - uint32 pad         (always 0)

  [offset] Texture Entry (128-byte header + pixel data):
    +0x00: uint16 flags           (0x0001)
    +0x02: uint16 unk             (0x0004)
    +0x04: uint32 header_size     (0x80 = 128)
    +0x08: uint32 zero
    +0x0C: uint32 gpu_tex_desc    (Xbox NV2A texture descriptor)
        byte 1: Xbox D3DFORMAT code (0x0C=DXT1, 0x0F=DXT5, 0x06=A8R8G8B8)
        byte 2 high nibble: log2(width)
        byte 3: log2(height)
    +0x10-0x2F: reserved (zeros)
    +0x30: uint32 zero
    +0x34: uint32 format          (Xbox D3DFORMAT code, matches byte 1 of gpu_tex_desc)
    +0x38: uint32 width           (pixels)
    +0x3C: uint32 height          (pixels)
    +0x40: uint32 stride_or_depth (32 typically)
    +0x44: uint32 zero
    +0x48: char[24] name          (null-terminated, zero-padded)
    +0x60-0x67: reserved
    +0x68: uint32 mip_count       (1 for base only)
    +0x6C-0x7F: reserved

    [+0x80] Pixel data (DXT compressed or raw ARGB, Xbox-swizzled)
"""

import struct
from . import formats


TEXTURE_HEADER_SIZE = 0x80  # 128 bytes


def parse_txd(data, verbose=False):
    """Parse a Criterion .txd file and return texture metadata.

    Args:
        data: Raw file bytes.
        verbose: Print progress info.

    Returns:
        dict with 'header' and 'textures' keys.
    """
    if len(data) < 0x20:
        raise ValueError("File too small to be a TXD")

    # Parse header
    magic, checksum, unk_flags, unk_count = struct.unpack_from("<4I", data, 0)
    if magic != 0x543C0000:
        raise ValueError(f"Not a Criterion TXD (magic=0x{magic:08x})")

    header = {
        "magic": f"0x{magic:08x}",
        "checksum": f"0x{checksum:08x}",
        "unk_flags": f"0x{unk_flags:02x}",
        "unk_count": unk_count,
    }

    if verbose:
        print(f"TXD header: magic={header['magic']} checksum={header['checksum']} "
              f"flags={header['unk_flags']} count={unk_count}")

    # Parse TOC entries
    toc_entries = []
    toc_off = 0x10
    while toc_off + 16 <= len(data):
        idx, pad1, offset, pad2 = struct.unpack_from("<4I", data, toc_off)
        if idx == 0 or idx > 10000:
            break
        if offset == 0 or offset >= len(data):
            break
        toc_entries.append((idx, offset))
        toc_off += 16

    if verbose:
        print(f"Found {len(toc_entries)} TOC entries")

    # Parse each texture entry
    textures = []
    for i, (idx, offset) in enumerate(toc_entries):
        if offset + TEXTURE_HEADER_SIZE > len(data):
            continue

        tex = _parse_texture_entry(data, offset, idx)
        if tex:
            # Calculate data size from next entry or end of file
            if i + 1 < len(toc_entries):
                next_offset = toc_entries[i + 1][1]
                tex["data_size_actual"] = next_offset - offset - TEXTURE_HEADER_SIZE
            else:
                tex["data_size_actual"] = len(data) - offset - TEXTURE_HEADER_SIZE

            textures.append(tex)

    if verbose:
        print(f"Parsed {len(textures)} textures")
        _print_texture_summary(textures)

    return {"header": header, "textures": textures}


def _parse_texture_entry(data, offset, index):
    """Parse a single texture entry at the given file offset."""
    # Read header fields
    flags, unk2, header_size, zero1, gpu_desc = struct.unpack_from(
        "<HHIII", data, offset
    )

    # Extract format from gpu_desc byte 1
    fmt_from_desc = (gpu_desc >> 8) & 0xFF
    log2_w_from_desc = (gpu_desc >> 20) & 0xF
    log2_h_from_desc = (gpu_desc >> 24) & 0xFF

    # Read explicit fields
    _, fmt_explicit, width, height, stride = struct.unpack_from(
        "<5I", data, offset + 0x30
    )

    # Read texture name (null-terminated string at +0x48)
    name_bytes = data[offset + 0x48 : offset + 0x60]
    name_end = name_bytes.find(b"\x00")
    if name_end >= 0:
        name = name_bytes[:name_end].decode("ascii", errors="replace")
    else:
        name = name_bytes.decode("ascii", errors="replace")

    # Read mip count
    mip_count = struct.unpack_from("<I", data, offset + 0x68)[0]
    if mip_count == 0:
        mip_count = 1

    # Use explicit format if available, fall back to gpu_desc
    fmt_code = fmt_explicit if fmt_explicit != 0 else fmt_from_desc

    # Calculate expected data size
    expected_size = formats.texture_data_size(width, height, fmt_code, mip_count)

    return {
        "index": index,
        "name": name,
        "offset": f"0x{offset:06x}",
        "width": width,
        "height": height,
        "format_code": fmt_code,
        "format_name": formats.xbox_fmt_name(fmt_code),
        "bpp": formats.xbox_fmt_bpp(fmt_code),
        "compressed": formats.xbox_fmt_is_compressed(fmt_code),
        "mip_count": mip_count,
        "stride": stride,
        "gpu_desc": f"0x{gpu_desc:08x}",
        "data_offset": f"0x{offset + TEXTURE_HEADER_SIZE:06x}",
        "data_size_expected": expected_size,
    }


def extract_texture_data(data, tex_info):
    """Extract raw pixel data for a texture entry.

    Returns bytes of the raw (still Xbox-format) pixel data.
    """
    data_off = int(tex_info["data_offset"], 16)
    size = tex_info.get("data_size_actual", tex_info["data_size_expected"])
    if data_off + size > len(data):
        size = len(data) - data_off
    return data[data_off : data_off + size]


def _print_texture_summary(textures):
    """Print a formatted summary of parsed textures."""
    print(f"\n{'Idx':>4} {'Name':<24} {'Size':>10} {'Format':<16} {'Mips':>4} {'DataSize':>10}")
    print("-" * 78)
    for tex in textures:
        dim = f"{tex['width']}x{tex['height']}"
        print(
            f"{tex['index']:4d} {tex['name']:<24} {dim:>10} "
            f"{tex['format_name']:<16} {tex['mip_count']:4d} "
            f"{tex['data_size_expected']:10d}"
        )

    # Format distribution
    fmt_counts = {}
    for tex in textures:
        fn = tex["format_name"]
        fmt_counts[fn] = fmt_counts.get(fn, 0) + 1
    print(f"\nFormat distribution:")
    for fn, count in sorted(fmt_counts.items(), key=lambda x: -x[1]):
        print(f"  {fn}: {count}")
