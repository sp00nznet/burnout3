/**
 * Xbox Direct3D 8 Compatibility Layer - Type Definitions
 *
 * Defines the Xbox D3D8 types, enums, and COM interface structures
 * used by the statically-linked RenderWare Xbox driver code.
 *
 * Xbox D3D8 differs from PC D3D8 in several ways:
 * - Push buffer (command buffer) based rendering
 * - Tiled/swizzled texture formats
 * - Hardware-specific render states
 * - Unified 64MB memory model (textures/VBs in main RAM)
 * - No CAPS querying (known fixed hardware)
 *
 * This header provides the ABI-compatible types so that translated
 * game/RW code can compile against our D3D11-backed implementation.
 */

#ifndef BURNOUT3_D3D8_XBOX_H
#define BURNOUT3_D3D8_XBOX_H

#include <stdint.h>
#include <windows.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ================================================================
 * Forward declarations
 * ================================================================ */

typedef struct IDirect3D8              IDirect3D8;
typedef struct IDirect3DDevice8        IDirect3DDevice8;
typedef struct IDirect3DTexture8       IDirect3DTexture8;
typedef struct IDirect3DSurface8       IDirect3DSurface8;
typedef struct IDirect3DVertexBuffer8  IDirect3DVertexBuffer8;
typedef struct IDirect3DIndexBuffer8   IDirect3DIndexBuffer8;
typedef struct IDirect3DBaseTexture8   IDirect3DBaseTexture8;
typedef struct IDirect3DCubeTexture8   IDirect3DCubeTexture8;
typedef struct IDirect3DVolumeTexture8 IDirect3DVolumeTexture8;

/* ================================================================
 * Basic D3D8 types
 * ================================================================ */

typedef DWORD D3DCOLOR;
typedef float D3DVALUE;

typedef struct D3DVECTOR {
    float x, y, z;
} D3DVECTOR;

typedef struct D3DMATRIX {
    union {
        struct {
            float _11, _12, _13, _14;
            float _21, _22, _23, _24;
            float _31, _32, _33, _34;
            float _41, _42, _43, _44;
        };
        float m[4][4];
    };
} D3DMATRIX;

typedef struct D3DRECT {
    LONG x1, y1, x2, y2;
} D3DRECT;

typedef struct D3DVIEWPORT8 {
    DWORD X, Y;
    DWORD Width, Height;
    float MinZ, MaxZ;
} D3DVIEWPORT8;

typedef struct D3DLOCKED_RECT {
    INT Pitch;
    void *pBits;
} D3DLOCKED_RECT;

typedef struct D3DBOX {
    UINT Left, Top, Right, Bottom, Front, Back;
} D3DBOX;

typedef struct D3DLOCKED_BOX {
    INT RowPitch;
    INT SlicePitch;
    void *pBits;
} D3DLOCKED_BOX;

typedef struct D3DGAMMARAMP {
    WORD red[256];
    WORD green[256];
    WORD blue[256];
} D3DGAMMARAMP;

/* ================================================================
 * D3D8 enumerations
 * ================================================================ */

typedef enum D3DFORMAT {
    D3DFMT_UNKNOWN       = 0,

    /* Standard RGB formats */
    D3DFMT_A8R8G8B8      = 6,
    D3DFMT_X8R8G8B8      = 7,
    D3DFMT_R5G6B5        = 5,
    D3DFMT_A1R5G5B5      = 3,
    D3DFMT_A4R4G4B4      = 4,
    D3DFMT_A8            = 19,
    D3DFMT_R8B8          = 16,

    /* Compressed formats */
    D3DFMT_DXT1          = 12,
    D3DFMT_DXT2          = 14,
    D3DFMT_DXT3          = 14,
    D3DFMT_DXT4          = 15,
    D3DFMT_DXT5          = 15,

    /* Depth/stencil */
    D3DFMT_D16           = 0x2C,
    D3DFMT_D24S8         = 0x2A,
    D3DFMT_F16           = 0x2D,
    D3DFMT_F24S8         = 0x2B,

    /* Xbox-specific swizzled formats */
    D3DFMT_LIN_A8R8G8B8  = 0x12,
    D3DFMT_LIN_X8R8G8B8  = 0x1E,
    D3DFMT_LIN_R5G6B5    = 0x11,
    D3DFMT_LIN_A1R5G5B5  = 0x10,
    D3DFMT_LIN_A4R4G4B4  = 0x1D,

    /* Luminance */
    D3DFMT_L8            = 0,
    D3DFMT_A8L8          = 1,

    /* Palette */
    D3DFMT_P8            = 0x0B,

    /* YUV */
    D3DFMT_YUY2          = 0x24,
    D3DFMT_UYVY          = 0x25,

    /* Index buffer formats */
    D3DFMT_INDEX16        = 101,
    D3DFMT_INDEX32        = 102,
} D3DFORMAT;

typedef enum D3DPRIMITIVETYPE {
    D3DPT_POINTLIST     = 1,
    D3DPT_LINELIST      = 2,
    D3DPT_LINESTRIP     = 3,
    D3DPT_TRIANGLELIST  = 4,
    D3DPT_TRIANGLESTRIP = 5,
    D3DPT_TRIANGLEFAN   = 6,
    /* Xbox-specific */
    D3DPT_QUADLIST      = 8,
} D3DPRIMITIVETYPE;

typedef enum D3DTRANSFORMSTATETYPE {
    D3DTS_VIEW          = 2,
    D3DTS_PROJECTION    = 3,
    D3DTS_TEXTURE0      = 16,
    D3DTS_TEXTURE1      = 17,
    D3DTS_TEXTURE2      = 18,
    D3DTS_TEXTURE3      = 19,
    D3DTS_WORLD         = 256,
    D3DTS_WORLD1        = 257,
    D3DTS_WORLD2        = 258,
    D3DTS_WORLD3        = 259,
} D3DTRANSFORMSTATETYPE;

typedef enum D3DRENDERSTATETYPE {
    /* Standard D3D8 render states */
    D3DRS_ZENABLE                  = 7,
    D3DRS_FILLMODE                 = 8,
    D3DRS_SHADEMODE                = 9,
    D3DRS_ZWRITEENABLE             = 14,
    D3DRS_ALPHATESTENABLE          = 15,
    D3DRS_SRCBLEND                 = 19,
    D3DRS_DESTBLEND                = 20,
    D3DRS_CULLMODE                 = 22,
    D3DRS_ZFUNC                    = 23,
    D3DRS_ALPHAREF                 = 24,
    D3DRS_ALPHAFUNC                = 25,
    D3DRS_DITHERENABLE             = 26,
    D3DRS_ALPHABLENDENABLE         = 27,
    D3DRS_FOGENABLE                = 28,
    D3DRS_SPECULARENABLE           = 29,
    D3DRS_FOGCOLOR                 = 34,
    D3DRS_FOGTABLEMODE             = 35,
    D3DRS_FOGSTART                 = 36,
    D3DRS_FOGEND                   = 37,
    D3DRS_FOGDENSITY               = 38,
    D3DRS_EDGEANTIALIAS            = 40,
    D3DRS_STENCILENABLE            = 52,
    D3DRS_STENCILFAIL              = 53,
    D3DRS_STENCILZFAIL             = 54,
    D3DRS_STENCILPASS              = 55,
    D3DRS_STENCILFUNC              = 56,
    D3DRS_STENCILREF               = 57,
    D3DRS_STENCILMASK              = 58,
    D3DRS_STENCILWRITEMASK         = 59,
    D3DRS_TEXTUREFACTOR            = 60,
    D3DRS_WRAP0                    = 128,
    D3DRS_WRAP1                    = 129,
    D3DRS_WRAP2                    = 130,
    D3DRS_WRAP3                    = 131,
    D3DRS_LIGHTING                 = 137,
    D3DRS_AMBIENT                  = 139,
    D3DRS_COLORVERTEX              = 141,
    D3DRS_LOCALVIEWER              = 142,
    D3DRS_NORMALIZENORMALS         = 143,
    D3DRS_DIFFUSEMATERIALSOURCE    = 145,
    D3DRS_SPECULARMATERIALSOURCE   = 146,
    D3DRS_AMBIENTMATERIALSOURCE    = 147,
    D3DRS_EMISSIVEMATERIALSOURCE   = 148,
    D3DRS_VERTEXBLEND              = 151,
    D3DRS_POINTSIZE                = 154,
    D3DRS_POINTSIZE_MIN            = 155,
    D3DRS_POINTSPRITEENABLE        = 156,
    D3DRS_POINTSCALEENABLE         = 157,
    D3DRS_MULTISAMPLEANTIALIAS     = 161,
    D3DRS_MULTISAMPLEMASK          = 162,
    D3DRS_COLORWRITEENABLE         = 168,
    D3DRS_BLENDOP                  = 171,
    /* Xbox-specific render states (200+) */
    D3DRS_PSALPHAINPUTS0           = 200,
    D3DRS_PSALPHAINPUTS1           = 201,
    D3DRS_PSALPHAINPUTS2           = 202,
    D3DRS_PSALPHAINPUTS3           = 203,
    D3DRS_PSALPHAINPUTS4           = 204,
    D3DRS_PSALPHAINPUTS5           = 205,
    D3DRS_PSALPHAINPUTS6           = 206,
    D3DRS_PSALPHAINPUTS7           = 207,
    D3DRS_PSFINALCOMBINERINPUTSABCD = 208,
    D3DRS_PSFINALCOMBINERINPUTSEFG  = 209,
} D3DRENDERSTATETYPE;

typedef enum D3DTEXTURESTAGESTATETYPE {
    D3DTSS_COLOROP      = 1,
    D3DTSS_COLORARG1    = 2,
    D3DTSS_COLORARG2    = 3,
    D3DTSS_ALPHAOP      = 4,
    D3DTSS_ALPHAARG1    = 5,
    D3DTSS_ALPHAARG2    = 6,
    D3DTSS_BUMPENVMAT00 = 7,
    D3DTSS_BUMPENVMAT01 = 8,
    D3DTSS_BUMPENVMAT10 = 9,
    D3DTSS_BUMPENVMAT11 = 10,
    D3DTSS_TEXCOORDINDEX = 11,
    D3DTSS_ADDRESSU     = 13,
    D3DTSS_ADDRESSV     = 14,
    D3DTSS_BORDERCOLOR  = 15,
    D3DTSS_MAGFILTER    = 16,
    D3DTSS_MINFILTER    = 17,
    D3DTSS_MIPFILTER    = 18,
    D3DTSS_MIPMAPLODBIAS = 19,
    D3DTSS_MAXMIPLEVEL  = 20,
    D3DTSS_MAXANISOTROPY = 21,
    D3DTSS_COLORKEYOP   = 24,
    D3DTSS_COLORSIGN    = 25,
    D3DTSS_ALPHAKILL    = 26,
    D3DTSS_COLORARG0    = 26,
    D3DTSS_ALPHAARG0    = 27,
    D3DTSS_RESULTARG    = 28,
} D3DTEXTURESTAGESTATETYPE;

typedef enum D3DTEXTUREOP {
    D3DTOP_DISABLE    = 1,
    D3DTOP_SELECTARG1 = 2,
    D3DTOP_SELECTARG2 = 3,
    D3DTOP_MODULATE   = 4,
    D3DTOP_MODULATE2X = 5,
    D3DTOP_MODULATE4X = 6,
    D3DTOP_ADD        = 7,
    D3DTOP_ADDSIGNED  = 8,
    D3DTOP_SUBTRACT   = 10,
    D3DTOP_DOTPRODUCT3 = 24,
    D3DTOP_MULTIPLYADD = 25,
    D3DTOP_LERP       = 26,
} D3DTEXTUREOP;

typedef enum D3DBLEND {
    D3DBLEND_ZERO            = 1,
    D3DBLEND_ONE             = 2,
    D3DBLEND_SRCCOLOR        = 3,
    D3DBLEND_INVSRCCOLOR     = 4,
    D3DBLEND_SRCALPHA        = 5,
    D3DBLEND_INVSRCALPHA     = 6,
    D3DBLEND_DESTALPHA       = 7,
    D3DBLEND_INVDESTALPHA    = 8,
    D3DBLEND_DESTCOLOR       = 9,
    D3DBLEND_INVDESTCOLOR    = 10,
    D3DBLEND_SRCALPHASAT     = 11,
} D3DBLEND;

typedef enum D3DCMPFUNC {
    D3DCMP_NEVER        = 1,
    D3DCMP_LESS         = 2,
    D3DCMP_EQUAL        = 3,
    D3DCMP_LESSEQUAL    = 4,
    D3DCMP_GREATER      = 5,
    D3DCMP_NOTEQUAL     = 6,
    D3DCMP_GREATEREQUAL = 7,
    D3DCMP_ALWAYS       = 8,
} D3DCMPFUNC;

typedef enum D3DCULL {
    D3DCULL_NONE = 1,
    D3DCULL_CW   = 2,
    D3DCULL_CCW  = 3,
} D3DCULL;

typedef enum D3DFILLMODE {
    D3DFILL_POINT     = 1,
    D3DFILL_WIREFRAME = 2,
    D3DFILL_SOLID     = 3,
} D3DFILLMODE;

typedef enum D3DPOOL {
    D3DPOOL_DEFAULT     = 0,
    D3DPOOL_MANAGED     = 1,
    D3DPOOL_SYSTEMMEM   = 2,
} D3DPOOL;

typedef enum D3DMULTISAMPLE_TYPE {
    D3DMULTISAMPLE_NONE = 0,
    D3DMULTISAMPLE_2_SAMPLES = 2,
    D3DMULTISAMPLE_4_SAMPLES = 4,
} D3DMULTISAMPLE_TYPE;

typedef enum D3DTEXTUREFILTERTYPE {
    D3DTEXF_NONE            = 0,
    D3DTEXF_POINT           = 1,
    D3DTEXF_LINEAR          = 2,
    D3DTEXF_ANISOTROPIC     = 3,
    D3DTEXF_QUINCUNX        = 4,  /* Xbox-specific */
    D3DTEXF_GAUSSIANCUBIC   = 5,  /* Xbox-specific */
} D3DTEXTUREFILTERTYPE;

typedef enum D3DTEXTUREADDRESS {
    D3DTADDRESS_WRAP        = 1,
    D3DTADDRESS_MIRROR      = 2,
    D3DTADDRESS_CLAMP       = 3,
    D3DTADDRESS_BORDER      = 4,
    D3DTADDRESS_MIRRORONCE  = 5,
} D3DTEXTUREADDRESS;

typedef enum D3DSWAPEFFECT {
    D3DSWAPEFFECT_DISCARD = 1,
    D3DSWAPEFFECT_FLIP    = 2,
    D3DSWAPEFFECT_COPY    = 3,
} D3DSWAPEFFECT;

typedef enum D3DCLEAR_FLAGS {
    D3DCLEAR_TARGET  = 0x01,
    D3DCLEAR_ZBUFFER = 0x02,
    D3DCLEAR_STENCIL = 0x04,
} D3DCLEAR_FLAGS;

/* ================================================================
 * Vertex declaration / FVF
 * ================================================================ */

#define D3DFVF_XYZ              0x002
#define D3DFVF_XYZRHW           0x004
#define D3DFVF_NORMAL           0x010
#define D3DFVF_DIFFUSE          0x040
#define D3DFVF_SPECULAR         0x080
#define D3DFVF_TEX0             0x000
#define D3DFVF_TEX1             0x100
#define D3DFVF_TEX2             0x200
#define D3DFVF_TEX3             0x300
#define D3DFVF_TEX4             0x400
#define D3DFVF_TEXCOUNT_MASK    0xF00
#define D3DFVF_TEXCOUNT_SHIFT   8

/* ================================================================
 * Structures
 * ================================================================ */

typedef struct D3DPRESENT_PARAMETERS {
    UINT BackBufferWidth;
    UINT BackBufferHeight;
    D3DFORMAT BackBufferFormat;
    UINT BackBufferCount;
    D3DMULTISAMPLE_TYPE MultiSampleType;
    D3DSWAPEFFECT SwapEffect;
    HWND hDeviceWindow;
    BOOL Windowed;
    BOOL EnableAutoDepthStencil;
    D3DFORMAT AutoDepthStencilFormat;
    DWORD Flags;
    UINT FullScreen_RefreshRateInHz;
    UINT FullScreen_PresentationInterval;
} D3DPRESENT_PARAMETERS;

typedef struct D3DMATERIAL8 {
    struct { float r, g, b, a; } Diffuse;
    struct { float r, g, b, a; } Ambient;
    struct { float r, g, b, a; } Specular;
    struct { float r, g, b, a; } Emissive;
    float Power;
} D3DMATERIAL8;

typedef struct D3DLIGHT8 {
    DWORD Type;
    struct { float r, g, b, a; } Diffuse;
    struct { float r, g, b, a; } Specular;
    struct { float r, g, b, a; } Ambient;
    D3DVECTOR Position;
    D3DVECTOR Direction;
    float Range;
    float Falloff;
    float Attenuation0;
    float Attenuation1;
    float Attenuation2;
    float Theta;
    float Phi;
} D3DLIGHT8;

typedef struct D3DSURFACE_DESC {
    D3DFORMAT Format;
    DWORD Type;
    DWORD Usage;
    D3DPOOL Pool;
    UINT Size;
    D3DMULTISAMPLE_TYPE MultiSampleType;
    UINT Width;
    UINT Height;
} D3DSURFACE_DESC;

/* ================================================================
 * Lock flags
 * ================================================================ */

#define D3DLOCK_READONLY    0x00000010
#define D3DLOCK_DISCARD     0x00002000
#define D3DLOCK_NOOVERWRITE 0x00001000
#define D3DLOCK_NOSYSLOCK   0x00000800

/* ================================================================
 * Usage flags
 * ================================================================ */

#define D3DUSAGE_RENDERTARGET       0x00000001
#define D3DUSAGE_DEPTHSTENCIL       0x00000002
#define D3DUSAGE_WRITEONLY          0x00000008
#define D3DUSAGE_DYNAMIC            0x00000200

/* ================================================================
 * Xbox-specific: Push buffer types
 * ================================================================ */

typedef struct D3DPushBuffer {
    DWORD Common;
    DWORD Data;
    DWORD Size;
    DWORD AllocationSize;
} D3DPushBuffer;

/* ================================================================
 * IDirect3DVertexBuffer8 interface
 * ================================================================ */

typedef struct IDirect3DVertexBuffer8Vtbl {
    /* IUnknown */
    HRESULT (__stdcall *QueryInterface)(IDirect3DVertexBuffer8 *self, const IID *riid, void **ppv);
    ULONG   (__stdcall *AddRef)(IDirect3DVertexBuffer8 *self);
    ULONG   (__stdcall *Release)(IDirect3DVertexBuffer8 *self);
    /* IDirect3DResource8 */
    HRESULT (__stdcall *GetDevice)(IDirect3DVertexBuffer8 *self, IDirect3DDevice8 **ppDevice);
    DWORD   (__stdcall *SetPriority)(IDirect3DVertexBuffer8 *self, DWORD Priority);
    DWORD   (__stdcall *GetPriority)(IDirect3DVertexBuffer8 *self);
    void    (__stdcall *PreLoad)(IDirect3DVertexBuffer8 *self);
    DWORD   (__stdcall *GetType)(IDirect3DVertexBuffer8 *self);
    /* IDirect3DVertexBuffer8 */
    HRESULT (__stdcall *Lock)(IDirect3DVertexBuffer8 *self, UINT OffsetToLock, UINT SizeToLock, BYTE **ppbData, DWORD Flags);
    HRESULT (__stdcall *Unlock)(IDirect3DVertexBuffer8 *self);
    HRESULT (__stdcall *GetDesc)(IDirect3DVertexBuffer8 *self, void *pDesc);
} IDirect3DVertexBuffer8Vtbl;

struct IDirect3DVertexBuffer8 {
    const IDirect3DVertexBuffer8Vtbl *lpVtbl;
};

/* ================================================================
 * IDirect3DIndexBuffer8 interface
 * ================================================================ */

typedef struct IDirect3DIndexBuffer8Vtbl {
    /* IUnknown */
    HRESULT (__stdcall *QueryInterface)(IDirect3DIndexBuffer8 *self, const IID *riid, void **ppv);
    ULONG   (__stdcall *AddRef)(IDirect3DIndexBuffer8 *self);
    ULONG   (__stdcall *Release)(IDirect3DIndexBuffer8 *self);
    /* IDirect3DResource8 */
    HRESULT (__stdcall *GetDevice)(IDirect3DIndexBuffer8 *self, IDirect3DDevice8 **ppDevice);
    DWORD   (__stdcall *SetPriority)(IDirect3DIndexBuffer8 *self, DWORD Priority);
    DWORD   (__stdcall *GetPriority)(IDirect3DIndexBuffer8 *self);
    void    (__stdcall *PreLoad)(IDirect3DIndexBuffer8 *self);
    DWORD   (__stdcall *GetType)(IDirect3DIndexBuffer8 *self);
    /* IDirect3DIndexBuffer8 */
    HRESULT (__stdcall *Lock)(IDirect3DIndexBuffer8 *self, UINT OffsetToLock, UINT SizeToLock, BYTE **ppbData, DWORD Flags);
    HRESULT (__stdcall *Unlock)(IDirect3DIndexBuffer8 *self);
    HRESULT (__stdcall *GetDesc)(IDirect3DIndexBuffer8 *self, void *pDesc);
} IDirect3DIndexBuffer8Vtbl;

struct IDirect3DIndexBuffer8 {
    const IDirect3DIndexBuffer8Vtbl *lpVtbl;
};

/* ================================================================
 * IDirect3DTexture8 interface
 * ================================================================ */

typedef struct IDirect3DTexture8Vtbl {
    /* IUnknown */
    HRESULT (__stdcall *QueryInterface)(IDirect3DTexture8 *self, const IID *riid, void **ppv);
    ULONG   (__stdcall *AddRef)(IDirect3DTexture8 *self);
    ULONG   (__stdcall *Release)(IDirect3DTexture8 *self);
    /* IDirect3DResource8 */
    HRESULT (__stdcall *GetDevice)(IDirect3DTexture8 *self, IDirect3DDevice8 **ppDevice);
    DWORD   (__stdcall *SetPriority)(IDirect3DTexture8 *self, DWORD Priority);
    DWORD   (__stdcall *GetPriority)(IDirect3DTexture8 *self);
    void    (__stdcall *PreLoad)(IDirect3DTexture8 *self);
    DWORD   (__stdcall *GetType)(IDirect3DTexture8 *self);
    /* IDirect3DBaseTexture8 */
    DWORD   (__stdcall *GetLevelCount)(IDirect3DTexture8 *self);
    /* IDirect3DTexture8 */
    HRESULT (__stdcall *GetLevelDesc)(IDirect3DTexture8 *self, UINT Level, D3DSURFACE_DESC *pDesc);
    HRESULT (__stdcall *GetSurfaceLevel)(IDirect3DTexture8 *self, UINT Level, IDirect3DSurface8 **ppSurface);
    HRESULT (__stdcall *LockRect)(IDirect3DTexture8 *self, UINT Level, D3DLOCKED_RECT *pLockedRect, const RECT *pRect, DWORD Flags);
    HRESULT (__stdcall *UnlockRect)(IDirect3DTexture8 *self, UINT Level);
} IDirect3DTexture8Vtbl;

struct IDirect3DTexture8 {
    const IDirect3DTexture8Vtbl *lpVtbl;
};

/* ================================================================
 * IDirect3DSurface8 interface
 * ================================================================ */

typedef struct IDirect3DSurface8Vtbl {
    HRESULT (__stdcall *QueryInterface)(IDirect3DSurface8 *self, const IID *riid, void **ppv);
    ULONG   (__stdcall *AddRef)(IDirect3DSurface8 *self);
    ULONG   (__stdcall *Release)(IDirect3DSurface8 *self);
    HRESULT (__stdcall *GetDevice)(IDirect3DSurface8 *self, IDirect3DDevice8 **ppDevice);
    HRESULT (__stdcall *GetDesc)(IDirect3DSurface8 *self, D3DSURFACE_DESC *pDesc);
    HRESULT (__stdcall *LockRect)(IDirect3DSurface8 *self, D3DLOCKED_RECT *pLockedRect, const RECT *pRect, DWORD Flags);
    HRESULT (__stdcall *UnlockRect)(IDirect3DSurface8 *self);
} IDirect3DSurface8Vtbl;

struct IDirect3DSurface8 {
    const IDirect3DSurface8Vtbl *lpVtbl;
};

/* ================================================================
 * IDirect3DDevice8 interface (COM vtable)
 *
 * This is the Xbox variant. The vtable layout matches the Xbox
 * D3D8 binary so that translated code can call through it.
 * ================================================================ */

typedef struct IDirect3DDevice8Vtbl {
    /* IUnknown */
    HRESULT (__stdcall *QueryInterface)(IDirect3DDevice8 *self, const IID *riid, void **ppv);
    ULONG   (__stdcall *AddRef)(IDirect3DDevice8 *self);
    ULONG   (__stdcall *Release)(IDirect3DDevice8 *self);

    /* IDirect3DDevice8 core */
    HRESULT (__stdcall *GetDirect3D)(IDirect3DDevice8 *self, IDirect3D8 **ppD3D8);
    HRESULT (__stdcall *GetDeviceCaps)(IDirect3DDevice8 *self, void *pCaps);
    HRESULT (__stdcall *GetDisplayMode)(IDirect3DDevice8 *self, void *pMode);
    HRESULT (__stdcall *GetCreationParameters)(IDirect3DDevice8 *self, void *pParams);

    /* Rendering */
    HRESULT (__stdcall *Reset)(IDirect3DDevice8 *self, D3DPRESENT_PARAMETERS *pPP);
    HRESULT (__stdcall *Present)(IDirect3DDevice8 *self, const RECT *src, const RECT *dst, HWND hWnd, void *pDirty);
    HRESULT (__stdcall *GetBackBuffer)(IDirect3DDevice8 *self, INT iBackBuffer, DWORD Type, IDirect3DSurface8 **ppSurface);

    /* Scene management */
    HRESULT (__stdcall *BeginScene)(IDirect3DDevice8 *self);
    HRESULT (__stdcall *EndScene)(IDirect3DDevice8 *self);
    HRESULT (__stdcall *Clear)(IDirect3DDevice8 *self, DWORD Count, const D3DRECT *pRects, DWORD Flags, D3DCOLOR Color, float Z, DWORD Stencil);

    /* Transforms */
    HRESULT (__stdcall *SetTransform)(IDirect3DDevice8 *self, D3DTRANSFORMSTATETYPE State, const D3DMATRIX *pMatrix);
    HRESULT (__stdcall *GetTransform)(IDirect3DDevice8 *self, D3DTRANSFORMSTATETYPE State, D3DMATRIX *pMatrix);

    /* Render state */
    HRESULT (__stdcall *SetRenderState)(IDirect3DDevice8 *self, D3DRENDERSTATETYPE State, DWORD Value);
    HRESULT (__stdcall *GetRenderState)(IDirect3DDevice8 *self, D3DRENDERSTATETYPE State, DWORD *pValue);

    /* Texture state */
    HRESULT (__stdcall *SetTextureStageState)(IDirect3DDevice8 *self, DWORD Stage, D3DTEXTURESTAGESTATETYPE Type, DWORD Value);
    HRESULT (__stdcall *GetTextureStageState)(IDirect3DDevice8 *self, DWORD Stage, D3DTEXTURESTAGESTATETYPE Type, DWORD *pValue);

    /* Textures */
    HRESULT (__stdcall *SetTexture)(IDirect3DDevice8 *self, DWORD Stage, IDirect3DBaseTexture8 *pTexture);
    HRESULT (__stdcall *GetTexture)(IDirect3DDevice8 *self, DWORD Stage, IDirect3DBaseTexture8 **ppTexture);

    /* Vertex/index buffers */
    HRESULT (__stdcall *SetStreamSource)(IDirect3DDevice8 *self, UINT StreamNumber, IDirect3DVertexBuffer8 *pStreamData, UINT Stride);
    HRESULT (__stdcall *GetStreamSource)(IDirect3DDevice8 *self, UINT StreamNumber, IDirect3DVertexBuffer8 **ppStreamData, UINT *pStride);
    HRESULT (__stdcall *SetIndices)(IDirect3DDevice8 *self, IDirect3DIndexBuffer8 *pIndexData, UINT BaseVertexIndex);
    HRESULT (__stdcall *GetIndices)(IDirect3DDevice8 *self, IDirect3DIndexBuffer8 **ppIndexData, UINT *pBaseVertexIndex);

    /* Drawing */
    HRESULT (__stdcall *DrawPrimitive)(IDirect3DDevice8 *self, D3DPRIMITIVETYPE PrimitiveType, UINT StartVertex, UINT PrimitiveCount);
    HRESULT (__stdcall *DrawIndexedPrimitive)(IDirect3DDevice8 *self, D3DPRIMITIVETYPE PrimitiveType, UINT MinVertexIndex, UINT NumVertices, UINT StartIndex, UINT PrimitiveCount);
    HRESULT (__stdcall *DrawPrimitiveUP)(IDirect3DDevice8 *self, D3DPRIMITIVETYPE PrimitiveType, UINT PrimitiveCount, const void *pVertexData, UINT VertexStreamZeroStride);
    HRESULT (__stdcall *DrawIndexedPrimitiveUP)(IDirect3DDevice8 *self, D3DPRIMITIVETYPE PrimitiveType, UINT MinVertexIndex, UINT NumVertices, UINT PrimitiveCount, const void *pIndexData, D3DFORMAT IndexDataFormat, const void *pVertexData, UINT VertexStreamZeroStride);

    /* Resource creation */
    HRESULT (__stdcall *CreateTexture)(IDirect3DDevice8 *self, UINT Width, UINT Height, UINT Levels, DWORD Usage, D3DFORMAT Format, D3DPOOL Pool, IDirect3DTexture8 **ppTexture);
    HRESULT (__stdcall *CreateVertexBuffer)(IDirect3DDevice8 *self, UINT Length, DWORD Usage, DWORD FVF, D3DPOOL Pool, IDirect3DVertexBuffer8 **ppVertexBuffer);
    HRESULT (__stdcall *CreateIndexBuffer)(IDirect3DDevice8 *self, UINT Length, DWORD Usage, D3DFORMAT Format, D3DPOOL Pool, IDirect3DIndexBuffer8 **ppIndexBuffer);
    HRESULT (__stdcall *CreateRenderTarget)(IDirect3DDevice8 *self, UINT Width, UINT Height, D3DFORMAT Format, D3DMULTISAMPLE_TYPE MultiSample, BOOL Lockable, IDirect3DSurface8 **ppSurface);
    HRESULT (__stdcall *CreateDepthStencilSurface)(IDirect3DDevice8 *self, UINT Width, UINT Height, D3DFORMAT Format, D3DMULTISAMPLE_TYPE MultiSample, IDirect3DSurface8 **ppSurface);

    /* Render targets */
    HRESULT (__stdcall *SetRenderTarget)(IDirect3DDevice8 *self, IDirect3DSurface8 *pRenderTarget, IDirect3DSurface8 *pZStencilSurface);
    HRESULT (__stdcall *GetRenderTarget)(IDirect3DDevice8 *self, IDirect3DSurface8 **ppRenderTarget);
    HRESULT (__stdcall *GetDepthStencilSurface)(IDirect3DDevice8 *self, IDirect3DSurface8 **ppZStencilSurface);

    /* Viewport */
    HRESULT (__stdcall *SetViewport)(IDirect3DDevice8 *self, const D3DVIEWPORT8 *pViewport);
    HRESULT (__stdcall *GetViewport)(IDirect3DDevice8 *self, D3DVIEWPORT8 *pViewport);

    /* Material / Lighting */
    HRESULT (__stdcall *SetMaterial)(IDirect3DDevice8 *self, const D3DMATERIAL8 *pMaterial);
    HRESULT (__stdcall *GetMaterial)(IDirect3DDevice8 *self, D3DMATERIAL8 *pMaterial);
    HRESULT (__stdcall *SetLight)(IDirect3DDevice8 *self, DWORD Index, const D3DLIGHT8 *pLight);
    HRESULT (__stdcall *GetLight)(IDirect3DDevice8 *self, DWORD Index, D3DLIGHT8 *pLight);
    HRESULT (__stdcall *LightEnable)(IDirect3DDevice8 *self, DWORD Index, BOOL Enable);

    /* Shaders */
    HRESULT (__stdcall *SetVertexShader)(IDirect3DDevice8 *self, DWORD Handle);
    HRESULT (__stdcall *GetVertexShader)(IDirect3DDevice8 *self, DWORD *pHandle);
    HRESULT (__stdcall *SetVertexShaderConstant)(IDirect3DDevice8 *self, INT Register, const void *pConstantData, DWORD ConstantCount);
    HRESULT (__stdcall *SetPixelShader)(IDirect3DDevice8 *self, DWORD Handle);
    HRESULT (__stdcall *GetPixelShader)(IDirect3DDevice8 *self, DWORD *pHandle);
    HRESULT (__stdcall *SetPixelShaderConstant)(IDirect3DDevice8 *self, INT Register, const void *pConstantData, DWORD ConstantCount);

    /* Gamma */
    void    (__stdcall *SetGammaRamp)(IDirect3DDevice8 *self, DWORD Flags, const D3DGAMMARAMP *pRamp);
    void    (__stdcall *GetGammaRamp)(IDirect3DDevice8 *self, D3DGAMMARAMP *pRamp);

    /* Palette (Xbox-specific) */
    HRESULT (__stdcall *SetPalette)(IDirect3DDevice8 *self, DWORD PaletteNumber, const void *pEntries);

    /* Xbox-specific: push buffer */
    HRESULT (__stdcall *BeginPush)(IDirect3DDevice8 *self, DWORD Count, DWORD **ppPush);
    HRESULT (__stdcall *EndPush)(IDirect3DDevice8 *self, DWORD *pPush);

    /* Swap / display */
    HRESULT (__stdcall *Swap)(IDirect3DDevice8 *self, DWORD Flags);
} IDirect3DDevice8Vtbl;

struct IDirect3DDevice8 {
    const IDirect3DDevice8Vtbl *lpVtbl;
};

/* Convenience macros for COM-style method calls */
#define IDirect3DDevice8_SetRenderState(p,s,v)       (p)->lpVtbl->SetRenderState(p,s,v)
#define IDirect3DDevice8_SetTransform(p,s,m)          (p)->lpVtbl->SetTransform(p,s,m)
#define IDirect3DDevice8_SetTexture(p,s,t)            (p)->lpVtbl->SetTexture(p,s,t)
#define IDirect3DDevice8_DrawPrimitive(p,t,sv,pc)     (p)->lpVtbl->DrawPrimitive(p,t,sv,pc)
#define IDirect3DDevice8_DrawIndexedPrimitive(p,t,mi,nv,si,pc) (p)->lpVtbl->DrawIndexedPrimitive(p,t,mi,nv,si,pc)
#define IDirect3DDevice8_Clear(p,n,r,f,c,z,s)         (p)->lpVtbl->Clear(p,n,r,f,c,z,s)
#define IDirect3DDevice8_Present(p,s,d,w,pd)          (p)->lpVtbl->Present(p,s,d,w,pd)
#define IDirect3DDevice8_BeginScene(p)                (p)->lpVtbl->BeginScene(p)
#define IDirect3DDevice8_EndScene(p)                  (p)->lpVtbl->EndScene(p)

/* ================================================================
 * IDirect3D8 interface (factory)
 * ================================================================ */

typedef struct IDirect3D8Vtbl {
    HRESULT (__stdcall *QueryInterface)(IDirect3D8 *self, const IID *riid, void **ppv);
    ULONG   (__stdcall *AddRef)(IDirect3D8 *self);
    ULONG   (__stdcall *Release)(IDirect3D8 *self);
    HRESULT (__stdcall *CreateDevice)(IDirect3D8 *self, UINT Adapter, DWORD DeviceType, HWND hFocusWindow, DWORD BehaviorFlags, D3DPRESENT_PARAMETERS *pPP, IDirect3DDevice8 **ppDevice);
} IDirect3D8Vtbl;

struct IDirect3D8 {
    const IDirect3D8Vtbl *lpVtbl;
};

/* ================================================================
 * Initialization
 * ================================================================ */

/**
 * Create the D3D8-compatible interface backed by D3D11.
 * This replaces the Xbox Direct3DCreate8() call.
 */
IDirect3D8 *xbox_Direct3DCreate8(UINT SDKVersion);

/**
 * Get the current D3D device (Xbox uses a global device pointer).
 */
IDirect3DDevice8 *xbox_GetD3DDevice(void);

#ifdef __cplusplus
}
#endif

#endif /* BURNOUT3_D3D8_XBOX_H */
