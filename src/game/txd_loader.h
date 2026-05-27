/**
 * Burnout 3: Takedown - Criterion TXD Texture Loader
 *
 * Loads Criterion's custom .txd texture dictionaries (NOT standard RenderWare TXD).
 * Parses the header, TOC, and texture entries, creating D3D8 textures for each.
 *
 * Format:
 *   16-byte header (magic 0x543C0000)
 *   16-byte TOC entries (index, pad, offset, pad) - terminated by index==0
 *   128-byte texture entry headers + pixel data at each offset
 */

#ifndef TXD_LOADER_H
#define TXD_LOADER_H

#include "platform/xbox_winnt.h"
#include <stdint.h>
#include "../d3d/d3d8_xbox.h"

/* Maximum textures per TXD dictionary */
#define TXD_MAX_TEXTURES 512

/* Maximum texture name length (from format: 24 bytes at +0x48) */
#define TXD_NAME_LEN 24

/* Loaded texture entry */
typedef struct {
    char name[TXD_NAME_LEN];
    IDirect3DTexture8 *texture;
    uint32_t width;
    uint32_t height;
    uint32_t format;   /* Xbox D3DFORMAT code */
} TXD_Entry;

/* Loaded texture dictionary */
typedef struct {
    TXD_Entry entries[TXD_MAX_TEXTURES];
    int count;
} TXD_Dict;

/**
 * Load a Criterion TXD file and create D3D8 textures for all entries.
 *
 * @param path      Path to the .txd file (e.g. "Burnout 3 Takedown\\Data\\Global.txd")
 * @param device    D3D8 device to create textures on
 * @param out_dict  Output dictionary (caller-allocated)
 * @return          Number of textures loaded, or -1 on error
 */
int txd_load(const char *path, IDirect3DDevice8 *device, TXD_Dict *out_dict);

/**
 * Look up a texture by name in a loaded dictionary.
 * Name comparison is case-insensitive.
 *
 * @param dict  Loaded TXD dictionary
 * @param name  Texture name to find
 * @return      Pointer to texture, or NULL if not found
 */
IDirect3DTexture8 *txd_find(const TXD_Dict *dict, const char *name);

/**
 * Release all D3D8 textures in a dictionary.
 */
void txd_release(TXD_Dict *dict);

#endif /* TXD_LOADER_H */
