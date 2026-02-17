"""
Recompiler configuration - section mappings and constants.
"""

# Section virtual address → file offset mappings
SECTIONS = [
    # (name, va_start, va_size, raw_addr)
    (".text",   0x00011000, 2863616, 0x00001000),
    ("XMV",     0x002CC200, 163124,  0x002BD000),
    ("DSOUND",  0x002F3F40, 52668,   0x002E5000),
    ("WMADEC",  0x00300D00, 105828,  0x002F2000),
    ("XONLINE", 0x0031AA80, 124764,  0x0030C000),
    ("XNET",    0x003391E0, 78056,   0x0032B000),
    ("D3D",     0x0034C2E0, 83828,   0x0033F000),
    ("XGRPH",   0x00360A60, 8300,    0x00350000),
    ("XPP",     0x00362AE0, 36052,   0x00353000),
    (".rdata",  0x0036B7C0, 289684,  0x0035C000),
    (".data",   0x003B2360, 3904988, 0x003A3000),
    ("DOLBY",   0x0076B940, 29056,   0x0040C000),
    ("XON_RD",  0x00772AC0, 5416,    0x00414000),
    (".data1",  0x00774000, 224,     0x00416000),
]

TEXT_VA_START = 0x00011000
TEXT_VA_END = 0x002BD000
RDATA_VA_START = 0x0036B7C0
RDATA_VA_END = 0x003B2394
DATA_VA_START = 0x003B2360
DATA_VA_END = 0x007700A0
KERNEL_THUNK_ADDR = 0x0036B7C0
ENTRY_POINT = 0x001D2807


def va_to_file_offset(va):
    """Convert virtual address to XBE file offset."""
    for _, sec_va, sec_size, sec_raw in SECTIONS:
        if sec_va <= va < sec_va + sec_size:
            return va - sec_va + sec_raw
    return None


def is_code_address(va):
    """Check if VA is in an executable section."""
    return TEXT_VA_START <= va < TEXT_VA_END


def is_data_address(va):
    """Check if VA is in .rdata or .data."""
    return RDATA_VA_START <= va <= DATA_VA_END
