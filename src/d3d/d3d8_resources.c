/**
 * D3D8 Resource Management - Vertex Buffers, Index Buffers, Textures
 *
 * Implements Xbox D3D8 resource creation and Lock/Unlock using D3D11.
 * Resources use system memory staging with UpdateSubresource on Unlock
 * for maximum compatibility with D3D8 Lock semantics.
 */

#include "d3d8_internal.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* ================================================================
 * Format conversion: Xbox D3DFORMAT → DXGI_FORMAT
 * ================================================================ */

DXGI_FORMAT d3d8_to_dxgi_format(D3DFORMAT fmt)
{
    switch (fmt) {
    case D3DFMT_A8R8G8B8:       return DXGI_FORMAT_B8G8R8A8_UNORM;
    case D3DFMT_X8R8G8B8:       return DXGI_FORMAT_B8G8R8X8_UNORM;
    case D3DFMT_R5G6B5:         return DXGI_FORMAT_B5G6R5_UNORM;
    case D3DFMT_A1R5G5B5:       return DXGI_FORMAT_B5G5R5A1_UNORM;
    case D3DFMT_DXT1:           return DXGI_FORMAT_BC1_UNORM;
    case D3DFMT_DXT3:           return DXGI_FORMAT_BC2_UNORM;
    case D3DFMT_DXT5:           return DXGI_FORMAT_BC3_UNORM;
    case D3DFMT_A8:             return DXGI_FORMAT_A8_UNORM;
    case D3DFMT_L8:             return DXGI_FORMAT_R8_UNORM;
    case D3DFMT_D24S8:          return DXGI_FORMAT_D24_UNORM_S8_UINT;
    case D3DFMT_D16:            return DXGI_FORMAT_D16_UNORM;
    case D3DFMT_LIN_A8R8G8B8:   return DXGI_FORMAT_B8G8R8A8_UNORM;
    case D3DFMT_LIN_X8R8G8B8:   return DXGI_FORMAT_B8G8R8X8_UNORM;
    case D3DFMT_LIN_R5G6B5:     return DXGI_FORMAT_B5G6R5_UNORM;
    case D3DFMT_LIN_A1R5G5B5:   return DXGI_FORMAT_B5G5R5A1_UNORM;
    case D3DFMT_INDEX16:        return DXGI_FORMAT_R16_UINT;
    case D3DFMT_INDEX32:        return DXGI_FORMAT_R32_UINT;
    default:
        fprintf(stderr, "D3D8: Unknown format 0x%X, using R8G8B8A8\n", fmt);
        return DXGI_FORMAT_R8G8B8A8_UNORM;
    }
}

UINT d3d8_format_bpp(D3DFORMAT fmt)
{
    switch (fmt) {
    case D3DFMT_A8R8G8B8:
    case D3DFMT_X8R8G8B8:
    case D3DFMT_LIN_A8R8G8B8:
    case D3DFMT_LIN_X8R8G8B8:
    case D3DFMT_INDEX32:
        return 32;
    case D3DFMT_R5G6B5:
    case D3DFMT_A1R5G5B5:
    case D3DFMT_A4R4G4B4:
    case D3DFMT_LIN_R5G6B5:
    case D3DFMT_LIN_A1R5G5B5:
    case D3DFMT_LIN_A4R4G4B4:
    case D3DFMT_D16:
    case D3DFMT_INDEX16:
        return 16;
    case D3DFMT_A8:
    case D3DFMT_L8:
    case D3DFMT_P8:
        return 8;
    case D3DFMT_DXT1:  return 4;   /* 4 bits per pixel (BC1) */
    case D3DFMT_DXT3:
    case D3DFMT_DXT5:  return 8;   /* 8 bits per pixel (BC2/BC3) */
    case D3DFMT_D24S8: return 32;
    default: return 32;
    }
}

BOOL d3d8_format_is_compressed(D3DFORMAT fmt)
{
    return fmt == D3DFMT_DXT1 || fmt == D3DFMT_DXT3 || fmt == D3DFMT_DXT5;
}

UINT d3d8_row_pitch(D3DFORMAT fmt, UINT width)
{
    if (d3d8_format_is_compressed(fmt)) {
        UINT block_width = (width + 3) / 4;
        UINT block_bytes = (fmt == D3DFMT_DXT1) ? 8 : 16;
        return block_width * block_bytes;
    }
    return (width * d3d8_format_bpp(fmt)) / 8;
}

/* ================================================================
 * Vertex Buffer Implementation
 * ================================================================ */

static D3D8VertexBuffer *vb_from_iface(IDirect3DVertexBuffer8 *iface)
{
    return (D3D8VertexBuffer *)iface;
}

static HRESULT __stdcall vb_QueryInterface(IDirect3DVertexBuffer8 *self, const IID *riid, void **ppv)
{
    (void)self; (void)riid; (void)ppv;
    return E_NOINTERFACE;
}

static ULONG __stdcall vb_AddRef(IDirect3DVertexBuffer8 *self)
{
    D3D8VertexBuffer *vb = vb_from_iface(self);
    return (ULONG)InterlockedIncrement(&vb->ref_count);
}

static ULONG __stdcall vb_Release(IDirect3DVertexBuffer8 *self)
{
    D3D8VertexBuffer *vb = vb_from_iface(self);
    LONG ref = InterlockedDecrement(&vb->ref_count);
    if (ref <= 0) {
        if (vb->d3d11_buffer) ID3D11Buffer_Release(vb->d3d11_buffer);
        free(vb->sys_mem);
        free(vb);
    }
    return (ULONG)ref;
}

static HRESULT __stdcall vb_GetDevice(IDirect3DVertexBuffer8 *self, IDirect3DDevice8 **ppDevice)
{
    (void)self;
    *ppDevice = xbox_GetD3DDevice();
    return S_OK;
}

static DWORD __stdcall vb_SetPriority(IDirect3DVertexBuffer8 *self, DWORD Priority)
{
    (void)self; (void)Priority;
    return 0;
}

static DWORD __stdcall vb_GetPriority(IDirect3DVertexBuffer8 *self)
{
    (void)self;
    return 0;
}

static void __stdcall vb_PreLoad(IDirect3DVertexBuffer8 *self)
{
    (void)self;
}

static DWORD __stdcall vb_GetType(IDirect3DVertexBuffer8 *self)
{
    (void)self;
    return 3; /* D3DRTYPE_VERTEXBUFFER */
}

static HRESULT __stdcall vb_Lock(IDirect3DVertexBuffer8 *self, UINT OffsetToLock, UINT SizeToLock, BYTE **ppbData, DWORD Flags)
{
    D3D8VertexBuffer *vb = vb_from_iface(self);
    (void)SizeToLock; (void)Flags;

    if (!ppbData) return E_INVALIDARG;
    if (vb->locked) return E_FAIL;

    *ppbData = vb->sys_mem + OffsetToLock;
    vb->locked = TRUE;
    return S_OK;
}

static HRESULT __stdcall vb_Unlock(IDirect3DVertexBuffer8 *self)
{
    D3D8VertexBuffer *vb = vb_from_iface(self);
    if (!vb->locked) return E_FAIL;

    vb->locked = FALSE;
    vb->dirty = TRUE;

    /* Upload to GPU */
    ID3D11DeviceContext *ctx = d3d8_GetD3D11Context();
    if (ctx && vb->d3d11_buffer) {
        ID3D11DeviceContext_UpdateSubresource(ctx,
            (ID3D11Resource *)vb->d3d11_buffer,
            0, NULL, vb->sys_mem, vb->size, 0);
        vb->dirty = FALSE;
    }
    return S_OK;
}

static HRESULT __stdcall vb_GetDesc(IDirect3DVertexBuffer8 *self, void *pDesc)
{
    (void)self; (void)pDesc;
    return E_NOTIMPL;
}

static const IDirect3DVertexBuffer8Vtbl g_vb_vtbl = {
    vb_QueryInterface,
    vb_AddRef,
    vb_Release,
    vb_GetDevice,
    vb_SetPriority,
    vb_GetPriority,
    vb_PreLoad,
    vb_GetType,
    vb_Lock,
    vb_Unlock,
    vb_GetDesc,
};

HRESULT d3d8_CreateVertexBufferImpl(UINT Length, DWORD Usage, DWORD FVF, IDirect3DVertexBuffer8 **ppVB)
{
    D3D8VertexBuffer *vb;
    D3D11_BUFFER_DESC bd;
    HRESULT hr;

    if (!ppVB) return E_INVALIDARG;

    vb = (D3D8VertexBuffer *)calloc(1, sizeof(*vb));
    if (!vb) return E_OUTOFMEMORY;

    vb->sys_mem = (BYTE *)calloc(1, Length);
    if (!vb->sys_mem) { free(vb); return E_OUTOFMEMORY; }

    /* Create D3D11 buffer */
    memset(&bd, 0, sizeof(bd));
    bd.ByteWidth = Length;
    bd.Usage = D3D11_USAGE_DEFAULT;
    bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;

    hr = ID3D11Device_CreateBuffer(d3d8_GetD3D11Device(), &bd, NULL, &vb->d3d11_buffer);
    if (FAILED(hr)) {
        free(vb->sys_mem);
        free(vb);
        return hr;
    }

    vb->iface.lpVtbl = &g_vb_vtbl;
    vb->ref_count = 1;
    vb->size = Length;
    vb->fvf = FVF;
    vb->usage = Usage;

    *ppVB = &vb->iface;
    return S_OK;
}

/* ================================================================
 * Index Buffer Implementation
 * ================================================================ */

static D3D8IndexBuffer *ib_from_iface(IDirect3DIndexBuffer8 *iface)
{
    return (D3D8IndexBuffer *)iface;
}

static HRESULT __stdcall ib_QueryInterface(IDirect3DIndexBuffer8 *self, const IID *riid, void **ppv)
{
    (void)self; (void)riid; (void)ppv;
    return E_NOINTERFACE;
}

static ULONG __stdcall ib_AddRef(IDirect3DIndexBuffer8 *self)
{
    return (ULONG)InterlockedIncrement(&ib_from_iface(self)->ref_count);
}

static ULONG __stdcall ib_Release(IDirect3DIndexBuffer8 *self)
{
    D3D8IndexBuffer *ib = ib_from_iface(self);
    LONG ref = InterlockedDecrement(&ib->ref_count);
    if (ref <= 0) {
        if (ib->d3d11_buffer) ID3D11Buffer_Release(ib->d3d11_buffer);
        free(ib->sys_mem);
        free(ib);
    }
    return (ULONG)ref;
}

static HRESULT __stdcall ib_GetDevice(IDirect3DIndexBuffer8 *self, IDirect3DDevice8 **ppDevice)
{
    (void)self;
    *ppDevice = xbox_GetD3DDevice();
    return S_OK;
}

static DWORD __stdcall ib_SetPriority(IDirect3DIndexBuffer8 *self, DWORD Priority) { (void)self; (void)Priority; return 0; }
static DWORD __stdcall ib_GetPriority(IDirect3DIndexBuffer8 *self) { (void)self; return 0; }
static void  __stdcall ib_PreLoad(IDirect3DIndexBuffer8 *self) { (void)self; }
static DWORD __stdcall ib_GetType(IDirect3DIndexBuffer8 *self) { (void)self; return 4; /* D3DRTYPE_INDEXBUFFER */ }

static HRESULT __stdcall ib_Lock(IDirect3DIndexBuffer8 *self, UINT OffsetToLock, UINT SizeToLock, BYTE **ppbData, DWORD Flags)
{
    D3D8IndexBuffer *ib = ib_from_iface(self);
    (void)SizeToLock; (void)Flags;
    if (!ppbData) return E_INVALIDARG;
    if (ib->locked) return E_FAIL;
    *ppbData = ib->sys_mem + OffsetToLock;
    ib->locked = TRUE;
    return S_OK;
}

static HRESULT __stdcall ib_Unlock(IDirect3DIndexBuffer8 *self)
{
    D3D8IndexBuffer *ib = ib_from_iface(self);
    if (!ib->locked) return E_FAIL;
    ib->locked = FALSE;
    ib->dirty = TRUE;

    ID3D11DeviceContext *ctx = d3d8_GetD3D11Context();
    if (ctx && ib->d3d11_buffer) {
        ID3D11DeviceContext_UpdateSubresource(ctx,
            (ID3D11Resource *)ib->d3d11_buffer,
            0, NULL, ib->sys_mem, ib->size, 0);
        ib->dirty = FALSE;
    }
    return S_OK;
}

static HRESULT __stdcall ib_GetDesc(IDirect3DIndexBuffer8 *self, void *pDesc)
{
    (void)self; (void)pDesc;
    return E_NOTIMPL;
}

static const IDirect3DIndexBuffer8Vtbl g_ib_vtbl = {
    ib_QueryInterface, ib_AddRef, ib_Release,
    ib_GetDevice, ib_SetPriority, ib_GetPriority, ib_PreLoad, ib_GetType,
    ib_Lock, ib_Unlock, ib_GetDesc,
};

HRESULT d3d8_CreateIndexBufferImpl(UINT Length, DWORD Usage, D3DFORMAT Format, IDirect3DIndexBuffer8 **ppIB)
{
    D3D8IndexBuffer *ib;
    D3D11_BUFFER_DESC bd;
    HRESULT hr;

    if (!ppIB) return E_INVALIDARG;

    ib = (D3D8IndexBuffer *)calloc(1, sizeof(*ib));
    if (!ib) return E_OUTOFMEMORY;

    ib->sys_mem = (BYTE *)calloc(1, Length);
    if (!ib->sys_mem) { free(ib); return E_OUTOFMEMORY; }

    memset(&bd, 0, sizeof(bd));
    bd.ByteWidth = Length;
    bd.Usage = D3D11_USAGE_DEFAULT;
    bd.BindFlags = D3D11_BIND_INDEX_BUFFER;

    hr = ID3D11Device_CreateBuffer(d3d8_GetD3D11Device(), &bd, NULL, &ib->d3d11_buffer);
    if (FAILED(hr)) { free(ib->sys_mem); free(ib); return hr; }

    ib->iface.lpVtbl = &g_ib_vtbl;
    ib->ref_count = 1;
    ib->size = Length;
    ib->format = Format;
    ib->usage = Usage;

    *ppIB = &ib->iface;
    return S_OK;
}

/* ================================================================
 * Texture Implementation
 * ================================================================ */

static D3D8Texture *tex_from_iface(IDirect3DTexture8 *iface)
{
    return (D3D8Texture *)iface;
}

static HRESULT __stdcall tex_QueryInterface(IDirect3DTexture8 *self, const IID *riid, void **ppv)
{
    (void)self; (void)riid; (void)ppv;
    return E_NOINTERFACE;
}

static ULONG __stdcall tex_AddRef(IDirect3DTexture8 *self)
{
    return (ULONG)InterlockedIncrement(&tex_from_iface(self)->ref_count);
}

static ULONG __stdcall tex_Release(IDirect3DTexture8 *self)
{
    D3D8Texture *tex = tex_from_iface(self);
    LONG ref = InterlockedDecrement(&tex->ref_count);
    if (ref <= 0) {
        if (tex->srv) ID3D11ShaderResourceView_Release(tex->srv);
        if (tex->d3d11_texture) ID3D11Texture2D_Release(tex->d3d11_texture);
        free(tex->sys_mem);
        free(tex);
    }
    return (ULONG)ref;
}

static HRESULT __stdcall tex_GetDevice(IDirect3DTexture8 *self, IDirect3DDevice8 **ppDevice)
{
    (void)self;
    *ppDevice = xbox_GetD3DDevice();
    return S_OK;
}

static DWORD __stdcall tex_SetPriority(IDirect3DTexture8 *self, DWORD Priority) { (void)self; (void)Priority; return 0; }
static DWORD __stdcall tex_GetPriority(IDirect3DTexture8 *self) { (void)self; return 0; }
static void  __stdcall tex_PreLoad(IDirect3DTexture8 *self) { (void)self; }
static DWORD __stdcall tex_GetType(IDirect3DTexture8 *self) { (void)self; return 5; /* D3DRTYPE_TEXTURE */ }

static DWORD __stdcall tex_GetLevelCount(IDirect3DTexture8 *self)
{
    return tex_from_iface(self)->levels;
}

static HRESULT __stdcall tex_GetLevelDesc(IDirect3DTexture8 *self, UINT Level, D3DSURFACE_DESC *pDesc)
{
    D3D8Texture *tex = tex_from_iface(self);
    if (!pDesc || Level >= tex->levels) return E_INVALIDARG;
    pDesc->Format = tex->d3d8_format;
    pDesc->Width = tex->width >> Level;
    pDesc->Height = tex->height >> Level;
    if (pDesc->Width < 1) pDesc->Width = 1;
    if (pDesc->Height < 1) pDesc->Height = 1;
    pDesc->Pool = D3DPOOL_DEFAULT;
    return S_OK;
}

static HRESULT __stdcall tex_GetSurfaceLevel(IDirect3DTexture8 *self, UINT Level, IDirect3DSurface8 **ppSurface)
{
    (void)self; (void)Level; (void)ppSurface;
    return E_NOTIMPL;
}

static HRESULT __stdcall tex_LockRect(IDirect3DTexture8 *self, UINT Level, D3DLOCKED_RECT *pLockedRect, const RECT *pRect, DWORD Flags)
{
    D3D8Texture *tex = tex_from_iface(self);
    (void)pRect; (void)Flags;

    if (!pLockedRect || Level != 0) return E_INVALIDARG;
    if (tex->locked) return E_FAIL;

    pLockedRect->Pitch = (INT)tex->pitch;
    pLockedRect->pBits = tex->sys_mem;
    tex->locked = TRUE;
    return S_OK;
}

static HRESULT __stdcall tex_UnlockRect(IDirect3DTexture8 *self, UINT Level)
{
    D3D8Texture *tex = tex_from_iface(self);
    if (Level != 0 || !tex->locked) return E_FAIL;

    tex->locked = FALSE;
    tex->dirty = TRUE;

    /* Upload level 0 to GPU */
    ID3D11DeviceContext *ctx = d3d8_GetD3D11Context();
    if (ctx && tex->d3d11_texture) {
        UINT rows;
        if (d3d8_format_is_compressed(tex->d3d8_format))
            rows = (tex->height + 3) / 4;
        else
            rows = tex->height;

        ID3D11DeviceContext_UpdateSubresource(ctx,
            (ID3D11Resource *)tex->d3d11_texture,
            0, NULL, tex->sys_mem, tex->pitch, tex->pitch * rows);
        tex->dirty = FALSE;
    }
    return S_OK;
}

static const IDirect3DTexture8Vtbl g_tex_vtbl = {
    tex_QueryInterface, tex_AddRef, tex_Release,
    tex_GetDevice, tex_SetPriority, tex_GetPriority, tex_PreLoad, tex_GetType,
    tex_GetLevelCount,
    tex_GetLevelDesc, tex_GetSurfaceLevel, tex_LockRect, tex_UnlockRect,
};

HRESULT d3d8_CreateTextureImpl(UINT Width, UINT Height, UINT Levels, DWORD Usage, D3DFORMAT Format, IDirect3DTexture8 **ppTex)
{
    D3D8Texture *tex;
    D3D11_TEXTURE2D_DESC td;
    D3D11_SHADER_RESOURCE_VIEW_DESC srvd;
    HRESULT hr;
    UINT data_size;
    (void)Usage;

    if (!ppTex) return E_INVALIDARG;

    tex = (D3D8Texture *)calloc(1, sizeof(*tex));
    if (!tex) return E_OUTOFMEMORY;

    tex->d3d8_format = Format;
    tex->dxgi_format = d3d8_to_dxgi_format(Format);
    tex->width = Width;
    tex->height = Height;
    tex->levels = Levels ? Levels : 1;
    tex->pitch = d3d8_row_pitch(Format, Width);

    /* Allocate system memory for level 0 */
    if (d3d8_format_is_compressed(Format))
        data_size = tex->pitch * ((Height + 3) / 4);
    else
        data_size = tex->pitch * Height;

    tex->sys_mem = (BYTE *)calloc(1, data_size);
    if (!tex->sys_mem) { free(tex); return E_OUTOFMEMORY; }

    /* Create D3D11 texture */
    memset(&td, 0, sizeof(td));
    td.Width = Width;
    td.Height = Height;
    td.MipLevels = tex->levels;
    td.ArraySize = 1;
    td.Format = tex->dxgi_format;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;

    hr = ID3D11Device_CreateTexture2D(d3d8_GetD3D11Device(), &td, NULL, &tex->d3d11_texture);
    if (FAILED(hr)) {
        fprintf(stderr, "D3D8: CreateTexture2D failed: 0x%08lX (fmt=%d %ux%u)\n", hr, Format, Width, Height);
        free(tex->sys_mem);
        free(tex);
        return hr;
    }

    /* Create shader resource view */
    memset(&srvd, 0, sizeof(srvd));
    srvd.Format = tex->dxgi_format;
    srvd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    srvd.Texture2D.MipLevels = tex->levels;

    hr = ID3D11Device_CreateShaderResourceView(d3d8_GetD3D11Device(),
        (ID3D11Resource *)tex->d3d11_texture, &srvd, &tex->srv);
    if (FAILED(hr)) {
        ID3D11Texture2D_Release(tex->d3d11_texture);
        free(tex->sys_mem);
        free(tex);
        return hr;
    }

    tex->iface.lpVtbl = &g_tex_vtbl;
    tex->ref_count = 1;

    *ppTex = &tex->iface;
    return S_OK;
}
