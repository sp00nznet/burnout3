/**
 * Burnout 3: Takedown - Criterion BGV Vehicle Model Loader
 *
 * Parses .bgv vehicle geometry files from the pveh/COMP directory.
 * Extracts vertex positions, normals, UVs and triangle indices from the
 * highest-detail LOD section, producing D3D8-ready vertex/index data.
 *
 * BGV Format (confirmed by analysis of Car1.bgv):
 *   - File header: magic 0x17, bounding radius at +0x14
 *   - 4 LOD section offsets at +0x4C..+0x58 (last = highest detail)
 *   - Each section: 128-byte header, then data block at +0x80
 *   - Vertex stride 24: float3 pos (12B) + packed normal (4B) + float2 UV (8B)
 *   - Packed normal: Xbox D3DVSDT_NORMPACKED3 (11-11-10 bit signed XYZ)
 *   - Index data: 16-bit triangle strips with degenerate restart markers
 *   - Draw calls extracted from sub-entry descriptors
 */

#ifndef BGV_LOADER_H
#define BGV_LOADER_H

#include "platform/xbox_winnt.h"
#include <stdint.h>
#include "../d3d/d3d8_xbox.h"

/* Vertex format matching D3DFVF_XYZ | D3DFVF_NORMAL | D3DFVF_DIFFUSE | D3DFVF_TEX1 */
typedef struct {
    float x, y, z;      /* position */
    float nx, ny, nz;   /* unpacked normal */
    DWORD color;         /* diffuse color (0xFFFFFFFF = white) */
    float u, v;          /* texture coords */
} BGV_Vertex;            /* 36 bytes */

/* Parsed model data ready for D3D8 rendering */
typedef struct {
    BGV_Vertex *vertices;
    uint32_t vertex_count;
    uint16_t *indices;       /* triangle list (expanded from strips) */
    uint32_t index_count;
    float bounding_radius;
} BGV_Model;

/**
 * Load a BGV vehicle model from file at a specific LOD level.
 * Converts triangle strips to triangle lists.
 *
 * @param path  Path to .bgv file
 * @param model Output model (caller-allocated, members set on success)
 * @param lod   LOD level: 0=lowest, 1=low, 2=medium, 3=highest detail
 * @return      0 on success, -1 on error
 */
int bgv_load_lod(const char *path, BGV_Model *model, int lod);

/**
 * Load a BGV vehicle model from file (highest detail LOD 3).
 */
int bgv_load(const char *path, BGV_Model *model);

/**
 * Free all memory allocated by bgv_load().
 */
void bgv_free(BGV_Model *model);

/**
 * Apply a color tint to all vertices of a loaded model.
 * Multiplies existing vertex colors by (r,g,b)/255.
 */
void bgv_tint(BGV_Model *model, uint8_t r, uint8_t g, uint8_t b);

#endif /* BGV_LOADER_H */
