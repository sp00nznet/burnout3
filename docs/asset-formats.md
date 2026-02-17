# Burnout 3: Takedown - Asset Format Documentation

Reverse-engineered file format specifications for all asset types in
the Xbox version of Burnout 3: Takedown (2004, Criterion Games).

**Key finding:** Criterion's Burnout 3 does NOT use standard RenderWare binary
streams. All major formats (TXD, RWS, AWD) are Criterion's custom formats built
on top of the RenderWare chunk infrastructure but with different chunk IDs,
layouts, and data organization.

## Asset Inventory

| Category | Count | Total Size | Extensions |
|----------|-------|------------|------------|
| Track data | 111 | 718 MB | .dat |
| Scene/Arena | 111 | 692 MB | .rws |
| Wave banks | 33 | 372 MB | .xwb |
| Video | 109 | 317 MB | .xmv |
| Game data | 36 | 64 MB | .bgd |
| Executable | 3 | 62 MB | .xbe |
| Vehicle HD | 67 | 43 MB | .hwd |
| Vehicle geom | 67 | 35 MB | .bgv |
| Audio | 40 | 27 MB | .awd |
| Vehicle tex | 40 | 11 MB | .btv |
| Vehicle LD | 67 | 9 MB | .lwd |
| Textures | 2 | 8 MB | .txd |
| Binary data | 9 | 4 MB | .bin |
| Burnout models | 17 | 731 KB | .bum |
| **Total** | **716** | **2.4 GB** | |

---

## Criterion Texture Dictionary (.txd)

**Status: Fully reverse-engineered. Parser and converter implemented.**
**Tool:** `py -3 -m tools.asset_tools txd <file>` / `extract <file>`

NOT standard RenderWare TXD (which uses chunk type 0x001C). Criterion's custom
format with a flat index table and compact texture entries.

### Header (16 bytes)
```
+0x00: uint32 magic        = 0x543C0000
+0x04: uint32 checksum     (varies per file)
+0x08: uint32 flags        (0xBF or 0xDA)
+0x0C: uint32 unk_count    (always 0x10)
```

### Table of Contents (16 bytes per entry)
```
+0x00: uint32 index        (1-based sequential)
+0x04: uint32 pad          (always 0)
+0x08: uint32 offset       (byte offset to texture entry from file start)
+0x0C: uint32 pad          (always 0)
```

### Texture Entry (128-byte header + pixel data)
```
+0x00: uint16 flags        (0x0001)
+0x02: uint16 unk          (0x0004)
+0x04: uint32 header_size  (0x80 = 128, always)
+0x08: uint32 zero
+0x0C: uint32 gpu_tex_desc (Xbox NV2A texture descriptor)
    byte 0: unknown flags (0x29 common)
    byte 1: Xbox D3DFORMAT code (0x0C, 0x0F, 0x0B, etc.)
    byte 2 high nibble: log2(width)
    byte 3: log2(height)
+0x10-0x2F: reserved (zeros)
+0x30: uint32 zero
+0x34: uint32 format       (Xbox D3DFORMAT code, same as byte 1 above)
+0x38: uint32 width        (pixels)
+0x3C: uint32 height       (pixels)
+0x40: uint32 stride       (32 typical)
+0x44: uint32 zero
+0x48: char[24] name       (null-terminated, zero-padded)
+0x60-0x67: reserved
+0x68: uint32 mip_flags    (bits 0-7: mip count, bits 8+: format flags)
+0x6C-0x7F: reserved
```

### Pixel Data Layout

**DXT-compressed textures (0x0C, 0x0E, 0x0F):**
Data follows immediately after the 128-byte header.
Standard DXT1/DXT3/DXT5 block encoding. NOT swizzled (DXT handles tiling).

**P8 palettized textures (0x0B):**
```
[header + 0x80]     w*h bytes of 8-bit Morton-swizzled indices
[+ w*h]             64 bytes metadata/padding
[+ w*h + 64]        256 * 4 bytes BGRA palette (1024 bytes)
```

### Xbox D3D Format Codes (NV2A GPU)
| Code | Format | BPP | Type |
|------|--------|-----|------|
| 0x00 | L8 | 8 | Luminance |
| 0x02 | A1R5G5B5 | 16 | 16-bit ARGB |
| 0x04 | A4R4G4B4 | 16 | 16-bit ARGB |
| 0x05 | R5G6B5 | 16 | 16-bit RGB |
| 0x06 | A8R8G8B8 | 32 | 32-bit ARGB (swizzled) |
| 0x07 | X8R8G8B8 | 32 | 32-bit RGB (swizzled) |
| 0x0B | P8 | 8 | 256-color palettized (swizzled) |
| 0x0C | DXT1 | 4 | BC1 compressed |
| 0x0E | DXT3 | 8 | BC2 compressed |
| 0x0F | DXT5 | 8 | BC3 compressed |
| 0x12 | LIN_A8R8G8B8 | 32 | 32-bit ARGB (linear/non-swizzled) |
| 0x1E | LIN_X8R8G8B8 | 32 | 32-bit RGB (linear) |

### Texture Statistics
- **Global.txd**: 191 textures (101 DXT1, 85 DXT5, 5 P8)
  - HUD elements, boost effects, particle FX, vehicle shadows
- **Frontend.txd**: 218 textures (59 DXT1, 63 DXT5, 96 P8)
  - Menu graphics, car photos, track previews, world map

---

## Criterion Arena (.rws)

**Status: Chunk structure identified. Data section is dense binary (possibly compressed).**
**Tool:** `py -3 -m tools.asset_tools arena <file>`

Used for track scenes and crash mode arenas. Uses Criterion's custom chunk types
(0x080D/0x080E/0x080F) rather than standard RenderWare world/clump chunks.

### Chunk Structure
```
crARENA (0x080D) - top-level container
  crARENA_HEADER (0x080E) - structured header (~2012 bytes)
    Contains: scene name, segment descriptors, sub-scene names,
    configuration metadata, Xbox memory addresses
  crARENA_DATA (0x080F) - packed scene data (7+ MB)
    Dense binary, high entropy, no embedded RW chunks found
    Likely contains: geometry, materials, textures, collision
```

### RenderWare Version
All arena files use RW 3.7.0.2 build 9, consistent with XDK 5849.

### Arena Header Contents
```
Strings found in typical header:
  - Scene name: "crash1", "_femain"
  - Segments: "Segment0"
  - Sub-scenes: "GenCrash01" (general), "SloCrash01" (slow-motion)
  - Metadata: "name", "id", "data", "uint", "notes", "true"
```

### Files
- 21 arena files in `Tracks/` (crash1-crash20.rws + _femain.rws)
- 90 track-specific arenas (CRASH1/2/3.RWS per variant directory)
- Crash mode arenas are consistently 7.4 MB each

---

## Criterion Audio (.awd)

**Status: Partially reverse-engineered. Sound names and PCM data identified.**
**Tool:** `py -3 -m tools.asset_tools audio <file>`

Custom audio container. Starts with Criterion chunk type 0x0809.

### Header
```
+0x00: uint32 magic        = 0x00000809 (crAUDIO)
+0x04: uint32 unk          = 0
+0x08: uint32 buffer_size  = 0x2000 (8192 bytes typical)
+0x0C: uint32 entry_count  (number of sound effects)
+0x14: uint32 data_offset  (absolute offset to PCM data section)
```

### Sound Entry Structure (~132-136 bytes, variable)
Entries follow the header with variable-length names (4-byte aligned):
```
+0x00: char[] name         (null-terminated, padded to 4 bytes)
+var:  16 bytes GUID/hash
+var:  uint32 data_size    (PCM data size in bytes)
+var:  uint32 sample_rate  (32000 Hz typical)
+var:  uint32 channels     (1 = mono)
+var:  ... additional params (format, Xbox memory addresses)
```

### Audio Data
Data section contains **16-bit signed PCM** (not compressed ADPCM).
Located at the `data_offset` specified in the header.

### Sound Names (Fe.awd, 44 effects)
```
Beep, GlobeHigh, GSweep, GTurn, MenuIn, MenuOut, RoadShow,
Vertical, Zoom, Zoomout, Select, Back, IconHiLite, IconPulse,
Resolve, TextMed, kbmove, kbdel, kbselect, Mailgot, PlayerJoin,
PlayerLeft, gamemade, joinedgame, gamestart, gameshut, SFXTestL,
SFXTestR, staticlp, p_intro_R, p_intro_L, p_count, p_total,
p_swsh2, p_swsh1, p_cash, p_multi, ding, trophy, swoosh,
buddy_neg, buddy_pos, chat, tick_box, paint2, paint1, new_car,
take1, take2, take3, take4
```

---

## Xbox Wave Bank (.xwb)

**Status: Standard XACT format. Can be parsed by existing tools.**

Standard Microsoft XACT Wave Bank format from the Xbox SDK.

### Header
```
+0x00: char[4] magic       = "WBND"
+0x04: uint32 version      = 3 (Xbox original SDK)
+0x08: Segment descriptors (5 regions):
    - Bank data (metadata, name)
    - Entry metadata (per-wave descriptors)
    - Seek tables (for streaming)
    - Entry names (optional)
    - Wave data (raw audio samples)
```

### Audio Codecs
- Xbox ADPCM (IMA ADPCM variant with Xbox-specific block layout)
- WMA (decoded by WMADEC section in XBE)
- PCM (for small SFX)

### Files
| File | Size | Content |
|------|------|---------|
| _EATrax0.xwb | 82 MB | Licensed music bank 0 |
| _EATrax1.xwb | 83 MB | Licensed music bank 1 |
| movie.xwb | 31 MB | Video audio tracks |
| E_DJ*.xwb | 0.6-5.6 MB | Regional DJ audio banks |
| E_DJRACE.xwb | 1.9-10 MB | Per-track DJ commentary |

---

## Xbox Media Video (.xmv)

**Status: Header structure identified. WMV-based compression.**

Xbox-specific video container format using WMV2/VC-1 video codec
and WMA audio.

### Header
```
+0x00: uint32 packet_size  (12288 to 147456, repeated 3 times)
+0x0C: char[4] magic       = "xobX" (Xbox spelled backwards)
+0x10: uint32 version      = 4
+0x14: uint32 width        (192 to 640 pixels)
+0x18: uint32 height       (144 to 480 pixels)
+0x1C: uint32 frame_count
```

### Video Properties
| File | Resolution | Frames | Size | Description |
|------|-----------|--------|------|-------------|
| Titles30.xmv | 640x480 | 57K | 28 MB | Intro sequence (30fps) |
| credits.xmv | 640x352 | 60K | 11 MB | End credits |
| _FEMain.xmv | - | - | 11 MB | Frontend background |
| Alpine1.xmv | 192x144 | 9.6K | 480 KB | Track preview |

---

## Track Data Files

### static.dat
Pre-baked static geometry with Xbox memory addresses in the BSS range.
First uint32 appears to be a section/element count.

### streamed.dat
Streaming geometry data with spatial bounding volumes:
```
+0x00: uint32 count
Per entry (32 bytes):
  +0x00: uint32[4] header (offsets, flags, data pointers)
  +0x10: float[4]  bounding volume (x, y, z, radius in world coords)
```

### enviro.dat
Environment/lighting parameters. Starts directly with float values:
```
float ambient_light_r, g, b
float fog_start, fog_end
... (additional lighting/atmosphere params)
```

### Gamedata.bgd
Game configuration data. First byte is 0x09 followed by what appears
to be compressed or encoded data.

---

## Vehicle Files (pveh/)

8 vehicle classes: COMP, CUPE, HEVY, HSPC, MSCL, SPRT, SUPR, TSPC

### .bgv (Burnout Geometry Vehicle)
```
+0x00: uint32 magic = 0x17 (23)
+0x04: geometry data (vertices, indices, materials)
```
500-580 KB per vehicle.

### .hwd / .lwd (High/Low Detail Meshes)
Start with Criterion audio magic (0x0809), sharing the container format.
HD: 500-820 KB, LD: 120-140 KB.

### .btv (Burnout Texture Variant)
Vehicle texture variants (paint jobs). ~250-280 KB each.

### .bum (Burnout Model)
All 17 files exactly 42 KB. Numbered 0-16. Located in Graphics/.
Likely standardized effect/damage model data.

---

## Directory Organization

```
Burnout 3 Takedown/
├── Data/
│   ├── Global.txd           # 191 gameplay textures (1.4 MB)
│   ├── Frontend.txd         # 218 menu/UI textures (6.7 MB)
│   ├── vdb.xml              # Vehicle database (binary despite .xml name)
│   ├── Globalus.bin         # US locale strings (201 KB)
│   ├── stagehed.bin         # Stage definitions (366 KB)
│   ├── PrgData.bin          # Progression data (18 KB)
│   └── LoadScrn.bin         # Loading screen images (3.3 MB)
├── Tracks/
│   ├── crash1-20.rws        # 20 crash mode arenas (7.4 MB each)
│   ├── _femain.rws          # Frontend 3D scene
│   ├── _EATrax0/1.xwb       # Music (166 MB total)
│   ├── E_DJ*.xwb            # DJ audio banks
│   └── [REGION]/[TRACK]_V[1-2]/
│       ├── CRASH1-3.RWS     # Track-specific arenas
│       ├── static.dat       # Static geometry
│       ├── streamed.dat     # Streaming geometry
│       ├── enviro.dat       # Environment params
│       ├── Gamedata.bgd     # Game configuration
│       ├── SOUND.AWD        # Track audio
│       └── E_DJRACE.xwb     # Track DJ audio
├── sound/
│   ├── Fe.awd               # 44 frontend SFX
│   ├── Generic.awd          # Generic game SFX
│   └── *.awd                # Mode-specific audio
├── ovid/
│   ├── credits.xmv          # End credits (640x352)
│   ├── Titles*.xmv          # Intro sequences (640x480)
│   └── *.xmv                # Cutscenes, unlocks
├── pveh/
│   └── [TYPE]/Car*.{bgv,hwd,lwd,btv}
└── Graphics/
    └── 0-16.bum             # Effect models (42 KB each)
```

### Region/Track Codes
- **AS** = Asia (championships C1-C3, main event M1)
- **EU** = Europe (C1-C4, M1-M2, P1-P2)
- **US** = United States (C1-C3, C5, M1, P1-P2)
- **V1/V2** = Track variant (forward/reverse routes)

---

## Texture Swizzling (Xbox → PC)

Xbox GPU (NV2A) uses Morton/Z-order curve swizzling for non-compressed
textures. DXT textures are NOT swizzled. Only P8, A8R8G8B8, R5G6B5,
and other raw pixel formats need unswizzling.

```python
def unswizzle(data, width, height, bytes_per_pixel):
    output = bytearray(len(data))
    for y in range(height):
        for x in range(width):
            morton = morton_encode(x, y)
            src = morton * bytes_per_pixel
            dst = (y * width + x) * bytes_per_pixel
            output[dst:dst + bytes_per_pixel] = data[src:src + bytes_per_pixel]
    return bytes(output)

def morton_encode(x, y):
    result = 0
    for i in range(16):
        result |= ((x >> i) & 1) << (2 * i)
        result |= ((y >> i) & 1) << (2 * i + 1)
    return result
```

## Recompilation Priority

1. **Textures (.txd)** - DONE: 409 textures extracted as PNG
2. **Track arenas (.rws)** - Chunk structure known, data format needs more RE
3. **Audio (.awd)** - PCM data accessible, need full entry parser
4. **Wave banks (.xwb)** - Standard XACT format, tools exist
5. **Videos (.xmv)** - Can be converted or skipped initially
6. **Track data (.dat/.bgd)** - Need game code analysis to understand fully
7. **Vehicle files** - Need game code analysis for format details
