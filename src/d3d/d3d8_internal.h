/**
 * D3D8 Compatibility Layer - Internal Header
 *
 * Shared types and declarations for the D3D8→D3D11 implementation.
 * Not part of the public API - only included by d3d8_*.c files.
 */

#ifndef BURNOUT3_D3D8_INTERNAL_H
#define BURNOUT3_D3D8_INTERNAL_H

#define COBJMACROS
#include "d3d8_xbox.h"

/* Portable: D3D8 device accessor used by recompiled code on both backends
 * (d3d8_device.c on Windows, d3d8_gl.c on POSIX). */
IDirect3DDevice8 *d3d8_GetDevice(void);

#if defined(_WIN32)
/* === Everything below is the D3D11 backend. The POSIX d3d8_compat
 * library (d3d8_gl.c) implements its own internal state and does not
 * need any of these declarations. === */

#include <d3d11.h>
#include <dxgi.h>

/* ================================================================
 * D3D11 device accessors (implemented in d3d8_device.c)
 * ================================================================ */

IDirect3DDevice8    *d3d8_GetDevice(void);
ID3D11Device        *d3d8_GetD3D11Device(void);
ID3D11DeviceContext *d3d8_GetD3D11Context(void);
IDXGISwapChain      *d3d8_GetSwapChain(void);
ID3D11RenderTargetView *d3d8_GetDefaultRTV(void);
HWND                 d3d8_GetHWND(void);
UINT                 d3d8_GetBackbufferWidth(void);
UINT                 d3d8_GetBackbufferHeight(void);

/* Current render state array accessor */
const DWORD         *d3d8_GetRenderStates(void);
const DWORD         *d3d8_GetTSS(DWORD stage);

/* Transform accessors */
const D3DMATRIX     *d3d8_GetTransform(D3DTRANSFORMSTATETYPE type);

/* Lighting accessors (d3d8_device.c) */
const D3DLIGHT8     *d3d8_GetLight(DWORD index);
BOOL                 d3d8_GetLightEnable(DWORD index);
const D3DMATERIAL8  *d3d8_GetMaterial(void);
UINT                 d3d8_GetNumLights(void);

/* ================================================================
 * Resource wrapper structures
 * ================================================================ */

typedef struct D3D8VertexBuffer {
    IDirect3DVertexBuffer8  iface;      /* COM interface (must be first) */
    LONG                    ref_count;
    ID3D11Buffer           *d3d11_buffer;
    UINT                    size;
    DWORD                   fvf;
    DWORD                   usage;
    BYTE                   *sys_mem;    /* System memory for Lock */
    BOOL                    locked;
    BOOL                    dirty;
} D3D8VertexBuffer;

typedef struct D3D8IndexBuffer {
    IDirect3DIndexBuffer8   iface;
    LONG                    ref_count;
    ID3D11Buffer           *d3d11_buffer;
    UINT                    size;
    D3DFORMAT               format;     /* INDEX16 or INDEX32 */
    DWORD                   usage;
    BYTE                   *sys_mem;
    BOOL                    locked;
    BOOL                    dirty;
} D3D8IndexBuffer;

typedef struct D3D8Texture {
    IDirect3DTexture8       iface;
    LONG                    ref_count;
    ID3D11Texture2D        *d3d11_texture;
    ID3D11ShaderResourceView *srv;
    UINT                    width;
    UINT                    height;
    UINT                    levels;
    D3DFORMAT               d3d8_format;
    DXGI_FORMAT             dxgi_format;
    BYTE                   *sys_mem;    /* Level 0 system memory */
    UINT                    pitch;      /* Row pitch of level 0 */
    BOOL                    locked;
    BOOL                    dirty;
} D3D8Texture;

typedef struct D3D8Surface {
    IDirect3DSurface8       iface;
    LONG                    ref_count;
    ID3D11Texture2D        *d3d11_texture;
    ID3D11RenderTargetView *rtv;
    ID3D11DepthStencilView *dsv;
    UINT                    width;
    UINT                    height;
    D3DFORMAT               format;
} D3D8Surface;

/* ================================================================
 * Format conversion (d3d8_resources.c)
 * ================================================================ */

DXGI_FORMAT d3d8_to_dxgi_format(D3DFORMAT fmt);
UINT        d3d8_format_bpp(D3DFORMAT fmt);
BOOL        d3d8_format_is_compressed(D3DFORMAT fmt);
UINT        d3d8_row_pitch(D3DFORMAT fmt, UINT width);

/* Resource creation (d3d8_resources.c) */
HRESULT d3d8_CreateVertexBufferImpl(UINT Length, DWORD Usage, DWORD FVF, IDirect3DVertexBuffer8 **ppVB);
HRESULT d3d8_CreateIndexBufferImpl(UINT Length, DWORD Usage, D3DFORMAT Format, IDirect3DIndexBuffer8 **ppIB);
HRESULT d3d8_CreateTextureImpl(UINT Width, UINT Height, UINT Levels, DWORD Usage, D3DFORMAT Format, IDirect3DTexture8 **ppTex);

/* ================================================================
 * Shader management (d3d8_shaders.c)
 * ================================================================ */

HRESULT d3d8_shaders_init(void);
void    d3d8_shaders_shutdown(void);

/* Bind shaders + input layout for the given FVF, upload transform CBs */
void    d3d8_shaders_prepare_draw(DWORD fvf);

/* ================================================================
 * NV2A Register Combiner pixel shaders (d3d8_combiners.c)
 * ================================================================ */

#include "d3d8_combiners.h"

/* ================================================================
 * NV2A Programmable Vertex Shaders (d3d8_vsh.c)
 * ================================================================ */

#include "d3d8_vsh.h"

/* ================================================================
 * Render state management (d3d8_states.c)
 * ================================================================ */

HRESULT d3d8_states_init(void);
void    d3d8_states_shutdown(void);

/* Apply current D3D8 render states as D3D11 state objects */
void    d3d8_states_apply(void);

/* Create sampler state from TSS and apply to slot */
void    d3d8_states_apply_sampler(DWORD stage);

#endif /* _WIN32 -- end of D3D11 backend section */

#endif /* BURNOUT3_D3D8_INTERNAL_H */
