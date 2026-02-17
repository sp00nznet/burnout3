"""
Configuration constants for the function identification tool.

Defines section address ranges, CRT byte-level signatures, and
confidence thresholds for each identification method.
"""

# ============================================================
# Section Address Ranges (from XBE analysis)
# ============================================================

# .text section: game code + CRT + RenderWare engine
TEXT_VA_START = 0x00011000
TEXT_VA_SIZE = 2863616
TEXT_VA_END = TEXT_VA_START + TEXT_VA_SIZE  # 0x002BD000
TEXT_RAW_ADDR = 0x00001000

# .rdata section: read-only data (strings, vtables, constants)
RDATA_VA_START = 0x0036B7C0
RDATA_VA_SIZE = 289684
RDATA_VA_END = RDATA_VA_START + RDATA_VA_SIZE  # 0x003B2394
RDATA_RAW_ADDR = 0x0035C000

# .data section
DATA_VA_START = 0x003B2360
DATA_VA_SIZE = 3904988
DATA_VA_END = DATA_VA_START + DATA_VA_SIZE
DATA_RAW_ADDR = 0x003A3000

# XBE base address
XBE_BASE_ADDRESS = 0x00010000

# ============================================================
# VA-to-file-offset helpers
# ============================================================

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


def va_to_file_offset(va):
    """Convert a virtual address to a file offset in the XBE."""
    for name, sec_va, sec_size, sec_raw in SECTIONS:
        if sec_va <= va < sec_va + sec_size:
            return va - sec_va + sec_raw
    return None


# ============================================================
# CRT Byte Signatures
# ============================================================
# Each entry: (name, pattern_bytes, mask_bytes_or_None, max_func_size)
# mask: 0xFF = must match, 0x00 = wildcard
# max_func_size: upper bound on expected function size (0 = no check)

CRT_SIGNATURES = [
    # memcpy - push ebp; mov ebp,esp; push edi; push esi; mov esi,[ebp+0C]; mov edi,[ebp+08]; mov ecx,[ebp+10]
    ("memcpy",
     bytes([0x55, 0x8B, 0xEC, 0x57, 0x56, 0x8B, 0x75, 0x0C, 0x8B, 0x7D, 0x08, 0x8B, 0x4D, 0x10]),
     None, 512),

    # memset - push ebp; mov ebp,esp; push edi; mov edi,[ebp+08]; mov eax,[ebp+0C]; mov ecx,[ebp+10]
    ("memset",
     bytes([0x55, 0x8B, 0xEC, 0x57, 0x8B, 0x7D, 0x08, 0x8B, 0x45, 0x0C, 0x8B, 0x4D, 0x10]),
     None, 256),

    # strlen - mov ecx,[esp+04]; test ecx,03
    ("strlen",
     bytes([0x8B, 0x4C, 0x24, 0x04, 0xF7, 0xC1, 0x03, 0x00, 0x00, 0x00]),
     None, 256),

    # strcmp - push esi; mov esi,[esp+08]; push edi; mov edi,[esp+10]
    ("strcmp",
     bytes([0x56, 0x8B, 0x74, 0x24, 0x08, 0x57, 0x8B, 0x7C, 0x24, 0x10]),
     None, 256),

    # strncmp - push edi; push esi; mov edi,[esp+10]; mov esi,[esp+0C]; mov ecx,[esp+14]
    ("strncmp",
     bytes([0x57, 0x56, 0x8B, 0x7C, 0x24, 0x10, 0x8B, 0x74, 0x24, 0x0C, 0x8B, 0x4C, 0x24, 0x14]),
     None, 256),

    # strcpy - push esi; mov esi,[esp+0C]; push edi; mov edi,[esp+0C]
    ("strcpy",
     bytes([0x56, 0x8B, 0x74, 0x24, 0x0C, 0x57, 0x8B, 0x7C, 0x24, 0x0C]),
     None, 256),

    # _ftol - MSVC float-to-long: fnstcw [esp-2]; ...
    ("_ftol",
     bytes([0xD9, 0x44, 0x24, 0x00]),
     bytes([0xFF, 0xFF, 0xFF, 0x00]),
     64),

    # _ftol2 - push ebp; mov ebp,esp; sub esp,20; and esp,F0
    ("_ftol2",
     bytes([0x55, 0x8B, 0xEC, 0x83, 0xEC, 0x20, 0x83, 0xE4, 0xF0]),
     None, 128),

    # _chkstk - push ecx; cmp eax,1000
    ("_chkstk",
     bytes([0x51, 0x3D, 0x00, 0x10, 0x00, 0x00]),
     None, 64),

    # _alloca_probe - push ecx; cmp eax,1000  (same as _chkstk on Xbox)
    ("_alloca_probe",
     bytes([0x51, 0x3D, 0x00, 0x10, 0x00, 0x00]),
     None, 64),

    # _allmul - 64-bit multiply: mov eax,[esp+08]; mov ecx,[esp+10]
    ("_allmul",
     bytes([0x8B, 0x44, 0x24, 0x08, 0x8B, 0x4C, 0x24, 0x10]),
     None, 128),

    # _alldiv - 64-bit divide: push edi; push esi; push ebx
    ("_alldiv",
     bytes([0x57, 0x56, 0x53, 0x33, 0xFF, 0x8B, 0x44, 0x24, 0x14]),
     None, 256),

    # _allrem - 64-bit remainder: push edi; push esi; push ebx
    ("_allrem",
     bytes([0x57, 0x56, 0x53, 0x33, 0xFF, 0x8B, 0x4C, 0x24, 0x18]),
     None, 256),

    # _aullshr - 64-bit unsigned shift right: cmp cl,40
    ("_aullshr",
     bytes([0x80, 0xF9, 0x40, 0x73]),
     None, 48),

    # _allshr - 64-bit signed shift right: cmp cl,40
    ("_allshr",
     bytes([0x80, 0xF9, 0x40, 0x73]),
     None, 48),

    # _allshl - 64-bit shift left: cmp cl,40
    ("_allshl",
     bytes([0x80, 0xF9, 0x40, 0x73]),
     None, 48),

    # _purecall - push ... ; call ... (typically just calls an error handler)
    # push 0x19 (STATUS_NOT_IMPLEMENTED); call RtlRaiseStatus
    ("_purecall",
     bytes([0x6A, 0x19]),
     None, 16),

    # memmove - push ebp; mov ebp,esp; push edi; push esi; mov esi,[ebp+0C]; mov ecx,[ebp+10]; mov edi,[ebp+08]
    ("memmove",
     bytes([0x55, 0x8B, 0xEC, 0x57, 0x56, 0x8B, 0x75, 0x0C, 0x8B, 0x4D, 0x10, 0x8B, 0x7D, 0x08]),
     None, 512),

    # memcmp - push ebp; mov ebp,esp; push esi; push edi; mov esi,[ebp+08]; mov edi,[ebp+0C]; mov ecx,[ebp+10]
    ("memcmp",
     bytes([0x55, 0x8B, 0xEC, 0x56, 0x57, 0x8B, 0x75, 0x08, 0x8B, 0x7D, 0x0C, 0x8B, 0x4D, 0x10]),
     None, 256),

    # strcat - mov ecx,[esp+04]; push edi; test ecx,03
    ("strcat",
     bytes([0x8B, 0x4C, 0x24, 0x04, 0x57, 0xF7, 0xC1, 0x03, 0x00, 0x00, 0x00]),
     None, 256),

    # _CIcos - fcos; ret
    ("_CIcos",
     bytes([0xD9, 0xFF, 0xC3]),
     None, 8),

    # _CIsin - fsin; ret
    ("_CIsin",
     bytes([0xD9, 0xFE, 0xC3]),
     None, 8),

    # _CIsqrt - fsqrt; ret
    ("_CIsqrt",
     bytes([0xD9, 0xFA, 0xC3]),
     None, 8),

    # _fmod - MSVC fmod: push ebp; mov ebp,esp; sub esp,0C
    ("fmod",
     bytes([0x55, 0x8B, 0xEC, 0x83, 0xEC, 0x0C]),
     None, 256),

    # sprintf / _sprintf - push ebp; mov ebp,esp; lea eax,[ebp+10]
    ("sprintf",
     bytes([0x55, 0x8B, 0xEC, 0x8D, 0x45, 0x10]),
     None, 128),
]

# ============================================================
# Confidence Thresholds
# ============================================================

CONFIDENCE_RW_STRING_REF = 0.95    # Function directly references an RW ID string
CONFIDENCE_RW_ZONE = 0.85          # Function references data in an RW .rdata zone
CONFIDENCE_CRT_SIGNATURE = 0.90    # CRT byte-pattern match
CONFIDENCE_CLUSTER_CALL = 0.75     # Label propagation via call graph
CONFIDENCE_CLUSTER_PROXIMITY = 0.65  # Label propagation via address proximity

# Maximum address gap for proximity clustering
PROXIMITY_GAP = 0x1000

# Maximum clustering iterations
MAX_CLUSTER_ITERATIONS = 10

# ============================================================
# RenderWare Module Categories
# ============================================================

RW_CATEGORIES = {
    "src/plcore/":       "rw_plcore",
    "src/pipe/p2/xbox/": "rw_pipe_xbox",
    "src/pipe/p2/":      "rw_pipe",
    "driver/xbox/":      "rw_driver_xbox",
    "driver/common/":    "rw_driver_common",
    "os/xbox/":          "rw_os_xbox",
    "world/pipe/p2/xbox/": "rw_world_pipe_xbox",
    "world/pipe/p2/":    "rw_world_pipe",
    "world/":            "rw_world",
    "src/":              "rw_core",
}

# ============================================================
# Game Sub-classification Keywords
# ============================================================

GAME_SUBCATEGORIES = {
    "vehicle":  ["vehicle", "car ", "wheel", "engine", "gear", "throttle", "brake",
                 "steer", "turbo", "boost", "rpm", "speed", "crash", "takedown",
                 "jolt", "wreck", "drift"],
    "audio":    ["sound", "audio", "music", "sfx", "voice", "wma", "dsp",
                 "volume", "pitch", "reverb", "impulse", "speaker", "channel"],
    "camera":   ["camera", "cam_", "look back", "bumper", "follow"],
    "physics":  ["collid", "collisi", "physics", "rigid", "force", "velocity",
                 "mass", "angular", "gravity", "friction", "momentum"],
    "ui":       ["menu", "hud", "font", "screen", "button", "press", "select",
                 "gamertag", "profile"],
    "network":  ["online", "xbox live", "session", "host", "join", "rank",
                 "sign in", "sign out", "matchmak"],
    "io":       ["save", "load", "partition", "device\\", "\\tdata", "content",
                 "savemeta", "saveimage"],
    "render":   ["render", "draw", "shader", "vertex", "texture", "material",
                 "d3d", "pixel"],
    "debug":    ["assert", "error", "warning", "debug"],
}

# ============================================================
# Default Paths
# ============================================================

DEFAULT_FUNCTIONS_JSON = "tools/disasm/output/functions.json"
DEFAULT_STRINGS_JSON = "tools/disasm/output/strings.json"
DEFAULT_XREFS_JSON = "tools/disasm/output/xrefs.json"
DEFAULT_OUTPUT_DIR = "tools/func_id/output"
