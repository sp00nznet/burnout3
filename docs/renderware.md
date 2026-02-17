# RenderWare Engine Notes

## Overview

Burnout 3: Takedown uses **Criterion Games' custom fork of RenderWare** (~version 3.7). Unlike many RenderWare games where the engine is a separate DLL, Criterion statically linked their RenderWare code directly into the executable.

This means all RenderWare engine code is inside the `.text` section of `default.xbe` and cannot be easily separated from game-specific code.

## RenderWare Architecture

### Core Systems
- **RwCore** - Memory management, plugin system, file I/O
- **RpWorld** - BSP world rendering, sectors, atomic rendering
- **RpGeometry** - Mesh/geometry management
- **RwTexture** - Texture management and loading
- **RwCamera** - Camera/viewport management
- **RwFrame** - Hierarchical transform system (bones, scene graph)
- **RwMaterial** - Material/shader system
- **RpSkin** - Skeletal animation and skinning
- **RpHAnim** - Hierarchical animation
- **RpCollision** - Collision detection

### Criterion's Custom Extensions
Criterion Games was the creator of RenderWare, so their fork includes proprietary extensions:
- **CgFX** - Criterion's shader/effect system
- **CgPhysics** - Custom physics engine (used for crash deformation)
- **CgTraffic** - AI traffic system
- **CgPVS** - Potentially Visible Set (occlusion culling)

## RenderWare Stream File Format (.rws)

### Chunk Structure
Every RenderWare stream file is composed of nested chunks:

```c
struct RwChunkHeader {
    uint32_t type;      // Chunk type ID
    uint32_t size;      // Size of data following this header
    uint32_t version;   // RenderWare version | library stamp
};
```

The version field encodes the RenderWare version:
```
Version 3.7.0.0:
  version = (3 << 16) | (7 << 12) | (0 << 8) | 0 | build_num
  Typically: 0x1803FFFF or similar for 3.7.x
```

### Common Chunk Types

| ID | Name | Description |
|----|------|-------------|
| 0x0001 | rwID_STRUCT | Raw struct data |
| 0x0002 | rwID_STRING | Text string |
| 0x0003 | rwID_EXTENSION | Extension chunk container |
| 0x0005 | rwID_CAMERA | Camera definition |
| 0x0006 | rwID_TEXTURE | Texture reference |
| 0x0007 | rwID_MATERIAL | Material definition |
| 0x0008 | rwID_MATLIST | Material list |
| 0x000E | rwID_FRAMELIST | Frame/bone hierarchy |
| 0x000F | rwID_ATOMIC | Renderable mesh instance |
| 0x0010 | rwID_TEXTURENATIVE | Platform-native texture |
| 0x0014 | rwID_GEOMETRYLIST | Geometry list |
| 0x0015 | rwID_GEOMETRY | Mesh geometry |
| 0x0016 | rwID_TEXDICTIONARY | Texture dictionary (TXD) |
| 0x001A | rwID_WORLD | World/BSP level geometry |
| 0x001C | rwID_CLUMP | Model/scene container |
| 0x001D | rwID_LIGHT | Light definition |

### Extension Chunk Types (Criterion/EA)

| ID | Name | Description |
|----|------|-------------|
| 0x0116 | rwID_BINMESHPLUGIN | Triangle strip/list mesh data |
| 0x011E | rwID_NATIVEDATA | Native platform mesh data |
| 0x0120 | rwID_VERTEXFORMATPLUGIN | Custom vertex format |
| 0x0253F2F3 | rwID_COLLISIONMODEL | Collision mesh |
| 0x0253F2F4 | rwID_EFFECTPLUGIN | Visual effects |
| 0x0253F2F5 | rwID_ENVIRONMENTPLUGIN | Environment settings |

## Xbox-Specific Rendering

### Vertex Buffers
Xbox RenderWare uses NV2A-native vertex buffer formats:
- Vertices are stored in Xbox GPU-native format
- Includes push buffer commands for direct GPU submission
- Will need conversion to D3D11 vertex buffer format

### Texture Formats
Xbox textures in RWS files use NV2A swizzled format:

| Format | Xbox Code | D3D11 Equivalent |
|--------|-----------|-------------------|
| DXT1 | D3DFMT_DXT1 | DXGI_FORMAT_BC1_UNORM |
| DXT3 | D3DFMT_DXT3 | DXGI_FORMAT_BC2_UNORM |
| DXT5 | D3DFMT_DXT5 | DXGI_FORMAT_BC3_UNORM |
| A8R8G8B8 | D3DFMT_A8R8G8B8 | DXGI_FORMAT_B8G8R8A8_UNORM |
| R5G6B5 | D3DFMT_R5G6B5 | DXGI_FORMAT_B5G6R5_UNORM |
| A4R4G4B4 | D3DFMT_A4R4G4B4 | Custom conversion needed |

DXT textures do NOT need de-swizzling (block compression has its own layout).
Only uncompressed formats (A8R8G8B8, R5G6B5, etc.) need Morton-order de-swizzling.

## Recompilation Strategy for RenderWare

### Option A: Re-implement RenderWare API (Recommended)
Write a new RenderWare-compatible API layer that:
1. Implements the same RwStream/RwChunk parsing
2. Loads geometry into D3D11 vertex/index buffers
3. Loads textures into D3D11 shader resource views
4. Implements the same camera/transform math
5. Provides the same plugin system interface

**Pros**: Clean, maintainable, can use modern rendering techniques
**Cons**: Large amount of work, need to match all API surfaces

### Option B: Port RenderWare Xbox Code
Take the decompiled RenderWare code from `.text` and port it:
1. Replace NV2A GPU commands with D3D11 calls
2. Replace Xbox memory management with standard allocators
3. Keep the same internal data structures

**Pros**: Higher compatibility, less reverse engineering needed
**Cons**: Messy code, hard to optimize, carries Xbox-specific baggage

### Option C: Use librw (Open-Source RenderWare Reimplementation)
[librw](https://github.com/aap/librw) is an open-source reimplementation of RenderWare 3.x with multiple backends (OpenGL, D3D9).

**Pros**: Already works, actively maintained, used by re3/reVC
**Cons**: May not support all Criterion-specific extensions, needs D3D11 backend

### Recommended Approach
Start with **Option C** (librw) as a foundation, extending it with Criterion-specific features as needed. This gives us a working renderer fastest while being maintainable long-term.

## Key Files to Analyze

| File | Format | Priority | Description |
|------|--------|----------|-------------|
| `Tracks/crash*.rws` | RWS | High | Crash mode levels |
| `Tracks/_femain.rws` | RWS | High | Frontend 3D scene |
| `Data/Frontend.txd` | TXD | High | UI textures |
| `Data/Global.txd` | TXD | High | Shared textures |
| `Graphics/*.bum` | Custom | Medium | Need format RE |
| `pveh/*/` | Custom | Medium | Vehicle data |

## Related Projects

- [librw](https://github.com/aap/librw) - Open-source RenderWare reimplementation
- [re3](https://github.com/halpz/re3) - GTA III reverse engineering (RenderWare)
- [reVC](https://github.com/halpz/reVC) - GTA Vice City RE
- [rwtools](https://github.com/aap/rwtools) - RenderWare file tools
- [Magic.TXD](https://github.com/quiret/magic-txd) - RenderWare TXD editor
