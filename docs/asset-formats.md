# Burnout 3: Takedown - Asset Format Documentation

## Overview

Burnout 3 uses Criterion Games' custom fork of the **RenderWare** engine (version ~3.7) for rendering, plus EA-standard formats for audio and video. The Xbox version uses Xbox-specific texture formats and audio codecs.

## Directory Structure

```
Burnout 3 Takedown/
├── default.xbe          # Main executable
├── update.xbe           # Game update
├── dashupdate.xbe       # Xbox dashboard update (not relevant)
├── Data/                # Game configuration and UI data
│   ├── Frontend.txd     # Frontend UI textures (RenderWare TXD)
│   ├── Global.txd       # Global textures
│   ├── Globalus.bin     # US locale strings/data
│   ├── Headus.bin       # Header data (US)
│   ├── LoadScrn.bin     # Loading screen data
│   ├── PrgData.bin      # Program/progression data
│   ├── stagehed.bin     # Stage header definitions
│   ├── vdb.xml          # Vehicle database (XML!)
│   ├── Intro.kfs        # Intro animation keyframes
│   ├── After.kfs        # Post-race animation keyframes
│   └── EALogin.ico      # EA login icon
├── Graphics/            # Render data
│   └── *.bum            # Burnout Model files (0-16)
├── ovid/                # Video (FMV) files
│   ├── *.xmv            # Xbox Media Video files
│   └── movie.xwb        # Video audio bank
├── pveh/                # Vehicle data
│   ├── COMP/            # Compact class
│   ├── CUPE/            # Coupe class
│   ├── HEVY/            # Heavy class
│   ├── HSPC/            # Hot Special class
│   ├── MSCL/            # Muscle class
│   ├── SPRT/            # Sport class
│   ├── SUPR/            # Super class
│   ├── TSPC/            # Special class
│   └── vlist.bin        # Vehicle list index
├── sound/               # Audio
│   ├── *.awd            # RenderWare Audio Wave Data files
│   └── DSP/             # DSP effect data
└── Tracks/              # Track/level data
    ├── _EATrax0.xwb     # EA Trax music bank 1 (~82 MB)
    ├── _EATrax1.xwb     # EA Trax music bank 2 (~83 MB)
    ├── _femain.rws       # Frontend main (RenderWare Stream)
    ├── _FEMain.xmv       # Frontend video
    ├── crash*.rws        # Crash mode levels (RenderWare)
    ├── AS/               # Additional stage data
    └── *.xmv            # Track preview videos
```

## File Formats

### RenderWare Stream (.rws)

RenderWare's container format for 3D scenes, models, and worlds.

**Structure**: Chunk-based format with nested chunks.
```
Each chunk:
  uint32 chunk_type    // Type ID (see below)
  uint32 chunk_size    // Size of data (not including header)
  uint32 rw_version    // RenderWare version stamp
  byte[] data          // Chunk data (may contain sub-chunks)
```

**Key Chunk Types**:
| Type ID | Name | Description |
|---------|------|-------------|
| 0x0001 | Struct | Raw data container |
| 0x0002 | String | Text string |
| 0x0003 | Extension | Extension data |
| 0x0006 | Texture | Texture reference |
| 0x0007 | Material | Material definition |
| 0x0008 | Material List | List of materials |
| 0x000F | Atomic | Renderable object instance |
| 0x0010 | Texture Native | Platform-specific texture |
| 0x0014 | Geometry List | List of geometries |
| 0x0015 | Geometry | Mesh geometry (vertices, triangles) |
| 0x001A | World | BSP world/level geometry |
| 0x001C | Clump | Model container (hierarchy + geometry) |
| 0x001E | Frame List | Bone/transform hierarchy |
| 0x0253F2F3 | Collision Model | Collision mesh data |

**Xbox-Specific Texture Format (Chunk 0x0010)**:
Xbox textures inside RWS use **swizzled** (Morton order) pixel layout for GPU cache efficiency. Need to be de-swizzled when loading on PC.

Common Xbox texture formats:
- DXT1 (BC1) - opaque/1-bit alpha compressed
- DXT3 (BC2) - explicit alpha compressed
- DXT5 (BC3) - interpolated alpha compressed
- A8R8G8B8 - 32-bit ARGB (swizzled)
- R5G6B5 - 16-bit RGB (swizzled)
- A4R4G4B4 - 16-bit ARGB (swizzled)

**Recompilation Strategy**: Use an existing RenderWare parser library or write one. The PC version of RenderWare uses the same chunk structure but different native texture formats. Key task is texture de-swizzling.

### RenderWare Texture Dictionary (.txd)

A RenderWare stream file specifically containing texture data.

**Structure**: Same chunk format as .rws, root chunk is type 0x0016 (Texture Dictionary).

Contains:
- Texture count
- Array of Native Texture chunks (0x0015)
  - Each has platform-specific image data
  - Xbox: swizzled + possibly compressed (DXT)

**Files**:
- `Frontend.txd` (6.7 MB) - All frontend/menu UI textures
- `Global.txd` (1.4 MB) - Shared textures (HUD, common elements)

### Burnout Model (.bum)

Custom Criterion/Burnout format for additional mesh data (likely bump maps, damage models, or LOD geometry).

**Observations**:
- 17 files (0.bum through 16.bum)
- All exactly 43,008 bytes (42 KB each)
- Uniform size suggests fixed-layout data (lookup tables, palettes, or fixed-size mesh buffers)

**Needs Investigation**: Dump and compare binary structure.

### Key Frame Script (.kfs)

Animation data for cinematic sequences.

**Files**:
- `Intro.kfs` (200 KB) - Intro sequence camera/object animation
- `After.kfs` (200 KB) - Post-race celebration animation

### Xbox Media Video (.xmv)

Xbox-proprietary video format. Contains WMV video + WMA audio.

**Key Videos**:
| File | Size | Description |
|------|------|-------------|
| credits.xmv | 10.9 MB | End credits |
| CrshHd01-10.xmv | 2-3 MB each | Crash headliner videos |
| SigTkd01-08.xmv | 2-3 MB each | Signature takedown videos |
| _FEMain.xmv | 10.6 MB | Frontend background video |

**Recompilation Strategy**: Convert to standard format (MP4/WebM) or use FFmpeg to decode at runtime.

### Xbox Wave Bank (.xwb)

Xbox audio bank format (part of XACT - Xbox Audio Creation Tool).

**Files**:
- `_EATrax0.xwb` (82 MB) - EA Trax licensed music, disc 1
- `_EATrax1.xwb` (83 MB) - EA Trax licensed music, disc 2
- `movie.xwb` (30 MB) - Video audio tracks

**Structure**:
```
Header:
  char[4] magic        // "WBND"
  uint32  version
  uint32  segment_count
  // Segment offsets and sizes

Segments:
  - Bank data (metadata)
  - Entry metadata (per-sound info)
  - Seek tables (for streaming)
  - Entry names (optional)
  - Wave data (raw audio)
```

Audio codecs used:
- Xbox ADPCM (IMA variant, Xbox-specific byte layout)
- WMA (via WMADEC section in XBE)
- PCM (rare, used for small SFX)

**Recompilation Strategy**: Extract audio data and convert Xbox ADPCM to standard PCM or Vorbis. XAudio2 on Windows can play standard formats.

### RenderWare Audio Wave Data (.awd)

Criterion's audio system format (built on top of RenderWare Audio).

**Files**:
| File | Size | Description |
|------|------|-------------|
| Fe.awd | 3.0 MB | Frontend sounds |
| Generic.awd | 1.6 MB | Generic game sounds |
| crash.awd | 620 KB | Crash mode sounds |
| crashmod.awd | 2.1 MB | Crash modifier sounds |
| elim.awd | 700 KB | Elimination mode sounds |
| roadrage.awd | 700 KB | Road Rage mode sounds |
| Single.awd | 690 KB | Single race sounds |
| g_crsh*.awd | 1.6 MB each | Crash junction sounds |
| Ident*.awd | 52 KB each | Vehicle identification sounds |

### Vehicle Database (vdb.xml)

**This is golden** - an actual XML file with structured vehicle data. This will be invaluable for understanding the game's vehicle system.

**Location**: `Data/vdb.xml` (70 KB)

### Binary Data Files (.bin)

| File | Size | Likely Contents |
|------|------|-----------------|
| Globalus.bin | 201 KB | US English strings/localization |
| Headus.bin | 342 B | US header/version info |
| PrgData.bin | 18 KB | Progression data (unlock requirements, etc.) |
| stagehed.bin | 366 KB | Stage/track header definitions |
| LoadScrn.bin | 3.3 MB | Loading screen images |
| vlist.bin | 4 KB | Vehicle list/index |

## Texture Swizzling (Xbox → PC)

Xbox GPU (NV2A) uses Morton-order (Z-order curve) swizzling for non-compressed textures. This must be reversed when loading on PC.

**De-swizzle algorithm**:
```python
def deswizzle(data, width, height, bpp):
    """Convert Xbox swizzled texture to linear layout."""
    output = bytearray(len(data))
    bytes_per_pixel = bpp // 8

    for y in range(height):
        for x in range(width):
            # Morton encode
            swizzled_offset = morton_encode(x, y) * bytes_per_pixel
            linear_offset = (y * width + x) * bytes_per_pixel
            output[linear_offset:linear_offset + bytes_per_pixel] = \
                data[swizzled_offset:swizzled_offset + bytes_per_pixel]

    return bytes(output)

def morton_encode(x, y):
    """Interleave bits of x and y for Morton/Z-order encoding."""
    result = 0
    for i in range(16):
        result |= ((x >> i) & 1) << (2 * i)
        result |= ((y >> i) & 1) << (2 * i + 1)
    return result
```

**Note**: DXT-compressed textures on Xbox are stored in standard DXT block order (not swizzled) and can be used directly on PC. Only uncompressed formats need de-swizzling.

## Priority for Recompilation

1. **vdb.xml** - Parse immediately, critical game data in readable format
2. **.rws files** - Track/level geometry, needed for any rendering
3. **.txd files** - Textures, needed for rendering
4. **.awd files** - Audio, needed for gameplay
5. **.xwb files** - Music, needed for full experience
6. **.bum files** - Investigate format, likely needed for vehicles
7. **.xmv files** - Videos, can be replaced/skipped initially
8. **.bin files** - Game logic data, reverse-engineer as needed
