# RenderWare 3.6/3.7 struct layouts (render-pipeline reference)

Field offsets for the RenderWare object graph, extracted from the **RenderWare
Graphics 3.7 SDK** public headers (`rwcore.h`, `rwplcore.h`, `rpworld.h`),
resolved for the 32-bit Xbox ABI (pointer = 4, `RwReal` = float = 4, natural
alignment). Burnout 3 links **RW36Active** (confirmed by the `$Id:` tags in both
the retail XBE and the Jun-22-2004 prototype). Core math/object/raster/camera
layouts are byte-stable across RW 3.5-3.7, so these offsets apply to RW36; the
PowerPipe2 types (`RxPipeline`, `RxHeap`) are the same p2 architecture the proto
tags confirm (`p2core.c`, `bapipe.c`).

## Why this matters for the boot

The boot spins in the Xbox render pipeline. The root cause (see
`src/game/recomp/recomp_manual.c`) is that the **Xbox D3D8 cache init
(`xbcache.c`, e.g. `sub_001D5707` / `sub_001D5E82`) is stubbed**, so the RW
raster / camera / pipeline structs are never populated â€” the pipeline then reads
uninitialized fields and gets garbage pointers (`0x8DCC5823`) and bogus alloc
sizes (357MB, 1.4GB). These tables are what a real `xbcache` implementation must
fill in, and what to check the reads against when decoding the garbage.

**Boundary:** the public SDK ships the *generic* `RwRaster` etc. The Xbox
platform stores its D3D8 surface/texture pointers in a per-raster **extension
block** (`rasterExt`) whose layout lives in the Xbox *driver source*
(`rwsdk/driver/xbox/*.c`), not in these public headers. So this reference covers
the core object graph, not the Xbox-private D3D fields â€” those still need to be
recovered from the disassembly of the driver functions themselves.

## Stub points these offsets map onto

| Struct | Fields the pipeline reads (from stub analysis) | Offsets |
|---|---|---|
| `RwRaster` | `cpPixels` (locked pixel ptr), `width`/`height`/`depth`, `stride` | +4, +12/+16/+20, +24 |
| `RwCamera` | `frameBuffer`, `zBuffer` rasters; `viewMatrix`; near/far plane | +96, +100, +32, +128/+132 |
| `RxPipeline` | `nodes` array + `numNodes`; `entryPoint`; the p2 node walk | +8/+4, +40 |
| `RwFrame` | `ltm` (local-to-world matrix) read by `RwFrameGetLTM` @0x1dd280 | +80 |
| `RwResEntry` | `owner`/`ownerRef` â€” instance cache the pipeline allocates | +12/+16 |

`sub_001F5C40` walks a linked list of pipeline entries via `RwLLLink` nodes
(`next`+0 / `prev`+4) embedded at a fixed offset in each pipeline object; the
garbage list head is exactly the `xbcache` output that is currently stubbed.

## Offset tables


### RwV2d — size 8 (0x8)
```
+0    0x000   RwReal x
+4    0x004   RwReal y
```

### RwV3d — size 12 (0xc)
```
+0    0x000   RwReal x
+4    0x004   RwReal y
+8    0x008   RwReal z
```

### RwMatrix — size 64 (0x40)
```
+0    0x000   RwV3d right
+12   0x00c   RwUInt32 flags
+16   0x010   RwV3d up
+28   0x01c   RwUInt32 pad1
+32   0x020   RwV3d at
+44   0x02c   RwUInt32 pad2
+48   0x030   RwV3d pos
+60   0x03c   RwUInt32 pad3
```

### RwSphere — size 16 (0x10)
```
+0    0x000   RwV3d center
+12   0x00c   RwReal radius
```

### RwBBox — size 24 (0x18)
```
+0    0x000   RwV3d sup
+12   0x00c   RwV3d inf
```

### RwObject — size 8 (0x8)
```
+0    0x000   RwUInt8 type
+1    0x001   RwUInt8 subType
+2    0x002   RwUInt8 flags
+3    0x003   RwUInt8 privateFlags
+4    0x004   void * parent
```

### RwLLLink — size 8 (0x8)
```
+0    0x000   RwLLLink * next
+4    0x004   RwLLLink * prev
```

### RwObjectHasFrame — size 20 (0x14)
```
+0    0x000   RwObject object
+8    0x008   RwLLLink lFrame
+16   0x010   RwObjectHasFrameSyncFunction sync
```

### RwResEntry — size 24 (0x18)
```
+0    0x000   RwLLLink link
+8    0x008   RwInt32 size
+12   0x00c   void * owner
+16   0x010   RwResEntry * ownerRef
+20   0x014   RwResEntryDestroyNotify destroyNotify
```

### RwFrame — size 164 (0xa4)
```
+0    0x000   RwObject object
+8    0x008   RwLLLink inDirtyListLink
+16   0x010   RwMatrix modelling
+80   0x050   RwMatrix ltm
+144  0x090   RwLinkList objectList
+152  0x098   RwFrame * child
+156  0x09c   RwFrame * next
+160  0x0a0   RwFrame * root
```

### RwRaster — size 52 (0x34)
```
+0    0x000   RwRaster * parent
+4    0x004   RwUInt8 * cpPixels
+8    0x008   RwUInt8 * palette
+12   0x00c   RwInt32 width
+16   0x010   RwInt32 height
+20   0x014   RwInt32 depth
+24   0x018   RwInt32 stride
+28   0x01c   RwInt16 nOffsetX
+30   0x01e   RwInt16 nOffsetY
+32   0x020   RwUInt8 cType
+33   0x021   RwUInt8 cFlags
+34   0x022   RwUInt8 privateFlags
+35   0x023   RwUInt8 cFormat
+36   0x024   RwUInt8 * originalPixels
+40   0x028   RwInt32 originalWidth
+44   0x02c   RwInt32 originalHeight
+48   0x030   RwInt32 originalStride
```

### RwTexture — size 88 (0x58)
```
+0    0x000   RwRaster * raster
+4    0x004   RwTexDictionary * dict
+8    0x008   RwLLLink lInDictionary
+16   0x010   RwChar name[rwTEXTUREBASENAMELENGTH]
+48   0x030   RwChar mask[rwTEXTUREBASENAMELENGTH]
+80   0x050   RwUInt32 filterAddressing
+84   0x054   RwInt32 refCount
```

### RwCamera — size 388 (0x184)
```
+0    0x000   RwObjectHasFrame object
+20   0x014   RwCameraProjection projectionType
+24   0x018   RwCameraBeginUpdateFunc beginUpdate
+28   0x01c   RwCameraEndUpdateFunc endUpdate
+32   0x020   RwMatrix viewMatrix
+96   0x060   RwRaster * frameBuffer
+100  0x064   RwRaster * zBuffer
+104  0x068   RwV2d viewWindow
+112  0x070   RwV2d recipViewWindow
+120  0x078   RwV2d viewOffset
+128  0x080   RwReal nearPlane
+132  0x084   RwReal farPlane
+136  0x088   RwReal fogPlane
+140  0x08c   RwReal zScale
+144  0x090   RwReal zShift
+148  0x094   RwFrustumPlane frustumPlanes[6]
+268  0x10c   RwBBox frustumBoundBox
+292  0x124   RwV3d frustumCorners[8]
```

### RxPipeline — size 52 (0x34)
```
+0    0x000   RwBool locked
+4    0x004   RwUInt32 numNodes
+8    0x008   RxPipelineNode * nodes
+12   0x00c   RwUInt32 packetNumClusterSlots
+16   0x010   rxEmbeddedPacketState embeddedPacketState
+20   0x014   RxPacket * embeddedPacket
+24   0x018   RwUInt32 numInputRequirements
+28   0x01c   RxPipelineRequiresCluster * inputRequirements
+32   0x020   void * superBlock
+36   0x024   RwUInt32 superBlockSize
+40   0x028   RwUInt32 entryPoint
+44   0x02c   RwUInt32 pluginId
+48   0x030   RwUInt32 pluginData
```

### RxHeap — size 28 (0x1c)
```
+0    0x000   RwUInt32 superBlockSize
+4    0x004   rxHeapSuperBlockDescriptor * head
+8    0x008   rxHeapBlockHeader * headBlock
+12   0x00c   rxHeapFreeBlock * freeBlocks
+16   0x010   RwUInt32 entriesAlloced
+20   0x014   RwUInt32 entriesUsed
+24   0x018   RwBool dirty
```

### RpGeometry — size 96 (0x60)
```
+0    0x000   RwObject object
+8    0x008   RwUInt32 flags
+12   0x00c   RwUInt16 lockedSinceLastInst
+14   0x00e   RwInt16 refCount
+16   0x010   RwInt32 numTriangles
+20   0x014   RwInt32 numVertices
+24   0x018   RwInt32 numMorphTargets
+28   0x01c   RwInt32 numTexCoordSets
+32   0x020   RpMaterialList matList
+44   0x02c   RpTriangle * triangles
+48   0x030   RwRGBA * preLitLum
+52   0x034   RwTexCoords * texCoords[rwMAXTEXTURECOORDS]
+84   0x054   RpMeshHeader * mesh
+88   0x058   RwResEntry * repEntry
+92   0x05c   RpMorphTarget * morphTarget
```

### RpAtomic — size 112 (0x70)
```
+0    0x000   RwObjectHasFrame object
+20   0x014   RwResEntry * repEntry
+24   0x018   RpGeometry * geometry
+28   0x01c   RwSphere boundingSphere
+44   0x02c   RwSphere worldBoundingSphere
+60   0x03c   RpClump * clump
+64   0x040   RwLLLink inClumpLink
+72   0x048   RpAtomicCallBackRender renderCallBack
+76   0x04c   RpInterpolator interpolator
+96   0x060   RwUInt16 renderFrame
+98   0x062   RwUInt16 pad
+100  0x064   RwLinkList llWorldSectorsInAtomic
+108  0x06c   RxPipeline * pipeline
```

### RpWorldSector — size 136 (0x88)
```
+0    0x000   RwInt32 type
+4    0x004   RpTriangle * triangles
+8    0x008   RwV3d * vertices
+12   0x00c   RpVertexNormal * normals
+16   0x010   RwTexCoords * texCoords[rwMAXTEXTURECOORDS]
+48   0x030   RwRGBA * preLitLum
+52   0x034   RwResEntry * repEntry
+56   0x038   RwLinkList collAtomicsInWorldSector
+64   0x040   RwLinkList lightsInWorldSector
+72   0x048   RwBBox boundingBox
+96   0x060   RwBBox tightBoundingBox
+120  0x078   RpMeshHeader * mesh
+124  0x07c   RxPipeline * pipeline
+128  0x080   RwUInt16 matListWindowBase
+130  0x082   RwUInt16 numVertices
+132  0x084   RwUInt16 numTriangles
+134  0x086   RwUInt16 pad
```

> 4-byte-assumed handles/fnptrs: RpAtomicCallBackRender, RwCameraBeginUpdateFunc, RwCameraEndUpdateFunc, RwObjectHasFrameSyncFunction, RwResEntryDestroyNotify
