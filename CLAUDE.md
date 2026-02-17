# Burnout 3: Takedown - Static Recompilation Project

## Project Overview
Static recompilation of Xbox Burnout 3: Takedown (2004) for Windows 11.
The goal is to translate the original x86 Xbox code into a native Windows executable.

## Key Facts
- **XBE**: `default.xbe` is a retail Xbox executable, XDK 5849, base address 0x00010000
- **Entry Point**: 0x001D2807 (retail, XOR-decoded with 0xA8FC57AB)
- **Engine**: Criterion's custom RenderWare fork (~3.7), statically linked
- **Code Size**: 2.73 MB in .text section (game + CRT + RW engine)
- **Kernel Imports**: 147 Xbox kernel functions to replace with Win32
- **Libraries**: 11 statically linked XDK libs (D3D8LTCG, DSOUND, XMV, XONLINE, etc.)

## Repository Structure
- `docs/` - Detailed analysis and documentation
- `tools/xbe_parser/` - XBE file parser (Python)
- `tools/disasm/` - Disassembly tools (planned)
- `tools/asset_tools/` - Asset conversion (planned)
- `src/kernel/` - Xbox kernel replacement layer (Win32)
- `src/d3d/` - D3D8→D3D11 graphics abstraction
- `src/audio/` - Audio system (DSOUND→XAudio2)
- `src/input/` - Input system (XPP→XInput)
- `src/game/` - Decompiled game code

## Git/GitHub
- Remote: https://github.com/sp00nznet/burnout3.git
- Game assets (`Burnout 3 Takedown/`) are gitignored (large binary files)
- All toolchain code and documentation IS committed

## Conventions
- Python tools use Python 3.10+
- C/C++ code targets MSVC (Visual Studio 2022) or MinGW-w64
- Addresses are always shown as hex with 0x prefix (e.g., 0x001D2807)
- Xbox kernel function names use their original Xbox names with `xbox_` prefix when reimplemented
