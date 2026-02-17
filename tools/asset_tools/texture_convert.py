"""
Xbox texture format converter for Burnout 3: Takedown.

Converts Xbox DXT-compressed and swizzled textures to standard formats:
- DXT1 (BC1) → RGBA pixels
- DXT3 (BC2) → RGBA pixels
- DXT5 (BC3) → RGBA pixels
- A8R8G8B8 → RGBA pixels (with unswizzle)
- P8 (palettized) → RGBA pixels

The converter produces raw RGBA pixel data. When pillow (PIL) is available,
it can also write PNG files.

Xbox texture swizzling:
Xbox textures use a Morton/Z-order curve (swizzle) for cache-efficient
GPU access. This must be undone for PC use. DXT-compressed textures
are NOT swizzled (compression handles the tiling).
"""

import struct


def decode_dxt1_block(data, offset):
    """Decode a single DXT1 (BC1) 4x4 block to 16 RGBA pixels."""
    if offset + 8 > len(data):
        return [(0, 0, 0, 255)] * 16

    c0, c1 = struct.unpack_from("<HH", data, offset)
    bits = struct.unpack_from("<I", data, offset + 4)[0]

    # Decode 565 colors
    r0, g0, b0 = _rgb565(c0)
    r1, g1, b1 = _rgb565(c1)

    # Build color table
    colors = [(r0, g0, b0, 255), (r1, g1, b1, 255)]
    if c0 > c1:
        colors.append(((2 * r0 + r1 + 1) // 3, (2 * g0 + g1 + 1) // 3,
                        (2 * b0 + b1 + 1) // 3, 255))
        colors.append(((r0 + 2 * r1 + 1) // 3, (g0 + 2 * g1 + 1) // 3,
                        (b0 + 2 * b1 + 1) // 3, 255))
    else:
        colors.append(((r0 + r1) // 2, (g0 + g1) // 2, (b0 + b1) // 2, 255))
        colors.append((0, 0, 0, 0))  # Transparent

    pixels = []
    for i in range(16):
        idx = (bits >> (i * 2)) & 0x3
        pixels.append(colors[idx])

    return pixels


def decode_dxt5_block(data, offset):
    """Decode a single DXT5 (BC3) 4x4 block to 16 RGBA pixels."""
    if offset + 16 > len(data):
        return [(0, 0, 0, 255)] * 16

    # Alpha block (8 bytes)
    a0 = data[offset]
    a1 = data[offset + 1]

    # Build alpha lookup table
    alphas = [a0, a1]
    if a0 > a1:
        for i in range(1, 7):
            alphas.append(((7 - i) * a0 + i * a1 + 3) // 7)
    else:
        for i in range(1, 5):
            alphas.append(((5 - i) * a0 + i * a1 + 2) // 5)
        alphas.append(0)
        alphas.append(255)

    # Decode 48-bit alpha index table (6 bytes, 3 bits per pixel)
    alpha_bits = 0
    for i in range(6):
        alpha_bits |= data[offset + 2 + i] << (i * 8)

    alpha_values = []
    for i in range(16):
        idx = (alpha_bits >> (i * 3)) & 0x7
        alpha_values.append(alphas[idx])

    # Color block (same as DXT1, at offset +8)
    color_pixels = decode_dxt1_block(data, offset + 8)

    # Combine color + alpha
    pixels = []
    for i in range(16):
        r, g, b, _ = color_pixels[i]
        pixels.append((r, g, b, alpha_values[i]))

    return pixels


def decode_dxt3_block(data, offset):
    """Decode a single DXT3 (BC2) 4x4 block to 16 RGBA pixels."""
    if offset + 16 > len(data):
        return [(0, 0, 0, 255)] * 16

    # Alpha block (8 bytes, 4 bits per pixel)
    alpha_values = []
    for i in range(8):
        byte = data[offset + i]
        alpha_values.append((byte & 0xF) * 17)        # Low nibble
        alpha_values.append(((byte >> 4) & 0xF) * 17)  # High nibble

    # Color block (same as DXT1, at offset +8)
    color_pixels = decode_dxt1_block(data, offset + 8)

    pixels = []
    for i in range(16):
        r, g, b, _ = color_pixels[i]
        pixels.append((r, g, b, alpha_values[i]))

    return pixels


def decode_dxt_texture(data, width, height, fmt_code):
    """Decode a DXT-compressed texture to RGBA pixel array.

    Args:
        data: Raw DXT compressed data.
        width: Texture width in pixels.
        height: Texture height in pixels.
        fmt_code: Xbox D3DFORMAT code (0x0C=DXT1, 0x0E=DXT3, 0x0F=DXT5).

    Returns:
        List of (R, G, B, A) tuples, row-major, top-to-bottom.
    """
    block_w = max(1, (width + 3) // 4)
    block_h = max(1, (height + 3) // 4)

    if fmt_code == 0x0C:
        block_size = 8
        decode_fn = decode_dxt1_block
    elif fmt_code == 0x0E:
        block_size = 16
        decode_fn = decode_dxt3_block
    elif fmt_code == 0x0F:
        block_size = 16
        decode_fn = decode_dxt5_block
    else:
        raise ValueError(f"Unsupported DXT format: 0x{fmt_code:02X}")

    # Initialize output
    pixels = [(0, 0, 0, 255)] * (width * height)

    offset = 0
    for by in range(block_h):
        for bx in range(block_w):
            block_pixels = decode_fn(data, offset)
            offset += block_size

            # Write 4x4 block to output
            for py in range(4):
                for px in range(4):
                    out_x = bx * 4 + px
                    out_y = by * 4 + py
                    if out_x < width and out_y < height:
                        pixels[out_y * width + out_x] = block_pixels[py * 4 + px]

    return pixels


def unswizzle_texture(data, width, height, bpp):
    """Unswizzle Xbox texture data using Morton/Z-order curve.

    Xbox uses swizzled textures for cache-efficient GPU access.
    This function converts them back to linear row-major order.

    Args:
        data: Swizzled pixel data.
        width: Texture width.
        height: Texture height.
        bpp: Bytes per pixel (1, 2, or 4).

    Returns:
        Unswizzled pixel data as bytes.
    """
    output = bytearray(width * height * bpp)

    for y in range(height):
        for x in range(width):
            # Morton encode (x, y) → swizzled index
            swizzled = _morton_encode(x, y)
            src_offset = swizzled * bpp
            dst_offset = (y * width + x) * bpp

            if src_offset + bpp <= len(data) and dst_offset + bpp <= len(output):
                output[dst_offset:dst_offset + bpp] = data[src_offset:src_offset + bpp]

    return bytes(output)


def decode_argb_texture(data, width, height, fmt_code, swizzled=True):
    """Decode an uncompressed Xbox texture to RGBA pixels.

    Args:
        data: Raw pixel data (possibly swizzled).
        width: Texture width.
        height: Texture height.
        fmt_code: Xbox D3DFORMAT code.
        swizzled: Whether data is Xbox-swizzled (True for standard formats).

    Returns:
        List of (R, G, B, A) tuples.
    """
    bpp_map = {
        0x06: 4,  # A8R8G8B8
        0x07: 4,  # X8R8G8B8
        0x05: 2,  # R5G6B5
        0x02: 2,  # A1R5G5B5
        0x04: 2,  # A4R4G4B4
        0x00: 1,  # L8
        0x12: 4,  # LIN_A8R8G8B8 (not swizzled)
        0x1E: 4,  # LIN_X8R8G8B8 (not swizzled)
        0x11: 2,  # LIN_R5G6B5 (not swizzled)
    }

    bpp = bpp_map.get(fmt_code, 0)
    if bpp == 0:
        raise ValueError(f"Unsupported uncompressed format: 0x{fmt_code:02X}")

    # Linear formats are not swizzled
    if fmt_code >= 0x10:
        swizzled = False

    if swizzled:
        data = unswizzle_texture(data, width, height, bpp)

    pixels = []
    for i in range(width * height):
        offset = i * bpp
        if offset + bpp > len(data):
            pixels.append((0, 0, 0, 255))
            continue

        if fmt_code in (0x06, 0x12):  # A8R8G8B8
            b, g, r, a = data[offset], data[offset + 1], data[offset + 2], data[offset + 3]
            pixels.append((r, g, b, a))
        elif fmt_code in (0x07, 0x1E):  # X8R8G8B8
            b, g, r = data[offset], data[offset + 1], data[offset + 2]
            pixels.append((r, g, b, 255))
        elif fmt_code in (0x05, 0x11):  # R5G6B5
            val = struct.unpack_from("<H", data, offset)[0]
            r, g, b = _rgb565(val)
            pixels.append((r, g, b, 255))
        elif fmt_code == 0x00:  # L8
            l = data[offset]
            pixels.append((l, l, l, 255))
        else:
            pixels.append((0, 0, 0, 255))

    return pixels


def decode_p8_texture(data, width, height, swizzled=True):
    """Decode an Xbox P8 (palettized) texture to RGBA pixels.

    Data layout (Criterion TXD format):
        [0]              w*h bytes of 8-bit swizzled indices
        [w*h]            64-byte metadata/padding
        [w*h + 64]       256 * 4 bytes BGRA palette (1024 bytes)

    Args:
        data: Raw data (indices + gap + palette).
        width: Texture width.
        height: Texture height.
        swizzled: Whether index data is swizzled.

    Returns:
        List of (R, G, B, A) tuples.
    """
    index_size = width * height
    palette_gap = 64
    palette_offset = index_size + palette_gap
    palette_size = 256 * 4  # 1024 bytes

    if len(data) < palette_offset + palette_size:
        # Fall back: try palette immediately after indices
        palette_offset = index_size
        if len(data) < palette_offset + palette_size:
            return [(255, 0, 255, 255)] * (width * height)

    # Parse 256-color BGRA palette
    palette = []
    for i in range(256):
        off = palette_offset + i * 4
        b, g, r, a = data[off], data[off + 1], data[off + 2], data[off + 3]
        palette.append((r, g, b, a))

    # Read index data
    index_data = data[:index_size]
    if len(index_data) < index_size:
        index_data = index_data + b"\x00" * (index_size - len(index_data))

    # Unswizzle if needed
    if swizzled:
        index_data = unswizzle_texture(index_data, width, height, 1)

    # Map indices to palette colors
    pixels = []
    for i in range(width * height):
        idx = index_data[i] if i < len(index_data) else 0
        pixels.append(palette[idx])

    return pixels


def pixels_to_rgba_bytes(pixels, width, height):
    """Convert pixel tuple list to flat RGBA byte array."""
    out = bytearray(width * height * 4)
    for i, (r, g, b, a) in enumerate(pixels):
        off = i * 4
        out[off] = r
        out[off + 1] = g
        out[off + 2] = b
        out[off + 3] = a
    return bytes(out)


def save_png(pixels, width, height, output_path):
    """Save RGBA pixels as a PNG file.

    Requires PIL/Pillow. Falls back to writing raw RGBA if unavailable.
    """
    try:
        from PIL import Image
        img = Image.new("RGBA", (width, height))
        img.putdata(pixels)
        img.save(output_path, "PNG")
        return True
    except ImportError:
        # Fall back to raw RGBA file
        raw_path = output_path.replace(".png", ".rgba")
        rgba_data = pixels_to_rgba_bytes(pixels, width, height)
        with open(raw_path, "wb") as f:
            f.write(struct.pack("<II", width, height))
            f.write(rgba_data)
        print(f"  PIL not available, wrote raw RGBA to {raw_path}")
        return False


# ── Internal helpers ────────────────────────────────────────────

def _rgb565(val):
    """Decode a 16-bit RGB565 color to (R, G, B) 0-255."""
    r = ((val >> 11) & 0x1F) * 255 // 31
    g = ((val >> 5) & 0x3F) * 255 // 63
    b = (val & 0x1F) * 255 // 31
    return r, g, b


def _morton_encode(x, y):
    """Encode (x, y) coordinates using Morton/Z-order curve."""
    result = 0
    for i in range(16):
        result |= ((x >> i) & 1) << (2 * i)
        result |= ((y >> i) & 1) << (2 * i + 1)
    return result
