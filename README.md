# Burnout 3: Takedown - Static Recompilation for Windows 11

A project to statically recompile the original Xbox version of **Burnout 3: Takedown** (2004, Criterion Games / EA) into a native Windows 11 x86-64 executable.

## Project Status

**Phase: Early Analysis / Toolchain Development**

## Overview

Static recompilation translates the original Xbox x86 machine code into equivalent C/C++ source code (or directly to x86-64), replacing Xbox kernel/SDK calls with Windows equivalents. Unlike dynamic recompilation (emulation), this produces a native binary that runs at full speed without an emulation layer.

### Why Static Recompilation?

- **Native performance** - no emulation overhead
- **Modern OS integration** - proper Windows 11 support, modern input, display
- **Moddability** - decompiled C code can be understood and modified
- **Preservation** - ensures the game remains playable as Xbox hardware ages

## Target Game Details

| Field | Value |
|-------|-------|
| **Title** | Burnout 3: Takedown |
| **Developer** | Criterion Games |
| **Publisher** | Electronic Arts |
| **Platform** | Xbox (Original) |
| **Engine** | RenderWare (custom fork) |
| **Title ID** | `0x4541005B` (EA-091) |
| **XDK Version** | 5849 |
| **Build Date** | 2004-07-29 |
| **Build Path** | `C:\work\B3\CG1Code\GameProjects\VisualStudio.NET\Burnout3\XboxExternal\` |
| **Region** | North America |

## Architecture

### Xbox Hardware (Source)
- **CPU**: Custom Intel Pentium III (x86, 733 MHz)
- **GPU**: NVidia NV2A (custom GeForce 3/4 derivative)
- **RAM**: 64 MB unified
- **Graphics API**: Direct3D 8 (Xbox variant)
- **Audio**: NVidia MCPX (AC97 + custom DSP)

### Windows 11 Target
- **CPU**: x86-64 (backward compatible with x86 source)
- **Graphics API**: Direct3D 11 or Vulkan
- **Audio**: XAudio2 / WASAPI
- **Input**: XInput / DirectInput

## XBE Analysis Summary

The main executable (`default.xbe`) contains:

- **17 sections** totaling 7.41 MB virtual address space
- **2.73 MB of game code** in `.text` section
- **147 Xbox kernel imports** to be replaced
- **11 statically-linked XDK libraries** (D3D8, DSOUND, XMV, XONLINE, XNET, etc.)
- **Entry point**: `0x001D2807` (retail, XOR-decoded)
- **Base address**: `0x00010000`

### Section Map

| Section | Size | Purpose |
|---------|------|---------|
| `.text` | 2.73 MB | Game code + CRT |
| `XMV` | 159 KB | Xbox Media Video decoder |
| `DSOUND` | 51 KB | DirectSound (Xbox) |
| `WMADEC` | 103 KB | WMA audio decoder |
| `XONLINE` | 121 KB | Xbox Live |
| `XNET` | 76 KB | Xbox networking |
| `D3D` | 81 KB | Direct3D 8 (LTCG) |
| `XGRPH` | 8 KB | Xbox graphics helpers |
| `XPP` | 35 KB | Xbox peripheral protocol |
| `.rdata` | 282 KB | Read-only data + kernel thunks |
| `.data` | 3.72 MB | Global data (3.32 MB BSS) |
| `DOLBY` | 28 KB | Dolby audio processing |

### Game Asset Formats

| Extension | Format | Description |
|-----------|--------|-------------|
| `.rws` | RenderWare Stream | 3D models, scenes, worlds |
| `.txd` | RenderWare Texture Dictionary | Textures |
| `.awd` | RenderWare Audio | Sound banks |
| `.xmv` | Xbox Media Video | FMV cutscenes |
| `.xwb` | Xbox Wave Bank | Music/SFX banks |
| `.bum` | Burnout Model | Vehicle/prop meshes |
| `.kfs` | Key Frame Script | Animations |
| `.bin` | Various | Game data (configs, stages, strings) |

## Recompilation Strategy

### Phase 1: Toolchain (Complete)
- [x] XBE header parser
- [x] Disassembler with function detection (20,816 functions, 920K instructions)
- [x] Cross-reference analysis (163,787 xrefs, 315 kernel call sites)
- [x] String extraction (1,988 strings)
- [x] Xbox kernel replacement layer (147/147 imports, builds as static lib)
- [x] Function identification tool (85.3% classified: RW, CRT, vtables, stubs)

### Phase 2: Analysis
- [x] Identify all function boundaries in `.text` (20,816 detected)
- [x] Match CRT/MSVC runtime functions (13 identified via byte signatures)
- [x] Identify RenderWare engine functions (2,758 classified across 67 source modules)
- [x] C++ vtable analysis (121 vtables, 517 virtual methods + constructors)
- [x] Map global variables and data structures (22,587 globals, 1,836 structures)
- [x] Document calling conventions and ABI (76% FPO, 1,182 thiscall methods)

### Phase 3: Core Recompilation
- [x] Replace Xbox kernel calls with Win32 equivalents (all 147)
- [x] Replace D3D8 (Xbox) with D3D11 (skeleton: 63 API entry points mapped)
- [x] Replace DirectSound (Xbox) with XAudio2 (skeleton: 40 entry points)
- [x] Replace Xbox input with XInput (functional: controller state + vibration)
- [x] Handle memory layout differences (VirtualAlloc maps data sections to Xbox VAs)

### Phase 4: Asset Pipeline
- [x] Asset catalog and format identification (716 files, 16 categories)
- [x] Criterion TXD texture parser (409 textures across 2 dictionaries)
- [x] Texture format converter (DXT1/DXT3/DXT5/P8 → PNG, Xbox unswizzle)
- [x] Criterion arena (.rws) chunk parser (track/scene structure)
- [x] Criterion audio (.awd) format analyzer
- [ ] Audio bank converter (Xbox ADPCM → standard PCM/Vorbis)
- [ ] Video player replacement (XMV → standard format)

### Phase 5: Integration & Testing
- [ ] Link recompiled code into Windows executable
- [ ] Asset loading and rendering pipeline
- [ ] Input mapping (Xbox controller → PC gamepad/keyboard)
- [ ] Performance profiling and optimization
- [ ] Gameplay testing and bug fixes

## Project Structure

```
burnout3/
├── README.md                 # This file
├── docs/                     # Detailed documentation
│   ├── xbe-analysis.md       # Full XBE header/section analysis
│   ├── kernel-imports.md     # Xbox kernel → Win32 mapping
│   ├── renderware.md         # RenderWare engine notes
│   └── asset-formats.md      # Game file format documentation
├── tools/                    # Recompilation toolchain
│   ├── xbe_parser/           # XBE file parser
│   ├── disasm/               # Disassembly and analysis
│   └── asset_tools/          # Asset conversion utilities
├── src/                      # Recompiled/reimplemented source
│   ├── kernel/               # Xbox kernel replacement (Win32)
│   ├── d3d/                  # Graphics abstraction layer
│   ├── audio/                # Audio system
│   ├── input/                # Input system
│   └── game/                 # Decompiled game code
└── Burnout 3 Takedown/       # Original game files (gitignored)
```

## Building

*Build instructions will be added as the toolchain matures.*

### Prerequisites
- Windows 11 (target platform)
- Visual Studio 2022 or MSYS2/MinGW-w64
- Python 3.10+ (for toolchain scripts)
- CMake 3.20+

## Legal Notice

This project is for educational and preservation purposes. You must own a legitimate copy of Burnout 3: Takedown for Xbox to use this project. Original game assets are not included in this repository.

## References

- [XBE File Format](https://xboxdevwiki.net/Xbe) - Xbox Dev Wiki
- [Xbox Kernel Exports](https://xboxdevwiki.net/Kernel) - Xbox Dev Wiki
- [RenderWare](https://gtamods.com/wiki/RenderWare) - GTA Modding Wiki (RenderWare documentation)
- [NV2A GPU](https://xboxdevwiki.net/NV2A) - Xbox GPU documentation
- [Xbox Architecture](https://www.copetti.org/writings/consoles/xbox/) - Copetti's console architecture analysis
