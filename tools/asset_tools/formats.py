"""
Xbox / Criterion format definitions for Burnout 3: Takedown.

Covers Xbox D3D texture formats, Criterion chunk IDs, and
file format magic numbers found in the game's assets.
"""

import struct

# ── Criterion chunk type IDs ────────────────────────────────────
# These extend the standard RenderWare chunk ID space (0x0800+)

CRITERION_CHUNKS = {
    0x0001: "rwSTRUCT",
    0x0002: "rwSTRING",
    0x0003: "rwEXTENSION",
    0x0006: "rwTEXTURE",
    0x0007: "rwMATERIAL",
    0x0008: "rwMATERIALLIST",
    0x000F: "rwATOMIC",
    0x0010: "rwCLUMP",
    0x0012: "rwTEXTURENATIVE",
    0x0014: "rwFRAMELIST",
    0x0015: "rwGEOMETRY",
    0x0016: "rwWORLD",
    0x001A: "rwGEOMETRYLIST",
    0x001C: "rwTEXDICT",
    0x0023: "rwBINMESH",
    0x0024: "rwCOLLTREE",
    # Criterion custom types
    0x0809: "crAUDIO",
    0x080A: "crAUDIO_HEADER",
    0x080C: "crDATA_0C",
    0x080D: "crARENA",
    0x080E: "crARENA_HEADER",
    0x080F: "crARENA_DATA",
}

# Container chunk types (can have child chunks)
CONTAINER_CHUNKS = {
    0x0008, 0x0010, 0x0014, 0x0016, 0x001A, 0x001C,
    0x0809, 0x080D, 0x080E, 0x080F,
}


# ── Xbox D3D texture format codes (NV2A GPU) ───────────────────

XBOX_D3DFMT = {
    0x00: ("L8", 8, False),            # 8-bit luminance
    0x01: ("AL8", 16, False),           # 8-bit alpha + luminance
    0x02: ("A1R5G5B5", 16, False),      # 16-bit with 1-bit alpha
    0x03: ("X1R5G5B5", 16, False),      # 16-bit no alpha
    0x04: ("A4R4G4B4", 16, False),      # 16-bit with 4-bit alpha
    0x05: ("R5G6B5", 16, False),        # 16-bit RGB
    0x06: ("A8R8G8B8", 32, False),      # 32-bit ARGB (swizzled)
    0x07: ("X8R8G8B8", 32, False),      # 32-bit RGB (swizzled)
    0x0B: ("P8", 8, False),             # 8-bit palettized
    0x0C: ("DXT1", 4, True),            # BC1 compressed
    0x0E: ("DXT3", 8, True),            # BC2 compressed (DXT2 on Xbox)
    0x0F: ("DXT5", 8, True),            # BC3 compressed (DXT4 on Xbox)
    # Linear (non-swizzled) variants
    0x10: ("LIN_A1R5G5B5", 16, False),
    0x11: ("LIN_R5G6B5", 16, False),
    0x12: ("LIN_A8R8G8B8", 32, False),
    0x1E: ("LIN_X8R8G8B8", 32, False),
}


def xbox_fmt_name(fmt_code):
    """Get human-readable name for an Xbox D3D format code."""
    entry = XBOX_D3DFMT.get(fmt_code)
    return entry[0] if entry else f"UNKNOWN_0x{fmt_code:02X}"


def xbox_fmt_bpp(fmt_code):
    """Get bits-per-pixel for an Xbox D3D format (effective for compressed)."""
    entry = XBOX_D3DFMT.get(fmt_code)
    return entry[1] if entry else 0


def xbox_fmt_is_compressed(fmt_code):
    """Check if an Xbox D3D format is block-compressed (DXT)."""
    entry = XBOX_D3DFMT.get(fmt_code)
    return entry[2] if entry else False


def texture_data_size(width, height, fmt_code, mip_count=1):
    """Calculate total texture data size in bytes including mipmaps."""
    entry = XBOX_D3DFMT.get(fmt_code)
    if not entry:
        return 0

    bpp = entry[1]
    compressed = entry[2]
    total = 0

    for mip in range(mip_count):
        w = max(1, width >> mip)
        h = max(1, height >> mip)

        if compressed:
            # DXT: 4x4 blocks, minimum 1 block per dimension
            bw = max(1, (w + 3) // 4)
            bh = max(1, (h + 3) // 4)
            if bpp == 4:  # DXT1: 8 bytes per block
                total += bw * bh * 8
            else:  # DXT3/5: 16 bytes per block
                total += bw * bh * 16
        else:
            total += w * h * (bpp // 8)

    return total


# ── RenderWare version decoding ─────────────────────────────────

def decode_rw_version(ver_stamp):
    """Decode a RenderWare library version stamp.

    Returns (major, minor, revision, build) or None if not a valid stamp.
    """
    if ver_stamp == 0:
        return None

    vhi = (ver_stamp >> 16) & 0xFFFF
    vlo = ver_stamp & 0xFFFF

    if vhi < 0x0800:
        return None

    v_major = ((vhi >> 14) & 0x3) + 3
    v_minor = (vhi >> 10) & 0xF
    v_rev = (vhi >> 6) & 0xF
    v_build = vhi & 0x3F

    return (v_major, v_minor, v_rev, v_build, vlo)


def rw_version_str(ver_stamp):
    """Format a RenderWare version stamp as a human-readable string."""
    v = decode_rw_version(ver_stamp)
    if v is None:
        return f"0x{ver_stamp:08x}"
    return f"RW {v[0]}.{v[1]}.{v[2]}.{v[3]} b{v[4]}"


# ── File magic identification ───────────────────────────────────

FILE_MAGICS = {
    0x0000080D: "Criterion Arena (.rws)",
    0x00000809: "Criterion Audio (.awd/.hwd/.lwd)",
    0x444E4257: "Xbox Wave Bank (.xwb)",  # "WBND"
}


def identify_file(data):
    """Identify file type from its first 4 bytes."""
    if len(data) < 4:
        return "unknown (too small)"
    magic = struct.unpack_from("<I", data, 0)[0]

    if magic in FILE_MAGICS:
        return FILE_MAGICS[magic]

    # Check for TXD (custom header with 0x543c0000)
    if magic == 0x543C0000:
        return "Criterion Texture Dictionary (.txd)"

    # Check for BGV (Burnout Geometry Vehicle)
    if magic == 0x00000017:
        return "Burnout Geometry Vehicle (.bgv)"

    # Check first bytes as float (enviro.dat starts with floats)
    try:
        f = struct.unpack_from("<f", data, 0)[0]
        if 0.01 < abs(f) < 10000.0 and len(data) >= 16:
            f2 = struct.unpack_from("<f", data, 4)[0]
            if 0.01 < abs(f2) < 10000.0:
                return "Float data (possibly enviro.dat)"
    except struct.error:
        pass

    return f"unknown (magic=0x{magic:08x})"
