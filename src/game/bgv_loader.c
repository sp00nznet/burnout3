/**
 * Burnout 3: Takedown - Criterion BGV Vehicle Model Loader
 * See bgv_loader.h for format details.
 */

#include "bgv_loader.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

/* ── Packed normal decoding ─────────────────────────────────── */

static void unpack_normal(uint32_t packed, float *nx, float *ny, float *nz)
{
    int32_t ix = (int32_t)(packed & 0x7FF);
    int32_t iy = (int32_t)((packed >> 11) & 0x7FF);
    int32_t iz = (int32_t)((packed >> 22) & 0x3FF);

    /* Sign-extend from 11-bit and 10-bit */
    if (ix & 0x400) ix |= (int32_t)~0x7FF;
    if (iy & 0x400) iy |= (int32_t)~0x7FF;
    if (iz & 0x200) iz |= (int32_t)~0x3FF;

    *nx = (float)ix / 1023.0f;
    *ny = (float)iy / 1023.0f;
    *nz = (float)iz / 511.0f;
}

/* ── BGV raw vertex format (on-disk, stride 24) ─────────────── */

#pragma pack(push, 1)
typedef struct {
    float px, py, pz;       /* 12 bytes: position */
    uint32_t packed_normal;  /* 4 bytes: D3DVSDT_NORMPACKED3 */
    float u, v;              /* 8 bytes: texture coords */
} BGV_RawVertex;             /* 24 bytes total */
#pragma pack(pop)

/* ── Draw call extraction ───────────────────────────────────── */

/* Maximum draw calls we expect per section */
#define MAX_DRAW_CALLS 128

typedef struct {
    uint32_t byte_offset;   /* byte offset from data block base */
    uint32_t index_count;   /* number of 16-bit indices */
} DrawCall;

/**
 * Scan sub-entry data for draw calls.
 * Draw calls follow the pattern: 1.0f (0x3F800000), 0, byte_offset, index_count
 * within the sub-entry descriptor region.
 */
static int find_draw_calls(const uint8_t *section, uint32_t section_size,
                           DrawCall *out, int max_calls)
{
    int count = 0;

    /* Sub-entry offset table starts at section+0x00 */
    /* Find the range of sub-entry data: first non-zero offset to terminator */
    uint32_t sub_end = 0x80; /* minimum: header is 128 bytes */

    /* Read sub-entry offsets to find the data range */
    int i;
    for (i = 0; i < 20; i++) {
        if ((uint32_t)(i * 4 + 4) > section_size) break;
        uint32_t off;
        memcpy(&off, section + i * 4, 4);
        if (off == 0 && i > 0) break;
        if (off > sub_end) sub_end = off;
    }

    /* Scan through the last sub-entry to find its end */
    /* Sub-entries contain draw call descriptors; scan up to +0x4C (vertex data offset) */
    uint32_t vtx_data_offset = 0;
    if (section_size >= 0x50) {
        memcpy(&vtx_data_offset, section + 0x4C, 4);
    }
    if (vtx_data_offset > 0x80 && vtx_data_offset < section_size) {
        /* The region between header end and vertex data is sub-entries + index data */
        /* Sub-entries end somewhere before the actual data begins */
        /* We'll scan up to the vertex data offset in the data block */
        sub_end = 0x80 + vtx_data_offset;
        if (sub_end > section_size) sub_end = section_size;
    } else {
        sub_end = section_size < 0x2000 ? section_size : 0x2000;
    }

    /* Scan for pattern: 0x3F800000 (1.0f), 0x00000000, offset, count */
    for (i = 0x70; (uint32_t)(i + 16) <= sub_end && count < max_calls; i += 4) {
        uint32_t val;
        memcpy(&val, section + i, 4);
        if (val == 0x3F800000) {
            uint32_t next, off_val, cnt_val;
            memcpy(&next, section + i + 4, 4);
            memcpy(&off_val, section + i + 8, 4);
            memcpy(&cnt_val, section + i + 12, 4);

            if (next == 0 && off_val > 0 && off_val < 0x100000 && cnt_val > 0 && cnt_val < 50000) {
                out[count].byte_offset = off_val;
                out[count].index_count = cnt_val;
                count++;
            }
        }
    }

    return count;
}

/* ── Main loader ────────────────────────────────────────────── */

int bgv_load(const char *path, BGV_Model *model)
{
    return bgv_load_lod(path, model, 3);
}

int bgv_load_lod(const char *path, BGV_Model *model, int lod)
{
    FILE *f = NULL;
    uint8_t *file_data = NULL;
    uint16_t *all_strip_indices = NULL;
    uint16_t *tri_indices = NULL;
    BGV_Vertex *vertices = NULL;
    int result = -1;

    memset(model, 0, sizeof(*model));

    /* Read entire file */
    char norm[MAX_PATH];
    snprintf(norm, sizeof(norm), "%s", path);
    xbox_path_normalize(norm);
    f = fopen(norm, "rb");
    if (!f) {
        fprintf(stderr, "BGV: Cannot open '%s'\n", norm);
        return -1;
    }

    fseek(f, 0, SEEK_END);
    long file_size = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (file_size < 0x60) {
        fprintf(stderr, "BGV: File too small (%ld bytes)\n", file_size);
        fclose(f);
        return -1;
    }

    file_data = (uint8_t *)malloc(file_size);
    if (!file_data) {
        fclose(f);
        return -1;
    }
    fread(file_data, 1, file_size, f);
    fclose(f);
    f = NULL;

    /* Validate magic */
    if (file_data[0] != 0x17) {
        fprintf(stderr, "BGV: Bad magic 0x%02X (expected 0x17)\n", file_data[0]);
        goto cleanup;
    }

    /* Read bounding radius from file header +0x14 */
    float bounding_radius;
    memcpy(&bounding_radius, file_data + 0x14, 4);
    model->bounding_radius = bounding_radius;

    /* Get LOD section offset (4 sections at +0x4C..+0x58, lod 0=lowest, 3=highest) */
    if (lod < 0) lod = 0;
    if (lod > 3) lod = 3;
    uint32_t section_offset;
    memcpy(&section_offset, file_data + 0x4C + lod * 4, 4);
    /* Fall back to higher LODs if requested one is empty */
    if (section_offset == 0 || section_offset >= (uint32_t)file_size) {
        int try_lod;
        for (try_lod = lod + 1; try_lod <= 3; try_lod++) {
            memcpy(&section_offset, file_data + 0x4C + try_lod * 4, 4);
            if (section_offset > 0 && section_offset < (uint32_t)file_size) {
                fprintf(stderr, "BGV: LOD %d unavailable, using LOD %d\n", lod, try_lod);
                break;
            }
        }
    }
    if (section_offset == 0 || section_offset >= (uint32_t)file_size) {
        fprintf(stderr, "BGV: No valid section offset for LOD %d\n", lod);
        goto cleanup;
    }

    uint32_t section_size = (uint32_t)file_size - section_offset;
    uint8_t *section = file_data + section_offset;

    fprintf(stderr, "BGV: LOD %d section at 0x%X, size=%u, bounding_r=%.3f\n",
            lod, section_offset, section_size, bounding_radius);

    /* Read vertex data offset from section header +0x4C */
    uint32_t vtx_data_rel;
    memcpy(&vtx_data_rel, section + 0x4C, 4);

    /* Data block base is section + 0x80 (128-byte header) */
    uint32_t data_base_off = 0x80;

    /* Find draw calls from sub-entry descriptors */
    DrawCall draw_calls[MAX_DRAW_CALLS];
    int num_dc = find_draw_calls(section, section_size, draw_calls, MAX_DRAW_CALLS);
    if (num_dc == 0) {
        fprintf(stderr, "BGV: No draw calls found\n");
        goto cleanup;
    }
    fprintf(stderr, "BGV: Found %d draw calls\n", num_dc);

    /* Determine actual vertex count from max index across all draw calls */
    uint32_t max_index = 0;
    uint32_t total_strip_indices = 0;
    {
        int dc;
        for (dc = 0; dc < num_dc; dc++) {
            uint32_t idx_file_off = section_offset + data_base_off + draw_calls[dc].byte_offset;
            uint32_t cnt = draw_calls[dc].index_count;

            if (idx_file_off + cnt * 2 > (uint32_t)file_size) {
                fprintf(stderr, "BGV: Draw call %d out of bounds (off=0x%X cnt=%u)\n",
                        dc, draw_calls[dc].byte_offset, cnt);
                continue;
            }

            uint32_t j;
            for (j = 0; j < cnt; j++) {
                uint16_t idx;
                memcpy(&idx, file_data + idx_file_off + j * 2, 2);
                if (idx > max_index) max_index = idx;
            }
            total_strip_indices += cnt;
        }
    }

    uint32_t actual_vtx_count = max_index + 1;
    fprintf(stderr, "BGV: Max index=%u, vertex count=%u, strip indices=%u\n",
            max_index, actual_vtx_count, total_strip_indices);

    /* Verify vertex data fits in file */
    uint32_t vtx_file_off = section_offset + vtx_data_rel;
    if (vtx_file_off + actual_vtx_count * 24 > (uint32_t)file_size) {
        fprintf(stderr, "BGV: Vertex data out of bounds (off=0x%X count=%u)\n",
                vtx_data_rel, actual_vtx_count);
        goto cleanup;
    }

    /* Convert raw vertices to BGV_Vertex format */
    vertices = (BGV_Vertex *)malloc(actual_vtx_count * sizeof(BGV_Vertex));
    if (!vertices) goto cleanup;

    {
        uint32_t i;
        for (i = 0; i < actual_vtx_count; i++) {
            BGV_RawVertex raw;
            memcpy(&raw, file_data + vtx_file_off + i * 24, 24);

            vertices[i].x = raw.px;
            vertices[i].y = raw.py;
            vertices[i].z = raw.pz;
            unpack_normal(raw.packed_normal, &vertices[i].nx, &vertices[i].ny, &vertices[i].nz);

            /* Directional lighting: light from upper-right-front.
             * Vertex color = pure white intensity so paint texture provides color.
             * Two-tone lighting: warm (sun) from above-right, cool (sky) fill from left. */
            {
                /* Primary light: warm sun from upper-right-front */
                float lx = 0.4f, ly = 0.7f, lz = -0.6f;
                float dot = vertices[i].nx * lx + vertices[i].ny * ly + vertices[i].nz * lz;
                if (dot < 0.0f) dot = 0.0f;
                /* Secondary fill light: cool sky from upper-left */
                float fx = -0.5f, fy = 0.5f, fz = 0.3f;
                float fdot = vertices[i].nx * fx + vertices[i].ny * fy + vertices[i].nz * fz;
                if (fdot < 0.0f) fdot = 0.0f;
                float ambient = 0.12f;
                float sun = dot * 0.75f;
                float fill = fdot * 0.25f;
                float intensity = ambient + sun + fill;
                if (intensity > 1.0f) intensity = 1.0f;
                uint8_t v = (uint8_t)(255.0f * intensity);
                vertices[i].color = 0xFF000000 | (v << 16) | (v << 8) | v;
            }

            vertices[i].u = raw.u;
            vertices[i].v = raw.v;
        }
    }

    /* Convert triangle strips to triangle list */
    /* First pass: count non-degenerate triangles */
    uint32_t total_triangles = 0;
    {
        int dc;
        for (dc = 0; dc < num_dc; dc++) {
            uint32_t idx_file_off = section_offset + data_base_off + draw_calls[dc].byte_offset;
            uint32_t cnt = draw_calls[dc].index_count;

            if (idx_file_off + cnt * 2 > (uint32_t)file_size || cnt < 3)
                continue;

            uint32_t j;
            for (j = 0; j + 2 < cnt; j++) {
                uint16_t i0, i1, i2;
                memcpy(&i0, file_data + idx_file_off + j * 2, 2);
                memcpy(&i1, file_data + idx_file_off + (j + 1) * 2, 2);
                memcpy(&i2, file_data + idx_file_off + (j + 2) * 2, 2);
                if (i0 != i1 && i1 != i2 && i0 != i2)
                    total_triangles++;
            }
        }
    }

    fprintf(stderr, "BGV: %u triangles after strip expansion\n", total_triangles);

    uint32_t tri_index_count = total_triangles * 3;
    tri_indices = (uint16_t *)malloc(tri_index_count * sizeof(uint16_t));
    if (!tri_indices) goto cleanup;

    /* Second pass: emit triangle list indices */
    {
        uint32_t out_idx = 0;
        int dc;
        for (dc = 0; dc < num_dc; dc++) {
            uint32_t idx_file_off = section_offset + data_base_off + draw_calls[dc].byte_offset;
            uint32_t cnt = draw_calls[dc].index_count;

            if (idx_file_off + cnt * 2 > (uint32_t)file_size || cnt < 3)
                continue;

            uint32_t j;
            for (j = 0; j + 2 < cnt; j++) {
                uint16_t i0, i1, i2;
                memcpy(&i0, file_data + idx_file_off + j * 2, 2);
                memcpy(&i1, file_data + idx_file_off + (j + 1) * 2, 2);
                memcpy(&i2, file_data + idx_file_off + (j + 2) * 2, 2);

                /* Skip degenerate triangles (strip restart markers) */
                if (i0 == i1 || i1 == i2 || i0 == i2)
                    continue;

                /* Triangle strips alternate winding order on odd triangles */
                if (j & 1) {
                    tri_indices[out_idx++] = i0;
                    tri_indices[out_idx++] = i2;
                    tri_indices[out_idx++] = i1;
                } else {
                    tri_indices[out_idx++] = i0;
                    tri_indices[out_idx++] = i1;
                    tri_indices[out_idx++] = i2;
                }
            }
        }
        tri_index_count = out_idx;
    }

    /* Success - transfer ownership */
    model->vertices = vertices;
    model->vertex_count = actual_vtx_count;
    model->indices = tri_indices;
    model->index_count = tri_index_count;
    result = 0;

    fprintf(stderr, "BGV: Loaded '%s': %u verts, %u indices (%u tris)\n",
            path, actual_vtx_count, tri_index_count, tri_index_count / 3);

    /* Don't free vertices/indices on success */
    vertices = NULL;
    tri_indices = NULL;

cleanup:
    free(file_data);
    free(all_strip_indices);
    free(tri_indices);
    free(vertices);
    if (f) fclose(f);
    return result;
}

void bgv_free(BGV_Model *model)
{
    if (model) {
        free(model->vertices);
        free(model->indices);
        memset(model, 0, sizeof(*model));
    }
}

void bgv_tint(BGV_Model *model, uint8_t r, uint8_t g, uint8_t b)
{
    if (!model || !model->vertices) return;
    uint32_t i;
    for (i = 0; i < model->vertex_count; i++) {
        uint32_t c = model->vertices[i].color;
        uint32_t cr = ((c >> 16) & 0xFF) * r / 255;
        uint32_t cg = ((c >> 8) & 0xFF) * g / 255;
        uint32_t cb = (c & 0xFF) * b / 255;
        model->vertices[i].color = 0xFF000000 | (cr << 16) | (cg << 8) | cb;
    }
}
