/**
 * D3D8 Fixed-Function Pipeline Emulation via D3D11 Shaders
 *
 * Emulates the Xbox D3D8 fixed-function pipeline using D3D11
 * programmable shaders. Handles:
 *   - FVF-based vertex formats (XYZ, XYZRHW, Normal, Diffuse, TexCoord)
 *   - World/View/Projection transform application
 *   - Pre-transformed vertex passthrough (XYZRHW)
 *   - Single texture stage with diffuse modulation
 *
 * The shader source is compiled at init time using D3DCompile.
 */

#include "d3d8_internal.h"
#include <d3dcompiler.h>
#include <string.h>
#include <stdio.h>
#include <math.h>

#pragma comment(lib, "d3dcompiler.lib")

/* ================================================================
 * HLSL shader source (embedded)
 * ================================================================ */

static const char g_vs_source[] =
    "cbuffer TransformCB : register(b0) {\n"
    "    float4x4 WorldViewProj;\n"
    "    float2   ScreenSize;\n"
    "    uint     Flags;\n"     /* bit 0: pre-transformed, bit 1: has diffuse, bit 2: has tex */
    "    float    _pad;\n"
    "};\n"
    "\n"
    "struct VS_IN {\n"
    "    float4 pos     : POSITION;\n"
    "    float3 normal  : NORMAL;\n"
    "    float4 diffuse : COLOR0;\n"
    "    float2 tex0    : TEXCOORD0;\n"
    "};\n"
    "\n"
    "struct VS_OUT {\n"
    "    float4 pos     : SV_POSITION;\n"
    "    float4 color   : COLOR0;\n"
    "    float2 tex0    : TEXCOORD0;\n"
    "};\n"
    "\n"
    "VS_OUT main(VS_IN input) {\n"
    "    VS_OUT o;\n"
    "    \n"
    "    if (Flags & 1u) {\n"
    "        // Pre-transformed (XYZRHW): convert screen space to NDC\n"
    "        o.pos.x = (input.pos.x / ScreenSize.x) * 2.0 - 1.0;\n"
    "        o.pos.y = 1.0 - (input.pos.y / ScreenSize.y) * 2.0;\n"
    "        o.pos.z = input.pos.z;\n"
    "        o.pos.w = 1.0;\n"
    "    } else {\n"
    "        // Standard transform\n"
    "        o.pos = mul(float4(input.pos.xyz, 1.0), WorldViewProj);\n"
    "    }\n"
    "    \n"
    "    // Diffuse color: use vertex color if present, else white\n"
    "    if (Flags & 2u)\n"
    "        o.color = input.diffuse;\n"
    "    else\n"
    "        o.color = float4(1, 1, 1, 1);\n"
    "    \n"
    "    o.tex0 = input.tex0;\n"
    "    return o;\n"
    "}\n";

static const char g_ps_source[] =
    "Texture2D    tex0  : register(t0);\n"
    "SamplerState samp0 : register(s0);\n"
    "\n"
    "cbuffer PixelCB : register(b0) {\n"
    "    float4 TexFactor;\n"
    "    float  AlphaRef;\n"
    "    uint   Flags;\n"      /* bit 0: texture enabled, bit 1: alpha test */
    "    uint   AlphaFunc;\n"
    "    float  _pad;\n"
    "};\n"
    "\n"
    "struct PS_IN {\n"
    "    float4 pos   : SV_POSITION;\n"
    "    float4 color : COLOR0;\n"
    "    float2 tex0  : TEXCOORD0;\n"
    "};\n"
    "\n"
    "float4 main(PS_IN input) : SV_TARGET {\n"
    "    float4 result = input.color;\n"
    "    \n"
    "    if (Flags & 1u) {\n"
    "        float4 texel = tex0.Sample(samp0, input.tex0);\n"
    "        result *= texel;\n"  /* MODULATE */
    "    }\n"
    "    \n"
    "    // Alpha test\n"
    "    if (Flags & 2u) {\n"
    "        bool alphaOk = true;\n"
    "        if (AlphaFunc == 1u) alphaOk = false;\n"            /* NEVER */
    "        else if (AlphaFunc == 2u) alphaOk = (result.a < AlphaRef);\n"  /* LESS */
    "        else if (AlphaFunc == 3u) alphaOk = (result.a == AlphaRef);\n" /* EQUAL */
    "        else if (AlphaFunc == 4u) alphaOk = (result.a <= AlphaRef);\n" /* LESSEQUAL */
    "        else if (AlphaFunc == 5u) alphaOk = (result.a > AlphaRef);\n"  /* GREATER */
    "        else if (AlphaFunc == 6u) alphaOk = (result.a != AlphaRef);\n" /* NOTEQUAL */
    "        else if (AlphaFunc == 7u) alphaOk = (result.a >= AlphaRef);\n" /* GREATEREQUAL */
    "        // AlphaFunc 8 = ALWAYS - alphaOk stays true\n"
    "        if (!alphaOk) discard;\n"
    "    }\n"
    "    \n"
    "    return result;\n"
    "}\n";

/* ================================================================
 * Compiled shader objects
 * ================================================================ */

static ID3D11VertexShader  *g_vs = NULL;
static ID3D11PixelShader   *g_ps = NULL;
static ID3DBlob            *g_vs_blob = NULL;   /* VS bytecode for input layouts */
static ID3D11Buffer        *g_vs_cb = NULL;      /* VS constant buffer */
static ID3D11Buffer        *g_ps_cb = NULL;      /* PS constant buffer */

/* VS constant buffer layout (must match HLSL) */
typedef struct {
    float wvp[16];          /* WorldViewProj matrix (column-major for HLSL) */
    float screen_w, screen_h;
    UINT  flags;
    float _pad;
} VSConstants;

/* PS constant buffer layout */
typedef struct {
    float tex_factor[4];
    float alpha_ref;
    UINT  flags;
    UINT  alpha_func;
    float _pad;
} PSConstants;

/* ================================================================
 * Input layout cache (FVF → ID3D11InputLayout)
 * ================================================================ */

#define MAX_LAYOUT_CACHE 16

typedef struct {
    DWORD               fvf;
    ID3D11InputLayout  *layout;
} LayoutCacheEntry;

static LayoutCacheEntry g_layout_cache[MAX_LAYOUT_CACHE];
static int g_layout_cache_count = 0;

/* Calculate vertex stride from FVF */
static UINT fvf_stride(DWORD fvf)
{
    UINT stride = 0;
    if (fvf & D3DFVF_XYZ)    stride += 12;  /* float3 */
    if (fvf & D3DFVF_XYZRHW) stride += 16;  /* float4 (x,y,z,rhw) */
    if (fvf & D3DFVF_NORMAL) stride += 12;  /* float3 */
    if (fvf & D3DFVF_DIFFUSE) stride += 4;  /* DWORD color */
    if (fvf & D3DFVF_SPECULAR) stride += 4; /* DWORD color */
    stride += ((fvf & D3DFVF_TEXCOUNT_MASK) >> D3DFVF_TEXCOUNT_SHIFT) * 8; /* float2 per texcoord */
    return stride;
}

static ID3D11InputLayout *get_or_create_layout(DWORD fvf)
{
    D3D11_INPUT_ELEMENT_DESC elems[8];
    UINT elem_count = 0;
    UINT offset = 0;
    HRESULT hr;
    int i;

    /* Check cache */
    for (i = 0; i < g_layout_cache_count; i++) {
        if (g_layout_cache[i].fvf == fvf)
            return g_layout_cache[i].layout;
    }

    /* Build input element description from FVF */
    if (fvf & D3DFVF_XYZRHW) {
        elems[elem_count].SemanticName = "POSITION";
        elems[elem_count].SemanticIndex = 0;
        elems[elem_count].Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
        elems[elem_count].InputSlot = 0;
        elems[elem_count].AlignedByteOffset = offset;
        elems[elem_count].InputSlotClass = D3D11_INPUT_PER_VERTEX_DATA;
        elems[elem_count].InstanceDataStepRate = 0;
        elem_count++;
        offset += 16;
    } else if (fvf & D3DFVF_XYZ) {
        elems[elem_count].SemanticName = "POSITION";
        elems[elem_count].SemanticIndex = 0;
        elems[elem_count].Format = DXGI_FORMAT_R32G32B32_FLOAT;
        elems[elem_count].InputSlot = 0;
        elems[elem_count].AlignedByteOffset = offset;
        elems[elem_count].InputSlotClass = D3D11_INPUT_PER_VERTEX_DATA;
        elems[elem_count].InstanceDataStepRate = 0;
        elem_count++;
        offset += 12;
    }

    if (fvf & D3DFVF_NORMAL) {
        elems[elem_count].SemanticName = "NORMAL";
        elems[elem_count].SemanticIndex = 0;
        elems[elem_count].Format = DXGI_FORMAT_R32G32B32_FLOAT;
        elems[elem_count].InputSlot = 0;
        elems[elem_count].AlignedByteOffset = offset;
        elems[elem_count].InputSlotClass = D3D11_INPUT_PER_VERTEX_DATA;
        elems[elem_count].InstanceDataStepRate = 0;
        elem_count++;
        offset += 12;
    }

    if (fvf & D3DFVF_DIFFUSE) {
        elems[elem_count].SemanticName = "COLOR";
        elems[elem_count].SemanticIndex = 0;
        elems[elem_count].Format = DXGI_FORMAT_B8G8R8A8_UNORM;  /* D3DCOLOR is BGRA */
        elems[elem_count].InputSlot = 0;
        elems[elem_count].AlignedByteOffset = offset;
        elems[elem_count].InputSlotClass = D3D11_INPUT_PER_VERTEX_DATA;
        elems[elem_count].InstanceDataStepRate = 0;
        elem_count++;
        offset += 4;
    }

    if (fvf & D3DFVF_SPECULAR) {
        /* Skip specular - VS doesn't use it, just advance offset */
        offset += 4;
    }

    {
        UINT tex_count = (fvf & D3DFVF_TEXCOUNT_MASK) >> D3DFVF_TEXCOUNT_SHIFT;
        UINT t;
        for (t = 0; t < tex_count && elem_count < 8; t++) {
            elems[elem_count].SemanticName = "TEXCOORD";
            elems[elem_count].SemanticIndex = t;
            elems[elem_count].Format = DXGI_FORMAT_R32G32_FLOAT;
            elems[elem_count].InputSlot = 0;
            elems[elem_count].AlignedByteOffset = offset;
            elems[elem_count].InputSlotClass = D3D11_INPUT_PER_VERTEX_DATA;
            elems[elem_count].InstanceDataStepRate = 0;
            elem_count++;
            offset += 8;
        }
    }

    if (elem_count == 0) {
        fprintf(stderr, "D3D8: No input elements for FVF 0x%lX\n", fvf);
        return NULL;
    }

    /* Create input layout */
    ID3D11InputLayout *layout = NULL;
    hr = ID3D11Device_CreateInputLayout(
        d3d8_GetD3D11Device(),
        elems, elem_count,
        ID3D10Blob_GetBufferPointer(g_vs_blob),
        ID3D10Blob_GetBufferSize(g_vs_blob),
        &layout);

    if (FAILED(hr)) {
        fprintf(stderr, "D3D8: CreateInputLayout failed for FVF 0x%lX: 0x%08lX\n", fvf, hr);
        return NULL;
    }

    /* Cache it */
    if (g_layout_cache_count < MAX_LAYOUT_CACHE) {
        g_layout_cache[g_layout_cache_count].fvf = fvf;
        g_layout_cache[g_layout_cache_count].layout = layout;
        g_layout_cache_count++;
    }

    return layout;
}

/* ================================================================
 * Matrix math helpers
 * ================================================================ */

/* Multiply two 4x4 matrices: result = a * b */
static void mat4_mul(float *result, const float *a, const float *b)
{
    int i, j, k;
    float tmp[16];
    for (i = 0; i < 4; i++) {
        for (j = 0; j < 4; j++) {
            tmp[i * 4 + j] = 0;
            for (k = 0; k < 4; k++)
                tmp[i * 4 + j] += a[i * 4 + k] * b[k * 4 + j];
        }
    }
    memcpy(result, tmp, sizeof(tmp));
}

/* Transpose 4x4 matrix (D3D8 row-major → HLSL column-major) */
static void mat4_transpose(float *result, const float *src)
{
    int i, j;
    for (i = 0; i < 4; i++)
        for (j = 0; j < 4; j++)
            result[j * 4 + i] = src[i * 4 + j];
}

/* ================================================================
 * Initialization / Shutdown
 * ================================================================ */

HRESULT d3d8_shaders_init(void)
{
    ID3DBlob *errors = NULL;
    D3D11_BUFFER_DESC cbd;
    HRESULT hr;

    /* Compile vertex shader */
    hr = D3DCompile(g_vs_source, strlen(g_vs_source), "vs_ffp",
                    NULL, NULL, "main", "vs_5_0", 0, 0, &g_vs_blob, &errors);
    if (FAILED(hr)) {
        fprintf(stderr, "D3D8: VS compile failed: %s\n",
                errors ? (char *)ID3D10Blob_GetBufferPointer(errors) : "unknown");
        if (errors) ID3D10Blob_Release(errors);
        return hr;
    }

    hr = ID3D11Device_CreateVertexShader(d3d8_GetD3D11Device(),
        ID3D10Blob_GetBufferPointer(g_vs_blob),
        ID3D10Blob_GetBufferSize(g_vs_blob),
        NULL, &g_vs);
    if (FAILED(hr)) return hr;

    /* Compile pixel shader */
    {
        ID3DBlob *ps_blob = NULL;
        hr = D3DCompile(g_ps_source, strlen(g_ps_source), "ps_ffp",
                        NULL, NULL, "main", "ps_5_0", 0, 0, &ps_blob, &errors);
        if (FAILED(hr)) {
            fprintf(stderr, "D3D8: PS compile failed: %s\n",
                    errors ? (char *)ID3D10Blob_GetBufferPointer(errors) : "unknown");
            if (errors) ID3D10Blob_Release(errors);
            return hr;
        }

        hr = ID3D11Device_CreatePixelShader(d3d8_GetD3D11Device(),
            ID3D10Blob_GetBufferPointer(ps_blob),
            ID3D10Blob_GetBufferSize(ps_blob),
            NULL, &g_ps);
        ID3D10Blob_Release(ps_blob);
        if (FAILED(hr)) return hr;
    }

    /* Create constant buffers */
    memset(&cbd, 0, sizeof(cbd));
    cbd.ByteWidth = sizeof(VSConstants);
    cbd.Usage = D3D11_USAGE_DYNAMIC;
    cbd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    cbd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;

    hr = ID3D11Device_CreateBuffer(d3d8_GetD3D11Device(), &cbd, NULL, &g_vs_cb);
    if (FAILED(hr)) return hr;

    cbd.ByteWidth = sizeof(PSConstants);
    hr = ID3D11Device_CreateBuffer(d3d8_GetD3D11Device(), &cbd, NULL, &g_ps_cb);
    if (FAILED(hr)) return hr;

    fprintf(stderr, "D3D8: Fixed-function shaders compiled OK\n");
    return S_OK;
}

void d3d8_shaders_shutdown(void)
{
    int i;
    for (i = 0; i < g_layout_cache_count; i++) {
        if (g_layout_cache[i].layout)
            ID3D11InputLayout_Release(g_layout_cache[i].layout);
    }
    g_layout_cache_count = 0;

    if (g_ps_cb)    { ID3D11Buffer_Release(g_ps_cb); g_ps_cb = NULL; }
    if (g_vs_cb)    { ID3D11Buffer_Release(g_vs_cb); g_vs_cb = NULL; }
    if (g_ps)       { ID3D11PixelShader_Release(g_ps); g_ps = NULL; }
    if (g_vs)       { ID3D11VertexShader_Release(g_vs); g_vs = NULL; }
    if (g_vs_blob)  { ID3D10Blob_Release(g_vs_blob); g_vs_blob = NULL; }
}

/* ================================================================
 * Pre-draw binding
 * ================================================================ */

void d3d8_shaders_prepare_draw(DWORD fvf)
{
    ID3D11DeviceContext *ctx = d3d8_GetD3D11Context();
    ID3D11InputLayout *layout;
    D3D11_MAPPED_SUBRESOURCE mapped;
    const D3DMATRIX *world, *view, *proj;
    const DWORD *rs;
    HRESULT hr;

    if (!ctx || !g_vs || !g_ps) return;

    /* Bind shaders */
    ID3D11DeviceContext_VSSetShader(ctx, g_vs, NULL, 0);
    ID3D11DeviceContext_PSSetShader(ctx, g_ps, NULL, 0);

    /* Bind input layout for this FVF */
    layout = get_or_create_layout(fvf);
    if (layout) {
        ID3D11DeviceContext_IASetInputLayout(ctx, layout);
    }

    /* Update VS constant buffer */
    hr = ID3D11DeviceContext_Map(ctx, (ID3D11Resource *)g_vs_cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
    if (SUCCEEDED(hr)) {
        VSConstants *cb = (VSConstants *)mapped.pData;

        if (fvf & D3DFVF_XYZRHW) {
            /* Pre-transformed: identity WVP, set screen size */
            float identity[16] = { 1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1 };
            memcpy(cb->wvp, identity, sizeof(identity));
            cb->screen_w = (float)d3d8_GetBackbufferWidth();
            cb->screen_h = (float)d3d8_GetBackbufferHeight();
            cb->flags = 1; /* pre-transformed */
        } else {
            /* Compute WVP = World * View * Projection, then transpose for HLSL */
            float wv[16], wvp[16], wvp_t[16];
            world = d3d8_GetTransform(D3DTS_WORLD);
            view  = d3d8_GetTransform(D3DTS_VIEW);
            proj  = d3d8_GetTransform(D3DTS_PROJECTION);

            mat4_mul(wv, (const float *)world, (const float *)view);
            mat4_mul(wvp, wv, (const float *)proj);
            mat4_transpose(wvp_t, wvp);
            memcpy(cb->wvp, wvp_t, sizeof(wvp_t));

            cb->screen_w = (float)d3d8_GetBackbufferWidth();
            cb->screen_h = (float)d3d8_GetBackbufferHeight();
            cb->flags = 0;
        }

        if (fvf & D3DFVF_DIFFUSE) cb->flags |= 2;
        if ((fvf & D3DFVF_TEXCOUNT_MASK) >= D3DFVF_TEX1) cb->flags |= 4;

        cb->_pad = 0;
        ID3D11DeviceContext_Unmap(ctx, (ID3D11Resource *)g_vs_cb, 0);
    }

    /* Update PS constant buffer */
    hr = ID3D11DeviceContext_Map(ctx, (ID3D11Resource *)g_ps_cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
    if (SUCCEEDED(hr)) {
        PSConstants *cb = (PSConstants *)mapped.pData;
        rs = d3d8_GetRenderStates();

        cb->tex_factor[0] = ((rs[D3DRS_TEXTUREFACTOR] >> 16) & 0xFF) / 255.0f;
        cb->tex_factor[1] = ((rs[D3DRS_TEXTUREFACTOR] >>  8) & 0xFF) / 255.0f;
        cb->tex_factor[2] = ((rs[D3DRS_TEXTUREFACTOR] >>  0) & 0xFF) / 255.0f;
        cb->tex_factor[3] = ((rs[D3DRS_TEXTUREFACTOR] >> 24) & 0xFF) / 255.0f;

        cb->alpha_ref = rs[D3DRS_ALPHAREF] / 255.0f;
        cb->flags = 0;
        /* Check if a texture is bound by looking at TSS stage 0 */
        {
            const DWORD *tss0 = d3d8_GetTSS(0);
            if (tss0 && tss0[D3DTSS_COLOROP] != D3DTOP_DISABLE)
                cb->flags |= 1;
        }
        if (rs[D3DRS_ALPHATESTENABLE])
            cb->flags |= 2;
        cb->alpha_func = rs[D3DRS_ALPHAFUNC];
        cb->_pad = 0;

        ID3D11DeviceContext_Unmap(ctx, (ID3D11Resource *)g_ps_cb, 0);
    }

    /* Bind constant buffers */
    ID3D11DeviceContext_VSSetConstantBuffers(ctx, 0, 1, &g_vs_cb);
    ID3D11DeviceContext_PSSetConstantBuffers(ctx, 0, 1, &g_ps_cb);
}
