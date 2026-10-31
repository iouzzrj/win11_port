#include "ghost_runtime.h"
#include "xbox_d3d8_types.h"
#include <d3d11.h>
#include <dxgi.h>
#include <d3dcompiler.h>
#include <math.h>
#include <stdio.h>
#include <vector>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3dcompiler.lib")

// ============================================================================
// Direct3D 11 Device & Xbox CDevice State
// ============================================================================

static ID3D11Device*             g_d3dDevice         = nullptr;
static ID3D11DeviceContext*      g_d3dCtx            = nullptr;
static IDXGISwapChain*           g_swapChain         = nullptr;
static ID3D11Texture2D*          g_backBufferTex     = nullptr;
static ID3D11Texture2D*          g_backBufferCopyTex = nullptr;
static ID3D11ShaderResourceView* g_backBufferCopySRV = nullptr;
static bool                      g_backBufferDirty   = true;
static ID3D11RenderTargetView*   g_rtv               = nullptr;
static ID3D11Texture2D*          g_depthTex          = nullptr;
static ID3D11DepthStencilView*   g_dsv               = nullptr;
static ID3D11RenderTargetView*   g_curActiveRTV      = nullptr;
static ID3D11DepthStencilView*   g_curActiveDSV      = nullptr;
static float                     g_gamma             = 1.0f;
static float                     g_brightness        = 1.0f;

static ID3D11VertexShader*       g_vs             = nullptr;
static ID3D11PixelShader*        g_ps             = nullptr;
static ID3D11InputLayout*        g_inputLayout    = nullptr;
static ID3D11Buffer*             g_cbPerDraw      = nullptr;
static ID3D11Buffer*             g_dynVB          = nullptr;
static uint32_t                  g_dynVBCapacity  = 0;

static uint32_t*                 g_dummyPushBuf   = nullptr;

// Xbox D3D globals inside Ghost.exe
static const uint32_t ADDR_D3D_PDEVICE       = 0x006C0888;
static const uint32_t ADDR_D3D_DEVICE        = 0x006C0890;
static const uint32_t ADDR_D3D_RENDERSTATE   = 0x006C05F0; // 166 DWORDs
static const uint32_t ADDR_D3D_TEXTURESTATE  = 0x006C03F0; // 4 stages * 32 DWORDs
static const uint32_t ADDR_D3D_INDEXDATA     = 0x006C03E4; // _D3D__IndexData
static const uint32_t ADDR_D3D_STREAM        = 0x006C2D48; // g_Stream (16 * 12 bytes)
static const uint32_t ADDR_SIMPLE_RS_ENCODE  = 0x007502A0; // 92 DWORDs

#pragma pack(push, 1)
struct XboxPixelContainer {
    uint32_t Common; // +0x00
    uint32_t Data;   // +0x04 (pointer to pixel memory)
    uint32_t Lock;   // +0x08
    uint32_t Format; // +0x0C (D3DFORMAT shift=8, dimensionality, mipmap levels, etc.)
    uint32_t Size;   // +0x10 (0 if power-of-two swizzled; non-zero for linear/rect textures)
};

struct XboxVertexBuffer {
    uint32_t Common; // +0x00
    uint32_t Data;   // +0x04 (pointer to vertex/index data)
    uint32_t Lock;   // +0x08
};

struct XboxPushBuffer {
    uint32_t Common;         // +0x00
    uint32_t Data;           // +0x04
    uint32_t Lock;           // +0x08
    uint32_t Size;           // +0x0C
    uint32_t AllocationSize; // +0x10
};

struct XboxLockedRect {
    INT   Pitch;
    void* pBits;
};

struct XboxViewport {
    DWORD X;
    DWORD Y;
    DWORD Width;
    DWORD Height;
    float MinZ;
    float MaxZ;
};

struct Mat44 {
    float m[4][4];
};
#pragma pack(pop)

// Xbox _XGUnswizzleRect@32 in Ghost.exe (0x0071FE62)
typedef void (__stdcall *PFN_XGUnswizzleRect)(
    const void* pSource, uint32_t Width, uint32_t Height, const RECT* pRect,
    void* pDest, uint32_t Pitch, const POINT* pPoint, uint32_t BytesPerPixel);
static PFN_XGUnswizzleRect g_pfnXGUnswizzleRect = reinterpret_cast<PFN_XGUnswizzleRect>(0x0071FE62);

// ============================================================================
// Dummy PushBuffer Management
// ============================================================================

static uint32_t __stdcall Bridge_D3DDevice_MakeSpace() {
    if (g_dummyPushBuf) {
        *reinterpret_cast<uint32_t*>(ADDR_D3D_DEVICE + 0x00) = reinterpret_cast<uint32_t>(g_dummyPushBuf);
        *reinterpret_cast<uint32_t*>(ADDR_D3D_DEVICE + 0x04) = reinterpret_cast<uint32_t>(g_dummyPushBuf) + 0xF000;
    }
    return reinterpret_cast<uint32_t>(g_dummyPushBuf);
}

static uint32_t* __stdcall Bridge_MakeRequestedSpace(uint32_t a, uint32_t b) {
    (void)a; (void)b;
    Bridge_D3DDevice_MakeSpace();
    return g_dummyPushBuf;
}

// ============================================================================
// HLSL Unified Vertex & Pixel Shaders + Constant Buffer
// ============================================================================

#pragma pack(push, 1)
struct UnifiedVertex {
    float x, y, z, w;
    float nx, ny, nz;
    float r, g, b, a;
    float u0, v0;
    float u1, v1;
    float u2, v2;
    float u3, v3;
};

struct alignas(16) PerDrawCBData {
    float    wvpRow0[4];
    float    wvpRow1[4];
    float    wvpRow2[4];
    float    wvpRow3[4];
    float    viewport[4];   // x, y, width, height
    float    tfactor[4];    // r, g, b, a
    uint32_t stage0Ops[4];  // colorOp, colorArg1, colorArg2, colorArg0
    uint32_t stage0Alpha[4];// alphaOp, alphaArg1, alphaArg2, alphaArg0
    uint32_t stage1Ops[4];  // colorOp, colorArg1, colorArg2, colorArg0
    uint32_t stage1Alpha[4];// alphaOp, alphaArg1, alphaArg2, alphaArg0
    uint32_t stage2Ops[4];  // colorOp, colorArg1, colorArg2, colorArg0
    uint32_t stage2Alpha[4];// alphaOp, alphaArg1, alphaArg2, alphaArg0
    uint32_t stage3Ops[4];  // colorOp, colorArg1, colorArg2, colorArg0
    uint32_t stage3Alpha[4];// alphaOp, alphaArg1, alphaArg2, alphaArg0
    uint32_t flags[4];      // x=isRhw, y=texMask(bit0..3), z=alphaTestEnable, w=psMode
    float    alphaTest[4];  // x=alphaFunc (1..8), y=alphaRef (0..1), z=gamma, w=brightness
    float    uvScale01[4];  // x=uScale0, y=vScale0, z=uScale1, w=vScale1 (for linear textures)
    float    uvScale23[4];  // x=uScale2, y=vScale2, z=uScale3, w=vScale3 (for linear textures)
    float    bumpParams[4]; // x=mat00, y=mat01, z=mat10, w=mat11
    float    fogParams[4];  // x=fogMode(0=off,1=exp,2=exp2,3=linear), y=fogStart, z=fogEnd, w=fogDensity
    float    fogColor[4];   // r, g, b, isAdditiveBlend
    float    psConst0[4];   // c0
    float    psConst1[4];   // c1
    float    psConst2[4];   // c2
    float    psConst3[4];   // c3
};
#pragma pack(pop)

static const char* kGhostHLSL = R"(
cbuffer PerDrawCB : register(b0) {
    float4 uWvpRow0;
    float4 uWvpRow1;
    float4 uWvpRow2;
    float4 uWvpRow3;
    float4 uViewport;
    float4 uTFactor;
    uint4  uStage0Ops;
    uint4  uStage0Alpha;
    uint4  uStage1Ops;
    uint4  uStage1Alpha;
    uint4  uStage2Ops;
    uint4  uStage2Alpha;
    uint4  uStage3Ops;
    uint4  uStage3Alpha;
    uint4  uFlags;
    float4 uAlphaTest;
    float4 uUvScale01;
    float4 uUvScale23;
    float4 uBumpParams;
    float4 uFogParams;
    float4 uFogColor;
    float4 uPsConst0;
    float4 uPsConst1;
    float4 uPsConst2;
    float4 uPsConst3;
};

Texture2D    tex0 : register(t0);
Texture2D    tex1 : register(t1);
Texture2D    tex2 : register(t2);
Texture2D    tex3 : register(t3);
SamplerState smp0 : register(s0);
SamplerState smp1 : register(s1);
SamplerState smp2 : register(s2);
SamplerState smp3 : register(s3);

struct VSIn {
    float4 pos   : POSITION;
    float3 norm  : NORMAL;
    float4 color : COLOR0;
    float2 uv0   : TEXCOORD0;
    float2 uv1   : TEXCOORD1;
    float2 uv2   : TEXCOORD2;
    float2 uv3   : TEXCOORD3;
};

struct PSIn {
    float4 pos    : SV_POSITION;
    float4 color  : COLOR0;
    float2 uv0    : TEXCOORD0;
    float2 uv1    : TEXCOORD1;
    float2 uv2    : TEXCOORD2;
    float2 uv3    : TEXCOORD3;
    float  eyeDist: TEXCOORD4;
};

PSIn VSMain(VSIn input) {
    PSIn o;
    if (uFlags.x != 0u) {
        float vpW = max(uViewport.z, 1.0f);
        float vpH = max(uViewport.w, 1.0f);
        float ndcX = ((input.pos.x - uViewport.x) / vpW) * 2.0f - 1.0f;
        float ndcY = 1.0f - ((input.pos.y - uViewport.y) / vpH) * 2.0f;
        o.pos = float4(ndcX, ndcY, saturate(input.pos.z), 1.0f);
        o.eyeDist = 0.0f;
    } else {
        float4 clipPos = input.pos.x * uWvpRow0 +
                         input.pos.y * uWvpRow1 +
                         input.pos.z * uWvpRow2 +
                         uWvpRow3;
        o.pos = clipPos;
        o.eyeDist = max(clipPos.w, 0.0f);
    }
    o.color = input.color;
    o.uv0 = input.uv0 * uUvScale01.xy;
    o.uv1 = input.uv1 * uUvScale01.zw;
    o.uv2 = input.uv2 * uUvScale23.xy;
    o.uv3 = input.uv3 * uUvScale23.zw;
    return o;
}

float4 EvalArg(uint arg, float4 diffuse, float4 current, float4 tex, float4 tfactor) {
    uint src = arg & 0x0Fu;
    float4 v = current;
    if (src == 0u)      v = diffuse;
    else if (src == 1u) v = current;
    else if (src == 2u) v = tex;
    else if (src == 3u) v = tfactor;
    else if (src == 4u) v = float4(0.0f, 0.0f, 0.0f, 1.0f);
    if ((arg & 0x10u) != 0u) v = 1.0f - v;
    if ((arg & 0x20u) != 0u) v = v.aaaa;
    return v;
}

float3 EvalColorOp(uint op, float4 a1, float4 a2, float4 a0, float4 diffuse, float4 current, float4 tex, float4 tfactor) {
    if (op == 2u)  return a1.rgb;
    if (op == 3u)  return a2.rgb;
    if (op == 4u)  return a1.rgb * a2.rgb;
    if (op == 5u)  return saturate(a1.rgb * a2.rgb * 2.0f);
    if (op == 6u)  return saturate(a1.rgb * a2.rgb * 4.0f);
    if (op == 7u)  return saturate(a1.rgb + a2.rgb);
    if (op == 8u)  return saturate(a1.rgb + a2.rgb - 0.5f);
    if (op == 9u)  return saturate((a1.rgb + a2.rgb - 0.5f) * 2.0f);
    if (op == 10u) return saturate(a1.rgb - a2.rgb);
    if (op == 11u) return saturate(a1.rgb + a2.rgb - a1.rgb * a2.rgb);
    if (op == 12u) return lerp(a2.rgb, a1.rgb, diffuse.a);
    if (op == 13u) return lerp(a2.rgb, a1.rgb, current.a);
    if (op == 14u) return lerp(a2.rgb, a1.rgb, tex.a);
    if (op == 15u) return lerp(a2.rgb, a1.rgb, tfactor.a);
    if (op == 16u) return saturate(a1.rgb + a2.rgb * (1.0f - tex.a));
    if (op == 17u) return saturate(a1.rgb + a2.rgb * (1.0f - current.a));
    if (op == 18u) return saturate(a1.rgb + a1.a * a2.rgb);
    if (op == 19u) return saturate(a1.rgb * a2.rgb + a1.a);
    if (op == 20u) return saturate(a1.rgb * a2.rgb + (1.0f - a1.rgb) * a2.a);
    if (op == 21u) return saturate(a1.rgb * a2.rgb + (1.0f - a1.a) * a2.rgb);
    if (op == 22u) return current.rgb;
    if (op == 23u) return saturate(a0.rgb + a1.rgb * a2.rgb);
    if (op == 24u) return lerp(a2.rgb, a1.rgb, a0.rgb);
    if (op == 25u) return saturate(dot((a1.rgb - 0.5f) * 2.0f, (a2.rgb - 0.5f) * 2.0f)).xxx;
    return a1.rgb * a2.rgb;
}

float EvalAlphaOp(uint op, float a1, float a2, float a0, float4 diffuse, float4 current, float4 tex, float4 tfactor) {
    if (op == 2u)  return a1;
    if (op == 3u)  return a2;
    if (op == 4u)  return a1 * a2;
    if (op == 5u)  return saturate(a1 * a2 * 2.0f);
    if (op == 6u)  return saturate(a1 * a2 * 4.0f);
    if (op == 7u)  return saturate(a1 + a2);
    if (op == 8u)  return saturate(a1 + a2 - 0.5f);
    if (op == 9u)  return saturate((a1 + a2 - 0.5f) * 2.0f);
    if (op == 10u) return saturate(a1 - a2);
    if (op == 11u) return saturate(a1 + a2 - a1 * a2);
    if (op == 12u) return lerp(a2, a1, diffuse.a);
    if (op == 13u) return lerp(a2, a1, current.a);
    if (op == 14u) return lerp(a2, a1, tex.a);
    if (op == 15u) return lerp(a2, a1, tfactor.a);
    if (op == 16u) return saturate(a1 + a2 * (1.0f - tex.a));
    if (op == 17u) return saturate(a1 + a2 * (1.0f - current.a));
    if (op == 23u) return saturate(a0 + a1 * a2);
    if (op == 24u) return lerp(a2, a1, a0);
    return a1 * a2;
}

void EvalStage(uint4 colorOps, uint4 alphaOps, float4 diffuse, inout float4 current, float4 tex, float4 tfactor) {
    float4 c1 = EvalArg(colorOps.y, diffuse, current, tex, tfactor);
    float4 c2 = EvalArg(colorOps.z, diffuse, current, tex, tfactor);
    float4 c0 = EvalArg(colorOps.w, diffuse, current, tex, tfactor);
    current.rgb = EvalColorOp(colorOps.x, c1, c2, c0, diffuse, current, tex, tfactor);
    if (alphaOps.x > 1u) {
        float4 a1 = EvalArg(alphaOps.y, diffuse, current, tex, tfactor);
        float4 a2 = EvalArg(alphaOps.z, diffuse, current, tex, tfactor);
        float4 a0 = EvalArg(alphaOps.w, diffuse, current, tex, tfactor);
        current.a = EvalAlphaOp(alphaOps.x, a1.a, a2.a, a0.a, diffuse, current, tex, tfactor);
    }
}

float4 PSMain(PSIn input) : SV_TARGET {
    float4 diffuse = input.color;
    float4 current = diffuse;
    uint texMask = uFlags.y;
    uint psMode  = uFlags.w;

    float2 uv0 = input.uv0;
    float2 uv1 = input.uv1;
    float2 uv2 = input.uv2;
    float2 uv3 = input.uv3;

    float4 t0 = ((texMask & 1u) != 0u) ? tex0.Sample(smp0, uv0) : float4(1.0f, 1.0f, 1.0f, 1.0f);
    float4 t1 = ((texMask & 2u) != 0u) ? tex1.Sample(smp1, uv1) : float4(1.0f, 1.0f, 1.0f, 1.0f);

    // Apply BumpEnv UV offset if stage 0 / stage 1 uses BUMPENVMAP (22) or texbem t1, t0 pixel shaders (emBump=1, lava=2, heatShimmer=3)
    if (uStage0Ops.x == 22u || psMode == 1u || psMode == 2u || psMode == 3u) {
        float2 d = t0.rg * 2.0f - 1.0f;
        uv1 += float2(d.x * uBumpParams.x + d.y * uBumpParams.z, d.x * uBumpParams.y + d.y * uBumpParams.w);
        if ((texMask & 2u) != 0u) t1 = tex1.Sample(smp1, uv1);
    }
    if (uStage1Ops.x == 22u) {
        float2 d = t1.rg * 2.0f - 1.0f;
        uv2 += float2(d.x * uBumpParams.x + d.y * uBumpParams.z, d.x * uBumpParams.y + d.y * uBumpParams.w);
    }

    float4 t2 = ((texMask & 4u) != 0u) ? tex2.Sample(smp2, uv2) : float4(1.0f, 1.0f, 1.0f, 1.0f);
    float4 t3 = ((texMask & 8u) != 0u) ? tex3.Sample(smp3, uv3) : float4(1.0f, 1.0f, 1.0f, 1.0f);

    if (psMode > 0u) {
        uint pt = psMode - 1u;
        if (pt == 0u) { // emBump.xpu: tex t0; texbem t1, t0; mov r0, t1
            current = t1;
        } else if (pt == 1u) { // lava.xpu: tex t0; texbem t1, t0; tex t2; mad r0, t1.a, t2, t1
            current.rgb = saturate(t1.a * t2.rgb + t1.rgb);
            current.a   = saturate(t1.a * t2.b   + t1.b);
        } else if (pt == 2u) { // heatShimmer.xpu: tex t0; texbem t1, t0; tex t2; mul r0.rgb, t1, t2; mul r0.a, t2.a, v0.a
            current.rgb = t1.rgb * t2.rgb;
            current.a   = t2.a * diffuse.a;
        } else if (pt == 3u) { // terrain_fog.xpu: outputs fog.rgb and diffuse.a
            current.rgb = uFogColor.rgb;
            current.a   = saturate(diffuse.a);
        } else if (pt == 4u) { // ghostsuit.xpu: add r1.rgb, t1, c1; mov r0.a, t0.a; xmmc r0, t0, v0, c0, r1 (r0.a>=0.5 ? CD : AB)
            float3 r1 = saturate(t1.rgb + uPsConst1.rgb);
            current.rgb = (t0.a >= 0.5f) ? (uPsConst0.rgb * r1)   : (t0.rgb * diffuse.rgb);
            current.a   = (t0.a >= 0.5f) ? (uPsConst0.a   * r1.b) : (t0.a   * diffuse.a);
        } else if (pt == 5u) { // selfglow.xpu: mad r0, c0, t0.a, v0; mul r0, r0, t0
            float4 r0 = saturate(uPsConst0 * t0.a + diffuse);
            current = r0 * t0;
        } else if (pt == 6u) { // bumpreflect.xpu: tex t0; texm3x3vspec t3; mul r0, t3, c0
            float2 pert = (t0.rg * 2.0f - 1.0f) * 0.25f;
            float4 refl = ((texMask & 8u) != 0u) ? tex3.Sample(smp3, uv3 + pert) : t3;
            current = refl * uPsConst0;
        } else if (pt == 7u) { // waterNM.xpu: add_d2 r0, t0, t1
            current = (t0 + t1) * 0.5f;
        } else if (pt == 8u) { // screenHaze.xpu: 7-stage register combiner (MUX: r0.a>=0.5 ? CD : AB)
            float r0a = dot(t1.rgb, uPsConst0.aaa);
            float r1a = dot(t2.rgb, uPsConst0.aaa);
            float4 r0 = (r0a >= 0.5f) ? t1 : float4(uPsConst0.rgb, uPsConst0.b);
            float4 r1 = (r1a >= 0.5f) ? t2 : float4(uPsConst0.rgb, uPsConst0.b);
            r1.rgb = saturate(r0.rgb * uPsConst1.rgb + r1.rgb * uPsConst1.rgb);
            r1.a   = saturate(r0.a   * uPsConst1.b   + r1.a   * uPsConst1.b);
            r0a = dot(t3.rgb, uPsConst0.aaa);
            r0 = (r0a >= 0.5f) ? t3 : float4(uPsConst0.rgb, uPsConst0.b);
            current.rgb = saturate(uPsConst1.rgb * r0.rgb + r1.rgb);
            current.a   = saturate(uPsConst1.a);
        } else if (pt == 9u) { // terrain.xpu
            float3 r0 = lerp(t2.rgb, t1.rgb, t0.a);
            r0 = saturate(r0 * t0.rgb * 2.0f);
            r0 = saturate(r0 * t3.rgb * 2.0f);
            current.rgb = r0 * diffuse.rgb;
            current.a   = 1.0f;
        } else if (pt == 10u) { // terrain_nodetail.xpu
            float3 r0 = lerp(t2.rgb, t1.rgb, t0.a);
            current.rgb = saturate(r0 * t0.rgb * 2.0f) * diffuse.rgb;
            current.a   = 1.0f;
        } else if (pt == 11u || pt == 14u) { // gaussBlurSource.xpu / gaussBlur_Source_ms.xpu
            float4 r0 = saturate(t0 - uPsConst0);
            float4 r1 = saturate(r0 * 2.0f);
            r1.rgb = saturate(uPsConst2.rgb - r1.rgb);
            r1.a   = saturate(uPsConst2.b   - r1.a);
            r1 = saturate((r1 - uPsConst2.a) * 2.0f);
            r1 = t1 * r1 * uPsConst1;
            current = saturate(r0 + r1);
        } else if (pt == 12u || pt == 13u) { // gaussBlur.xpu / gaussBlur_ms.xpu
            current = saturate(saturate(t0 * uPsConst0 + t1 * uPsConst1) + saturate(t2 * uPsConst2 + t3 * uPsConst3));
        } else if (pt == 15u) { // sightFinal.xpu: tex t0; texreg2gb t1, t0; mov r0, t1
            current = ((texMask & 2u) != 0u) ? tex1.Sample(smp1, t0.gb) : t0;
        } else if (pt == 16u) { // cloak.xpu: t0=backbuffer, t1=membrane, t2=mask, t3=base
            float3 r1 = saturate(t1.a * t1.rgb + t0.rgb);
            float3 r0 = lerp(t3.rgb, t3.rgb * diffuse.rgb, uPsConst1.a);
            r0 = lerp(r1, r0, uPsConst0.rgb);
            current.rgb = (t2.a >= 0.5f) ? r0 : r1;
            current.a   = uPsConst0.a;
        } else if (pt == 17u) { // cloakDissolve.xpu: t0=backbuffer, t1=membrane, t2=dissolve, t3=base
            float3 r0 = saturate(t1.a * t1.rgb + t0.rgb);
            float3 r1 = lerp(t3.rgb, t3.rgb * diffuse.rgb, uPsConst1.a);
            r1 = ((t2.b - (uPsConst2.a - 0.5f)) >= 0.5f) ? r1 : uPsConst2.rgb;
            float3 blended = ((uPsConst2.a - (t2.b - 0.5f)) >= 0.5f) ? r0 : r1;
            current.rgb = lerp(blended, uPsConst3.rgb, uPsConst3.a);
            current.a   = uPsConst0.a;
        } else if (pt == 18u) { // cloakDissolveShadow.xpu: t0=dissolve
            float r0a = t0.b - (uPsConst1.a - 0.5f);
            current.rgb = ((r0a >= 0.5f) ? uPsConst1.rgb : uPsConst0.rgb) * uPsConst0.a;
            current.a   = saturate(r0a * uPsConst0.a);
        } else if (pt == 19u) { // desaturate.xpu: max(r,g,b) + c1, then * c0
            float m = max(t0.r, max(t0.g, t0.b));
            current.rgb = saturate(m + uPsConst1.rgb) * uPsConst0.rgb;
            current.a   = m;
        } else if (pt == 20u) { // shield.xpu: texreg2ar t1, t0 + membrane t2 + dissolve t3 (cnd r1, r0.a, r0, c1)
            float4 s1 = ((texMask & 2u) != 0u) ? tex1.Sample(smp1, t0.ar) : t1;
            float4 r1 = t2 * uPsConst0.a;
            float4 r0 = saturate(s1 + r1) * r1.a;
            float r0a = saturate(t3.b - uPsConst0.b);
            r1 = (r0a >= 0.5f) ? float4(r0.rgb, r0a) : uPsConst1;
            current.rgb = r1.rgb;
            current.a   = r0a;
        } else if (pt == 21u) { // shieldBlur.xpu: 4-tap signed alpha blur + c0.a bias
            float r0a = ((t0.a - 0.5f) + (t1.a - 0.5f)) * 0.5f;
            float r1a = ((t2.a - 0.5f) + (t3.a - 0.5f)) * 0.5f;
            r0a = saturate((r0a + r1a) * 0.5f + uPsConst0.a);
            current = float4(r0a, r0a, r0a, r0a);
        } else if (pt == 22u) { // shield_part1.xpu: 4-tap wave Laplacian propagation
            float r0a = clamp((t1.a - t0.a) + (t2.a - t0.a) + (t3.a - t0.a), -1.0f, 1.0f);
            float r1a = (uPsConst0.a - t0.a) * uPsConst1.a + r0a;
            r0a = saturate(r1a + uPsConst0.a);
            current = float4(r0a, r0a, r0a, 1.0f);
        } else if (pt == 23u) { // shield_part2.xpu: wave height/velocity update
            float d = dot(t0.rgb, uPsConst2.rgb);
            float r0a = t1.a - t0.a;
            float4 r0 = saturate(float4(d + r0a, d + r0a, d + r0a, d + r0a));
            float4 r1 = clamp((float4(t0.rgb, t0.b) - uPsConst1) * 2.0f, -1.0f, 1.0f);
            r1.rgb = ((2.0f * r0.rgb - 1.0f) * uPsConst0.rgb + r1.rgb * uPsConst3.a) * 0.5f;
            r1.a   = ((2.0f * r0.a   - 1.0f) * uPsConst0.b   + r1.a   * uPsConst3.a) * 0.5f;
            r1.rgb = saturate(r1.rgb + uPsConst1.rgb);
            r1.a   = saturate(r1.a   + uPsConst1.b);
            float r0a2 = clamp((t0.a - uPsConst1.a) * 2.0f, -1.0f, 1.0f);
            r0a2 = ((2.0f * r1.a - 1.0f) * uPsConst0.a + r0a2) * 0.5f;
            current.rgb = r1.rgb;
            current.a   = saturate(r0a2 + uPsConst1.a);
        } else if (pt == 24u) { // dof.xpu: texreg2ar t1, t0 + xmmc r0.a, t1.a, c1.a, 1, r0.b (r0.a>=0.5 ? CD : AB)
            float4 s1 = ((texMask & 2u) != 0u) ? tex1.Sample(smp1, t0.ar) : t1;
            float r0_rgb = saturate(((2.0f * saturate(s1.a) - 1.0f) * uPsConst0.a + uPsConst1.a) * 0.5f);
            float r1_rgb = saturate((2.0f * saturate(s1.a) - 1.0f) * uPsConst1.a);
            float r0_a   = (s1.a >= 0.5f) ? r0_rgb : saturate(s1.a * uPsConst1.a);
            current.rgb  = lerp(t2.rgb, t3.rgb, r1_rgb);
            current.a    = saturate(r0_a);
        } else if (pt == 25u) { // alphadissolve.xpu: sub r0.a, t0.a, c0_bias.a; cnd r0.rgb, r0.a, t0, t1
            float r0a = t0.a - (saturate(uPsConst0.a) - 0.5f);
            current.rgb = (r0a >= 0.5f) ? t0.rgb : t1.rgb;
            current.a   = saturate(r0a);
        } else if (pt == 26u) { // alphadissolve_sep.xpu: sub r0.a, t1.b, c0_bias.a; cnd r0.rgb, r0.a, t0, c1
            float r0a = t1.b - (saturate(uPsConst0.a) - 0.5f);
            current.rgb = (r0a >= 0.5f) ? t0.rgb : uPsConst1.rgb;
            current.a   = saturate(r0a);
        } else { // pt == 27u: alphadissolve_sep_glo.xpu: add r0, t1.b, v0; mul r1.rgb, r0, t0; cnd r1, r0.a, r1, c1; cnd r0, r0.a, r1, c2
            float4 r1 = float4(saturate(t1.bbb + diffuse.rgb) * t0.rgb, t0.a);
            float r0a = t2.b - (saturate(uPsConst0.a) - 0.5f);
            r1 = (r0a >= 0.5f) ? r1 : uPsConst1;
            r0a = t2.b - (saturate(uPsConst0.b) - 0.5f);
            current = (r0a >= 0.5f) ? r1 : uPsConst2;
        }
    } else if (uStage0Ops.x > 1u) {
        EvalStage(uStage0Ops, uStage0Alpha, diffuse, current, t0, uTFactor);
        if (uStage1Ops.x > 1u) {
            EvalStage(uStage1Ops, uStage1Alpha, diffuse, current, t1, uTFactor);
            if (uStage2Ops.x > 1u) {
                EvalStage(uStage2Ops, uStage2Alpha, diffuse, current, t2, uTFactor);
                if (uStage3Ops.x > 1u) {
                    EvalStage(uStage3Ops, uStage3Alpha, diffuse, current, t3, uTFactor);
                }
            }
        }
    }

    if (uFlags.z != 0u) {
        uint fn = (uint)uAlphaTest.x;
        float refVal = uAlphaTest.y;
        bool passAlpha = true;
        if (fn == 1u)      passAlpha = false;
        else if (fn == 2u) passAlpha = (current.a <  refVal - 0.001f);
        else if (fn == 3u) passAlpha = (abs(current.a - refVal) < 0.004f);
        else if (fn == 4u) passAlpha = (current.a <= refVal + 0.001f);
        else if (fn == 5u) passAlpha = (current.a >  refVal + 0.001f);
        else if (fn == 6u) passAlpha = (abs(current.a - refVal) >= 0.004f);
        else if (fn == 7u) passAlpha = (current.a >= refVal - 0.001f);
        if (!passAlpha) discard;
    }

    // Hardware Distance & Atmospheric Fog (3D geometry only)
    if (uFogParams.x > 0.5f && uFlags.x == 0u) {
        float fogMode = uFogParams.x;
        float d = input.eyeDist;
        float fogFactor = 1.0f;
        if (fogMode > 2.5f) { // D3DFOG_LINEAR (3)
            float span = max(uFogParams.z - uFogParams.y, 0.001f);
            fogFactor = saturate((uFogParams.z - d) / span);
        } else if (fogMode < 1.5f) { // D3DFOG_EXP (1)
            fogFactor = saturate(exp(-d * uFogParams.w));
        } else { // D3DFOG_EXP2 (2)
            float df = d * uFogParams.w;
            fogFactor = saturate(exp(-(df * df)));
        }
        float fogAmt = 1.0f - fogFactor;
        if (uFogColor.w > 0.5f) {
            current.rgb *= fogFactor;
        } else {
            current.rgb = lerp(current.rgb, uFogColor.rgb, fogAmt);
        }
    }

    // Gamma & Brightness adjustment
    if (uAlphaTest.z > 0.1f && abs(uAlphaTest.z - 1.0f) > 0.01f) {
        current.rgb = pow(max(current.rgb, 0.0f), 1.0f / uAlphaTest.z) * uAlphaTest.w;
    }

    return current;
}
)";

// ============================================================================
// RenderState, BlendState, DepthStencilState, RasterizerState, SamplerState Cache
// ============================================================================

static D3D11_COMPARISON_FUNC MapXboxCmpFuncD3D11(uint32_t v) {
    uint32_t idx = v;
    if (v >= 0x200 && v <= 0x207) idx = (v - 0x200) + 1;
    if (idx >= 1 && idx <= 8) return (D3D11_COMPARISON_FUNC)idx;
    return D3D11_COMPARISON_LESS_EQUAL;
}

static uint32_t MapXboxCmpFuncInt(uint32_t v) {
    if (v >= 0x200 && v <= 0x207) return (v - 0x200) + 1;
    if (v >= 1 && v <= 8) return v;
    return 8; // ALWAYS
}

static D3D11_BLEND MapXboxBlendD3D11(uint32_t v, bool isAlpha) {
    D3D11_BLEND b = D3D11_BLEND_ONE;
    switch (v) {
        case 0x0000:          b = D3D11_BLEND_ZERO; break;
        case 0x0001: case 2:  b = D3D11_BLEND_ONE; break;
        case 0x0300: case 3:  b = D3D11_BLEND_SRC_COLOR; break;
        case 0x0301: case 4:  b = D3D11_BLEND_INV_SRC_COLOR; break;
        case 0x0302: case 5:  b = D3D11_BLEND_SRC_ALPHA; break;
        case 0x0303: case 6:  b = D3D11_BLEND_INV_SRC_ALPHA; break;
        case 0x0304: case 7:  b = D3D11_BLEND_DEST_ALPHA; break;
        case 0x0305: case 8:  b = D3D11_BLEND_INV_DEST_ALPHA; break;
        case 0x0306: case 9:  b = D3D11_BLEND_DEST_COLOR; break;
        case 0x0307: case 10: b = D3D11_BLEND_INV_DEST_COLOR; break;
        case 0x0308: case 11: b = D3D11_BLEND_SRC_ALPHA_SAT; break;
        default:              b = D3D11_BLEND_ONE; break;
    }
    if (isAlpha) {
        if (b == D3D11_BLEND_SRC_COLOR)      return D3D11_BLEND_SRC_ALPHA;
        if (b == D3D11_BLEND_INV_SRC_COLOR)  return D3D11_BLEND_INV_SRC_ALPHA;
        if (b == D3D11_BLEND_DEST_COLOR)     return D3D11_BLEND_DEST_ALPHA;
        if (b == D3D11_BLEND_INV_DEST_COLOR) return D3D11_BLEND_INV_DEST_ALPHA;
    }
    return b;
}

static D3D11_BLEND_OP MapXboxBlendOpD3D11(uint32_t v) {
    switch (v) {
        case 0x8006: case 1: return D3D11_BLEND_OP_ADD;
        case 0x800A: case 2: return D3D11_BLEND_OP_SUBTRACT;
        case 0x800B: case 3: return D3D11_BLEND_OP_REV_SUBTRACT;
        case 0x8007: case 4: return D3D11_BLEND_OP_MIN;
        case 0x8008: case 5: return D3D11_BLEND_OP_MAX;
        default:             return D3D11_BLEND_OP_ADD;
    }
}

struct CachedBlendState {
    uint64_t           key;
    ID3D11BlendState*  state;
};
static std::vector<CachedBlendState> g_blendCache;

static ID3D11BlendState* GetBlendState(uint32_t alphaBlendEnable, uint32_t srcBlend, uint32_t dstBlend, uint32_t blendOp, uint32_t colorWrite) {
    uint64_t key = ((uint64_t)(alphaBlendEnable ? 1 : 0)) |
                   ((uint64_t)(srcBlend & 0xFFFF) << 1) |
                   ((uint64_t)(dstBlend & 0xFFFF) << 17) |
                   ((uint64_t)(blendOp & 0xFFFF) << 33) |
                   ((uint64_t)(colorWrite & 0xFF) << 49);
    for (size_t i = 0; i < g_blendCache.size(); ++i) {
        if (g_blendCache[i].key == key) return g_blendCache[i].state;
    }
    D3D11_BLEND_DESC desc = {};
    desc.RenderTarget[0].BlendEnable = alphaBlendEnable ? TRUE : FALSE;
    desc.RenderTarget[0].SrcBlend = MapXboxBlendD3D11(srcBlend, false);
    desc.RenderTarget[0].DestBlend = MapXboxBlendD3D11(dstBlend, false);
    desc.RenderTarget[0].BlendOp = MapXboxBlendOpD3D11(blendOp);
    desc.RenderTarget[0].SrcBlendAlpha = MapXboxBlendD3D11(srcBlend, true);
    desc.RenderTarget[0].DestBlendAlpha = MapXboxBlendD3D11(dstBlend, true);
    desc.RenderTarget[0].BlendOpAlpha = MapXboxBlendOpD3D11(blendOp);
    desc.RenderTarget[0].RenderTargetWriteMask = (UINT8)(colorWrite & 0x0F);

    ID3D11BlendState* st = nullptr;
    g_d3dDevice->CreateBlendState(&desc, &st);
    g_blendCache.push_back({ key, st });
    return st;
}

struct CachedDepthState {
    uint64_t                 key;
    ID3D11DepthStencilState* state;
};
static std::vector<CachedDepthState> g_depthCache;

static D3D11_STENCIL_OP MapXboxStencilOpD3D11(uint32_t v) {
    switch (v) {
        case 0x0000: case 2: return D3D11_STENCIL_OP_ZERO;
        case 0x1E00: case 1: return D3D11_STENCIL_OP_KEEP;
        case 0x1E01: case 3: return D3D11_STENCIL_OP_REPLACE;
        case 0x1E02: case 4: return D3D11_STENCIL_OP_INCR_SAT;
        case 0x1E03: case 5: return D3D11_STENCIL_OP_DECR_SAT;
        case 0x150A: case 6: return D3D11_STENCIL_OP_INVERT;
        case 0x8507: case 7: return D3D11_STENCIL_OP_INCR;
        case 0x8508: case 8: return D3D11_STENCIL_OP_DECR;
        default:             return D3D11_STENCIL_OP_KEEP;
    }
}

static ID3D11DepthStencilState* GetDepthState(
    uint32_t zEnable, uint32_t zWriteEnable, uint32_t zFunc,
    uint32_t stencilEnable, uint32_t stencilFail, uint32_t stencilZFail, uint32_t stencilPass,
    uint32_t stencilFunc, uint32_t stencilReadMask, uint32_t stencilWriteMask)
{
    D3D11_COMPARISON_FUNC dFunc   = MapXboxCmpFuncD3D11(zFunc);
    D3D11_COMPARISON_FUNC sFunc   = MapXboxCmpFuncD3D11(stencilFunc);
    D3D11_STENCIL_OP      sFail   = MapXboxStencilOpD3D11(stencilFail);
    D3D11_STENCIL_OP      sZFail  = MapXboxStencilOpD3D11(stencilZFail);
    D3D11_STENCIL_OP      sPass   = MapXboxStencilOpD3D11(stencilPass);
    uint8_t               sRMask  = (uint8_t)(stencilReadMask & 0xFFu);
    uint8_t               sWMask  = (uint8_t)(stencilWriteMask & 0xFFu);

    uint64_t key =
        (uint64_t)(zEnable ? 1u : 0u) |
        ((uint64_t)(zWriteEnable ? 1u : 0u) << 1) |
        ((uint64_t)(dFunc & 0xFu) << 2) |
        ((uint64_t)(stencilEnable ? 1u : 0u) << 6) |
        ((uint64_t)(sFail & 0xFu) << 7) |
        ((uint64_t)(sZFail & 0xFu) << 11) |
        ((uint64_t)(sPass & 0xFu) << 15) |
        ((uint64_t)(sFunc & 0xFu) << 19) |
        ((uint64_t)sRMask << 24) |
        ((uint64_t)sWMask << 32);

    for (size_t i = 0; i < g_depthCache.size(); ++i) {
        if (g_depthCache[i].key == key) return g_depthCache[i].state;
    }
    D3D11_DEPTH_STENCIL_DESC desc = {};
    desc.DepthEnable      = zEnable ? TRUE : FALSE;
    desc.DepthWriteMask   = zWriteEnable ? D3D11_DEPTH_WRITE_MASK_ALL : D3D11_DEPTH_WRITE_MASK_ZERO;
    desc.DepthFunc        = dFunc;
    desc.StencilEnable    = stencilEnable ? TRUE : FALSE;
    desc.StencilReadMask  = sRMask;
    desc.StencilWriteMask = sWMask;
    desc.FrontFace.StencilFailOp      = sFail;
    desc.FrontFace.StencilDepthFailOp = sZFail;
    desc.FrontFace.StencilPassOp      = sPass;
    desc.FrontFace.StencilFunc        = sFunc;
    desc.BackFace                     = desc.FrontFace;

    ID3D11DepthStencilState* st = nullptr;
    g_d3dDevice->CreateDepthStencilState(&desc, &st);
    g_depthCache.push_back({ key, st });
    return st;
}

struct CachedRastState {
    uint32_t               key;
    ID3D11RasterizerState* state;
};
static std::vector<CachedRastState> g_rastCache;
static bool g_scissorEnabled = false;

static ID3D11RasterizerState* GetRastState(uint32_t cullMode, uint32_t fillMode, uint32_t zBias, bool scissor) {
    uint32_t key = (cullMode & 0xFFFFu) | ((fillMode & 0xFFu) << 16) | ((zBias & 0x3Fu) << 24) | (scissor ? 0x80000000u : 0u);
    for (size_t i = 0; i < g_rastCache.size(); ++i) {
        if (g_rastCache[i].key == key) return g_rastCache[i].state;
    }
    D3D11_RASTERIZER_DESC desc = {};
    desc.FillMode = (fillMode == 0x1B01 || fillMode == 2) ? D3D11_FILL_WIREFRAME : D3D11_FILL_SOLID;
    if (cullMode == 0 || cullMode == 1) {
        desc.CullMode = D3D11_CULL_NONE;
    } else if (cullMode == 0x0900 || cullMode == 2) {
        desc.CullMode = D3D11_CULL_FRONT;
    } else {
        desc.CullMode = D3D11_CULL_BACK;
    }
    desc.FrontCounterClockwise = FALSE;
    desc.DepthBias = -((INT)(int32_t)zBias) * 16;
    desc.DepthClipEnable = TRUE;
    desc.ScissorEnable = scissor ? TRUE : FALSE;

    ID3D11RasterizerState* st = nullptr;
    g_d3dDevice->CreateRasterizerState(&desc, &st);
    g_rastCache.push_back({ key, st });
    return st;
}

static constexpr uint32_t kXboxTexAddressWrap        = 1u;
static constexpr uint32_t kXboxTexAddressMirror      = 2u;
static constexpr uint32_t kXboxTexAddressClamp       = 3u;
static constexpr uint32_t kXboxTexAddressBorder      = 4u;
static constexpr uint32_t kXboxTexFilterPoint        = 1u;
static constexpr uint32_t kXboxTssBorderColorIndex   = XBOX_D3DTSS_BORDERCOLOR;
static constexpr uint32_t kXboxTssColorKeyColorIndex = XBOX_D3DTSS_COLORKEYCOLOR;

struct CachedSamplerState {
    uint32_t            key;
    uint32_t            borderColor;
    ID3D11SamplerState* state;
};
static std::vector<CachedSamplerState> g_samplerCache;

static D3D11_TEXTURE_ADDRESS_MODE MapXboxAddress(uint32_t v) {
    switch (v) {
        case kXboxTexAddressWrap:   return D3D11_TEXTURE_ADDRESS_WRAP;
        case kXboxTexAddressMirror: return D3D11_TEXTURE_ADDRESS_MIRROR;
        case kXboxTexAddressClamp:  return D3D11_TEXTURE_ADDRESS_CLAMP;
        case kXboxTexAddressBorder: return D3D11_TEXTURE_ADDRESS_BORDER;
        default:                    return D3D11_TEXTURE_ADDRESS_WRAP;
    }
}

static ID3D11SamplerState* GetSamplerState(uint32_t addrU, uint32_t addrV, uint32_t magFilter, uint32_t borderColor) {
    uint32_t key = (addrU & 0xFu) | ((addrV & 0xFu) << 4) | ((magFilter & 0xFu) << 8);
    uint32_t effBorder = (addrU == kXboxTexAddressBorder || addrV == kXboxTexAddressBorder) ? borderColor : 0u;
    for (size_t i = 0; i < g_samplerCache.size(); ++i) {
        if (g_samplerCache[i].key == key && g_samplerCache[i].borderColor == effBorder) {
            return g_samplerCache[i].state;
        }
    }
    D3D11_SAMPLER_DESC desc = {};
    desc.Filter = (magFilter == kXboxTexFilterPoint) ? D3D11_FILTER_MIN_MAG_MIP_POINT : D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    desc.AddressU = MapXboxAddress(addrU);
    desc.AddressV = MapXboxAddress(addrV);
    desc.AddressW = D3D11_TEXTURE_ADDRESS_WRAP;
    desc.ComparisonFunc = D3D11_COMPARISON_NEVER;
    desc.MaxLOD = D3D11_FLOAT32_MAX;
    if (effBorder != 0u) {
        desc.BorderColor[0] = ((effBorder >> 16) & 0xFFu) * (1.0f / 255.0f);
        desc.BorderColor[1] = ((effBorder >> 8)  & 0xFFu) * (1.0f / 255.0f);
        desc.BorderColor[2] = (effBorder         & 0xFFu) * (1.0f / 255.0f);
        desc.BorderColor[3] = ((effBorder >> 24) & 0xFFu) * (1.0f / 255.0f);
    }

    ID3D11SamplerState* st = nullptr;
    g_d3dDevice->CreateSamplerState(&desc, &st);
    g_samplerCache.push_back({ key, effBorder, st });
    return st;
}

// ============================================================================
// RenderState Bridges (Xbox NV2A -> _D3D__RenderState)
// ============================================================================

// @D3DDevice_SetRenderState_Simple@8 (0x006AEC10)
static void __fastcall Bridge_D3DDevice_SetRenderState_Simple(uint32_t encodedReg, uint32_t val) {
    const uint32_t* table = reinterpret_cast<const uint32_t*>(ADDR_SIMPLE_RS_ENCODE);
    uint32_t* xboxRS = reinterpret_cast<uint32_t*>(ADDR_D3D_RENDERSTATE);
    for (uint32_t i = 0; i < 92; ++i) {
        if (table[i] == encodedReg) {
            xboxRS[i] = val;
            return;
        }
    }
}

static void __stdcall Bridge_D3DDevice_SetRenderStateNotInline(uint32_t State, uint32_t Value) {
    if (State < XBOX_D3DRS_MAX) {
        reinterpret_cast<uint32_t*>(ADDR_D3D_RENDERSTATE)[State] = Value;
    }
}

static void __stdcall Bridge_D3DDevice_SetTextureStageStateNotInline(uint32_t Stage, uint32_t Type, uint32_t Value) {
    if (Stage < 4 && Type < XBOX_D3DTSS_MAX) {
        reinterpret_cast<uint32_t*>(ADDR_D3D_TEXTURESTATE)[Stage * XBOX_D3DTSS_MAX + Type] = Value;
    }
}

static void __stdcall Bridge_D3DDevice_SetTextureState_TexCoordIndex(uint32_t Stage, uint32_t Value) {
    if (Stage < 4) {
        reinterpret_cast<uint32_t*>(ADDR_D3D_TEXTURESTATE)[Stage * XBOX_D3DTSS_MAX + XBOX_D3DTSS_TEXCOORDINDEX] = Value;
    }
}

static void __stdcall Bridge_D3DDevice_SetTextureState_BumpEnv(uint32_t Stage, uint32_t Type, uint32_t Value) {
    if (Stage < 4 && Type < XBOX_D3DTSS_MAX) {
        reinterpret_cast<uint32_t*>(ADDR_D3D_TEXTURESTATE)[Stage * XBOX_D3DTSS_MAX + Type] = Value;
    }
}

static void __stdcall Bridge_D3DDevice_SetTextureState_BorderColor(uint32_t Stage, uint32_t Value) {
    if (Stage < 4) {
        reinterpret_cast<uint32_t*>(ADDR_D3D_TEXTURESTATE)[Stage * XBOX_D3DTSS_MAX + XBOX_D3DTSS_BORDERCOLOR] = Value;
    }
}

static void __stdcall Bridge_D3DDevice_SetTextureState_ColorKeyColor(uint32_t Stage, uint32_t Value) {
    if (Stage < 4) {
        reinterpret_cast<uint32_t*>(ADDR_D3D_TEXTURESTATE)[Stage * XBOX_D3DTSS_MAX + XBOX_D3DTSS_COLORKEYCOLOR] = Value;
    }
}

static void __stdcall Bridge_D3DDevice_SetRenderState_FogColor(uint32_t Value) {
    reinterpret_cast<uint32_t*>(ADDR_D3D_RENDERSTATE)[XBOX_D3DRS_FOGCOLOR] = Value;
}

static void __stdcall Bridge_D3DDevice_SetRenderState_FillMode(uint32_t Value) {
    reinterpret_cast<uint32_t*>(ADDR_D3D_RENDERSTATE)[XBOX_D3DRS_FILLMODE] = Value;
}

static void __stdcall Bridge_D3DDevice_SetRenderState_ZEnable(uint32_t Value) {
    reinterpret_cast<uint32_t*>(ADDR_D3D_RENDERSTATE)[XBOX_D3DRS_ZENABLE] = Value;
}

static void __stdcall Bridge_D3DDevice_SetRenderState_CullMode(uint32_t Value) {
    reinterpret_cast<uint32_t*>(ADDR_D3D_RENDERSTATE)[XBOX_D3DRS_CULLMODE] = Value;
}

static void __stdcall Bridge_D3DDevice_SetRenderState_TextureFactor(uint32_t Value) {
    reinterpret_cast<uint32_t*>(ADDR_D3D_RENDERSTATE)[XBOX_D3DRS_TEXTUREFACTOR] = Value;
}

static void __stdcall Bridge_D3DDevice_SetRenderState_ZBias(uint32_t Value) {
    reinterpret_cast<uint32_t*>(ADDR_D3D_RENDERSTATE)[XBOX_D3DRS_ZBIAS] = Value;
}

static void __stdcall Bridge_D3DDevice_SetRenderState_StencilEnable(uint32_t Value) {
    reinterpret_cast<uint32_t*>(ADDR_D3D_RENDERSTATE)[XBOX_D3DRS_STENCILENABLE] = Value;
}

static void __stdcall Bridge_D3DDevice_SetRenderState_StencilFail(uint32_t Value) {
    reinterpret_cast<uint32_t*>(ADDR_D3D_RENDERSTATE)[XBOX_D3DRS_STENCILFAIL] = Value;
}

// ============================================================================
// Texture Cache & Xbox Swizzled/Linear/DXT Upload to D3D11
// ============================================================================

struct CachedTexture {
    XboxPixelContainer*       xboxTex;
    uint32_t                  dataPtr;
    uint32_t                  format;
    uint32_t                  sizeField;
    uint64_t                  contentHash;
    uint32_t                  lastCheckedFrame;
    size_t                    maxReadableBytes;
    bool                      dirty;
    bool                      isRenderTarget;
    bool                      isLinear;
    uint32_t                  width;
    uint32_t                  height;
    DXGI_FORMAT               dxgiFormat;
    ID3D11Texture2D*          tex2D;
    ID3D11ShaderResourceView* srv;
    ID3D11RenderTargetView*   rtv;
};

static std::vector<CachedTexture> g_texCache;

static void MarkTextureDirtyByData(uint32_t dataPtr) {
    for (size_t i = 0; i < g_texCache.size(); ++i) {
        if (g_texCache[i].dataPtr == dataPtr || reinterpret_cast<uint32_t>(g_texCache[i].xboxTex) == dataPtr) {
            g_texCache[i].dirty = true;
            g_texCache[i].isRenderTarget = false;
        }
    }
}

static size_t GetReadableBytesAt(const void* ptr) {
    if (!ptr || (uintptr_t)ptr < 0x00400000) return 0;
    MEMORY_BASIC_INFORMATION mbi = {};
    if (VirtualQuery(ptr, &mbi, sizeof(mbi)) == 0) return 0;
    if (mbi.State != MEM_COMMIT) return 0;
    if (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) return 0;
    uintptr_t regEnd = reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
    uintptr_t cur = reinterpret_cast<uintptr_t>(ptr);
    return (regEnd > cur) ? (regEnd - cur) : 0;
}

static uint64_t ComputeFastSampleHash(const void* data, size_t byteLen, size_t knownMaxReadable = 0) {
    size_t maxReadable = knownMaxReadable ? knownMaxReadable : GetReadableBytesAt(data);
    if (byteLen > maxReadable) byteLen = maxReadable;
    if (!data || byteLen < 64) return 0;
    const uint32_t* p = reinterpret_cast<const uint32_t*>(data);
    size_t dwords = byteLen / 4;
    size_t step = dwords / 64;
    if (step == 0) step = 1;
    uint64_t h = 14695981039346656037ULL;
    for (size_t i = 0; i < 64 && (i * step) < dwords; ++i) {
        h ^= p[i * step];
        h *= 1099511628211ULL;
    }
    return h;
}

static void DecodeXboxTextureFormat(
    const XboxPixelContainer* pTex,
    uint32_t* outWidth, uint32_t* outHeight, uint32_t* outLevels,
    uint32_t* outXboxFmt, bool* outSwizzled, uint32_t* outLinearPitch)
{
    uint32_t fmt = pTex->Format;
    uint32_t size = pTex->Size;
    *outXboxFmt = (fmt >> 8) & 0xFF;
    *outLevels = (fmt >> 16) & 0x0F;
    if (*outLevels == 0) *outLevels = 1;

    if (size != 0) {
        *outWidth = (size & 0xFFF) + 1;
        *outHeight = ((size >> 12) & 0xFFF) + 1;
        *outLinearPitch = (((size >> 24) & 0xFF) + 1) * 64;
        *outSwizzled = false;
    } else {
        *outWidth = 1u << ((fmt >> 20) & 0x0F);
        *outHeight = 1u << ((fmt >> 24) & 0x0F);
        *outLinearPitch = 0;
        *outSwizzled = true;
    }
}

// Helper to decode a 4x4 DXT1/3/5 block into 32-bit BGRA8 (0xAARRGGBB)
static void DecodeDXTBlockToBGRA8(
    const uint8_t* block, uint32_t xboxFmt,
    uint32_t* dstPixels, uint32_t dstWidth, uint32_t dstHeight, uint32_t bx, uint32_t by)
{
    const uint8_t* colorBlock = (xboxFmt == 0x0C) ? block : (block + 8);
    uint16_t c0 = (uint16_t)(colorBlock[0] | (colorBlock[1] << 8));
    uint16_t c1 = (uint16_t)(colorBlock[2] | (colorBlock[3] << 8));
    uint32_t lutBits = colorBlock[4] | (colorBlock[5] << 8) | (colorBlock[6] << 16) | (colorBlock[7] << 24);

    uint32_t r[4], g[4], b[4], a[4] = { 255, 255, 255, 255 };
    r[0] = ((c0 >> 11) & 0x1F) * 255 / 31;
    g[0] = ((c0 >> 5)  & 0x3F) * 255 / 63;
    b[0] = (c0         & 0x1F) * 255 / 31;
    r[1] = ((c1 >> 11) & 0x1F) * 255 / 31;
    g[1] = ((c1 >> 5)  & 0x3F) * 255 / 63;
    b[1] = (c1         & 0x1F) * 255 / 31;

    if (xboxFmt != 0x0C || c0 > c1) {
        r[2] = (2 * r[0] + r[1]) / 3;
        g[2] = (2 * g[0] + g[1]) / 3;
        b[2] = (2 * b[0] + b[1]) / 3;
        r[3] = (r[0] + 2 * r[1]) / 3;
        g[3] = (g[0] + 2 * g[1]) / 3;
        b[3] = (b[0] + 2 * b[1]) / 3;
    } else {
        r[2] = (r[0] + r[1]) / 2;
        g[2] = (g[0] + g[1]) / 2;
        b[2] = (b[0] + b[1]) / 2;
        r[3] = 0; g[3] = 0; b[3] = 0; a[3] = 0;
    }

    uint8_t dxt5Alpha[8] = {};
    uint64_t dxt5AlphaBits = 0;
    if (xboxFmt == 0x0F) {
        dxt5Alpha[0] = block[0];
        dxt5Alpha[1] = block[1];
        if (dxt5Alpha[0] > dxt5Alpha[1]) {
            for (int i = 1; i <= 6; ++i)
                dxt5Alpha[i + 1] = (uint8_t)(((7 - i) * dxt5Alpha[0] + i * dxt5Alpha[1]) / 7);
        } else {
            for (int i = 1; i <= 4; ++i)
                dxt5Alpha[i + 1] = (uint8_t)(((5 - i) * dxt5Alpha[0] + i * dxt5Alpha[1]) / 5);
            dxt5Alpha[6] = 0;
            dxt5Alpha[7] = 255;
        }
        for (int i = 0; i < 6; ++i)
            dxt5AlphaBits |= ((uint64_t)block[2 + i]) << (8 * i);
    }

    for (uint32_t py = 0; py < 4; ++py) {
        uint32_t y = by * 4 + py;
        if (y >= dstHeight) continue;
        uint16_t dxt3Row = (xboxFmt == 0x0E) ? (uint16_t)(block[py * 2] | (block[py * 2 + 1] << 8)) : 0;
        for (uint32_t px = 0; px < 4; ++px) {
            uint32_t x = bx * 4 + px;
            if (x >= dstWidth) continue;
            uint32_t pIdx = py * 4 + px;
            uint32_t cIdx = (lutBits >> (pIdx * 2)) & 0x3;
            uint32_t alpha = a[cIdx];
            if (xboxFmt == 0x0E) {
                alpha = ((dxt3Row >> (px * 4)) & 0xF) * 17;
            } else if (xboxFmt == 0x0F) {
                uint32_t aIdx = (uint32_t)((dxt5AlphaBits >> (pIdx * 3)) & 0x7);
                alpha = dxt5Alpha[aIdx];
            }
            dstPixels[y * dstWidth + x] = (alpha << 24) | (r[cIdx] << 16) | (g[cIdx] << 8) | b[cIdx];
        }
    }
}

// Computes the 128-byte aligned byte stride between cubemap faces in an Xbox D3DPixelContainer,
// matching FindSurfaceWithinTexture@PixelJar@D3D (0x006B6F9B..0x006B701B).
static size_t ComputeXboxCubemapFaceStride(uint32_t fmt, uint32_t xboxFmt, uint32_t levels, uint32_t bitsPerPixel) {
    constexpr uint32_t kCubeFaceAlignMask = 0x7Fu; // 128-byte alignment (0x80)
    constexpr uint32_t kFmtLogWidthShift  = 20u;
    constexpr uint32_t kFmtLogHeightShift = 24u;
    constexpr uint32_t kFmtLogMask        = 0x0Fu;
    constexpr uint32_t kMinDxtLog2Dim     = 2u;    // 4x4 minimum DXT block dimension (1 << 2)

    bool isDxt = (xboxFmt == 0x0Cu || xboxFmt == 0x0Eu || xboxFmt == 0x0Fu);
    uint32_t minLog = isDxt ? kMinDxtLog2Dim : 0u;
    uint32_t logW   = (fmt >> kFmtLogWidthShift)  & kFmtLogMask;
    uint32_t logH   = (fmt >> kFmtLogHeightShift) & kFmtLogMask;

    size_t totalMipBytes = 0;
    for (uint32_t m = 0; m < levels; ++m) {
        uint32_t effW = (logW > minLog) ? logW : minLog;
        uint32_t effH = (logH > minLog) ? logH : minLog;
        size_t mipBytes = ((size_t)1u << (effW + effH)) * bitsPerPixel / 8u;
        totalMipBytes += mipBytes;
        if (logW > 0) --logW;
        if (logH > 0) --logH;
    }
    return (totalMipBytes + kCubeFaceAlignMask) & ~(size_t)kCubeFaceAlignMask;
}

// Unswizzles a single 2D Z-slice from an Xbox 3D Morton-swizzled volume texture,
// matching XGInternalSwizzler::Init (0x0071E71F) and _XGSwizzleBox@40 (0x00720579).
static void UnswizzleBoxSliceToLinear(
    const uint8_t* srcBox, uint32_t width, uint32_t height, uint32_t depth,
    uint32_t zSlice, uint32_t bpp, uint8_t* dstSlice)
{
    uint32_t maskX = 0u, maskY = 0u, maskZ = 0u;
    uint32_t bit = 1u;
    for (uint32_t i = 1u; ; i <<= 1u) {
        uint32_t prev = bit;
        if (i < width)  { maskX |= bit; bit <<= 1u; }
        if (i < height) { maskY |= bit; bit <<= 1u; }
        if (i < depth)  { maskZ |= bit; bit <<= 1u; }
        if (bit == prev) break;
    }

    uint32_t offZ = 0u;
    for (uint32_t z = 0u; z < zSlice; ++z) {
        offZ = (offZ - maskZ) & maskZ;
    }

    uint32_t offY = 0u;
    for (uint32_t y = 0u; y < height; ++y) {
        uint8_t* dstRow = dstSlice + (size_t)y * width * bpp;
        uint32_t offX = 0u;
        for (uint32_t x = 0u; x < width; ++x) {
            uint32_t swizIdx = offX | offY | offZ;
            memcpy(dstRow + (size_t)x * bpp, srcBox + (size_t)swizIdx * bpp, bpp);
            offX = (offX - maskX) & maskX;
        }
        offY = (offY - maskY) & maskY;
    }
}

static void UploadXboxTextureToD3D11(CachedTexture& entry, const XboxPixelContainer* pTex) {
    if (!g_d3dDevice || !g_d3dCtx) return;

    constexpr uint32_t kXboxTexFormatCubemapMask   = 0x00000004u;
    constexpr uint32_t kXboxTexFormatDimShift      = 4u;
    constexpr uint32_t kXboxTexFormatDimMask       = 0x0Fu;
    constexpr uint32_t kXboxTexDimensionVolume     = 3u;
    constexpr uint32_t kXboxTexFormatDepthShift    = 28u;
    constexpr uint32_t kXboxCubemapFaceCount       = 6u;

    uint32_t w = 0, h = 0, levels = 1, xboxFmt = 0, linearPitch = 0;
    bool isSwizzled = false;
    DecodeXboxTextureFormat(pTex, &w, &h, &levels, &xboxFmt, &isSwizzled, &linearPitch);
    if (w == 0 || h == 0 || w > 4096 || h > 4096 || !pTex->Data) return;

    bool isCubeMap = (pTex->Format & kXboxTexFormatCubemapMask) != 0u;
    bool isVolume  = (((pTex->Format >> kXboxTexFormatDimShift) & kXboxTexFormatDimMask) == kXboxTexDimensionVolume);
    uint32_t depth = isVolume ? (1u << ((pTex->Format >> kXboxTexFormatDepthShift) & 0x0Fu)) : 1u;
    (void)isCubeMap; (void)depth;

    std::vector<uint32_t> bgraPixels(w * h, 0xFFFFFFFFu);
    const uint8_t* srcData = reinterpret_cast<const uint8_t*>(pTex->Data);
    size_t maxReadable = GetReadableBytesAt(srcData);
    if (maxReadable < 16) return;

    if (xboxFmt == 0x0C || xboxFmt == 0x0E || xboxFmt == 0x0F) {
        uint32_t blockBytes = (xboxFmt == 0x0C) ? 8 : 16;
        uint32_t blocksW = (w + 3) / 4;
        uint32_t blocksH = (h + 3) / 4;
        size_t sliceBytes = (size_t)blocksW * blocksH * blockBytes;
        if (sliceBytes > maxReadable) return;
        if (isVolume && depth > 1u) {
            uint32_t zSlice = depth / 2u;
            if ((size_t)(zSlice + 1u) * sliceBytes <= maxReadable) {
                srcData += (size_t)zSlice * sliceBytes;
            }
        }
        for (uint32_t by = 0; by < blocksH; ++by) {
            for (uint32_t bx = 0; bx < blocksW; ++bx) {
                const uint8_t* blk = srcData + (by * blocksW + bx) * blockBytes;
                DecodeDXTBlockToBGRA8(blk, xboxFmt, bgraPixels.data(), w, h, bx, by);
            }
        }
    } else {
        uint32_t bpp = 4;
        switch (xboxFmt) {
            case 0x00: case 0x01: case 0x0B: case 0x13: case 0x19: case 0x2E: bpp = 1; break;
            case 0x02: case 0x03: case 0x04: case 0x05: case 0x11: case 0x12:
            case 0x1A: case 0x1B: case 0x1C: case 0x1D: case 0x20: case 0x24:
            case 0x25: case 0x27: case 0x28: case 0x2D: bpp = 2; break;
            default: bpp = 4; break;
        }
        uint32_t srcPitchCheck = linearPitch ? linearPitch : (w * bpp);
        size_t sliceBytes = (size_t)srcPitchCheck * h;
        if (sliceBytes > maxReadable) return;

        // If this is a cubemap whose Face 0 (+X) is all zeros (such as directShader_pSpecularCube
        // created by cTexture::CreateFunctionTextures at 0x005EF040, which only populates Face 4 +Z),
        // advance srcData to the first non-zero face using FindSurfaceWithinTexture's exact face stride.
        if (isCubeMap) {
            bool face0AllZero = true;
            for (size_t k = 0; k < sliceBytes; k += 4) {
                if (*reinterpret_cast<const uint32_t*>(srcData + k) != 0u) {
                    face0AllZero = false;
                    break;
                }
            }
            if (face0AllZero) {
                size_t faceStride = ComputeXboxCubemapFaceStride(pTex->Format, xboxFmt, levels, bpp * 8u);
                for (uint32_t face = 1u; face < kXboxCubemapFaceCount; ++face) {
                    size_t faceOff = (size_t)face * faceStride;
                    if (faceOff + sliceBytes > maxReadable) break;
                    const uint8_t* candFace = srcData + faceOff;
                    bool hasNonZero = false;
                    for (size_t k = 0; k < sliceBytes; k += 4) {
                        if (*reinterpret_cast<const uint32_t*>(candFace + k) != 0u) {
                            hasNonZero = true;
                            break;
                        }
                    }
                    if (hasNonZero) {
                        srcData = candFace;
                        break;
                    }
                }
            }
        }

        std::vector<uint8_t> unswizzled;
        const uint8_t* linearSrc = srcData;
        uint32_t srcPitch = linearPitch ? linearPitch : (w * bpp);

        if (isSwizzled) {
            unswizzled.resize(w * h * bpp);
            if (isVolume && depth > 1u && sliceBytes * depth <= maxReadable) {
                UnswizzleBoxSliceToLinear(srcData, w, h, depth, depth / 2u, bpp, unswizzled.data());
            } else {
                g_pfnXGUnswizzleRect(srcData, w, h, nullptr, unswizzled.data(), w * bpp, nullptr, bpp);
            }
            linearSrc = unswizzled.data();
            srcPitch = w * bpp;
        }

        for (uint32_t y = 0; y < h; ++y) {
            const uint8_t* sRow = linearSrc + y * srcPitch;
            uint32_t* dRow = &bgraPixels[y * w];
            for (uint32_t x = 0; x < w; ++x) {
                uint32_t argb = 0xFFFFFFFFu;
                switch (xboxFmt) {
                    case 0x06: // Swizzled A8R8G8B8
                    case 0x1E: // Linear A8R8G8B8
                        argb = reinterpret_cast<const uint32_t*>(sRow)[x];
                        break;
                    case 0x07: // Swizzled X8R8G8B8
                    case 0x1F: // Linear X8R8G8B8
                        argb = reinterpret_cast<const uint32_t*>(sRow)[x] | 0xFF000000u;
                        break;
                    case 0x29: // A8B8G8R8
                    case 0x3F: {
                        uint32_t v = reinterpret_cast<const uint32_t*>(sRow)[x];
                        argb = (v & 0xFF00FF00u) | ((v & 0xFFu) << 16) | ((v >> 16) & 0xFFu);
                        break;
                    }
                    case 0x2A: // B8G8R8A8
                    case 0x40: {
                        uint32_t v = reinterpret_cast<const uint32_t*>(sRow)[x];
                        argb = ((v & 0xFFu) << 24) | ((v >> 8) & 0xFFFFFFu);
                        break;
                    }
                    case 0x02: // Swizzled A1R5G5B5
                    case 0x1C: { // Linear A1R5G5B5
                        uint16_t v = reinterpret_cast<const uint16_t*>(sRow)[x];
                        uint32_t a = (v & 0x8000) ? 0xFFu : 0x00u;
                        uint32_t r = ((v >> 10) & 0x1F) * 255 / 31;
                        uint32_t g = ((v >> 5)  & 0x1F) * 255 / 31;
                        uint32_t b = (v         & 0x1F) * 255 / 31;
                        argb = (a << 24) | (r << 16) | (g << 8) | b;
                        break;
                    }
                    case 0x03: // Swizzled X1R5G5B5
                    case 0x1D: {
                        uint16_t v = reinterpret_cast<const uint16_t*>(sRow)[x];
                        uint32_t r = ((v >> 10) & 0x1F) * 255 / 31;
                        uint32_t g = ((v >> 5)  & 0x1F) * 255 / 31;
                        uint32_t b = (v         & 0x1F) * 255 / 31;
                        argb = 0xFF000000u | (r << 16) | (g << 8) | b;
                        break;
                    }
                    case 0x04: // Swizzled A4R4G4B4
                    case 0x1B: { // Linear A4R4G4B4
                        uint16_t v = reinterpret_cast<const uint16_t*>(sRow)[x];
                        uint32_t a = ((v >> 12) & 0x0F) * 17;
                        uint32_t r = ((v >> 8)  & 0x0F) * 17;
                        uint32_t g = ((v >> 4)  & 0x0F) * 17;
                        uint32_t b = (v         & 0x0F) * 17;
                        argb = (a << 24) | (r << 16) | (g << 8) | b;
                        break;
                    }
                    case 0x05: // Swizzled R5G6B5
                    case 0x11: { // Linear R5G6B5
                        uint16_t v = reinterpret_cast<const uint16_t*>(sRow)[x];
                        uint32_t r = ((v >> 11) & 0x1F) * 255 / 31;
                        uint32_t g = ((v >> 5)  & 0x3F) * 255 / 63;
                        uint32_t b = (v         & 0x1F) * 255 / 31;
                        argb = 0xFF000000u | (r << 16) | (g << 8) | b;
                        break;
                    }
                    case 0x00: // L8
                    case 0x13: {
                        uint32_t l = sRow[x];
                        argb = 0xFF000000u | (l << 16) | (l << 8) | l;
                        break;
                    }
                    case 0x01: // AL8
                    case 0x19: // A8
                    case 0x2F: {
                        uint32_t a = sRow[x];
                        argb = (a << 24) | 0x00FFFFFFu;
                        break;
                    }
                    case 0x1A: // A8L8
                    case 0x20: {
                        uint16_t v = reinterpret_cast<const uint16_t*>(sRow)[x];
                        uint32_t l = v & 0xFF;
                        uint32_t a = (v >> 8) & 0xFF;
                        argb = (a << 24) | (l << 16) | (l << 8) | l;
                        break;
                    }
                    default:
                        if (bpp == 4) argb = reinterpret_cast<const uint32_t*>(sRow)[x];
                        break;
                }
                dRow[x] = argb;
            }
        }
    }

    entry.isLinear = !isSwizzled;
    if (!entry.tex2D || entry.width != w || entry.height != h) {
        if (entry.rtv)   { entry.rtv->Release();   entry.rtv = nullptr; }
        if (entry.srv)   { entry.srv->Release();   entry.srv = nullptr; }
        if (entry.tex2D) { entry.tex2D->Release(); entry.tex2D = nullptr; }
        entry.width = w;
        entry.height = h;
        entry.dxgiFormat = DXGI_FORMAT_B8G8R8A8_UNORM;

        D3D11_TEXTURE2D_DESC td = {};
        td.Width = w;
        td.Height = h;
        td.MipLevels = 1;
        td.ArraySize = 1;
        td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;

        D3D11_SUBRESOURCE_DATA initData = {};
        initData.pSysMem = bgraPixels.data();
        initData.SysMemPitch = w * 4;

        if (SUCCEEDED(g_d3dDevice->CreateTexture2D(&td, &initData, &entry.tex2D))) {
            g_d3dDevice->CreateShaderResourceView(entry.tex2D, nullptr, &entry.srv);
            g_d3dDevice->CreateRenderTargetView(entry.tex2D, nullptr, &entry.rtv);
        }
    } else if (entry.tex2D) {
        g_d3dCtx->UpdateSubresource(entry.tex2D, 0, nullptr, bgraPixels.data(), w * 4, 0);
    }
    entry.dirty = false;
}

struct OffscreenDSVEntry {
    uint32_t                width;
    uint32_t                height;
    ID3D11Texture2D*        tex;
    ID3D11DepthStencilView* dsv;
};
static std::vector<OffscreenDSVEntry> g_offscreenDSVs;

static ID3D11DepthStencilView* GetOrCreateOffscreenDSV(uint32_t w, uint32_t h) {
    if (!g_d3dDevice || w == 0 || h == 0) return nullptr;
    if (w == 640 && h == 480 && g_dsv) return g_dsv;
    for (size_t i = 0; i < g_offscreenDSVs.size(); ++i) {
        if (g_offscreenDSVs[i].width == w && g_offscreenDSVs[i].height == h) {
            return g_offscreenDSVs[i].dsv;
        }
    }
    D3D11_TEXTURE2D_DESC dd = {};
    dd.Width = w;
    dd.Height = h;
    dd.MipLevels = 1;
    dd.ArraySize = 1;
    dd.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
    dd.SampleDesc.Count = 1;
    dd.Usage = D3D11_USAGE_DEFAULT;
    dd.BindFlags = D3D11_BIND_DEPTH_STENCIL;
    OffscreenDSVEntry e = { w, h, nullptr, nullptr };
    if (SUCCEEDED(g_d3dDevice->CreateTexture2D(&dd, nullptr, &e.tex))) {
        g_d3dDevice->CreateDepthStencilView(e.tex, nullptr, &e.dsv);
        if (g_d3dCtx && e.dsv) {
            g_d3dCtx->ClearDepthStencilView(e.dsv, D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL, 1.0f, 0);
        }
    }
    g_offscreenDSVs.push_back(e);
    return e.dsv;
}

static CachedTexture* GetOrCreateRenderTargetEntry(XboxPixelContainer* pTex) {
    if (!g_d3dDevice || !pTex || !pTex->Data || (uintptr_t)pTex->Data < 0x00400000) return nullptr;

    uint32_t w = 0, h = 0, levels = 1, xboxFmt = 0, linearPitch = 0;
    bool isSwizzled = false;
    DecodeXboxTextureFormat(pTex, &w, &h, &levels, &xboxFmt, &isSwizzled, &linearPitch);
    if (w == 0 || h == 0 || w > 4096 || h > 4096) return nullptr;

    for (size_t i = 0; i < g_texCache.size(); ++i) {
        CachedTexture& c = g_texCache[i];
        if (c.dataPtr == pTex->Data && c.width == w && c.height == h) {
            c.xboxTex = pTex;
            c.format = pTex->Format;
            c.sizeField = pTex->Size;
            c.isLinear = !isSwizzled;
            c.isRenderTarget = true;
            c.dirty = false;
            if (c.tex2D && !c.rtv) {
                g_d3dDevice->CreateRenderTargetView(c.tex2D, nullptr, &c.rtv);
            }
            return &c;
        }
    }

    CachedTexture entry = {};
    entry.xboxTex = pTex;
    entry.dataPtr = pTex->Data;
    entry.format = pTex->Format;
    entry.sizeField = pTex->Size;
    entry.isLinear = !isSwizzled;
    entry.isRenderTarget = true;
    entry.dirty = false;
    entry.width = w;
    entry.height = h;
    entry.dxgiFormat = DXGI_FORMAT_B8G8R8A8_UNORM;

    D3D11_TEXTURE2D_DESC td = {};
    td.Width = w;
    td.Height = h;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;

    if (SUCCEEDED(g_d3dDevice->CreateTexture2D(&td, nullptr, &entry.tex2D))) {
        g_d3dDevice->CreateShaderResourceView(entry.tex2D, nullptr, &entry.srv);
        g_d3dDevice->CreateRenderTargetView(entry.tex2D, nullptr, &entry.rtv);
        float zero[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
        if (g_d3dCtx && entry.rtv) g_d3dCtx->ClearRenderTargetView(entry.rtv, zero);
    }
    g_texCache.push_back(entry);
    return &g_texCache.back();
}

static ID3D11ShaderResourceView* GetOrCreateD3D11SRV(XboxPixelContainer* pTex, float* outUScale = nullptr, float* outVScale = nullptr) {
    if (outUScale) *outUScale = 1.0f;
    if (outVScale) *outVScale = 1.0f;
    if (!pTex || !pTex->Data || (uintptr_t)pTex->Data < 0x00400000) return nullptr;

    // Check if pTex is the Xbox main backbuffer (bound as a source texture by bloom/blur/refraction passes)
    XboxPixelContainer* mainBB0 = *reinterpret_cast<XboxPixelContainer**>(0x008AA344);
    XboxPixelContainer* mainBB1 = *reinterpret_cast<XboxPixelContainer**>(ADDR_D3D_DEVICE + 0x1A14);
    XboxPixelContainer* mainBB2 = *reinterpret_cast<XboxPixelContainer**>(ADDR_D3D_DEVICE + 0x1A18);
    XboxPixelContainer* mainBB3 = *reinterpret_cast<XboxPixelContainer**>(ADDR_D3D_DEVICE + 0x1A1C);
    if (pTex == mainBB0 || pTex == mainBB1 || pTex == mainBB2 || pTex == mainBB3 ||
        (mainBB0 && pTex->Data == mainBB0->Data))
    {
        if (outUScale) *outUScale = 1.0f / 640.0f;
        if (outVScale) *outVScale = 1.0f / 480.0f;
        if (g_d3dDevice && g_d3dCtx && g_backBufferTex) {
            if (!g_backBufferCopyTex) {
                D3D11_TEXTURE2D_DESC td = {};
                g_backBufferTex->GetDesc(&td);
                td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
                td.Usage = D3D11_USAGE_DEFAULT;
                td.CPUAccessFlags = 0;
                if (SUCCEEDED(g_d3dDevice->CreateTexture2D(&td, nullptr, &g_backBufferCopyTex))) {
                    g_d3dDevice->CreateShaderResourceView(g_backBufferCopyTex, nullptr, &g_backBufferCopySRV);
                }
            }
            if (g_backBufferCopyTex && g_backBufferDirty) {
                g_d3dCtx->CopyResource(g_backBufferCopyTex, g_backBufferTex);
                g_backBufferDirty = false;
            }
            return g_backBufferCopySRV;
        }
    }

    uint32_t w = 0, h = 0, levels = 1, xboxFmt = 0, linearPitch = 0;
    bool isSwizzled = false;
    DecodeXboxTextureFormat(pTex, &w, &h, &levels, &xboxFmt, &isSwizzled, &linearPitch);
    if (!isSwizzled && w > 0 && h > 0) {
        if (outUScale) *outUScale = 1.0f / (float)w;
        if (outVScale) *outVScale = 1.0f / (float)h;
    }

    for (size_t i = 0; i < g_texCache.size(); ++i) {
        CachedTexture& c = g_texCache[i];
        if (c.dataPtr == pTex->Data && c.width == w && c.height == h) {
            if (c.isRenderTarget) {
                return (c.rtv && c.rtv == g_curActiveRTV) ? nullptr : c.srv;
            }
            if (c.format == pTex->Format && c.sizeField == pTex->Size) {
                if (c.dirty || c.lastCheckedFrame != g_swapFrameCount) {
                    c.lastCheckedFrame = g_swapFrameCount;
                    size_t approxBytes = (xboxFmt == 0x0C) ? (((size_t)w * (size_t)h) / 2) : ((size_t)w * (size_t)h);
                    uint64_t curHash = ComputeFastSampleHash(reinterpret_cast<const void*>(pTex->Data), approxBytes, c.maxReadableBytes);
                    if (c.dirty || c.contentHash != curHash) {
                        c.contentHash = curHash;
                        UploadXboxTextureToD3D11(c, pTex);
                    }
                }
                return c.srv;
            }
        }
    }

    size_t maxReadable = GetReadableBytesAt(reinterpret_cast<const void*>(pTex->Data));
    size_t approxBytes = (xboxFmt == 0x0C) ? (((size_t)w * (size_t)h) / 2) : ((size_t)w * (size_t)h);
    uint64_t curHash = ComputeFastSampleHash(reinterpret_cast<const void*>(pTex->Data), approxBytes, maxReadable);

    CachedTexture entry = {};
    entry.xboxTex = pTex;
    entry.dataPtr = pTex->Data;
    entry.format = pTex->Format;
    entry.sizeField = pTex->Size;
    entry.contentHash = curHash;
    entry.lastCheckedFrame = g_swapFrameCount;
    entry.maxReadableBytes = maxReadable;
    entry.dirty = true;
    UploadXboxTextureToD3D11(entry, pTex);
    g_texCache.push_back(entry);
    return entry.srv;
}

// ============================================================================
// Vertex Declarations, Shaders, Pixel Shaders & CPU Skinning Bridges
// ============================================================================

struct XboxShaderDeclInfo {
    bool     isValid;
    bool     hasPos;
    bool     hasRhw;
    bool     hasNorm;
    bool     hasDiff;
    bool     hasUv0;
    bool     hasUv1;
    bool     hasUv2;
    bool     hasUv3;
    uint16_t offPos;
    uint16_t offRhw;
    uint16_t offNorm;
    uint16_t offDiff;
    uint16_t offUv0;
    uint16_t offUv1;
    uint16_t offUv2;
    uint16_t offUv3;
    uint32_t stride;
    int32_t  vsType; // 0..65 if from aVertexShaders[0..65] at 0x007D0BE0, else -1
};

static std::vector<XboxShaderDeclInfo> g_vertexShaders;
static uint32_t g_currentVsHandle = 0x002;
static uint32_t g_currentPsHandle = 0;
static int32_t  g_currentPsType   = -1; // -1 = fixed-functionTSS, 0..27 = aPixelShaders[type]
static float    g_psConstants[8][4] = {};

struct XboxPixelShaderObject {
    uint32_t refCount;
    int32_t  engineType; // 0..27 if from aPixelShaders[0..27] at 0x007D1214, else 0
    void*    pDef;
    uint8_t  defBytes[240];
};

static HRESULT __stdcall Bridge_D3DDevice_CreatePixelShader(const void* pShaderDef, DWORD* pHandle) {
    if (!pHandle) return E_INVALIDARG;
    XboxPixelShaderObject* obj = reinterpret_cast<XboxPixelShaderObject*>(calloc(1, sizeof(XboxPixelShaderObject)));
    if (!obj) return E_OUTOFMEMORY;
    obj->refCount = 1;
    obj->engineType = 0;
    obj->pDef = obj->defBytes;
    if (pShaderDef) {
        memcpy(obj->defBytes, pShaderDef, 240);
    }
    uintptr_t hAddr = reinterpret_cast<uintptr_t>(pHandle);
    if (hAddr >= 0x007D1214 && hAddr < 0x007D1214 + 28 * 8) {
        obj->engineType = (int32_t)((hAddr - 0x007D1214) / 8);
    }
    *pHandle = reinterpret_cast<DWORD>(obj);
    return S_OK;
}

static HRESULT __stdcall Bridge_D3DDevice_SetPixelShader(DWORD Handle) {
    g_currentPsHandle = Handle;
    *reinterpret_cast<uint32_t*>(ADDR_D3D_DEVICE + 0x784) = Handle;
    if (Handle != 0 && Handle >= 0x00400000) {
        const XboxPixelShaderObject* obj = reinterpret_cast<const XboxPixelShaderObject*>(Handle);
        g_currentPsType = obj->engineType;
        // Load embedded def cN constants from D3DPIXELSHADERDEF (PSC0Mapping=dwords[57], PSC1Mapping=dwords[58])
        const uint32_t* dwords = reinterpret_cast<const uint32_t*>(obj->defBytes);
        uint32_t c0Map = dwords[57];
        uint32_t c1Map = dwords[58];
        for (int s = 0; s < 8; ++s) {
            uint32_t r0 = (c0Map >> (s * 4)) & 0xFu;
            if (r0 < 8 && dwords[10 + s] != 0) {
                uint32_t c = dwords[10 + s];
                g_psConstants[r0][0] = ((c >> 16) & 0xFFu) * (1.0f / 255.0f);
                g_psConstants[r0][1] = ((c >> 8)  & 0xFFu) * (1.0f / 255.0f);
                g_psConstants[r0][2] = (c         & 0xFFu) * (1.0f / 255.0f);
                g_psConstants[r0][3] = ((c >> 24) & 0xFFu) * (1.0f / 255.0f);
            }
            uint32_t r1 = (c1Map >> (s * 4)) & 0xFu;
            if (r1 < 8 && dwords[18 + s] != 0) {
                uint32_t c = dwords[18 + s];
                g_psConstants[r1][0] = ((c >> 16) & 0xFFu) * (1.0f / 255.0f);
                g_psConstants[r1][1] = ((c >> 8)  & 0xFFu) * (1.0f / 255.0f);
                g_psConstants[r1][2] = (c         & 0xFFu) * (1.0f / 255.0f);
                g_psConstants[r1][3] = ((c >> 24) & 0xFFu) * (1.0f / 255.0f);
            }
        }
    } else {
        g_currentPsType = -1;
    }
    return S_OK;
}

static void __stdcall Bridge_D3DDevice_SetPixelShaderProgram(DWORD Handle) {
    Bridge_D3DDevice_SetPixelShader(Handle);
}

static HRESULT __stdcall Bridge_D3DDevice_SetPixelShaderConstant(DWORD Register, const void* pConstantData, DWORD ConstantCount) {
    if (pConstantData && Register < 8 && ConstantCount > 0) {
        DWORD cnt = ConstantCount;
        if (Register + cnt > 8) cnt = 8 - Register;
        memcpy(&g_psConstants[Register][0], pConstantData, cnt * 16);
    }
    return S_OK;
}

static uint32_t GetXboxDeclTypeByteSize(uint32_t xboxType) {
    switch (xboxType) {
        case 0x12: return 4;  // FLOAT1
        case 0x22: return 8;  // FLOAT2
        case 0x32: return 12; // FLOAT3
        case 0x42: return 16; // FLOAT4
        case 0x40: return 4;  // D3DCOLOR
        case 0x41: return 4;  // UBYTE4
        case 0x25: return 4;  // SHORT2
        case 0x45: return 8;  // SHORT4
        case 0x24: return 4;  // SHORT2N
        case 0x44: return 8;  // SHORT4N
        default:   return 12;
    }
}

static HRESULT __stdcall Bridge_D3DDevice_CreateVertexShader(
    const DWORD* pDeclaration, const DWORD* pFunction, DWORD* pHandle, DWORD Usage)
{
    (void)pFunction; (void)Usage;
    if (!pHandle) return E_INVALIDARG;

    XboxShaderDeclInfo info = {};
    info.isValid = true;
    info.vsType  = -1;

    uintptr_t hAddr = reinterpret_cast<uintptr_t>(pHandle);
    if (hAddr >= 0x007D0BE0 && hAddr < 0x007D0BE0 + 66 * 24) {
        info.vsType = (int32_t)((hAddr - 0x007D0BE0) / 24);
    }

    if (pDeclaration) {
        uint16_t curStream = 0;
        uint16_t curOffset = 0;
        for (int i = 0; i < 64 && pDeclaration[i] != 0xFFFFFFFF; ++i) {
            DWORD tok = pDeclaration[i];
            DWORD tokType = (tok >> 29) & 0x7;
            if (tokType == 1) {
                curStream = (uint16_t)(tok & 0x0F);
                curOffset = 0;
            } else if (tokType == 2 && curStream == 0) {
                if (tok & 0x10000000) {
                    DWORD skipDwords = (tok >> 16) & 0x0F;
                    curOffset += (uint16_t)(skipDwords * 4);
                } else {
                    uint32_t reg = tok & 0x1F;
                    uint32_t xType = (tok >> 16) & 0xFF;
                    uint32_t byteSize = GetXboxDeclTypeByteSize(xType);
                    // Ghost.exe D3DVSD register convention (0x007A980C..0x007A996C):
                    // reg 0 = Pos, reg 1 = Normal, reg 2 = Diffuse, reg 3..6 = UV0..UV3, reg 10 = RHW
                    switch (reg) {
                        case 0:  info.hasPos  = true; info.offPos  = curOffset; break;
                        case 1:  info.hasNorm = true; info.offNorm = curOffset; break;
                        case 2:  info.hasDiff = true; info.offDiff = curOffset; break;
                        case 3:  info.hasUv0  = true; info.offUv0  = curOffset; break;
                        case 4:  info.hasUv1  = true; info.offUv1  = curOffset; break;
                        case 5:  info.hasUv2  = true; info.offUv2  = curOffset; break;
                        case 6:  info.hasUv3  = true; info.offUv3  = curOffset; break;
                        case 10: info.hasRhw  = true; info.offRhw  = curOffset; break;
                        default: break;
                    }
                    curOffset += (uint16_t)byteSize;
                    info.stride = curOffset;
                }
            }
        }
    }

    g_vertexShaders.push_back(info);
    uint32_t idx = (uint32_t)g_vertexShaders.size();
    *pHandle = (idx << 4) | 0x00010001u;
    return S_OK;
}

static int32_t g_activeCustomVS = 0; // 0=none, 1=Cloak, 2=ScreenEffect, 3=Membrane, 4=Sight, 5=Shield, 6=Teleport, 7=MotionBlur, 8=BumpEnv

static void __stdcall Bridge_D3DDevice_SetVertexShader(DWORD Handle) {
    g_currentVsHandle = Handle;
    g_activeCustomVS  = 0;
    *reinterpret_cast<uint32_t*>(ADDR_D3D_DEVICE + 0x794) = 0x006C2F20;
    *reinterpret_cast<uint32_t*>(ADDR_D3D_DEVICE + 0x798) = Handle;
    *reinterpret_cast<uint32_t*>(ADDR_D3D_DEVICE + 0x79C) = 0;

    if ((Handle & 1u) == 0) {
        typedef void (__stdcall *PFN_InitVsFromFvf)(void* pVs, DWORD fvf);
        PFN_InitVsFromFvf fnInitFvf = reinterpret_cast<PFN_InitVsFromFvf>(0x006B22F0);
        fnInitFvf(reinterpret_cast<void*>(0x006C2F20), Handle);
    }
}

static void ApplyVsConstants(int32_t xboxReg, const void* pConstantData, uint32_t ConstantCount) {
    if (!pConstantData || ConstantCount == 0) return;
    if (xboxReg >= 0 && xboxReg < 192) {
        uint32_t copyCnt = ConstantCount;
        if (xboxReg + (int32_t)copyCnt > 192) copyCnt = 192 - xboxReg;
        memcpy(reinterpret_cast<void*>(0x006BF7D0 + xboxReg * 16), pConstantData, copyCnt * 16);
    }
}

static void __fastcall Bridge_D3DDevice_SetVertexShaderConstant1(int32_t reg, const void* pData) {
    ApplyVsConstants(reg, pData, 1);
}

static void __fastcall Bridge_D3DDevice_SetVertexShaderConstant4(int32_t reg, const void* pData) {
    ApplyVsConstants(reg, pData, 4);
}

static void __fastcall Bridge_D3DDevice_SetVertexShaderConstantNotInline(int32_t reg, const void* pData, uint32_t dwordCount) {
    uint32_t vec4Count = (dwordCount >= 4) ? (dwordCount >> 2) : dwordCount;
    ApplyVsConstants(reg, pData, vec4Count);
}

// ============================================================================
// CPU Skinning Bridges (cShader::VertexSetup* -> cMeshGroup::RenderSetup*)
// ============================================================================

typedef uint32_t (__thiscall *PFN_MeshGroup_RenderSetup)(void* pMeshGroup, void* pModelInst, uint32_t lod);
static PFN_MeshGroup_RenderSetup g_fnRenderSetupRigid   = reinterpret_cast<PFN_MeshGroup_RenderSetup>(0x006056A0);
static PFN_MeshGroup_RenderSetup g_fnRenderSetupSkinned = reinterpret_cast<PFN_MeshGroup_RenderSetup>(0x006071C0);

static void __stdcall Bridge_D3DDevice_SetStreamSource(UINT StreamNumber, XboxVertexBuffer* pStreamData, UINT Stride);
static void __stdcall Bridge_D3DDevice_SetIndices(XboxVertexBuffer* pIndexData, UINT BaseVertexIndex);
static void __stdcall Bridge_D3DDevice_SetTransform(DWORD State, const Mat44* pMatrix);

// Replacement for cMeshGroup::RenderSetupWeightedSkin (0x00606E50) and RenderSetupBlendWeightedSkin (0x00607390).
// Fixes the stack-aliasing bug in Ghost.exe's unused CPU fallback where Bone 1's normal overwrote Bone 0's normal
// ([ebp-0x54] / [ebp-0x1c]), causing vertices with w0=1, w1=0, w2=0 (e.g. Nova's face, abdomen, crotch) to get normal (0,0,0).
static uint32_t Host_RenderSetupWeightedSkin(uint8_t* pMeshGroup, uint8_t* pModelInst, uint32_t lod, bool isBlend) {
    if (!pMeshGroup) return 0;
    uint32_t numVerts = *reinterpret_cast<const uint32_t*>(pMeshGroup + 0x14 + lod * 16);
    void* pDynVB = *reinterpret_cast<void**>(0x008AAA80);
    if (!pDynVB) return 0;

    typedef uint8_t* (__thiscall *PFN_DynVB_Lock)(void* thisPtr, uint32_t count, uint32_t* outBaseVert);
    typedef void     (__thiscall *PFN_DynVB_Unlock)(void* thisPtr);
    PFN_DynVB_Lock   fnLock   = reinterpret_cast<PFN_DynVB_Lock>(0x005FBFA0);
    PFN_DynVB_Unlock fnUnlock = reinterpret_cast<PFN_DynVB_Unlock>(0x005FC010);

    uint32_t outBaseVert = 0;
    uint8_t* dstBase = fnLock(pDynVB, numVerts, &outBaseVert);

    uint32_t numTargets = 0;
    const float* targetWeights = nullptr;
    const float* targetPtrs[16] = {};
    if (isBlend) {
        if (!pModelInst || !*reinterpret_cast<void* const*>(pModelInst + 0xAC)) return 0;
        typedef const uint8_t* (__thiscall *PFN_GetBlendState)(void* pBlendCtrl, uint32_t blendGroupIdx);
        PFN_GetBlendState fnGetBlendState = reinterpret_cast<PFN_GetBlendState>(0x005E2CF0);
        const uint8_t* pBlendState = fnGetBlendState(*reinterpret_cast<void* const*>(pModelInst + 0xAC), pMeshGroup[0x53]);
        if (!pBlendState) return 0;
        numTargets = pBlendState[0];
        if (numTargets > 16) numTargets = 16;
        targetWeights = reinterpret_cast<const float*>(pBlendState + 4);
        uint32_t lod0Verts = *reinterpret_cast<const uint32_t*>(pMeshGroup + 0x14);
        const uint8_t* morphBase = *reinterpret_cast<const uint8_t* const*>(pMeshGroup + 0x4C);
        for (uint32_t t = 0; t < numTargets; ++t) {
            targetPtrs[t] = reinterpret_cast<const float*>(morphBase + (uint32_t)pBlendState[1 + t] * lod0Verts * 24);
        }
    }

    uint8_t lodIdx = pMeshGroup[0x50];
    const uint8_t* pModel = *reinterpret_cast<const uint8_t* const*>(pMeshGroup + 0x04);
    const uint8_t* pLod = pModel + (uint32_t)lodIdx * 24;
    uint8_t vertType = pLod[0x24];
    uint32_t srcStride = *reinterpret_cast<const uint8_t*>(0x007AB8DC + vertType);
    uint32_t meshBaseVert = *reinterpret_cast<const uint32_t*>(pMeshGroup + 0x48);
    const uint8_t* srcPtr = *reinterpret_cast<const uint8_t* const*>(pLod + 0x18) + meshBaseVert * srcStride;
    const float* boneMats = *reinterpret_cast<const float* const*>(0x008ACC34);

    if (dstBase && srcPtr && boneMats && numVerts > 0) {
        uint8_t* dstPtr = dstBase;
        for (uint32_t i = 0; i < numVerts; ++i) {
            const float* sf = reinterpret_cast<const float*>(srcPtr);
            float px = sf[0], py = sf[1], pz = sf[2];
            float nx = sf[3], ny = sf[4], nz = sf[5];
            float u  = sf[6], v  = sf[7];
            float w0 = sf[8], w1 = sf[9], w2 = sf[10];

            if (isBlend && numTargets > 0) {
                for (uint32_t t = 0; t < numTargets; ++t) {
                    const float* d = targetPtrs[t] + i * 6;
                    float tw = targetWeights[t];
                    px += d[0] * tw;
                    py += d[1] * tw;
                    pz += d[2] * tw;
                }
            }

            uint32_t b0 = isBlend ? (uint32_t)srcPtr[0x2C] : (uint32_t)pMeshGroup[0x54 + srcPtr[0x2C]];
            uint32_t b1 = isBlend ? (uint32_t)srcPtr[0x2D] : (uint32_t)pMeshGroup[0x54 + srcPtr[0x2D]];
            uint32_t b2 = isBlend ? (uint32_t)srcPtr[0x2E] : (uint32_t)pMeshGroup[0x54 + srcPtr[0x2E]];

            const float* m0 = boneMats + b0 * 16;
            const float* m1 = boneMats + b1 * 16;
            const float* m2 = boneMats + b2 * 16;

            float opx = w0 * (px * m0[0] + py * m0[4] + pz * m0[8]  + m0[12])
                      + w1 * (px * m1[0] + py * m1[4] + pz * m1[8]  + m1[12])
                      + w2 * (px * m2[0] + py * m2[4] + pz * m2[8]  + m2[12]);
            float opy = w0 * (px * m0[1] + py * m0[5] + pz * m0[9]  + m0[13])
                      + w1 * (px * m1[1] + py * m1[5] + pz * m1[9]  + m1[13])
                      + w2 * (px * m2[1] + py * m2[5] + pz * m2[9]  + m2[13]);
            float opz = w0 * (px * m0[2] + py * m0[6] + pz * m0[10] + m0[14])
                      + w1 * (px * m1[2] + py * m1[6] + pz * m1[10] + m1[14])
                      + w2 * (px * m2[2] + py * m2[6] + pz * m2[10] + m2[14]);

            float onx = w0 * (nx * m0[0] + ny * m0[4] + nz * m0[8])
                      + w1 * (nx * m1[0] + ny * m1[4] + nz * m1[8])
                      + w2 * (nx * m2[0] + ny * m2[4] + nz * m2[8]);
            float ony = w0 * (nx * m0[1] + ny * m0[5] + nz * m0[9])
                      + w1 * (nx * m1[1] + ny * m1[5] + nz * m1[9])
                      + w2 * (nx * m2[1] + ny * m2[5] + nz * m2[9]);
            float onz = w0 * (nx * m0[2] + ny * m0[6] + nz * m0[10])
                      + w1 * (nx * m1[2] + ny * m1[6] + nz * m1[10])
                      + w2 * (nx * m2[2] + ny * m2[6] + nz * m2[10]);

            float* df = reinterpret_cast<float*>(dstPtr);
            df[0] = opx; df[1] = opy; df[2] = opz;
            df[3] = onx; df[4] = ony; df[5] = onz;
            df[6] = u;   df[7] = v;

            srcPtr += srcStride;
            dstPtr += 0x20;
        }
    }

    fnUnlock(pDynVB);

    if (*reinterpret_cast<uint8_t*>(0x008AA454) == 0) {
        Bridge_D3DDevice_SetTransform(6, reinterpret_cast<const Mat44*>(0x007D0790));
        *reinterpret_cast<uint8_t*>(0x008AA454) = 1;
    }
    if (*reinterpret_cast<uint32_t*>(0x008AA468) != 0x112) {
        *reinterpret_cast<uint32_t*>(0x008AA468) = 0x112;
        Bridge_D3DDevice_SetVertexShader(0x112);
    }
    XboxVertexBuffer* dynVbObj = *reinterpret_cast<XboxVertexBuffer**>(reinterpret_cast<uint8_t*>(pDynVB) + 0x10);
    if (*reinterpret_cast<XboxVertexBuffer**>(0x008AA480) != dynVbObj) {
        *reinterpret_cast<XboxVertexBuffer**>(0x008AA480) = dynVbObj;
        Bridge_D3DDevice_SetStreamSource(0, dynVbObj, 0x20);
    }
    XboxVertexBuffer* idxBuf = *reinterpret_cast<XboxVertexBuffer* const*>(pModel + 0x7C);
    Bridge_D3DDevice_SetIndices(idxBuf, outBaseVert);
    return 0;
}

static const uint32_t* g_curPrelitColors     = nullptr;
static uint32_t        g_curPrelitBaseVertex = 0;
static float           g_cloakDistort        = 16.0f;
static float           g_cloakC35[4]         = { 1.0f, 0.0f, 0.0f, 0.5f };
static float           g_shieldC35x          = 1.0f;
static float           g_teleportC32[2]      = { 1.0f / 640.0f, 1.0f / 480.0f };
static float           g_teleportC33x        = 0.0f;
static float           g_motionBlurVec[4]    = { 0.0f, 0.0f, 0.0f, 1.0f };

static void SetupPrelitStreamForMesh(const uint8_t* pMeshGroup, const uint8_t* pModelInst) {
    g_curPrelitColors     = nullptr;
    g_curPrelitBaseVertex = 0;
    if (pMeshGroup && pModelInst && (pModelInst[0x14] & 1) != 0) {
        uint8_t lodIdx = pMeshGroup[0x50];
        if (lodIdx < 3) {
            XboxVertexBuffer* prelitVB = *reinterpret_cast<XboxVertexBuffer* const*>(pModelInst + 0xB0 + lodIdx * 4);
            if (!prelitVB) {
                const uint8_t* pModel = *reinterpret_cast<const uint8_t* const*>(pMeshGroup + 0x04);
                if (pModel) {
                    prelitVB = *reinterpret_cast<XboxVertexBuffer* const*>(pModel + 0x2C + lodIdx * 0x18);
                }
            }
            if (prelitVB && prelitVB->Data) {
                g_curPrelitColors     = reinterpret_cast<const uint32_t*>(prelitVB->Data);
                g_curPrelitBaseVertex = *reinterpret_cast<const uint32_t*>(pMeshGroup + 0x48);
            }
        }
    }
}

static int __fastcall Bridge_Shader_VertexSetup(void* pShader, void* edxUnused, uint8_t* pMeshGroup, uint8_t* pModelInst) {
    (void)pShader; (void)edxUnused;
    g_activeCustomVS = 0;
    if (!pMeshGroup) {
        g_curPrelitColors = nullptr;
        return -1;
    }

    SetupPrelitStreamForMesh(pMeshGroup, pModelInst);

    if (pMeshGroup[0x52] != 0 && pModelInst && *reinterpret_cast<uint32_t*>(pModelInst + 0xAC) != 0) {
        return (int)Host_RenderSetupWeightedSkin(pMeshGroup, pModelInst, 0, true);
    }

    uint8_t lodIdx = pMeshGroup[0x50];
    uint8_t* pModel = *reinterpret_cast<uint8_t**>(pMeshGroup + 0x04);
    if (!pModel) return -1;

    uint8_t vertType = pModel[lodIdx * 24 + 0x24];
    uint32_t lod = pModelInst ? pModelInst[0x28] : 0;

    switch (vertType) {
        case 0:
        case 3:
            return (int)g_fnRenderSetupRigid(pMeshGroup, pModelInst, lod);
        case 1:
        case 4:
            return (int)g_fnRenderSetupSkinned(pMeshGroup, pModelInst, lod);
        case 2:
        case 5:
            return (int)Host_RenderSetupWeightedSkin(pMeshGroup, pModelInst, lod, false);
        default:
            return -1;
    }
}

static int __fastcall Bridge_CloakShader_VertexSetup(uint8_t* pShader, void* edxUnused, uint8_t* pMeshGroup, uint8_t* pModelInst) {
    int rc = Bridge_Shader_VertexSetup(pShader, edxUnused, pMeshGroup, pModelInst);
    g_activeCustomVS = 1;
    if (pShader) {
        float distortBase = *reinterpret_cast<const float*>(pShader + 0x68);
        float targetLerp  = *reinterpret_cast<const float*>(pShader + 0x6C);
        float cloakLerp   = *reinterpret_cast<const float*>(pShader + 0x70);
        uint32_t flag58   = *reinterpret_cast<const uint32_t*>(pShader + 0x58);
        uint32_t empFlag  = *reinterpret_cast<const uint32_t*>(pShader + 0x5C);
        float d = distortBase;
        if (flag58 == 0) {
            d = distortBase + (1.0f - cloakLerp) * (distortBase * 60.0f);
        }
        if (fabsf(d) < 0.5f) d = 14.0f;
        g_cloakDistort = d;

        float c35x = 1.0f - cloakLerp;
        float c35y = *reinterpret_cast<const float*>(pShader + 0x78);
        float c35z = *reinterpret_cast<const float*>(pShader + 0x7C);
        float c35w = 0.0f;
        if (flag58 == 0 && empFlag == 0 && cloakLerp < 1.0f) {
            c35y = 0.0f;
            c35z = 0.0f;
            if (cloakLerp <= targetLerp && cloakLerp >= 0.94f) {
                c35w = cloakLerp;
            }
        }
        g_cloakC35[0] = (c35x > 0.01f) ? c35x : 1.0f;
        g_cloakC35[1] = c35y;
        g_cloakC35[2] = c35z;
        g_cloakC35[3] = c35w;
    }
    return rc;
}

static int __fastcall Bridge_ScreenEffectShader_VertexSetup(uint8_t* pShader, void* edxUnused, uint8_t* pMeshGroup, uint8_t* pModelInst) {
    int rc = Bridge_Shader_VertexSetup(pShader, edxUnused, pMeshGroup, pModelInst);
    g_activeCustomVS = 2;
    return rc;
}

static int __fastcall Bridge_MembraneShader_VertexSetup(uint8_t* pShader, void* edxUnused, uint8_t* pMeshGroup, uint8_t* pModelInst) {
    int rc = Bridge_Shader_VertexSetup(pShader, edxUnused, pMeshGroup, pModelInst);
    g_activeCustomVS = 3;
    return rc;
}

static int __fastcall Bridge_GhostSuitShader_VertexSetup(uint8_t* pShader, void* edxUnused, uint8_t* pMeshGroup, uint8_t* pModelInst) {
    int rc = Bridge_Shader_VertexSetup(pShader, edxUnused, pMeshGroup, pModelInst);
    if (pShader) {
        memcpy(reinterpret_cast<void*>(ADDR_D3D_DEVICE + 0xC60 + 3 * 64), pShader + 0x60, 64);
    }
    return rc;
}

static int __fastcall Bridge_SightShader_VertexSetup(uint8_t* pShader, void* edxUnused, uint8_t* pMeshGroup, uint8_t* pModelInst) {
    int rc = Bridge_Shader_VertexSetup(pShader, edxUnused, pMeshGroup, pModelInst);
    g_activeCustomVS = 4;
    return rc;
}

static int __fastcall Bridge_ShieldShader_VertexSetup(uint8_t* pShader, void* edxUnused, uint8_t* pMeshGroup, uint8_t* pModelInst) {
    int rc = Bridge_Shader_VertexSetup(pShader, edxUnused, pMeshGroup, pModelInst);
    g_activeCustomVS = 5;
    if (pShader) {
        uint16_t mode = *reinterpret_cast<const uint16_t*>(pShader + 0x5C);
        g_shieldC35x  = (mode == 1 || mode == 2) ? *reinterpret_cast<const float*>(pShader + 0x58) : 0.0f;
    }
    return rc;
}

static int __fastcall Bridge_TeleportShader_VertexSetup(uint8_t* pShader, void* edxUnused, uint8_t* pMeshGroup, uint8_t* pModelInst) {
    int rc = Bridge_Shader_VertexSetup(pShader, edxUnused, pMeshGroup, pModelInst);
    g_activeCustomVS = 6;
    if (pShader) {
        float t = *reinterpret_cast<const float*>(pShader + 0xA8);
        if (t < 0.0f) t = 0.0f; else if (t > 1.0f) t = 1.0f;
        *reinterpret_cast<float*>(pShader + 0xA0) = t;
        *reinterpret_cast<float*>(pShader + 0xA4) = 1.0f - t;
        float scale = (pShader[0xB0] == 0) ? 255.0f : 128.0f;
        uint32_t b = (uint32_t)(t * scale);
        if (b > 255u) b = 255u;
        *reinterpret_cast<uint32_t*>(pShader + 0xAC) = (b << 24) | (b << 16) | (b << 8) | b;
        g_teleportC32[0] = (t * 3.0f + 1.0f) * (1.0f / 640.0f);
        g_teleportC32[1] = ((1.0f - t) * 3.0f + 1.0f) * (1.0f / 480.0f);
        g_teleportC33x   = t;
    }
    return rc;
}

static int __fastcall Bridge_MotionBlur_VertexSetup(uint8_t* pShader, void* edxUnused, uint8_t* pMeshGroup, uint8_t* pModelInst) {
    int rc = Bridge_Shader_VertexSetup(pShader, edxUnused, pMeshGroup, pModelInst);
    g_activeCustomVS = 7;
    if (pShader) {
        const float* v = reinterpret_cast<const float*>(pShader + 0x54);
        g_motionBlurVec[0] = v[0];
        g_motionBlurVec[1] = v[1];
        g_motionBlurVec[2] = v[2];
        g_motionBlurVec[3] = v[3];
    }
    return rc;
}

static constexpr uint32_t kCustomVsBumpEnv = 8u;
static constexpr uint32_t kCustomVsEnvmap  = 9u;

static int __fastcall Bridge_BumpEnvShader_VertexSetup(uint8_t* pShader, void* edxUnused, uint8_t* pMeshGroup, uint8_t* pModelInst) {
    int rc = Bridge_Shader_VertexSetup(pShader, edxUnused, pMeshGroup, pModelInst);
    g_activeCustomVS = kCustomVsBumpEnv;
    return rc;
}

static int __fastcall Bridge_EnvmapShader_VertexSetup(uint8_t* pShader, void* edxUnused, uint8_t* pMeshGroup, uint8_t* pModelInst) {
    int rc = Bridge_Shader_VertexSetup(pShader, edxUnused, pMeshGroup, pModelInst);
    g_activeCustomVS = kCustomVsEnvmap;
    return rc;
}

static int __fastcall Bridge_Shader_VertexSetupRigid(void* pShader, void* edxUnused, uint8_t* pMeshGroup, uint32_t lod, int bSetVS) {
    (void)pShader; (void)edxUnused; (void)bSetVS;
    g_activeCustomVS = 0;
    uint8_t* pModelInst = *reinterpret_cast<uint8_t**>(0x008ACC30);
    SetupPrelitStreamForMesh(pMeshGroup, pModelInst);
    return (int)g_fnRenderSetupRigid(pMeshGroup, pModelInst, lod);
}

static int __fastcall Bridge_Shader_VertexSetupWeightedSkin(void* pShader, void* edxUnused, uint8_t* pMeshGroup, uint32_t lod, int bSetVS) {
    (void)pShader; (void)edxUnused; (void)bSetVS;
    g_activeCustomVS = 0;
    uint8_t* pModelInst = *reinterpret_cast<uint8_t**>(0x008ACC30);
    SetupPrelitStreamForMesh(pMeshGroup, pModelInst);
    return (int)Host_RenderSetupWeightedSkin(pMeshGroup, pModelInst, lod, false);
}

static int __fastcall Bridge_Shader_VertexSetupSkinned(void* pShader, void* edxUnused, uint8_t* pMeshGroup, uint32_t lod, int bSetVS) {
    (void)pShader; (void)edxUnused; (void)bSetVS;
    g_activeCustomVS = 0;
    uint8_t* pModelInst = *reinterpret_cast<uint8_t**>(0x008ACC30);
    SetupPrelitStreamForMesh(pMeshGroup, pModelInst);
    return (int)g_fnRenderSetupSkinned(pMeshGroup, pModelInst, lod);
}

static int __fastcall Bridge_Shader_VertexSetupPrelit2(void* pShader, void* edxUnused, void* pMeshGroup, void* pModelInst) {
    (void)pShader; (void)edxUnused; (void)pMeshGroup; (void)pModelInst;
    return 0; // Return 0 so caller falls back to standard rigid/skinned setup
}

static int __fastcall Bridge_Shader_VertexSetupPrelit4(void* pShader, void* edxUnused, void* pMeshGroup, void* pModelInst, uint32_t lod, int bSetVS) {
    (void)pShader; (void)edxUnused; (void)pMeshGroup; (void)pModelInst; (void)lod; (void)bSetVS;
    return 0;
}

// ============================================================================
// Stream Sources, Indices, Textures & Transforms
// ============================================================================

struct BoundStream {
    const void* data;
    uint32_t    stride;
};
static BoundStream g_streams[16] = {};
static const uint16_t* g_indexData = nullptr;
static uint32_t g_baseVertexIndex = 0;
static XboxPixelContainer* g_boundTextures[4] = {};

static Mat44 g_matView  = { {{1,0,0,0}, {0,1,0,0}, {0,0,1,0}, {0,0,0,1}} };
static Mat44 g_matProj  = { {{1,0,0,0}, {0,1,0,0}, {0,0,1,0}, {0,0,0,1}} };
static Mat44 g_matWorld = { {{1,0,0,0}, {0,1,0,0}, {0,0,1,0}, {0,0,0,1}} };
static XboxViewport g_viewport = { 0, 0, 640, 480, 0.0f, 1.0f };

static Mat44 MulMat44(const Mat44& A, const Mat44& B) {
    Mat44 R = {};
    for (int i = 0; i < 4; ++i) {
        for (int j = 0; j < 4; ++j) {
            R.m[i][j] = A.m[i][0] * B.m[0][j] +
                        A.m[i][1] * B.m[1][j] +
                        A.m[i][2] * B.m[2][j] +
                        A.m[i][3] * B.m[3][j];
        }
    }
    return R;
}

static void __stdcall Bridge_D3DDevice_SetStreamSource(UINT StreamNumber, XboxVertexBuffer* pStreamData, UINT Stride) {
    if (StreamNumber < 16) {
        g_streams[StreamNumber].data = (pStreamData && pStreamData->Data) ? reinterpret_cast<const void*>(pStreamData->Data) : nullptr;
        g_streams[StreamNumber].stride = Stride;
        *reinterpret_cast<uint32_t*>(ADDR_D3D_STREAM + StreamNumber * 12 + 0) = Stride;
        *reinterpret_cast<XboxVertexBuffer**>(ADDR_D3D_STREAM + StreamNumber * 12 + 8) = pStreamData;
    }
}

static void __stdcall Bridge_D3DDevice_SetIndices(XboxVertexBuffer* pIndexData, UINT BaseVertexIndex) {
    g_indexData = (pIndexData && pIndexData->Data) ? reinterpret_cast<const uint16_t*>(pIndexData->Data) : nullptr;
    g_baseVertexIndex = BaseVertexIndex;
    *reinterpret_cast<const uint16_t**>(ADDR_D3D_INDEXDATA) = g_indexData;
    *reinterpret_cast<XboxVertexBuffer**>(ADDR_D3D_DEVICE + 0x7A0) = pIndexData;
    *reinterpret_cast<uint32_t*>(ADDR_D3D_DEVICE + 0x1C) = BaseVertexIndex;
}

static void __stdcall Bridge_D3DDevice_SetTexture(DWORD Stage, XboxPixelContainer* pTexture) {
    if (Stage < 4) {
        g_boundTextures[Stage] = pTexture;
        *reinterpret_cast<XboxPixelContainer**>(ADDR_D3D_DEVICE + 0x1A58 + Stage * 4) = pTexture;
    }
}

static void __stdcall Bridge_D3DDevice_SetTransform(DWORD State, const Mat44* pMatrix) {
    if (!pMatrix) return;
    if (State < 10) {
        memcpy(reinterpret_cast<void*>(ADDR_D3D_DEVICE + 0xC60 + State * 64), pMatrix, 64);
    }
    switch (State) {
        case 0: g_matView  = *pMatrix; break;
        case 1: g_matProj  = *pMatrix; break;
        case 6: g_matWorld = *pMatrix; break;
        default: break;
    }
}

static void* g_curOffscreenRT = nullptr;

// _D3DDevice_SetViewport@4 (0x006B12B0)
// Matches Xbox D3D8 viewport clamping (0x006B12E7..0x006B14A9): clamps (X, Y) and
// (X + Width, Y + Height) to the active render target dimensions. This is critical
// because _D3DDevice_SetRenderTarget@8 (0x006B08D0 -> 0x006AFB00) passes Xbox's
// sentinel g_DefaultViewport at 0x006BE4B4 ({0, 0, 0x7FFFFFFF, 0x7FFFFFFF, 0.0f, 1.0f})
// expecting SetViewport to clamp Width/Height to the current render target (640x480).
static void __stdcall Bridge_D3DDevice_SetViewport(const XboxViewport* pViewport) {
    constexpr uint32_t kDefaultBackBufferWidth  = 640u;
    constexpr uint32_t kDefaultBackBufferHeight = 480u;
    uint32_t rtW = kDefaultBackBufferWidth;
    uint32_t rtH = kDefaultBackBufferHeight;

    if (g_curOffscreenRT) {
        uint32_t w = 0, h = 0, levels = 1, fmt = 0, pitch = 0;
        bool swiz = false;
        DecodeXboxTextureFormat(reinterpret_cast<const XboxPixelContainer*>(g_curOffscreenRT), &w, &h, &levels, &fmt, &swiz, &pitch);
        if (w > 0 && h > 0) {
            rtW = w;
            rtH = h;
        }
    }

    if (pViewport) {
        uint32_t x = (pViewport->X < rtW) ? pViewport->X : 0u;
        uint32_t y = (pViewport->Y < rtH) ? pViewport->Y : 0u;
        uint64_t right  = static_cast<uint64_t>(x) + static_cast<uint64_t>(pViewport->Width);
        uint64_t bottom = static_cast<uint64_t>(y) + static_cast<uint64_t>(pViewport->Height);
        if (right > rtW || pViewport->Width == 0)   right  = rtW;
        if (bottom > rtH || pViewport->Height == 0) bottom = rtH;

        g_viewport.X      = x;
        g_viewport.Y      = y;
        g_viewport.Width  = static_cast<DWORD>(right - x);
        g_viewport.Height = static_cast<DWORD>(bottom - y);
        g_viewport.MinZ   = pViewport->MinZ;
        g_viewport.MaxZ   = (pViewport->MaxZ > pViewport->MinZ) ? pViewport->MaxZ : 1.0f;
    } else {
        g_viewport = { 0, 0, rtW, rtH, 0.0f, 1.0f };
    }

    memcpy(reinterpret_cast<void*>(ADDR_D3D_DEVICE + 0xEE0), &g_viewport, sizeof(XboxViewport));
    *reinterpret_cast<uint32_t*>(0x006C03E8) |= 0x100u;
    g_scissorEnabled = false;

    if (g_d3dCtx) {
        D3D11_VIEWPORT vp11 = {};
        vp11.TopLeftX = static_cast<float>(g_viewport.X);
        vp11.TopLeftY = static_cast<float>(g_viewport.Y);
        vp11.Width    = static_cast<float>(g_viewport.Width);
        vp11.Height   = static_cast<float>(g_viewport.Height);
        vp11.MinDepth = g_viewport.MinZ;
        vp11.MaxDepth = g_viewport.MaxZ;
        g_d3dCtx->RSSetViewports(1, &vp11);
    }
}

#pragma pack(push, 1)
struct XboxMaterial8 {
    float Diffuse[4];
    float Ambient[4];
    float Specular[4];
    float Emissive[4];
    float Power;
};

struct XboxLight8 {
    uint32_t Type; // 1=POINT, 2=SPOT, 3=DIRECTIONAL
    float    Diffuse[4];
    float    Specular[4];
    float    Ambient[4];
    float    Position[3];
    float    Direction[3];
    float    Range;
    float    Falloff;
    float    Attenuation0;
    float    Attenuation1;
    float    Attenuation2;
    float    Theta;
    float    Phi;
};
#pragma pack(pop)

static XboxMaterial8 g_material = {
    { 1.0f, 1.0f, 1.0f, 1.0f },
    { 1.0f, 1.0f, 1.0f, 1.0f },
    { 0.0f, 0.0f, 0.0f, 0.0f },
    { 0.0f, 0.0f, 0.0f, 0.0f },
    0.0f
};
static bool       g_hasMaterial      = false;
static XboxLight8 g_lights[16]       = {};
static bool       g_lightEnabled[16] = {};

static void __stdcall Bridge_D3DDevice_SetMaterial(const void* pMaterial) {
    if (pMaterial) {
        memcpy(reinterpret_cast<void*>(ADDR_D3D_DEVICE + 0xF00), pMaterial, 68);
        memcpy(&g_material, pMaterial, sizeof(XboxMaterial8));
        g_hasMaterial = true;
    }
}

static void __stdcall Bridge_D3DDevice_SetLight(DWORD Index, const void* pLight) {
    if (Index < 16 && pLight) {
        memcpy(&g_lights[Index], pLight, sizeof(XboxLight8));
    }
}

static void __stdcall Bridge_D3DDevice_LightEnable(DWORD Index, BOOL bEnable) {
    if (Index < 16) {
        g_lightEnabled[Index] = (bEnable != FALSE);
    }
}

static void __stdcall Bridge_D3DDevice_SetScissors(DWORD Count, BOOL bExclusive, const RECT* pRects) {
    (void)bExclusive;
    if (!g_d3dCtx) return;
    if (Count > 0 && pRects) {
        g_scissorEnabled = true;
        g_d3dCtx->RSSetScissorRects(1, &pRects[0]);
    } else {
        g_scissorEnabled = false;
    }
}

// ============================================================================
// Vertex Layout Decoding & Draw Call Execution on D3D11
// ============================================================================

static void UnpackD3DColor(uint32_t c, float* r, float* g, float* b, float* a) {
    *a = ((c >> 24) & 0xFFu) * (1.0f / 255.0f);
    *r = ((c >> 16) & 0xFFu) * (1.0f / 255.0f);
    *g = ((c >> 8)  & 0xFFu) * (1.0f / 255.0f);
    *b = (c         & 0xFFu) * (1.0f / 255.0f);
}

static void DecodeVerticesToUnified(
    const uint8_t* srcData, uint32_t stride, uint32_t vertexCount,
    const uint16_t* optIndices, uint32_t baseVertexIndex,
    std::vector<UnifiedVertex>& outVerts, bool* outIsRhw)
{
    XboxShaderDeclInfo decl = {};
    decl.vsType = -1;
    if ((g_currentVsHandle & 0x00010001u) == 0x00010001u) {
        uint32_t idx = (g_currentVsHandle >> 4) - 1;
        if (idx < g_vertexShaders.size()) {
            decl = g_vertexShaders[idx];
        }
    }
    if (!decl.hasPos) {
        // Decode as FVF (or infer from stride if programmable handle with no decl)
        uint32_t fvf = ((g_currentVsHandle & 1u) == 0) ? g_currentVsHandle : 0x112u;
        if ((g_currentVsHandle & 1u) != 0) {
            if (stride == 28)      fvf = 0x144u;
            else if (stride == 24) fvf = 0x142u;
            else if (stride == 36) fvf = 0x152u;
            else if (stride == 40) fvf = 0x212u;
            else if (stride == 44) fvf = 0x252u;
            else if (stride == 20) fvf = 0x102u;
            else if (stride == 16) fvf = 0x042u;
            else if (stride == 12) fvf = 0x002u;
            else                   fvf = 0x112u;
        }
        uint16_t off = 0;
        uint32_t posMask = fvf & 0x0Eu;
        if (posMask == 0x04u) {
            decl.hasPos = true; decl.offPos = off; off += 12;
            decl.hasRhw = true; decl.offRhw = off; off += 4;
        } else if (posMask != 0) {
            decl.hasPos = true; decl.offPos = off; off += 12;
            if (posMask >= 0x06u) {
                uint32_t numWeights = (posMask - 0x04u) >> 1;
                off += (uint16_t)(numWeights * 4);
            }
        }
        if (fvf & 0x010u) { decl.hasNorm = true; decl.offNorm = off; off += 12; }
        if (fvf & 0x020u) { off += 4; } // D3DFVF_PSIZE
        if (fvf & 0x040u) { decl.hasDiff = true; decl.offDiff = off; off += 4; }
        if (fvf & 0x080u) { off += 4; } // D3DFVF_SPECULAR
        uint32_t texCount = (fvf >> 8) & 0x0Fu;
        if (texCount >= 1) { decl.hasUv0 = true; decl.offUv0 = off; off += 8; }
        if (texCount >= 2) { decl.hasUv1 = true; decl.offUv1 = off; off += 8; }
        if (texCount >= 3) { decl.hasUv2 = true; decl.offUv2 = off; off += 8; }
        if (texCount >= 4) { decl.hasUv3 = true; decl.offUv3 = off; off += 8; }
    }

    *outIsRhw = decl.hasRhw;
    outVerts.resize(vertexCount);

    const uint32_t* xboxRS  = reinterpret_cast<const uint32_t*>(ADDR_D3D_RENDERSTATE);
    const uint32_t* xboxTSS = reinterpret_cast<const uint32_t*>(ADDR_D3D_TEXTURESTATE);
    const Mat44*    texMats = reinterpret_cast<const Mat44*>(ADDR_D3D_DEVICE + 0xC60 + 2 * 64); // D3DTS_TEXTURE0..3
    const float*    vsC126  = reinterpret_cast<const float*>(0x006BF7D0 + 126 * 16);

    // Xbox D3DRS_LIGHTING is index 102 (0x006C0788); AMBIENTMATERIALSOURCE is 111 (0x006C07AC);
    // DIFFUSEMATERIALSOURCE is 113 (0x006C07B4); D3DRS_AMBIENT is 115 (0x006C07BC);
    // cRenderState::m_lightingMode is at 0x008AA460 (0=unlit, 1=dyn+prelit, 2=prelit, 3=prelit+dyn, 4=pure-dyn)
    uint32_t lightingMode = *reinterpret_cast<const uint32_t*>(0x008AA460);
    bool lightingEnabled  = (xboxRS[102] != 0);
    bool pureDynamicMode  = (lightingEnabled && xboxRS[111] == 0 && xboxRS[113] == 0); // SetLighting(4)
    float ambR = 0.0f, ambG = 0.0f, ambB = 0.0f, ambA = 1.0f;
    UnpackD3DColor(xboxRS[115], &ambR, &ambG, &ambB, &ambA);
    if (ambA <= 0.001f) ambA = 1.0f;

    Mat44 worldView = MulMat44(g_matWorld, g_matView);
    Mat44 wvp       = MulMat44(worldView, g_matProj);

    for (uint32_t i = 0; i < vertexCount; ++i) {
        uint32_t vIdx = optIndices ? ((uint32_t)optIndices[i] + baseVertexIndex) : i;
        const uint8_t* vPtr = srcData + vIdx * stride;
        UnifiedVertex& dst = outVerts[i];

        if (decl.hasPos) {
            const float* p = reinterpret_cast<const float*>(vPtr + decl.offPos);
            dst.x = p[0]; dst.y = p[1]; dst.z = p[2]; dst.w = 1.0f;
        } else {
            dst.x = 0.0f; dst.y = 0.0f; dst.z = 0.0f; dst.w = 1.0f;
        }
        if (decl.hasRhw) {
            dst.w = *reinterpret_cast<const float*>(vPtr + decl.offRhw);
        }
        if (decl.hasNorm) {
            const float* n = reinterpret_cast<const float*>(vPtr + decl.offNorm);
            dst.nx = n[0]; dst.ny = n[1]; dst.nz = n[2];
        } else {
            dst.nx = 0.0f; dst.ny = 1.0f; dst.nz = 0.0f;
        }

        float wx = 0.0f, wy = 1.0f, wz = 0.0f;
        float wpx = dst.x, wpy = dst.y, wpz = dst.z;
        if (!decl.hasRhw) {
            wpx = dst.x * g_matWorld.m[0][0] + dst.y * g_matWorld.m[1][0] + dst.z * g_matWorld.m[2][0] + g_matWorld.m[3][0];
            wpy = dst.x * g_matWorld.m[0][1] + dst.y * g_matWorld.m[1][1] + dst.z * g_matWorld.m[2][1] + g_matWorld.m[3][1];
            wpz = dst.x * g_matWorld.m[0][2] + dst.y * g_matWorld.m[1][2] + dst.z * g_matWorld.m[2][2] + g_matWorld.m[3][2];
            if (decl.hasNorm) {
                wx = dst.nx * g_matWorld.m[0][0] + dst.ny * g_matWorld.m[1][0] + dst.nz * g_matWorld.m[2][0];
                wy = dst.nx * g_matWorld.m[0][1] + dst.ny * g_matWorld.m[1][1] + dst.nz * g_matWorld.m[2][1];
                wz = dst.nx * g_matWorld.m[0][2] + dst.ny * g_matWorld.m[1][2] + dst.nz * g_matWorld.m[2][2];
                float len = sqrtf(wx * wx + wy * wy + wz * wz);
                if (len > 0.0001f) { wx /= len; wy /= len; wz /= len; }
            }
        }

        auto evalDynamicLights = [&](float matDiffR, float matDiffG, float matDiffB, float matAmbR, float matAmbG, float matAmbB, float& totR, float& totG, float& totB) {
            for (int l = 0; l < 16; ++l) {
                if (!g_lightEnabled[l]) continue;
                const XboxLight8& lt = g_lights[l];
                float lx = 0.0f, ly = 0.0f, lz = 0.0f, atten = 1.0f;
                if (lt.Type == 3) { // D3DLIGHT_DIRECTIONAL
                    lx = -lt.Direction[0]; ly = -lt.Direction[1]; lz = -lt.Direction[2];
                    float lLen = sqrtf(lx * lx + ly * ly + lz * lz);
                    if (lLen > 0.0001f) { lx /= lLen; ly /= lLen; lz /= lLen; }
                } else if (lt.Type == 1 || lt.Type == 2) { // POINT or SPOT
                    lx = lt.Position[0] - wpx;
                    ly = lt.Position[1] - wpy;
                    lz = lt.Position[2] - wpz;
                    float dist = sqrtf(lx * lx + ly * ly + lz * lz);
                    if (lt.Range > 0.0f && dist > lt.Range) continue;
                    if (dist > 0.0001f) { lx /= dist; ly /= dist; lz /= dist; }
                    float denom = lt.Attenuation0 + lt.Attenuation1 * dist + lt.Attenuation2 * dist * dist;
                    if (denom > 0.0001f) atten = 1.0f / denom;
                    if (lt.Type == 2) { // SPOT
                        float sdx = lt.Direction[0], sdy = lt.Direction[1], sdz = lt.Direction[2];
                        float sdLen = sqrtf(sdx * sdx + sdy * sdy + sdz * sdz);
                        if (sdLen > 0.0001f) { sdx /= sdLen; sdy /= sdLen; sdz /= sdLen; }
                        float cosAlpha = -(lx * sdx + ly * sdy + lz * sdz);
                        float cosHalfPhi = cosf(lt.Phi * 0.5f);
                        float cosHalfTheta = cosf(lt.Theta * 0.5f);
                        if (cosAlpha <= cosHalfPhi) continue;
                        if (cosAlpha < cosHalfTheta && (cosHalfTheta - cosHalfPhi) > 0.0001f) {
                            atten *= powf((cosAlpha - cosHalfPhi) / (cosHalfTheta - cosHalfPhi), lt.Falloff > 0.0f ? lt.Falloff : 1.0f);
                        }
                    }
                } else {
                    continue;
                }
                float ndotl = wx * lx + wy * ly + wz * lz;
                if (ndotl < 0.0f) ndotl = 0.0f;
                totR += atten * (lt.Ambient[0] * matAmbR + ndotl * lt.Diffuse[0] * matDiffR);
                totG += atten * (lt.Ambient[1] * matAmbG + ndotl * lt.Diffuse[1] * matDiffG);
                totB += atten * (lt.Ambient[2] * matAmbB + ndotl * lt.Diffuse[2] * matDiffB);
            }
        };

        if (decl.hasDiff && !pureDynamicMode) {
            uint32_t c = *reinterpret_cast<const uint32_t*>(vPtr + decl.offDiff);
            UnpackD3DColor(c, &dst.r, &dst.g, &dst.b, &dst.a);
            if (!decl.hasRhw && decl.hasNorm && decl.vsType == 1) { // defaultWorldLit0..3.xvu
                float totR = dst.r + ambR;
                float totG = dst.g + ambG;
                float totB = dst.b + ambB;
                evalDynamicLights(1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, totR, totG, totB);
                dst.r = totR < 0.0f ? 0.0f : (totR > 1.0f ? 1.0f : totR);
                dst.g = totG < 0.0f ? 0.0f : (totG > 1.0f ? 1.0f : totG);
                dst.b = totB < 0.0f ? 0.0f : (totB > 1.0f ? 1.0f : totB);
            }
        } else if (decl.hasNorm && !decl.hasRhw) {
            if (lightingMode != 0 && (lightingEnabled || g_curPrelitColors != nullptr)) {
                float matDiffR = g_hasMaterial ? g_material.Diffuse[0] : 1.0f;
                float matDiffG = g_hasMaterial ? g_material.Diffuse[1] : 1.0f;
                float matDiffB = g_hasMaterial ? g_material.Diffuse[2] : 1.0f;
                float matAmbR  = g_hasMaterial ? g_material.Ambient[0] : 1.0f;
                float matAmbG  = g_hasMaterial ? g_material.Ambient[1] : 1.0f;
                float matAmbB  = g_hasMaterial ? g_material.Ambient[2] : 1.0f;

                float totR = lightingEnabled ? (ambR * matAmbR + (g_hasMaterial ? g_material.Emissive[0] : 0.0f)) : 0.0f;
                float totG = lightingEnabled ? (ambG * matAmbG + (g_hasMaterial ? g_material.Emissive[1] : 0.0f)) : 0.0f;
                float totB = lightingEnabled ? (ambB * matAmbB + (g_hasMaterial ? g_material.Emissive[2] : 0.0f)) : 0.0f;

                if (!pureDynamicMode && g_curPrelitColors != nullptr) {
                    uint32_t preIdx = g_curPrelitBaseVertex + (optIndices ? (uint32_t)optIndices[i] : i);
                    float preR = 0.0f, preG = 0.0f, preB = 0.0f, preA = 1.0f;
                    UnpackD3DColor(g_curPrelitColors[preIdx], &preR, &preG, &preB, &preA);
                    totR += preR;
                    totG += preG;
                    totB += preB;
                }

                if (lightingEnabled) {
                    evalDynamicLights(matDiffR, matDiffG, matDiffB, matAmbR, matAmbG, matAmbB, totR, totG, totB);
                }

                dst.r = totR < 0.0f ? 0.0f : (totR > 1.0f ? 1.0f : totR);
                dst.g = totG < 0.0f ? 0.0f : (totG > 1.0f ? 1.0f : totG);
                dst.b = totB < 0.0f ? 0.0f : (totB > 1.0f ? 1.0f : totB);
                dst.a = (ambA > 0.001f && ambA <= 1.0f) ? ambA : 1.0f;
            } else {
                dst.r = 1.0f; dst.g = 1.0f; dst.b = 1.0f;
                dst.a = (ambA > 0.001f && ambA <= 1.0f) ? ambA : 1.0f;
            }
        } else {
            dst.r = 1.0f; dst.g = 1.0f; dst.b = 1.0f; dst.a = 1.0f;
        }

        // Read raw UV0..3 from vertex stream
        float rawU[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
        float rawV[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
        if (decl.hasUv0) {
            const float* uv = reinterpret_cast<const float*>(vPtr + decl.offUv0);
            rawU[0] = uv[0]; rawV[0] = uv[1];
        }
        if (decl.hasUv1) {
            const float* uv = reinterpret_cast<const float*>(vPtr + decl.offUv1);
            rawU[1] = uv[0]; rawV[1] = uv[1];
        } else {
            rawU[1] = rawU[0]; rawV[1] = rawV[0];
        }
        if (decl.hasUv2) {
            const float* uv = reinterpret_cast<const float*>(vPtr + decl.offUv2);
            rawU[2] = uv[0]; rawV[2] = uv[1];
        } else {
            rawU[2] = rawU[0]; rawV[2] = rawV[0];
        }
        if (decl.hasUv3) {
            const float* uv = reinterpret_cast<const float*>(vPtr + decl.offUv3);
            rawU[3] = uv[0]; rawV[3] = uv[1];
        } else {
            rawU[3] = rawU[0]; rawV[3] = rawV[0];
        }

        // Custom 3D Programmable Vertex Shaders (.xvu 0x13..0x32 and specialized c*Shader::VertexSetup bridges)
        if (!decl.hasRhw && (g_activeCustomVS != 0 || (decl.vsType >= 0x13 && decl.vsType <= 0x32))) {
            const uint8_t* pCam = *reinterpret_cast<const uint8_t* const*>(0x0088CD6C);
            const float* camPos = pCam ? reinterpret_cast<const float*>(pCam + 0x30) : reinterpret_cast<const float*>(0x008ACC38);
            float vx = camPos[0] - wpx, vy = camPos[1] - wpy, vz = camPos[2] - wpz;
            float vLen = sqrtf(vx * vx + vy * vy + vz * vz);
            if (vLen > 0.0001f) { vx /= vLen; vy /= vLen; vz /= vLen; }
            float ndotv = vx * wx + vy * wy + vz * wz;
            if (ndotv < 0.0f) ndotv = 0.0f; else if (ndotv > 1.0f) ndotv = 1.0f;

            if (g_activeCustomVS == 1 || (decl.vsType >= 0x1C && decl.vsType <= 0x1E)) { // cCloakShader (cloak*.xvu)
                float clipX = dst.x * wvp.m[0][0] + dst.y * wvp.m[1][0] + dst.z * wvp.m[2][0] + wvp.m[3][0];
                float clipY = dst.x * wvp.m[0][1] + dst.y * wvp.m[1][1] + dst.z * wvp.m[2][1] + wvp.m[3][1];
                float clipW = dst.x * wvp.m[0][3] + dst.y * wvp.m[1][3] + dst.z * wvp.m[2][3] + wvp.m[3][3];
                float invW  = (fabsf(clipW) > 0.0001f) ? (1.0f / clipW) : 1.0f;
                float scrX  = (clipX * invW * 0.5f + 0.5f) * 640.0f;
                float scrY  = (0.5f - clipY * invW * 0.5f) * 480.0f;

                const float* camRight = pCam ? reinterpret_cast<const float*>(pCam + 0x40) : reinterpret_cast<const float*>(0x008ACC50);
                const float* camUp    = pCam ? reinterpret_cast<const float*>(pCam + 0x50) : reinterpret_cast<const float*>(0x008ACC60);
                float rnx = wx * camRight[0] + wy * camRight[1] + wz * camRight[2];
                float rny = wx * camUp[0]    + wy * camUp[1]    + wz * camUp[2];

                // Stage 0 (backbuffer 640x480 linear texture) UV in pixel coordinates [0..639, 0..479]
                float u0 = scrX + rnx * g_cloakDistort;
                float v0 = scrY - rny * g_cloakDistort;
                if (u0 < 0.0f) u0 = 0.0f; else if (u0 > 639.0f) u0 = 639.0f;
                if (v0 < 0.0f) v0 = 0.0f; else if (v0 > 479.0f) v0 = 479.0f;
                dst.u0 = u0;
                dst.v0 = v0;

                // Stage 1 (Membrane Fresnel ramp): u1 = saturate(dot(V, wNorm)), v1 = c35.w
                dst.u1 = ndotv;
                dst.v1 = g_cloakC35[3];

                // Stage 2 (Dissolve / mask pattern) & Stage 3 (Base diffuse texture)
                dst.u2 = (rawU[0] + g_cloakC35[1]) * g_cloakC35[0];
                dst.v2 = (rawV[0] + g_cloakC35[2]) * g_cloakC35[0];
                dst.u3 = rawU[0];
                dst.v3 = rawV[0];
                continue;
            } else if (decl.vsType == 0x1B) { // heatShimmer0.xvu (cHeatShader)
                float clipX = dst.x * wvp.m[0][0] + dst.y * wvp.m[1][0] + dst.z * wvp.m[2][0] + wvp.m[3][0];
                float clipY = dst.x * wvp.m[0][1] + dst.y * wvp.m[1][1] + dst.z * wvp.m[2][1] + wvp.m[3][1];
                float clipW = dst.x * wvp.m[0][3] + dst.y * wvp.m[1][3] + dst.z * wvp.m[2][3] + wvp.m[3][3];
                float invW  = (fabsf(clipW) > 0.0001f) ? (1.0f / clipW) : 1.0f;
                float scrX  = (clipX * invW * 0.5f + 0.5f) * 640.0f;
                float scrY  = (0.5f - clipY * invW * 0.5f) * 480.0f;
                if (scrX < 0.0f) scrX = 0.0f; else if (scrX > 639.0f) scrX = 639.0f;
                if (scrY < 0.0f) scrY = 0.0f; else if (scrY > 479.0f) scrY = 479.0f;
                dst.u0 = rawU[0] * vsC126[0 * 4 + 0] + rawV[0] * vsC126[0 * 4 + 1] + vsC126[0 * 4 + 3] + vsC126[2 * 4 + 0];
                dst.v0 = rawU[0] * vsC126[1 * 4 + 0] + rawV[0] * vsC126[1 * 4 + 1] + vsC126[1 * 4 + 3] + vsC126[2 * 4 + 1];
                dst.u1 = scrX;    dst.v1 = scrY;
                dst.u2 = rawU[0]; dst.v2 = rawV[0];
                dst.u3 = rawU[0]; dst.v3 = rawV[0];
                continue;
            } else if (g_activeCustomVS == 2 || (decl.vsType >= 0x17 && decl.vsType <= 0x1A)) { // cScreenEffectShader (ScreenUV*.xvu)
                float clipX = dst.x * wvp.m[0][0] + dst.y * wvp.m[1][0] + dst.z * wvp.m[2][0] + wvp.m[3][0];
                float clipY = dst.x * wvp.m[0][1] + dst.y * wvp.m[1][1] + dst.z * wvp.m[2][1] + wvp.m[3][1];
                float clipW = dst.x * wvp.m[0][3] + dst.y * wvp.m[1][3] + dst.z * wvp.m[2][3] + wvp.m[3][3];
                float invW  = (fabsf(clipW) > 0.0001f) ? (1.0f / clipW) : 1.0f;
                float scrX  = (clipX * invW * 0.5f + 0.5f) * 640.0f;
                float scrY  = (0.5f - clipY * invW * 0.5f) * 480.0f;
                if (scrX < 0.0f) scrX = 0.0f; else if (scrX > 639.0f) scrX = 639.0f;
                if (scrY < 0.0f) scrY = 0.0f; else if (scrY > 479.0f) scrY = 479.0f;
                dst.u0 = rawU[0]; dst.v0 = rawV[0];
                dst.u1 = scrX;    dst.v1 = scrY;
                dst.u2 = rawU[1]; dst.v2 = rawV[1];
                dst.u3 = rawU[2]; dst.v3 = rawV[2];
                continue;
            } else if (decl.vsType >= 0x26 && decl.vsType <= 0x29) { // cSightShader additive mode (sightMemb*Add.xvu)
                dst.u0 = rawU[0]; dst.v0 = rawV[0];
                dst.u1 = ndotv;   dst.v1 = ndotv;
                dst.u2 = rawU[0]; dst.v2 = rawV[0];
                dst.u3 = rawU[0]; dst.v3 = rawV[0];
                continue;
            } else if (g_activeCustomVS == 3 || g_activeCustomVS == 4 || (decl.vsType >= 0x1F && decl.vsType <= 0x25)) { // cMembraneShader / cSightShader
                dst.u0 = ndotv;   dst.v0 = ndotv;
                dst.u1 = rawU[0]; dst.v1 = rawV[0];
                dst.u2 = rawU[0]; dst.v2 = rawV[0];
                dst.u3 = rawU[0]; dst.v3 = rawV[0];
                continue;
            } else if (g_activeCustomVS == 5 || (decl.vsType >= 0x30 && decl.vsType <= 0x32)) { // cShieldShader (shield*.xvu)
                dst.u0 = rawU[0];              dst.v0 = rawV[0];
                dst.u1 = rawU[0];              dst.v1 = rawV[0];
                dst.u2 = ndotv * g_shieldC35x; dst.v2 = 0.5f;
                dst.u3 = rawU[0];              dst.v3 = rawV[0];
                continue;
            } else if (g_activeCustomVS == 6 || (decl.vsType >= 0x2D && decl.vsType <= 0x2F)) { // cTeleportShader (teleport*.xvu)
                float clipX = dst.x * wvp.m[0][0] + dst.y * wvp.m[1][0] + dst.z * wvp.m[2][0] + wvp.m[3][0];
                float clipY = dst.x * wvp.m[0][1] + dst.y * wvp.m[1][1] + dst.z * wvp.m[2][1] + wvp.m[3][1];
                float clipW = dst.x * wvp.m[0][3] + dst.y * wvp.m[1][3] + dst.z * wvp.m[2][3] + wvp.m[3][3];
                float invW  = (fabsf(clipW) > 0.0001f) ? (1.0f / clipW) : 1.0f;
                float scrX  = (clipX * invW * 0.5f + 0.5f) * 640.0f;
                float scrY  = (0.5f - clipY * invW * 0.5f) * 480.0f;
                dst.u0 = rawU[0]; dst.v0 = rawV[0];
                dst.u1 = scrX * g_teleportC32[0] + g_teleportC33x;
                dst.v1 = scrY * g_teleportC32[1] + g_teleportC33x;
                dst.u2 = rawU[0]; dst.v2 = rawV[0];
                dst.u3 = rawU[0]; dst.v3 = rawV[0];
                continue;
            } else if (g_activeCustomVS == 7 || (decl.vsType >= 0x2A && decl.vsType <= 0x2C)) { // cMotionBlur (motionBlur*.xvu)
                float d = wx * g_motionBlurVec[0] + wy * g_motionBlurVec[1] + wz * g_motionBlurVec[2];
                float backFacing = (d < 0.0f) ? 1.0f : 0.0f;
                dst.x -= backFacing * g_motionBlurVec[0];
                dst.y -= backFacing * g_motionBlurVec[1];
                dst.z -= backFacing * g_motionBlurVec[2];
                dst.a  = backFacing * g_motionBlurVec[3];
                dst.u0 = rawU[0]; dst.v0 = rawV[0];
                dst.u1 = rawU[0]; dst.v1 = rawV[0];
                dst.u2 = rawU[0]; dst.v2 = rawV[0];
                dst.u3 = rawU[0]; dst.v3 = rawV[0];
                continue;
            } else if (g_activeCustomVS == 8 || (decl.vsType >= 0x13 && decl.vsType <= 0x16)) { // cBumpEnvShader (BumpReflection*.xvu)
                float rx = 2.0f * ndotv * wx - vx;
                float ry = 2.0f * ndotv * wy - vy;
                dst.u0 = rawU[0];             dst.v0 = rawV[0];
                dst.u1 = rawU[0];             dst.v1 = rawV[0];
                dst.u2 = rawU[0];             dst.v2 = rawV[0];
                dst.u3 = rx * 0.5f + 0.5f;    dst.v3 = 0.5f - ry * 0.5f;
                continue;
            }
        }

        // Screen-space 2D programmable vertex shaders (.xvu: 0x33..0x37, 0x3C)
        if (decl.hasRhw && (decl.vsType == 0x33 || decl.vsType == 0x34 || decl.vsType == 0x35 ||
                            decl.vsType == 0x36 || decl.vsType == 0x37 || decl.vsType == 0x3C))
        {
            if ((decl.vsType == 0x35 || decl.vsType == 0x36) &&
                dst.x >= -1.5f && dst.x <= 1.5f && dst.y >= -1.5f && dst.y <= 1.5f)
            {
                float vpW = (float)(g_viewport.Width  ? g_viewport.Width  : 640);
                float vpH = (float)(g_viewport.Height ? g_viewport.Height : 480);
                dst.x = (float)g_viewport.X + (dst.x * 0.5f + 0.5f) * vpW;
                dst.y = (float)g_viewport.Y + (0.5f - dst.y * 0.5f) * vpH;
            }
            if (decl.vsType == 0x33 || decl.vsType == 0x36) { // shield / gaussBlur
                dst.u0 = rawU[0] + vsC126[0 * 4 + 0]; dst.v0 = rawV[0] + vsC126[0 * 4 + 1];
                dst.u1 = rawU[0] + vsC126[1 * 4 + 0]; dst.v1 = rawV[0] + vsC126[1 * 4 + 1];
                dst.u2 = rawU[0] + vsC126[2 * 4 + 0]; dst.v2 = rawV[0] + vsC126[2 * 4 + 1];
                dst.u3 = rawU[0] + vsC126[3 * 4 + 0]; dst.v3 = rawV[0] + vsC126[3 * 4 + 1];
            } else if (decl.vsType == 0x34) { // waterNM
                dst.u0 = rawU[0] + vsC126[0 * 4 + 0]; dst.v0 = rawV[0] + vsC126[0 * 4 + 1];
                dst.u1 = rawU[0] + vsC126[1 * 4 + 0]; dst.v1 = rawV[0] + vsC126[1 * 4 + 1];
                dst.u2 = rawU[0];                     dst.v2 = rawV[0];
                dst.u3 = rawU[0];                     dst.v3 = rawV[0];
            } else if (decl.vsType == 0x35) { // screenHaze
                dst.u0 = rawU[0];                     dst.v0 = rawV[0];
                dst.u1 = rawU[0] + vsC126[0 * 4 + 0]; dst.v1 = rawV[0] + vsC126[0 * 4 + 1];
                dst.u2 = rawU[0] + vsC126[1 * 4 + 0]; dst.v2 = rawV[0] + vsC126[1 * 4 + 1];
                dst.u3 = rawU[0] + vsC126[2 * 4 + 0]; dst.v3 = rawV[0] + vsC126[2 * 4 + 1];
            } else if (decl.vsType == 0x37) { // dof
                dst.u0 = rawU[0] * vsC126[0 * 4 + 0]; dst.v0 = rawV[0] * vsC126[0 * 4 + 1];
                dst.u1 = rawU[0] * vsC126[1 * 4 + 0]; dst.v1 = rawV[0] * vsC126[1 * 4 + 1];
                dst.u2 = dst.u1;                      dst.v2 = dst.v1;
                dst.u3 = dst.u1;                      dst.v3 = dst.v1;
            } else { // 0x3C: ScreenUVScale
                dst.u0 = rawU[0] * vsC126[0 * 4 + 0]; dst.v0 = rawV[0] * vsC126[0 * 4 + 1];
                dst.u1 = rawU[0] * vsC126[1 * 4 + 0]; dst.v1 = rawV[0] * vsC126[1 * 4 + 1];
                dst.u2 = rawU[0];                     dst.v2 = rawV[0];
                dst.u3 = rawU[0];                     dst.v3 = rawV[0];
            }
            continue;
        }

        // Standard 4-stage UV routing: D3DTSS_TEXCOORDINDEX (xboxTSS[s*32 + 28]) & D3DTSS_TEXTURETRANSFORMFLAGS (xboxTSS[s*32 + 21])
        float camNx = 0.0f, camNy = 0.0f, camNz = 1.0f;
        float camPx = 0.0f, camPy = 0.0f, camPz = 1.0f;
        bool  camComputed = false;

        float finalU[4], finalV[4];
        for (uint32_t s = 0; s < 4; ++s) {
            uint32_t tci = xboxTSS[s * XBOX_D3DTSS_MAX + XBOX_D3DTSS_TEXCOORDINDEX];
            uint32_t ttf = xboxTSS[s * XBOX_D3DTSS_MAX + XBOX_D3DTSS_TEXTURETRANSFORMFLAGS];
            uint32_t srcIdx = tci & 0x03u;
            uint32_t genMode = tci & 0xFFFF0000u;

            float tu = rawU[srcIdx];
            float tv = rawV[srcIdx];
            float tw = 1.0f;
            bool  is3DCoord = false;

            if (!decl.hasRhw && genMode != 0) {
                if (!camComputed) {
                    camNx = dst.nx * worldView.m[0][0] + dst.ny * worldView.m[1][0] + dst.nz * worldView.m[2][0];
                    camNy = dst.nx * worldView.m[0][1] + dst.ny * worldView.m[1][1] + dst.nz * worldView.m[2][1];
                    camNz = dst.nx * worldView.m[0][2] + dst.ny * worldView.m[1][2] + dst.nz * worldView.m[2][2];
                    float nLen = sqrtf(camNx * camNx + camNy * camNy + camNz * camNz);
                    if (nLen > 0.0001f) { camNx /= nLen; camNy /= nLen; camNz /= nLen; }

                    camPx = dst.x * worldView.m[0][0] + dst.y * worldView.m[1][0] + dst.z * worldView.m[2][0] + worldView.m[3][0];
                    camPy = dst.x * worldView.m[0][1] + dst.y * worldView.m[1][1] + dst.z * worldView.m[2][1] + worldView.m[3][1];
                    camPz = dst.x * worldView.m[0][2] + dst.y * worldView.m[1][2] + dst.z * worldView.m[2][2] + worldView.m[3][2];
                    camComputed = true;
                }
                constexpr uint32_t kXboxTciCameraSpaceNormal           = 0x00010000u;
                constexpr uint32_t kXboxTciCameraSpacePosition         = 0x00020000u;
                constexpr uint32_t kXboxTciCameraSpaceReflectionVector = 0x00030000u;
                if (genMode == kXboxTciCameraSpaceNormal) {
                    tu = camNx; tv = camNy; tw = camNz; is3DCoord = true;
                } else if (genMode == kXboxTciCameraSpacePosition) {
                    tu = camPx; tv = camPy; tw = camPz; is3DCoord = true;
                } else if (genMode == kXboxTciCameraSpaceReflectionVector) {
                    float pLen = sqrtf(camPx * camPx + camPy * camPy + camPz * camPz);
                    float vx = (pLen > 0.0001f) ? (camPx / pLen) : 0.0f;
                    float vy = (pLen > 0.0001f) ? (camPy / pLen) : 0.0f;
                    float vz = (pLen > 0.0001f) ? (camPz / pLen) : 1.0f;
                    float dotVN = vx * camNx + vy * camNy + vz * camNz;
                    tu = vx - 2.0f * dotVN * camNx;
                    tv = vy - 2.0f * dotVN * camNy;
                    tw = vz - 2.0f * dotVN * camNz;
                    is3DCoord = true;
                }
            }

            constexpr uint32_t kXboxTciDirectionMask  = 0x00010000u; // Normal (0x10000) & ReflectionVector (0x30000) have bit 16 set
            constexpr uint32_t kXboxTtffProjectedFlag = 0x00000100u;
            uint32_t dimCount = ttf & 0xFFu;
            if (!decl.hasRhw && dimCount > 0 && dimCount <= 4) {
                const Mat44& tm = texMats[s];
                float inZ = is3DCoord ? tw : 1.0f;
                // Direction vectors (normal / reflection vector) must not include translation row m[3][0..2]
                float inW = ((genMode & kXboxTciDirectionMask) != 0u) ? 0.0f : 1.0f;
                float outU = tu * tm.m[0][0] + tv * tm.m[1][0] + inZ * tm.m[2][0] + inW * tm.m[3][0];
                float outV = tu * tm.m[0][1] + tv * tm.m[1][1] + inZ * tm.m[2][1] + inW * tm.m[3][1];
                float outZ = tu * tm.m[0][2] + tv * tm.m[1][2] + inZ * tm.m[2][2] + inW * tm.m[3][2];
                float outW = tu * tm.m[0][3] + tv * tm.m[1][3] + inZ * tm.m[2][3] + inW * tm.m[3][3];
                if ((ttf & kXboxTtffProjectedFlag) != 0u) { // D3DTTFF_PROJECTED
                    float denom = (dimCount == 3u) ? outZ : outW;
                    if (fabsf(denom) > 0.0001f) {
                        outU /= denom;
                        outV /= denom;
                    }
                    tu = outU;
                    tv = outV;
                } else if (is3DCoord) {
                    // Non-projected 3D reflection/normal vector (e.g. SetUVReflection D3DTTFF_COUNT3):
                    // normalize direction and map [-1, +1] -> [0, 1] UV space
                    float rLen = sqrtf(outU * outU + outV * outV + outZ * outZ);
                    if (rLen > 0.0001f) {
                        outU /= rLen;
                        outV /= rLen;
                    }
                    tu = outU * 0.5f + 0.5f;
                    tv = 0.5f - outV * 0.5f;
                } else {
                    tu = outU;
                    tv = outV;
                }
            } else if (is3DCoord) {
                tu = tu * 0.5f + 0.5f;
                tv = 0.5f - tv * 0.5f;
            }

            finalU[s] = tu;
            finalV[s] = tv;
        }

        // cEnvmapShader (with base texture) & cMetalEnvmapShader bind the environment map on Stage 1
        // and use programmable vertex shaders (0x0B..0x0F, 0x38..0x39) or Bridge_EnvmapShader_VertexSetup
        if (!decl.hasRhw &&
            (g_activeCustomVS == kCustomVsEnvmap ||
             (decl.vsType >= 0x0Bu && decl.vsType <= 0x0Fu) ||
             (decl.vsType >= 0x38u && decl.vsType <= 0x39u)))
        {
            constexpr uint32_t kXboxTciReflectionMode = 0x00030000u;
            uint32_t stage0Gen = xboxTSS[0 * XBOX_D3DTSS_MAX + XBOX_D3DTSS_TEXCOORDINDEX] & 0xFFFF0000u;
            uint32_t stage1Gen = xboxTSS[1 * XBOX_D3DTSS_MAX + XBOX_D3DTSS_TEXCOORDINDEX] & 0xFFFF0000u;
            if (stage0Gen != kXboxTciReflectionMode && stage1Gen == 0u) {
                const uint8_t* pCam = *reinterpret_cast<const uint8_t* const*>(0x0088CD6C);
                const float* camPos = pCam ? reinterpret_cast<const float*>(pCam + 0x30) : reinterpret_cast<const float*>(0x008ACC38);
                float vx = camPos[0] - wpx;
                float vy = camPos[1] - wpy;
                float vz = camPos[2] - wpz;
                float vLen = sqrtf(vx * vx + vy * vy + vz * vz);
                if (vLen > 0.0001f) { vx /= vLen; vy /= vLen; vz /= vLen; }
                float ndotv = wx * vx + wy * vy + wz * vz;
                float rx = 2.0f * ndotv * wx - vx;
                float ry = 2.0f * ndotv * wy - vy;
                finalU[1] = rx * 0.5f + 0.5f;
                finalV[1] = 0.5f - ry * 0.5f;
            }
        }

        dst.u0 = finalU[0]; dst.v0 = finalV[0];
        dst.u1 = finalU[1]; dst.v1 = finalV[1];
        dst.u2 = finalU[2]; dst.v2 = finalV[2];
        dst.u3 = finalU[3]; dst.v3 = finalV[3];
    }
}

static void ApplyRenderTargetSurfaceD3D11(XboxPixelContainer* pRenderTarget, XboxPixelContainer* pZStencil) {
    (void)pZStencil;
    if (!g_d3dCtx) return;

    // Unbind PS shader resources before switching RTV to avoid D3D11 SRV/RTV hazard
    ID3D11ShaderResourceView* nullSrvs[4] = { nullptr, nullptr, nullptr, nullptr };
    g_d3dCtx->PSSetShaderResources(0, 4, nullSrvs);

    XboxPixelContainer* mainBB0 = *reinterpret_cast<XboxPixelContainer**>(0x008AA344);
    XboxPixelContainer* mainBB1 = *reinterpret_cast<XboxPixelContainer**>(ADDR_D3D_DEVICE + 0x1A14);
    XboxPixelContainer* mainBB2 = *reinterpret_cast<XboxPixelContainer**>(ADDR_D3D_DEVICE + 0x1A18);
    XboxPixelContainer* mainBB3 = *reinterpret_cast<XboxPixelContainer**>(ADDR_D3D_DEVICE + 0x1A1C);

    bool isMainBB = (!pRenderTarget) ||
                    (pRenderTarget == mainBB0) || (pRenderTarget == mainBB1) ||
                    (pRenderTarget == mainBB2) || (pRenderTarget == mainBB3) ||
                    (mainBB0 && pRenderTarget->Data == mainBB0->Data);

    if (isMainBB) {
        g_curOffscreenRT = nullptr;
        g_curActiveRTV   = g_rtv;
        g_curActiveDSV   = g_dsv;
        g_d3dCtx->OMSetRenderTargets(1, &g_curActiveRTV, g_curActiveDSV);
        XboxViewport vp = { 0, 0, 640, 480, 0.0f, 1.0f };
        Bridge_D3DDevice_SetViewport(&vp);
    } else {
        CachedTexture* rtEntry = GetOrCreateRenderTargetEntry(pRenderTarget);
        if (rtEntry && rtEntry->rtv) {
            g_curOffscreenRT = pRenderTarget;
            g_curActiveRTV   = rtEntry->rtv;
            g_curActiveDSV   = GetOrCreateOffscreenDSV(rtEntry->width, rtEntry->height);
            g_d3dCtx->OMSetRenderTargets(1, &g_curActiveRTV, g_curActiveDSV);
            XboxViewport vp = { 0, 0, rtEntry->width, rtEntry->height, 0.0f, 1.0f };
            Bridge_D3DDevice_SetViewport(&vp);
        }
    }
}

// ?rlMain_SetRenderTarget@@YAXPAVcTexture@@HH@Z (0x005F41F0)
static void __cdecl Bridge_rlMain_SetRenderTarget(void* pTex, int bClear, int bFast) {
    (void)bFast;
    void* prevRT = *reinterpret_cast<void**>(0x008AA350);
    if (pTex == prevRT) return;
    *reinterpret_cast<void**>(0x008AA350) = pTex; // pCurRenderTarget
    if (pTex) {
        XboxPixelContainer* d3dTex = *reinterpret_cast<XboxPixelContainer**>(reinterpret_cast<uint8_t*>(pTex) + 0x10);
        ApplyRenderTargetSurfaceD3D11(d3dTex, nullptr);
        if (bClear && g_d3dCtx && g_curActiveRTV) {
            float zero[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
            g_d3dCtx->ClearRenderTargetView(g_curActiveRTV, zero);
        }
    } else {
        ApplyRenderTargetSurfaceD3D11(nullptr, nullptr);
    }
}

static void SubmitUnifiedVerticesD3D11(uint32_t xboxPrim, std::vector<UnifiedVertex>& verts, bool isRhw) {
    if (!g_d3dDevice || !g_d3dCtx || !g_vs || !g_ps || verts.empty()) return;

    D3D11_PRIMITIVE_TOPOLOGY topo = D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
    static std::vector<UnifiedVertex> expanded;
    expanded.clear();
    const UnifiedVertex* drawPtr = verts.data();
    uint32_t drawCount = (uint32_t)verts.size();

    switch (xboxPrim) {
        case 1: // POINTLIST
            topo = D3D11_PRIMITIVE_TOPOLOGY_POINTLIST;
            break;
        case 2: // LINELIST
            topo = D3D11_PRIMITIVE_TOPOLOGY_LINELIST;
            break;
        case 3: // LINELOOP
            if (drawCount < 2) return;
            expanded = verts;
            expanded.push_back(verts[0]);
            drawPtr = expanded.data();
            drawCount = (uint32_t)expanded.size();
            topo = D3D11_PRIMITIVE_TOPOLOGY_LINESTRIP;
            break;
        case 4: // LINESTRIP
            if (drawCount < 2) return;
            topo = D3D11_PRIMITIVE_TOPOLOGY_LINESTRIP;
            break;
        case 5: // TRIANGLELIST
            if (drawCount < 3) return;
            topo = D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
            break;
        case 6: // TRIANGLESTRIP
        case 9: // QUADSTRIP
            if (drawCount < 3) return;
            topo = D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP;
            break;
        case 7: // TRIANGLEFAN
        case 10: { // POLYGON
            if (drawCount < 3) return;
            uint32_t numTris = drawCount - 2;
            expanded.resize(numTris * 3);
            for (uint32_t t = 0; t < numTris; ++t) {
                expanded[t * 3 + 0] = verts[0];
                expanded[t * 3 + 1] = verts[t + 1];
                expanded[t * 3 + 2] = verts[t + 2];
            }
            drawPtr = expanded.data();
            drawCount = (uint32_t)expanded.size();
            topo = D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
            break;
        }
        case 8: { // QUADLIST
            uint32_t numQuads = drawCount / 4;
            if (numQuads == 0) return;
            expanded.resize(numQuads * 6);
            for (uint32_t q = 0; q < numQuads; ++q) {
                expanded[q * 6 + 0] = verts[q * 4 + 0];
                expanded[q * 6 + 1] = verts[q * 4 + 1];
                expanded[q * 6 + 2] = verts[q * 4 + 2];
                expanded[q * 6 + 3] = verts[q * 4 + 0];
                expanded[q * 6 + 4] = verts[q * 4 + 2];
                expanded[q * 6 + 5] = verts[q * 4 + 3];
            }
            drawPtr = expanded.data();
            drawCount = (uint32_t)expanded.size();
            topo = D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
            break;
        }
        default:
            return;
    }

    // Ensure dynamic VB capacity (ring buffer using WRITE_NO_OVERWRITE to avoid 5MB/draw WDDM stalls)
    static uint32_t s_dynVBOffset = 0;
    if (!g_dynVB || drawCount > g_dynVBCapacity) {
        if (g_dynVB) { g_dynVB->Release(); g_dynVB = nullptr; }
        g_dynVBCapacity = (drawCount > 65536) ? (drawCount * 2) : 65536;
        s_dynVBOffset = 0;
        D3D11_BUFFER_DESC bd = {};
        bd.ByteWidth = g_dynVBCapacity * sizeof(UnifiedVertex);
        bd.Usage = D3D11_USAGE_DYNAMIC;
        bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
        bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        if (FAILED(g_d3dDevice->CreateBuffer(&bd, nullptr, &g_dynVB))) return;
    }

    D3D11_MAP mapMode = D3D11_MAP_WRITE_NO_OVERWRITE;
    if (s_dynVBOffset == 0 || (s_dynVBOffset + drawCount) > g_dynVBCapacity) {
        mapMode = D3D11_MAP_WRITE_DISCARD;
        s_dynVBOffset = 0;
    }
    uint32_t startVertex = s_dynVBOffset;
    D3D11_MAPPED_SUBRESOURCE mappedVB = {};
    if (SUCCEEDED(g_d3dCtx->Map(g_dynVB, 0, mapMode, 0, &mappedVB))) {
        memcpy(reinterpret_cast<UnifiedVertex*>(mappedVB.pData) + startVertex, drawPtr, drawCount * sizeof(UnifiedVertex));
        g_d3dCtx->Unmap(g_dynVB, 0);
        s_dynVBOffset += drawCount;
    }

    const uint32_t* xboxRS  = reinterpret_cast<const uint32_t*>(ADDR_D3D_RENDERSTATE);
    const uint32_t* xboxTSS = reinterpret_cast<const uint32_t*>(ADDR_D3D_TEXTURESTATE);

    float uScale[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
    float vScale[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
    ID3D11ShaderResourceView* srvs[4] = {
        GetOrCreateD3D11SRV(g_boundTextures[0], &uScale[0], &vScale[0]),
        GetOrCreateD3D11SRV(g_boundTextures[1], &uScale[1], &vScale[1]),
        GetOrCreateD3D11SRV(g_boundTextures[2], &uScale[2], &vScale[2]),
        GetOrCreateD3D11SRV(g_boundTextures[3], &uScale[3], &vScale[3])
    };

    // Build PerDrawCB
    PerDrawCBData cb = {};
    Mat44 wvp = MulMat44(MulMat44(g_matWorld, g_matView), g_matProj);
    for (int i = 0; i < 4; ++i) {
        cb.wvpRow0[i] = wvp.m[0][i];
        cb.wvpRow1[i] = wvp.m[1][i];
        cb.wvpRow2[i] = wvp.m[2][i];
        cb.wvpRow3[i] = wvp.m[3][i];
    }
    cb.viewport[0] = (float)g_viewport.X;
    cb.viewport[1] = (float)g_viewport.Y;
    cb.viewport[2] = (float)(g_viewport.Width  ? g_viewport.Width  : 640);
    cb.viewport[3] = (float)(g_viewport.Height ? g_viewport.Height : 480);

    UnpackD3DColor(xboxRS[XBOX_D3DRS_TEXTUREFACTOR], &cb.tfactor[0], &cb.tfactor[1], &cb.tfactor[2], &cb.tfactor[3]);

    // Stage 0..3 Combiner states (Xbox _D3D__TextureState offsets: 12=COLOROP, 13=ARG0, 14=ARG1, 15=ARG2, 16=ALPHAOP, 17=AARG0, 18=AARG1, 19=AARG2)
    uint32_t* stageOpsPtrs[4]   = { cb.stage0Ops,   cb.stage1Ops,   cb.stage2Ops,   cb.stage3Ops   };
    uint32_t* stageAlphaPtrs[4] = { cb.stage0Alpha, cb.stage1Alpha, cb.stage2Alpha, cb.stage3Alpha };
    for (int s = 0; s < 4; ++s) {
        stageOpsPtrs[s][0]   = xboxTSS[s * XBOX_D3DTSS_MAX + XBOX_D3DTSS_COLOROP];
        stageOpsPtrs[s][1]   = xboxTSS[s * XBOX_D3DTSS_MAX + XBOX_D3DTSS_COLORARG1];
        stageOpsPtrs[s][2]   = xboxTSS[s * XBOX_D3DTSS_MAX + XBOX_D3DTSS_COLORARG2];
        stageOpsPtrs[s][3]   = xboxTSS[s * XBOX_D3DTSS_MAX + XBOX_D3DTSS_COLORARG0];
        stageAlphaPtrs[s][0] = xboxTSS[s * XBOX_D3DTSS_MAX + XBOX_D3DTSS_ALPHAOP];
        stageAlphaPtrs[s][1] = xboxTSS[s * XBOX_D3DTSS_MAX + XBOX_D3DTSS_ALPHAARG1];
        stageAlphaPtrs[s][2] = xboxTSS[s * XBOX_D3DTSS_MAX + XBOX_D3DTSS_ALPHAARG2];
        stageAlphaPtrs[s][3] = xboxTSS[s * XBOX_D3DTSS_MAX + XBOX_D3DTSS_ALPHAARG0];
    }

    uint32_t texMask = 0;
    for (int s = 0; s < 4; ++s) {
        if (srvs[s]) texMask |= (1u << s);
    }

    cb.flags[0] = isRhw ? 1u : 0u;
    cb.flags[1] = texMask;
    cb.flags[2] = xboxRS[XBOX_D3DRS_ALPHATESTENABLE] ? 1u : 0u;
    cb.flags[3] = (g_currentPsHandle != 0 && g_currentPsType >= 0) ? (uint32_t)(g_currentPsType + 1) : 0u;

    cb.alphaTest[0] = (float)MapXboxCmpFuncInt(xboxRS[XBOX_D3DRS_ALPHAFUNC]);
    cb.alphaTest[1] = (float)(xboxRS[XBOX_D3DRS_ALPHAREF] & 0xFFu) * (1.0f / 255.0f);
    cb.alphaTest[2] = g_gamma;
    cb.alphaTest[3] = g_brightness;

    cb.uvScale01[0] = uScale[0]; cb.uvScale01[1] = vScale[0];
    cb.uvScale01[2] = uScale[1]; cb.uvScale01[3] = vScale[1];
    cb.uvScale23[0] = uScale[2]; cb.uvScale23[1] = vScale[2];
    cb.uvScale23[2] = uScale[3]; cb.uvScale23[3] = vScale[3];

    // BumpEnv matrix from Stage 1 (or Stage 0 if Stage 1's BumpEnvMat is zero)
    const uint32_t* bumpMat1 = &xboxTSS[1 * XBOX_D3DTSS_MAX + XBOX_D3DTSS_BUMPENVMAT00];
    const uint32_t* bumpMat0 = &xboxTSS[0 * XBOX_D3DTSS_MAX + XBOX_D3DTSS_BUMPENVMAT00];
    const uint32_t* chosenBump = (bumpMat1[0] || bumpMat1[1] || bumpMat1[2] || bumpMat1[3]) ? bumpMat1 : bumpMat0;
    memcpy(&cb.bumpParams[0], chosenBump, 16);
    if (cb.bumpParams[0] == 0.0f && cb.bumpParams[1] == 0.0f && cb.bumpParams[2] == 0.0f && cb.bumpParams[3] == 0.0f) {
        cb.bumpParams[0] = 0.04f;
        cb.bumpParams[3] = 0.04f;
    }

    // Hardware Distance & Atmospheric Fog (xboxRS[92]=FOGENABLE, [93]=FOGTABLEMODE, [94]=FOGSTART, [95]=FOGEND, [96]=FOGDENSITY, [138]=FOGCOLOR)
    if (xboxRS[XBOX_D3DRS_FOGENABLE] != 0 && g_curOffscreenRT == nullptr && !isRhw) {
        uint32_t fogMode = xboxRS[XBOX_D3DRS_FOGTABLEMODE];
        if (fogMode == 0) fogMode = 3; // Default to LINEAR if FOGENABLE is true
        cb.fogParams[0] = (float)fogMode;
        memcpy(&cb.fogParams[1], &xboxRS[XBOX_D3DRS_FOGSTART], 4);
        memcpy(&cb.fogParams[2], &xboxRS[XBOX_D3DRS_FOGEND], 4);
        memcpy(&cb.fogParams[3], &xboxRS[XBOX_D3DRS_FOGDENSITY], 4);
        float fA = 1.0f;
        UnpackD3DColor(xboxRS[XBOX_D3DRS_FOGCOLOR], &cb.fogColor[0], &cb.fogColor[1], &cb.fogColor[2], &fA);
        bool isAddBlend = (xboxRS[XBOX_D3DRS_ALPHABLENDENABLE] != 0) && (xboxRS[XBOX_D3DRS_DESTBLEND] == 1 || xboxRS[XBOX_D3DRS_DESTBLEND] == 0x302);
        cb.fogColor[3] = isAddBlend ? 1.0f : 0.0f;
    }

    memcpy(cb.psConst0, g_psConstants[0], 16);
    memcpy(cb.psConst1, g_psConstants[1], 16);
    memcpy(cb.psConst2, g_psConstants[2], 16);
    memcpy(cb.psConst3, g_psConstants[3], 16);

    D3D11_MAPPED_SUBRESOURCE mappedCB = {};
    if (SUCCEEDED(g_d3dCtx->Map(g_cbPerDraw, 0, D3D11_MAP_WRITE_DISCARD, 0, &mappedCB))) {
        memcpy(mappedCB.pData, &cb, sizeof(cb));
        g_d3dCtx->Unmap(g_cbPerDraw, 0);
    }

    // Bind pipeline states (COLORWRITEENABLE is 0x040358 -> index 67)
    uint32_t cwRaw = xboxRS[XBOX_D3DRS_COLORWRITEENABLE];
    uint32_t cwMask = 0x0Fu;
    if (cwRaw == 0) {
        cwMask = 0;
    } else if (cwRaw <= 0x0Fu) {
        cwMask = cwRaw;
    } else {
        cwMask = 0;
        if (cwRaw & 0x00FF0000u) cwMask |= D3D11_COLOR_WRITE_ENABLE_RED;
        if (cwRaw & 0x0000FF00u) cwMask |= D3D11_COLOR_WRITE_ENABLE_GREEN;
        if (cwRaw & 0x000000FFu) cwMask |= D3D11_COLOR_WRITE_ENABLE_BLUE;
        if (cwRaw & 0xFF000000u) cwMask |= D3D11_COLOR_WRITE_ENABLE_ALPHA;
    }

    // ALPHABLENDENABLE=[59], SRCBLEND=[62], DESTBLEND=[63], BLENDOP=[74]
    ID3D11BlendState* bs = GetBlendState(
        xboxRS[XBOX_D3DRS_ALPHABLENDENABLE], xboxRS[XBOX_D3DRS_SRCBLEND],
        xboxRS[XBOX_D3DRS_DESTBLEND], xboxRS[XBOX_D3DRS_BLENDOP], cwMask);
    // ZENABLE=[143], ZWRITEENABLE=[64], ZFUNC=[57], STENCILENABLE=[144], STENCILFAIL=[145],
    // STENCILZFAIL=[68], STENCILPASS=[69], STENCILFUNC=[70], STENCILREF=[71], STENCILMASK=[72], STENCILWRITEMASK=[73]
    ID3D11DepthStencilState* ds = GetDepthState(
        xboxRS[XBOX_D3DRS_ZENABLE], xboxRS[XBOX_D3DRS_ZWRITEENABLE], xboxRS[XBOX_D3DRS_ZFUNC],
        xboxRS[XBOX_D3DRS_STENCILENABLE], xboxRS[XBOX_D3DRS_STENCILFAIL],
        xboxRS[XBOX_D3DRS_STENCILZFAIL], xboxRS[XBOX_D3DRS_STENCILPASS],
        xboxRS[XBOX_D3DRS_STENCILFUNC], xboxRS[XBOX_D3DRS_STENCILMASK], xboxRS[XBOX_D3DRS_STENCILWRITEMASK]);
    // Disable backface culling on RHW 2D UI quads; use Xbox CULLMODE=[147] for 3D
    uint32_t cull = isRhw ? 0 : xboxRS[XBOX_D3DRS_CULLMODE];
    ID3D11RasterizerState* rs = GetRastState(
        cull, xboxRS[XBOX_D3DRS_FILLMODE], xboxRS[XBOX_D3DRS_ZBIAS], g_scissorEnabled);

    // Pre-transformed XYZRHW vertices bypass viewport MinZ..MaxZ on Xbox NV2A
    D3D11_VIEWPORT vp11 = {};
    vp11.TopLeftX = (float)g_viewport.X;
    vp11.TopLeftY = (float)g_viewport.Y;
    vp11.Width    = (float)(g_viewport.Width  ? g_viewport.Width  : 640);
    vp11.Height   = (float)(g_viewport.Height ? g_viewport.Height : 480);
    vp11.MinDepth = isRhw ? 0.0f : g_viewport.MinZ;
    vp11.MaxDepth = isRhw ? 1.0f : ((g_viewport.MaxZ > g_viewport.MinZ) ? g_viewport.MaxZ : 1.0f);
    g_d3dCtx->RSSetViewports(1, &vp11);

    ID3D11SamplerState* smps[4] = {
        GetSamplerState(xboxTSS[0 * XBOX_D3DTSS_MAX + XBOX_D3DTSS_ADDRESSU], xboxTSS[0 * XBOX_D3DTSS_MAX + XBOX_D3DTSS_ADDRESSV], xboxTSS[0 * XBOX_D3DTSS_MAX + XBOX_D3DTSS_MAGFILTER], xboxTSS[0 * XBOX_D3DTSS_MAX + XBOX_D3DTSS_BORDERCOLOR]),
        GetSamplerState(xboxTSS[1 * XBOX_D3DTSS_MAX + XBOX_D3DTSS_ADDRESSU], xboxTSS[1 * XBOX_D3DTSS_MAX + XBOX_D3DTSS_ADDRESSV], xboxTSS[1 * XBOX_D3DTSS_MAX + XBOX_D3DTSS_MAGFILTER], xboxTSS[1 * XBOX_D3DTSS_MAX + XBOX_D3DTSS_BORDERCOLOR]),
        GetSamplerState(xboxTSS[2 * XBOX_D3DTSS_MAX + XBOX_D3DTSS_ADDRESSU], xboxTSS[2 * XBOX_D3DTSS_MAX + XBOX_D3DTSS_ADDRESSV], xboxTSS[2 * XBOX_D3DTSS_MAX + XBOX_D3DTSS_MAGFILTER], xboxTSS[2 * XBOX_D3DTSS_MAX + XBOX_D3DTSS_BORDERCOLOR]),
        GetSamplerState(xboxTSS[3 * XBOX_D3DTSS_MAX + XBOX_D3DTSS_ADDRESSU], xboxTSS[3 * XBOX_D3DTSS_MAX + XBOX_D3DTSS_ADDRESSV], xboxTSS[3 * XBOX_D3DTSS_MAX + XBOX_D3DTSS_MAGFILTER], xboxTSS[3 * XBOX_D3DTSS_MAX + XBOX_D3DTSS_BORDERCOLOR])
    };

    float blendFactor[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
    g_d3dCtx->OMSetBlendState(bs, blendFactor, 0xFFFFFFFFu);
    g_d3dCtx->OMSetDepthStencilState(ds, xboxRS[XBOX_D3DRS_STENCILREF] & 0xFFu);
    g_d3dCtx->RSSetState(rs);

    g_d3dCtx->IASetInputLayout(g_inputLayout);
    UINT vbStride = sizeof(UnifiedVertex);
    UINT vbOffset = 0;
    g_d3dCtx->IASetVertexBuffers(0, 1, &g_dynVB, &vbStride, &vbOffset);
    g_d3dCtx->IASetPrimitiveTopology(topo);

    g_d3dCtx->VSSetShader(g_vs, nullptr, 0);
    g_d3dCtx->VSSetConstantBuffers(0, 1, &g_cbPerDraw);
    g_d3dCtx->PSSetShader(g_ps, nullptr, 0);
    g_d3dCtx->PSSetConstantBuffers(0, 1, &g_cbPerDraw);

    g_d3dCtx->PSSetShaderResources(0, 4, srvs);
    g_d3dCtx->PSSetSamplers(0, 4, smps);

    g_d3dCtx->Draw(drawCount, startVertex);
    if (g_curOffscreenRT == nullptr) {
        g_backBufferDirty = true;
    }
}

static void __stdcall Bridge_D3DDevice_DrawVerticesUP(
    DWORD PrimitiveType, UINT VertexCount, const void* pVertexStreamZeroData, UINT VertexStreamZeroStride)
{
    g_curPrelitColors = nullptr;
    g_activeCustomVS  = 0;
    if (!pVertexStreamZeroData || VertexCount == 0 || VertexStreamZeroStride == 0) return;

    static std::vector<UnifiedVertex> verts;
    verts.clear();
    bool isRhw = false;
    DecodeVerticesToUnified(
        reinterpret_cast<const uint8_t*>(pVertexStreamZeroData),
        VertexStreamZeroStride, VertexCount, nullptr, 0, verts, &isRhw);
    SubmitUnifiedVerticesD3D11(PrimitiveType, verts, isRhw);
}

static void __stdcall Bridge_D3DDevice_DrawVertices(
    DWORD PrimitiveType, UINT StartVertex, UINT VertexCount)
{
    if (!g_streams[0].data || g_streams[0].stride == 0) return;
    const uint8_t* basePtr = reinterpret_cast<const uint8_t*>(g_streams[0].data) + StartVertex * g_streams[0].stride;
    Bridge_D3DDevice_DrawVerticesUP(PrimitiveType, VertexCount, basePtr, g_streams[0].stride);
}

static void __stdcall Bridge_D3DDevice_DrawIndexedVertices(
    DWORD PrimitiveType, UINT VertexCount, const uint16_t* pIndexData)
{
    if (!g_streams[0].data || g_streams[0].stride == 0 || !pIndexData || VertexCount == 0) return;

    static std::vector<UnifiedVertex> verts;
    verts.clear();
    bool isRhw = false;
    DecodeVerticesToUnified(
        reinterpret_cast<const uint8_t*>(g_streams[0].data),
        g_streams[0].stride, VertexCount, pIndexData, g_baseVertexIndex, verts, &isRhw);
    SubmitUnifiedVerticesD3D11(PrimitiveType, verts, isRhw);
}

// _D3DDevice_RunPushBuffer@8 (0x006BACD0) - executes terrain & world pushbuffers
static void __stdcall Bridge_D3DDevice_RunPushBuffer(XboxPushBuffer* pPushBuffer, void* pFixup) {
    (void)pFixup;
    g_curPrelitColors = nullptr;
    g_activeCustomVS  = 0;
    if (!pPushBuffer || !pPushBuffer->Data || pPushBuffer->Size < 8) return;

    const uint32_t* p = reinterpret_cast<const uint32_t*>(pPushBuffer->Data);
    const uint32_t* pEnd = p + (pPushBuffer->Size / 4);

    uint32_t curPrim = 0;
    static std::vector<uint16_t> curIndices;
    curIndices.clear();

    while (p < pEnd) {
        uint32_t hdr = *p;
        if (hdr == 0) break;

        if (hdr == 0x000417FCu && (p + 1) < pEnd) {
            uint32_t nextPrim = p[1];
            p += 2;
            if (nextPrim == 0) {
                if (curPrim != 0 && !curIndices.empty()) {
                    Bridge_D3DDevice_DrawIndexedVertices(curPrim, (UINT)curIndices.size(), curIndices.data());
                }
                curIndices.clear();
                curPrim = 0;
            } else {
                curPrim = nextPrim;
                curIndices.clear();
            }
        } else if ((hdr & 0x4003FFFFu) == 0x40001800u) {
            uint32_t dwordCount = (hdr >> 18) & 0x7FFu;
            if (p + 1 + dwordCount > pEnd) break;
            const uint16_t* idxPtr = reinterpret_cast<const uint16_t*>(p + 1);
            curIndices.insert(curIndices.end(), idxPtr, idxPtr + dwordCount * 2);
            p += 1 + dwordCount;
        } else if (hdr == 0x00041808u && (p + 1) < pEnd) {
            curIndices.push_back((uint16_t)(p[1] & 0xFFFFu));
            p += 2;
        } else {
            break;
        }
    }
}

// ============================================================================
// Immediate Mode: D3DDevice_Begin / SetVertexData* / D3DDevice_End
// ============================================================================

static constexpr int      kXboxVsdeAutoCommit = -1;
static constexpr int      kXboxVsdePosition   = 0;  // D3DVSDE_POSITION
static constexpr int      kXboxVsdeNormal     = 2;  // D3DVSDE_NORMAL
static constexpr int      kXboxVsdeDiffuse    = 3;  // D3DVSDE_DIFFUSE
static constexpr int      kXboxVsdeTexCoord0  = 9;  // D3DVSDE_TEXCOORD0
static constexpr int      kXboxVsdeTexCoord1  = 10; // D3DVSDE_TEXCOORD1
static constexpr int      kXboxVsdeTexCoord2  = 11; // D3DVSDE_TEXCOORD2
static constexpr int      kXboxVsdeTexCoord3  = 12; // D3DVSDE_TEXCOORD3
static constexpr uint32_t kXboxTexStageCount  = 4u;
static constexpr uint32_t kXboxTssStride      = XBOX_D3DTSS_MAX;
static constexpr uint32_t kXboxTssTtfOffset          = XBOX_D3DTSS_TEXTURETRANSFORMFLAGS;
static constexpr uint32_t kXboxTssTciOffset          = XBOX_D3DTSS_TEXCOORDINDEX;
static constexpr uint32_t kXboxTtffProjected         = 0x100u;
static constexpr uint32_t kXboxDeviceTexMatrixOffset = 0xCE0u; // CDevice+0xC60 + 2*64 (D3DTS_TEXTURE0..3)

static uint32_t                   g_immPrimType = 0;
static UnifiedVertex              g_immCur      = { 0, 0, 0, 1.0f, 0, 0, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 0, 0, 0, 0, 0, 0, 0, 0 };
static bool                       g_immHasUv1   = false;
static bool                       g_immHasUv2   = false;
static bool                       g_immHasUv3   = false;
static std::vector<UnifiedVertex> g_immVerts;

static void __stdcall Bridge_D3DDevice_Begin(DWORD PrimitiveType) {
    g_immPrimType = PrimitiveType;
    g_immHasUv1   = false;
    g_immHasUv2   = false;
    g_immHasUv3   = false;
    g_immVerts.clear();
}

static void __stdcall Bridge_D3DDevice_SetVertexData2f(int Register, float x, float y) {
    if (Register == kXboxVsdePosition || Register == kXboxVsdeAutoCommit) {
        g_immCur.x = x;
        g_immCur.y = y;
        g_immCur.z = 0.0f;
        g_immCur.w = 1.0f;
        g_immVerts.push_back(g_immCur);
    } else if (Register == kXboxVsdeTexCoord0) {
        g_immCur.u0 = x;
        g_immCur.v0 = y;
        if (!g_immHasUv1) { g_immCur.u1 = x; g_immCur.v1 = y; }
        if (!g_immHasUv2) { g_immCur.u2 = x; g_immCur.v2 = y; }
        if (!g_immHasUv3) { g_immCur.u3 = x; g_immCur.v3 = y; }
    } else if (Register == kXboxVsdeTexCoord1) {
        g_immCur.u1 = x;
        g_immCur.v1 = y;
        g_immHasUv1 = true;
    } else if (Register == kXboxVsdeTexCoord2) {
        g_immCur.u2 = x;
        g_immCur.v2 = y;
        g_immHasUv2 = true;
    } else if (Register == kXboxVsdeTexCoord3) {
        g_immCur.u3 = x;
        g_immCur.v3 = y;
        g_immHasUv3 = true;
    }
}

static void __stdcall Bridge_D3DDevice_SetVertexData4f(int Register, float x, float y, float z, float w) {
    if (Register == kXboxVsdePosition || Register == kXboxVsdeAutoCommit) {
        g_immCur.x = x;
        g_immCur.y = y;
        g_immCur.z = z;
        g_immCur.w = (w != 0.0f) ? w : 1.0f;
        g_immVerts.push_back(g_immCur);
    } else if (Register == kXboxVsdeNormal) {
        g_immCur.nx = x;
        g_immCur.ny = y;
        g_immCur.nz = z;
    } else if (Register == kXboxVsdeTexCoord0) {
        g_immCur.u0 = x;
        g_immCur.v0 = y;
        if (!g_immHasUv1) { g_immCur.u1 = x; g_immCur.v1 = y; }
        if (!g_immHasUv2) { g_immCur.u2 = x; g_immCur.v2 = y; }
        if (!g_immHasUv3) { g_immCur.u3 = x; g_immCur.v3 = y; }
    } else if (Register == kXboxVsdeTexCoord1) {
        g_immCur.u1 = x;
        g_immCur.v1 = y;
        g_immHasUv1 = true;
    } else if (Register == kXboxVsdeTexCoord2) {
        g_immCur.u2 = x;
        g_immCur.v2 = y;
        g_immHasUv2 = true;
    } else if (Register == kXboxVsdeTexCoord3) {
        g_immCur.u3 = x;
        g_immCur.v3 = y;
        g_immHasUv3 = true;
    }
}

static void __stdcall Bridge_D3DDevice_SetVertexData4ub(int Register, uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    if (Register == kXboxVsdeDiffuse || Register == kXboxVsdePosition) {
        g_immCur.r = r * (1.0f / 255.0f);
        g_immCur.g = g * (1.0f / 255.0f);
        g_immCur.b = b * (1.0f / 255.0f);
        g_immCur.a = a * (1.0f / 255.0f);
    }
}

static void __stdcall Bridge_D3DDevice_SetVertexDataColor(int Register, DWORD Color) {
    if (Register == kXboxVsdeDiffuse || Register == kXboxVsdePosition) {
        UnpackD3DColor(Color, &g_immCur.r, &g_immCur.g, &g_immCur.b, &g_immCur.a);
    }
}

static void __stdcall Bridge_D3DDevice_End() {
    if (g_immVerts.empty()) return;
    bool isRhw = ((g_currentVsHandle & 0x00010001u) == 0) && ((g_currentVsHandle & 0x004u) != 0);

    // Route Stage 0..3 UVs according to D3DTSS_TEXCOORDINDEX and D3DTSS_TEXTURETRANSFORMFLAGS
    const uint32_t* xboxTSS = reinterpret_cast<const uint32_t*>(ADDR_D3D_TEXTURESTATE);
    const Mat44*    texMats = reinterpret_cast<const Mat44*>(ADDR_D3D_DEVICE + kXboxDeviceTexMatrixOffset);
    for (size_t vi = 0; vi < g_immVerts.size(); ++vi) {
        UnifiedVertex& v = g_immVerts[vi];
        float rawU[kXboxTexStageCount] = { v.u0, v.u1, v.u2, v.u3 };
        float rawV[kXboxTexStageCount] = { v.v0, v.v1, v.v2, v.v3 };
        float finalU[kXboxTexStageCount];
        float finalV[kXboxTexStageCount];

        for (uint32_t s = 0; s < kXboxTexStageCount; ++s) {
            uint32_t tci    = xboxTSS[s * kXboxTssStride + kXboxTssTciOffset];
            uint32_t ttf    = xboxTSS[s * kXboxTssStride + kXboxTssTtfOffset];
            uint32_t srcIdx = tci & 0x03u;
            float tu = rawU[srcIdx];
            float tv = rawV[srcIdx];

            uint32_t dimCount = ttf & 0xFFu;
            if (!isRhw && dimCount > 0 && dimCount <= 4) {
                const Mat44& tm = texMats[s];
                float outU = tu * tm.m[0][0] + tv * tm.m[1][0] + tm.m[2][0] + tm.m[3][0];
                float outV = tu * tm.m[0][1] + tv * tm.m[1][1] + tm.m[2][1] + tm.m[3][1];
                float outZ = tu * tm.m[0][2] + tv * tm.m[1][2] + tm.m[2][2] + tm.m[3][2];
                float outW = tu * tm.m[0][3] + tv * tm.m[1][3] + tm.m[2][3] + tm.m[3][3];
                if (ttf & kXboxTtffProjected) {
                    float denom = (dimCount == 3) ? outZ : outW;
                    if (fabsf(denom) > 0.0001f) {
                        outU /= denom;
                        outV /= denom;
                    }
                }
                tu = outU;
                tv = outV;
            }
            finalU[s] = tu;
            finalV[s] = tv;
        }
        v.u0 = finalU[0]; v.v0 = finalV[0];
        v.u1 = finalU[1]; v.v1 = finalV[1];
        v.u2 = finalU[2]; v.v2 = finalV[2];
        v.u3 = finalU[3]; v.v3 = finalV[3];
    }

    SubmitUnifiedVerticesD3D11(g_immPrimType, g_immVerts, isRhw);
    g_immVerts.clear();
}

// ============================================================================
// Hardware Occlusion Queries (BeginVisibilityTest / EndVisibilityTest / GetVisibilityTestResult)
// Used by cEffectGlow::RenderVisCheck (0x0055FD10) & cEffectLensFlare::RenderVisCheck (0x00566F30)
// ============================================================================

static constexpr uint32_t kMaxXboxVisibilityQueries = 1024u;
static constexpr uint32_t kDefaultVisiblePixels     = 1u;

struct XboxVisibilityQuerySlot {
    ID3D11Query* query;
    bool         pending;
    UINT         lastPixelsPassed;
};

static XboxVisibilityQuerySlot g_visQuerySlots[kMaxXboxVisibilityQueries] = {};
static ID3D11Query*            g_activeVisQuery = nullptr;

static HRESULT __stdcall Bridge_D3DDevice_BeginVisibilityTest() {
    if (!g_d3dDevice || !g_d3dCtx) return S_OK;
    if (g_activeVisQuery) {
        g_d3dCtx->End(g_activeVisQuery);
        g_activeVisQuery->Release();
        g_activeVisQuery = nullptr;
    }
    D3D11_QUERY_DESC qd = {};
    qd.Query = D3D11_QUERY_OCCLUSION;
    qd.MiscFlags = 0;
    if (SUCCEEDED(g_d3dDevice->CreateQuery(&qd, &g_activeVisQuery)) && g_activeVisQuery) {
        g_d3dCtx->Begin(g_activeVisQuery);
    }
    return S_OK;
}

static HRESULT __stdcall Bridge_D3DDevice_EndVisibilityTest(DWORD Index) {
    if (!g_d3dCtx || !g_activeVisQuery) return S_OK;
    g_d3dCtx->End(g_activeVisQuery);

    uint32_t slotIdx = Index % kMaxXboxVisibilityQueries;
    XboxVisibilityQuerySlot& slot = g_visQuerySlots[slotIdx];
    if (slot.query) {
        uint64_t passed = 0;
        if (g_d3dCtx->GetData(slot.query, &passed, sizeof(passed), D3D11_ASYNC_GETDATA_DONOTFLUSH) == S_OK) {
            slot.lastPixelsPassed = (UINT)passed;
        }
        slot.query->Release();
    }
    slot.query   = g_activeVisQuery;
    slot.pending = true;
    g_activeVisQuery = nullptr;
    return S_OK;
}

static HRESULT __stdcall Bridge_D3DDevice_GetVisibilityTestResult(
    DWORD Index, UINT* pResult, uint64_t* pTime)
{
    uint32_t slotIdx = Index % kMaxXboxVisibilityQueries;
    XboxVisibilityQuerySlot& slot = g_visQuerySlots[slotIdx];

    if (slot.query && slot.pending && g_d3dCtx) {
        uint64_t passed = 0;
        HRESULT hr = g_d3dCtx->GetData(slot.query, &passed, sizeof(passed), D3D11_ASYNC_GETDATA_DONOTFLUSH);
        if (hr == S_OK) {
            slot.lastPixelsPassed = (UINT)passed;
            slot.pending = false;
        }
    }

    if (pResult) {
        *pResult = slot.query ? slot.lastPixelsPassed : kDefaultVisiblePixels;
    }
    if (pTime) {
        *pTime = 1ULL;
    }
    return S_OK;
}

// ============================================================================
// Clear, Swap (Present), Overlay (YUY2 Video Cutscenes), LockRect Hooks
// ============================================================================

static void __stdcall Bridge_D3DDevice_Clear(
    DWORD Count, const void* pRects, DWORD Flags, DWORD Color, float Z, DWORD Stencil)
{
    (void)Count; (void)pRects;
    if (!g_d3dCtx) return;

    uint32_t clearFlags = Flags;
    if (clearFlags == 0) clearFlags = 0xF1u;

    ID3D11RenderTargetView* rtv = g_curActiveRTV ? g_curActiveRTV : g_rtv;
    ID3D11DepthStencilView* dsv = g_curActiveDSV ? g_curActiveDSV : g_dsv;

    if ((clearFlags & 0x000000F0u) && rtv) {
        float rgba[4];
        UnpackD3DColor(Color, &rgba[0], &rgba[1], &rgba[2], &rgba[3]);
        g_d3dCtx->ClearRenderTargetView(rtv, rgba);
        if (rtv == g_rtv) g_backBufferDirty = true;
    }
    if ((clearFlags & 0x00000003u) && dsv) {
        UINT dsFlags = 0;
        if (clearFlags & 0x01u) dsFlags |= D3D11_CLEAR_DEPTH;
        if (clearFlags & 0x02u) dsFlags |= D3D11_CLEAR_STENCIL;
        g_d3dCtx->ClearDepthStencilView(dsv, dsFlags, Z, (UINT8)(Stencil & 0xFF));
    }
}

// YUY2 Hardware Overlay Emulation for .vid Movies
static ID3D11Texture2D*          g_overlayTex = nullptr;
static ID3D11ShaderResourceView* g_overlaySRV = nullptr;
static uint32_t g_overlayW = 0;
static uint32_t g_overlayH = 0;
static bool     g_overlayEnabled = false;

static void __stdcall Bridge_D3DDevice_EnableOverlay(BOOL bEnable) {
    g_overlayEnabled = (bEnable != FALSE);
}

static BOOL __stdcall Bridge_D3DDevice_GetOverlayUpdateStatus() {
    return TRUE;
}

static std::vector<uint32_t> g_overlayPixels;
volatile uint32_t g_swapFrameCount    = 0;

// ============================================================================
// Top-Right In-Game OSD Overlay (GPU Name & FPS)
// ============================================================================

static ID3D11Texture2D*          g_osdTex  = nullptr;
static ID3D11ShaderResourceView* g_osdSRV  = nullptr;
static ID3D11Buffer*             g_osdVB   = nullptr;
static uint32_t                  g_osdTexW = 0;
static uint32_t                  g_osdTexH = 0;
static wchar_t                   g_lastOsdText[256] = { 0 };
static HFONT                     g_hOsdFont = nullptr;

static void UpdateOsdTexture(const wchar_t* text) {
    if (!g_d3dDevice || !g_d3dCtx || !text || !text[0]) return;
    if (!g_hOsdFont) {
        g_hOsdFont = CreateFontW(
            -12, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
            OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY,
            DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
    }

    HDC hdcScreen = GetDC(nullptr);
    HDC hdc = CreateCompatibleDC(hdcScreen);
    ReleaseDC(nullptr, hdcScreen);
    if (!hdc) return;

    HGDIOBJ oldFont = SelectObject(hdc, g_hOsdFont);
    SIZE sz = {};
    GetTextExtentPoint32W(hdc, text, (int)wcslen(text), &sz);

    uint32_t w = (uint32_t)(sz.cx + 14);
    uint32_t h = (uint32_t)(sz.cy + 6);
    if (w < 16)  w = 16;
    if (w > 620) w = 620;
    if (h < 14)  h = 14;
    if (h > 64)  h = 64;

    BITMAPINFO bmi = {};
    bmi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth       = (LONG)w;
    bmi.bmiHeader.biHeight      = -(LONG)h; // top-down DIB
    bmi.bmiHeader.biPlanes      = 1;
    bmi.bmiHeader.biBitCount    = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    void* dibBits = nullptr;
    HBITMAP hbm = CreateDIBSection(hdc, &bmi, DIB_RGB_COLORS, &dibBits, nullptr, 0);
    if (hbm && dibBits) {
        HGDIOBJ oldBm = SelectObject(hdc, hbm);
        SetBkMode(hdc, TRANSPARENT);
        SetTextColor(hdc, RGB(255, 255, 255));

        uint32_t numPx = w * h;
        std::vector<uint8_t> shadowMask(numPx, 0);
        std::vector<uint8_t> glyphMask(numPx, 0);

        // 1. Render 1px drop-shadow mask at (+6, +4)
        memset(dibBits, 0, numPx * 4);
        RECT rcShadow = { 8, 4, (LONG)w, (LONG)h };
        DrawTextW(hdc, text, -1, &rcShadow, DT_LEFT | DT_TOP | DT_SINGLELINE | DT_NOPREFIX);
        GdiFlush();
        const uint32_t* px = reinterpret_cast<const uint32_t*>(dibBits);
        for (uint32_t i = 0; i < numPx; ++i) {
            uint8_t r = (uint8_t)((px[i] >> 16) & 0xFF);
            uint8_t g = (uint8_t)((px[i] >> 8)  & 0xFF);
            uint8_t b = (uint8_t)(px[i] & 0xFF);
            uint8_t m = r > g ? r : g;
            shadowMask[i] = m > b ? m : b;
        }

        // 2. Render crisp foreground glyph mask at (+7, +3)
        memset(dibBits, 0, numPx * 4);
        RECT rcGlyph = { 7, 3, (LONG)w - 1, (LONG)h - 1 };
        DrawTextW(hdc, text, -1, &rcGlyph, DT_LEFT | DT_TOP | DT_SINGLELINE | DT_NOPREFIX);
        GdiFlush();
        for (uint32_t i = 0; i < numPx; ++i) {
            uint8_t r = (uint8_t)((px[i] >> 16) & 0xFF);
            uint8_t g = (uint8_t)((px[i] >> 8)  & 0xFF);
            uint8_t b = (uint8_t)(px[i] & 0xFF);
            uint8_t m = r > g ? r : g;
            glyphMask[i] = m > b ? m : b;
        }

        // 3. Composite semi-transparent rounded dark pill + shadow + bright glyph into BGRA8
        std::vector<uint32_t> outBgra(numPx, 0);
        for (uint32_t y = 0; y < h; ++y) {
            for (uint32_t x = 0; x < w; ++x) {
                // 3px corner bevel clip for rounded badge
                uint32_t dx = (x < 3) ? (2 - x) : ((x >= w - 3) ? (x - (w - 3)) : 0);
                uint32_t dy = (y < 3) ? (2 - y) : ((y >= h - 3) ? (y - (h - 3)) : 0);
                bool insidePill = (dx + dy <= 2);

                uint32_t idx = y * w + x;
                float bgA = insidePill ? (135.0f / 255.0f) : 0.0f;
                float shA = shadowMask[idx] * (1.0f / 255.0f);
                float fgA = glyphMask[idx]  * (1.0f / 255.0f);

                // Base dark translucent pill color: RGB(12, 16, 22)
                float r = 12.0f * (1.0f - shA);
                float g = 16.0f * (1.0f - shA);
                float b = 22.0f * (1.0f - shA);
                float a = bgA + shA * (1.0f - bgA);

                // Blend foreground glyph: RGB(240, 246, 255)
                r = r * (1.0f - fgA) + 240.0f * fgA;
                g = g * (1.0f - fgA) + 246.0f * fgA;
                b = b * (1.0f - fgA) + 255.0f * fgA;
                a = a + fgA * (1.0f - a);

                uint32_t outR = (uint32_t)(r + 0.5f); if (outR > 255) outR = 255;
                uint32_t outG = (uint32_t)(g + 0.5f); if (outG > 255) outG = 255;
                uint32_t outB = (uint32_t)(b + 0.5f); if (outB > 255) outB = 255;
                uint32_t outA = (uint32_t)(a * 255.0f + 0.5f); if (outA > 255) outA = 255;

                outBgra[idx] = (outA << 24) | (outR << 16) | (outG << 8) | outB;
            }
        }

        if (!g_osdTex || g_osdTexW != w || g_osdTexH != h) {
            if (g_osdSRV) { g_osdSRV->Release(); g_osdSRV = nullptr; }
            if (g_osdTex) { g_osdTex->Release(); g_osdTex = nullptr; }
            g_osdTexW = w;
            g_osdTexH = h;

            D3D11_TEXTURE2D_DESC td = {};
            td.Width            = w;
            td.Height           = h;
            td.MipLevels        = 1;
            td.ArraySize        = 1;
            td.Format           = DXGI_FORMAT_B8G8R8A8_UNORM;
            td.SampleDesc.Count = 1;
            td.Usage            = D3D11_USAGE_DEFAULT;
            td.BindFlags        = D3D11_BIND_SHADER_RESOURCE;

            D3D11_SUBRESOURCE_DATA initData = {};
            initData.pSysMem     = outBgra.data();
            initData.SysMemPitch = w * 4;
            if (SUCCEEDED(g_d3dDevice->CreateTexture2D(&td, &initData, &g_osdTex))) {
                g_d3dDevice->CreateShaderResourceView(g_osdTex, nullptr, &g_osdSRV);
            }
        } else {
            g_d3dCtx->UpdateSubresource(g_osdTex, 0, nullptr, outBgra.data(), w * 4, 0);
        }

        SelectObject(hdc, oldBm);
        DeleteObject(hbm);
    }

    SelectObject(hdc, oldFont);
    DeleteDC(hdc);
}

static void DrawOsdOverlayOnBackbuffer() {
    if (!g_d3dDevice || !g_d3dCtx || !g_rtv || !g_vs || !g_ps || !g_cbPerDraw) return;
    if (!g_osdShowFps) return;

    wchar_t curText[64];
    swprintf(curText, 64, L"%.1f FPS", g_osdCurrentFps);

    if (wcscmp(curText, g_lastOsdText) != 0 || !g_osdSRV) {
        wcscpy(g_lastOsdText, curText);
        UpdateOsdTexture(curText);
    }
    if (!g_osdSRV || g_osdTexW == 0 || g_osdTexH == 0) return;

    if (!g_osdVB) {
        D3D11_BUFFER_DESC bd = {};
        bd.ByteWidth      = 4 * sizeof(UnifiedVertex);
        bd.Usage          = D3D11_USAGE_DYNAMIC;
        bd.BindFlags      = D3D11_BIND_VERTEX_BUFFER;
        bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        if (FAILED(g_d3dDevice->CreateBuffer(&bd, nullptr, &g_osdVB))) return;
    }

    float x0 = (float)(640 - (int)g_osdTexW - 8);
    float y0 = 8.0f;
    float x1 = x0 + (float)g_osdTexW;
    float y1 = y0 + (float)g_osdTexH;

    UnifiedVertex quad[4] = {};
    quad[0] = { x0, y0, 0.0f, 1.0f, 0,0,1, 1,1,1,1, 0.0f, 0.0f, 0,0, 0,0, 0,0 };
    quad[1] = { x1, y0, 0.0f, 1.0f, 0,0,1, 1,1,1,1, 1.0f, 0.0f, 0,0, 0,0, 0,0 };
    quad[2] = { x0, y1, 0.0f, 1.0f, 0,0,1, 1,1,1,1, 0.0f, 1.0f, 0,0, 0,0, 0,0 };
    quad[3] = { x1, y1, 0.0f, 1.0f, 0,0,1, 1,1,1,1, 1.0f, 1.0f, 0,0, 0,0, 0,0 };

    D3D11_MAPPED_SUBRESOURCE mappedVB = {};
    if (SUCCEEDED(g_d3dCtx->Map(g_osdVB, 0, D3D11_MAP_WRITE_DISCARD, 0, &mappedVB))) {
        memcpy(mappedVB.pData, quad, sizeof(quad));
        g_d3dCtx->Unmap(g_osdVB, 0);
    }

    PerDrawCBData cb = {};
    cb.viewport[0]    = 0.0f;
    cb.viewport[1]    = 0.0f;
    cb.viewport[2]    = 640.0f;
    cb.viewport[3]    = 480.0f;
    cb.stage0Ops[0]   = 2u; // D3DTOP_SELECTARG1
    cb.stage0Ops[1]   = 2u; // D3DTA_TEXTURE
    cb.stage0Alpha[0] = 2u; // D3DTOP_SELECTARG1
    cb.stage0Alpha[1] = 2u; // D3DTA_TEXTURE
    cb.flags[0]       = 1u; // isRhw = 1
    cb.flags[1]       = 1u; // texMask = Stage 0
    cb.alphaTest[2]   = 1.0f; // gamma = 1.0
    cb.alphaTest[3]   = 1.0f; // brightness = 1.0
    cb.uvScale01[0]   = 1.0f;
    cb.uvScale01[1]   = 1.0f;

    D3D11_MAPPED_SUBRESOURCE mappedCB = {};
    if (SUCCEEDED(g_d3dCtx->Map(g_cbPerDraw, 0, D3D11_MAP_WRITE_DISCARD, 0, &mappedCB))) {
        memcpy(mappedCB.pData, &cb, sizeof(cb));
        g_d3dCtx->Unmap(g_cbPerDraw, 0);
    }

    g_d3dCtx->OMSetRenderTargets(1, &g_rtv, nullptr);
    D3D11_VIEWPORT vp11 = { 0.0f, 0.0f, 640.0f, 480.0f, 0.0f, 1.0f };
    g_d3dCtx->RSSetViewports(1, &vp11);

    ID3D11BlendState*        bs  = GetBlendState(1u, 5u /* SRCALPHA */, 6u /* INVSRCALPHA */, 1u /* ADD */, 0x0Fu);
    ID3D11DepthStencilState* ds  = GetDepthState(0u, 0u, 8u, 0u, 1u, 1u, 1u, 8u, 0xFFu, 0xFFu);
    ID3D11RasterizerState*   rs  = GetRastState(0u, 3u, 0u, false);
    ID3D11SamplerState*      smp = GetSamplerState(kXboxTexAddressClamp, kXboxTexAddressClamp, kXboxTexFilterPoint, 0u);

    float blendFactor[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
    g_d3dCtx->OMSetBlendState(bs, blendFactor, 0xFFFFFFFFu);
    g_d3dCtx->OMSetDepthStencilState(ds, 0);
    g_d3dCtx->RSSetState(rs);

    g_d3dCtx->IASetInputLayout(g_inputLayout);
    UINT stride = sizeof(UnifiedVertex);
    UINT offset = 0;
    g_d3dCtx->IASetVertexBuffers(0, 1, &g_osdVB, &stride, &offset);
    g_d3dCtx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);

    g_d3dCtx->VSSetShader(g_vs, nullptr, 0);
    g_d3dCtx->VSSetConstantBuffers(0, 1, &g_cbPerDraw);
    g_d3dCtx->PSSetShader(g_ps, nullptr, 0);
    g_d3dCtx->PSSetConstantBuffers(0, 1, &g_cbPerDraw);

    ID3D11ShaderResourceView* srvs[4] = { g_osdSRV, nullptr, nullptr, nullptr };
    ID3D11SamplerState*       smps[4] = { smp, smp, smp, smp };
    g_d3dCtx->PSSetShaderResources(0, 4, srvs);
    g_d3dCtx->PSSetSamplers(0, 4, smps);

    g_d3dCtx->Draw(4, 0);

    ID3D11ShaderResourceView* nullSrv = nullptr;
    g_d3dCtx->PSSetShaderResources(0, 1, &nullSrv);
}

static void __stdcall Bridge_D3DDevice_UpdateOverlay(
    XboxPixelContainer* pSurface, const RECT* pSrcRect, const RECT* pDstRect,
    BOOL bColorKey, DWORD ColorKey)
{
    (void)pDstRect; (void)bColorKey; (void)ColorKey;
    if (!pSurface || !pSurface->Data) return;

    uint32_t w = 0, h = 0, levels = 1, xboxFmt = 0, pitch = 0;
    bool isSwizzled = false;
    DecodeXboxTextureFormat(pSurface, &w, &h, &levels, &xboxFmt, &isSwizzled, &pitch);
    if (pSrcRect && pSrcRect->right > pSrcRect->left && pSrcRect->bottom > pSrcRect->top) {
        w = (uint32_t)(pSrcRect->right - pSrcRect->left);
        h = (uint32_t)(pSrcRect->bottom - pSrcRect->top);
    }
    if (w == 0 || h == 0 || w > 2048 || h > 2048) return;
    if (pitch == 0) pitch = w * 2;

    g_overlayPixels.resize(w * h);
    const uint8_t* srcBase = reinterpret_cast<const uint8_t*>(pSurface->Data);
    for (uint32_t y = 0; y < h; ++y) {
        const uint8_t* sRow = srcBase + y * pitch;
        uint32_t* dRow = &g_overlayPixels[y * w];
        for (uint32_t x = 0; x < w; x += 2) {
            int y0, u, y1, v;
            if (xboxFmt == 0x25) {
                u  = (int)sRow[x * 2 + 0] - 128;
                y0 = (int)sRow[x * 2 + 1];
                v  = (int)sRow[x * 2 + 2] - 128;
                y1 = (int)sRow[x * 2 + 3];
            } else {
                y0 = (int)sRow[x * 2 + 0];
                u  = (int)sRow[x * 2 + 1] - 128;
                y1 = (int)sRow[x * 2 + 2];
                v  = (int)sRow[x * 2 + 3] - 128;
            }

            int r0 = y0 + ((359 * v) >> 8);
            int g0 = y0 - ((88 * u + 183 * v) >> 8);
            int b0 = y0 + ((454 * u) >> 8);
            r0 = r0 < 0 ? 0 : (r0 > 255 ? 255 : r0);
            g0 = g0 < 0 ? 0 : (g0 > 255 ? 255 : g0);
            b0 = b0 < 0 ? 0 : (b0 > 255 ? 255 : b0);
            dRow[x] = 0xFF000000u | ((uint32_t)r0 << 16) | ((uint32_t)g0 << 8) | (uint32_t)b0;

            if (x + 1 < w) {
                int r1 = y1 + ((359 * v) >> 8);
                int g1 = y1 - ((88 * u + 183 * v) >> 8);
                int b1 = y1 + ((454 * u) >> 8);
                r1 = r1 < 0 ? 0 : (r1 > 255 ? 255 : r1);
                g1 = g1 < 0 ? 0 : (g1 > 255 ? 255 : g1);
                b1 = b1 < 0 ? 0 : (b1 > 255 ? 255 : b1);
                dRow[x + 1] = 0xFF000000u | ((uint32_t)r1 << 16) | ((uint32_t)g1 << 8) | (uint32_t)b1;
            }
        }
    }

    if (g_d3dDevice && g_d3dCtx && g_swapChain) {
        if (!g_overlayTex || g_overlayW != w || g_overlayH != h) {
            if (g_overlaySRV) { g_overlaySRV->Release(); g_overlaySRV = nullptr; }
            if (g_overlayTex) { g_overlayTex->Release(); g_overlayTex = nullptr; }
            g_overlayW = w;
            g_overlayH = h;
            D3D11_TEXTURE2D_DESC td = {};
            td.Width = w;
            td.Height = h;
            td.MipLevels = 1;
            td.ArraySize = 1;
            td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
            td.SampleDesc.Count = 1;
            td.Usage = D3D11_USAGE_DEFAULT;
            td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            if (SUCCEEDED(g_d3dDevice->CreateTexture2D(&td, nullptr, &g_overlayTex))) {
                g_d3dDevice->CreateShaderResourceView(g_overlayTex, nullptr, &g_overlaySRV);
            }
        }
        if (g_overlayTex && g_backBufferTex && w == 640 && h == 480) {
            g_swapFrameCount++;
            g_d3dCtx->UpdateSubresource(g_backBufferTex, 0, nullptr, g_overlayPixels.data(), w * 4, 0);
            DrawOsdOverlayOnBackbuffer();
            g_swapChain->Present(0, 0);
            if (g_rtv && g_dsv) {
                g_d3dCtx->OMSetRenderTargets(1, &g_rtv, g_dsv);
            }
        }
    }
    UpdateXboxKernelTick();
    UpdateXboxInputTick();
    UpdateXboxAudioTick();
}

static void __stdcall Bridge_D3DDevice_Swap(DWORD Flags) {
    (void)Flags;
    g_swapFrameCount++;
    g_curOffscreenRT = nullptr;
    g_curActiveRTV   = g_rtv;
    g_curActiveDSV   = g_dsv;
    g_backBufferDirty = true;

    DrawOsdOverlayOnBackbuffer();

    if (g_swapChain) {
        g_swapChain->Present(0, 0);
    }
    if (g_d3dCtx && g_rtv && g_dsv) {
        g_d3dCtx->OMSetRenderTargets(1, &g_rtv, g_dsv);
    }
    Bridge_D3DDevice_MakeSpace();
    UpdateXboxKernelTick();
    UpdateXboxInputTick();
    UpdateXboxAudioTick();
}

// Lock2DSurface@PixelJar@D3D (0x006B7150) & Lock3DSurface@PixelJar@D3D (0x006B71F0)
typedef void (__stdcall *PFN_Lock2DSurface)(
    XboxPixelContainer* pContainer, uint32_t FaceType, UINT Level,
    XboxLockedRect* pLockedRect, const RECT* pRect, DWORD Flags);
static PFN_Lock2DSurface g_pfnLock2DSurface = reinterpret_cast<PFN_Lock2DSurface>(0x006B7150);

typedef void (__stdcall *PFN_Lock3DSurface)(
    XboxPixelContainer* pContainer, UINT Level,
    void* pLockedVolume, const void* pBox, DWORD Flags);
static constexpr uint32_t kAddrLock3DSurface = 0x006B71F0u;
static PFN_Lock3DSurface g_pfnLock3DSurface = reinterpret_cast<PFN_Lock3DSurface>(kAddrLock3DSurface);

static HRESULT __stdcall Bridge_D3DTexture_LockRect(
    XboxPixelContainer* pTexture, UINT Level, XboxLockedRect* pLockedRect, const RECT* pRect, DWORD Flags)
{
    if (pTexture) {
        MarkTextureDirtyByData(pTexture->Data);
        g_pfnLock2DSurface(pTexture, 0, Level, pLockedRect, pRect, Flags);
    }
    return S_OK;
}

static HRESULT __stdcall Bridge_D3DSurface_LockRect(
    XboxPixelContainer* pSurface, XboxLockedRect* pLockedRect, const RECT* pRect, DWORD Flags)
{
    if (pSurface) {
        MarkTextureDirtyByData(pSurface->Data);
        g_pfnLock2DSurface(pSurface, 0, 0, pLockedRect, pRect, Flags);
    }
    return S_OK;
}

static HRESULT __stdcall Bridge_D3DVolumeTexture_LockBox(
    XboxPixelContainer* pVolumeTexture, UINT Level, void* pLockedVolume, const void* pBox, DWORD Flags)
{
    if (pVolumeTexture) {
        MarkTextureDirtyByData(pVolumeTexture->Data);
        g_pfnLock3DSurface(pVolumeTexture, Level, pLockedVolume, pBox, Flags);
    }
    return S_OK;
}

static void __stdcall Bridge_D3DDevice_CopyRects(
    XboxPixelContainer* pSourceSurface, const RECT* pSourceRectsArray,
    UINT cRects, XboxPixelContainer* pDestinationSurface, const POINT* pDestPointsArray)
{
    (void)pSourceRectsArray; (void)cRects; (void)pDestPointsArray;
    if (!pSourceSurface || !pDestinationSurface || !pSourceSurface->Data || !pDestinationSurface->Data) return;

    uint32_t sw = 0, sh = 0, slev = 1, sfmt = 0, spitch = 0;
    bool sswiz = false;
    DecodeXboxTextureFormat(pSourceSurface, &sw, &sh, &slev, &sfmt, &sswiz, &spitch);

    uint32_t dw = 0, dh = 0, dlev = 1, dfmt = 0, dpitch = 0;
    bool dswiz = false;
    DecodeXboxTextureFormat(pDestinationSurface, &dw, &dh, &dlev, &dfmt, &dswiz, &dpitch);

    if (sw == dw && sh == dh && pSourceSurface->Data != pDestinationSurface->Data) {
        // Check if source or destination is a GPU render target or the main backbuffer
        ID3D11Texture2D* srcGpuTex = nullptr;
        XboxPixelContainer* mainBB0 = *reinterpret_cast<XboxPixelContainer**>(0x008AA344);
        XboxPixelContainer* mainBB1 = *reinterpret_cast<XboxPixelContainer**>(ADDR_D3D_DEVICE + 0x1A14);
        XboxPixelContainer* mainBB2 = *reinterpret_cast<XboxPixelContainer**>(ADDR_D3D_DEVICE + 0x1A18);
        XboxPixelContainer* mainBB3 = *reinterpret_cast<XboxPixelContainer**>(ADDR_D3D_DEVICE + 0x1A1C);
        if (pSourceSurface == mainBB0 || pSourceSurface == mainBB1 ||
            pSourceSurface == mainBB2 || pSourceSurface == mainBB3 ||
            (mainBB0 && pSourceSurface->Data == mainBB0->Data) ||
            (mainBB1 && pSourceSurface->Data == mainBB1->Data))
        {
            srcGpuTex = g_backBufferTex;
        } else {
            for (size_t i = 0; i < g_texCache.size(); ++i) {
                if (g_texCache[i].dataPtr == pSourceSurface->Data && g_texCache[i].width == sw && g_texCache[i].height == sh) {
                    if (g_texCache[i].isRenderTarget) {
                        srcGpuTex = g_texCache[i].tex2D;
                    }
                    break;
                }
            }
        }

        if (srcGpuTex && g_d3dCtx) {
            CachedTexture* dstEntry = GetOrCreateRenderTargetEntry(pDestinationSurface);
            if (dstEntry && dstEntry->tex2D) {
                g_d3dCtx->CopyResource(dstEntry->tex2D, srcGpuTex);
                return;
            }
        }

        uint32_t bpp = (sfmt == 0x06 || sfmt == 0x07 || sfmt == 0x1E || sfmt == 0x1F) ? 4 : 2;
        if (!sswiz && dswiz) {
            typedef void (__stdcall *PFN_XGSwizzleRect)(
                const void* pSource, uint32_t Pitch, const RECT* pRect,
                void* pDest, uint32_t Width, uint32_t Height, const POINT* pPoint, uint32_t BytesPerPixel);
            PFN_XGSwizzleRect fnSwiz = reinterpret_cast<PFN_XGSwizzleRect>(0x0071F711);
            fnSwiz(reinterpret_cast<const void*>(pSourceSurface->Data), spitch ? spitch : sw * bpp, nullptr,
                   reinterpret_cast<void*>(pDestinationSurface->Data), dw, dh, nullptr, bpp);
        } else {
            size_t bytes = (spitch ? spitch : sw * bpp) * sh;
            memcpy(reinterpret_cast<void*>(pDestinationSurface->Data), reinterpret_cast<const void*>(pSourceSurface->Data), bytes);
        }
        MarkTextureDirtyByData(pDestinationSurface->Data);
    }
}

// ?rlMain_SetGamma@@YAXMM@Z (0x005F4280)
static void __cdecl Bridge_rlMain_SetGamma(float gamma, float brightness) {
    if (gamma > 0.2f && gamma < 4.0f) g_gamma = gamma;
    if (brightness > 0.1f && brightness < 4.0f) g_brightness = brightness;
}

// _D3DDevice_SetGammaRamp@8 (0x006B0310)
static void __stdcall Bridge_D3DDevice_SetGammaRamp(DWORD Flags, const uint8_t* pRamp) {
    (void)Flags;
    if (pRamp) {
        memcpy(reinterpret_cast<void*>(ADDR_D3D_DEVICE + 0x1A2C), pRamp, 768);
        // Sample mid-gray (index 128) from the 256-entry ramp to infer gamma/brightness
        float midR = pRamp[128] * (1.0f / 255.0f);
        if (midR > 0.05f && midR < 0.95f) {
            // 0.5^(1/gamma) = midR -> gamma = ln(0.5)/ln(midR)
            float inferredGamma = -0.693147f / logf(midR);
            if (inferredGamma >= 0.4f && inferredGamma <= 2.5f) {
                g_gamma = inferredGamma;
            }
        }
    }
}

// ============================================================================
// Device Creation (_Direct3D_CreateDevice@24 at 0x006BCD20)
// ============================================================================

static void* __stdcall Bridge_Direct3DCreate8(UINT SDKVersion) {
    (void)SDKVersion;
    return reinterpret_cast<void*>(0x006C0890);
}

bool InitD3D11HostDevice() {
    if (g_d3dDevice) return true;

    DXGI_SWAP_CHAIN_DESC sd = {};
    sd.BufferCount = 1;
    sd.BufferDesc.Width = 640;
    sd.BufferDesc.Height = 480;
    sd.BufferDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    sd.BufferDesc.RefreshRate.Numerator = 60;
    sd.BufferDesc.RefreshRate.Denominator = 1;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = g_hWnd;
    sd.SampleDesc.Count = 1;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    D3D_FEATURE_LEVEL featureLevels[] = {
        D3D_FEATURE_LEVEL_11_0,
        D3D_FEATURE_LEVEL_10_1,
        D3D_FEATURE_LEVEL_10_0
    };
    D3D_FEATURE_LEVEL flOut = D3D_FEATURE_LEVEL_11_0;

    IDXGIFactory* pFactory = nullptr;
    IDXGIAdapter* pSelectedAdapter = nullptr;
    if (SUCCEEDED(CreateDXGIFactory(__uuidof(IDXGIFactory), reinterpret_cast<void**>(&pFactory))) && pFactory) {
        pFactory->EnumAdapters((UINT)g_selectedAdapterIndex, &pSelectedAdapter);
    }

    HRESULT hr = E_FAIL;
    if (pSelectedAdapter) {
        // Note: D3D11 requires D3D_DRIVER_TYPE_UNKNOWN when a non-null IDXGIAdapter* is passed!
        hr = D3D11CreateDeviceAndSwapChain(
            pSelectedAdapter, D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0,
            featureLevels, 3, D3D11_SDK_VERSION, &sd,
            &g_swapChain, &g_d3dDevice, &flOut, &g_d3dCtx);
        pSelectedAdapter->Release();
        pSelectedAdapter = nullptr;
    }
    if (pFactory) {
        pFactory->Release();
        pFactory = nullptr;
    }

    if (FAILED(hr)) {
        hr = D3D11CreateDeviceAndSwapChain(
            nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0,
            featureLevels, 3, D3D11_SDK_VERSION, &sd,
            &g_swapChain, &g_d3dDevice, &flOut, &g_d3dCtx);
    }

    if (FAILED(hr)) {
        hr = D3D11CreateDeviceAndSwapChain(
            nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0,
            featureLevels, 3, D3D11_SDK_VERSION, &sd,
            &g_swapChain, &g_d3dDevice, &flOut, &g_d3dCtx);
    }

    if (FAILED(hr) || !g_d3dDevice || !g_d3dCtx || !g_swapChain) {
        return false;
    }

    // Decouple DXGI swapchain from the Win32 UI thread's message queue (no Alt+Enter / window hooks)
    IDXGIFactory* pSwapFactory = nullptr;
    if (SUCCEEDED(g_swapChain->GetParent(__uuidof(IDXGIFactory), reinterpret_cast<void**>(&pSwapFactory))) && pSwapFactory) {
        pSwapFactory->MakeWindowAssociation(g_hWnd, DXGI_MWA_NO_WINDOW_CHANGES | DXGI_MWA_NO_ALT_ENTER);
        pSwapFactory->Release();
    }

    g_swapChain->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&g_backBufferTex));
    g_d3dDevice->CreateRenderTargetView(g_backBufferTex, nullptr, &g_rtv);

    D3D11_TEXTURE2D_DESC dd = {};
    dd.Width = 640;
    dd.Height = 480;
    dd.MipLevels = 1;
    dd.ArraySize = 1;
    dd.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
    dd.SampleDesc.Count = 1;
    dd.Usage = D3D11_USAGE_DEFAULT;
    dd.BindFlags = D3D11_BIND_DEPTH_STENCIL;
    g_d3dDevice->CreateTexture2D(&dd, nullptr, &g_depthTex);
    g_d3dDevice->CreateDepthStencilView(g_depthTex, nullptr, &g_dsv);

    g_curActiveRTV = g_rtv;
    g_curActiveDSV = g_dsv;
    g_d3dCtx->OMSetRenderTargets(1, &g_rtv, g_dsv);

    D3D11_VIEWPORT vp = { 0.0f, 0.0f, 640.0f, 480.0f, 0.0f, 1.0f };
    g_d3dCtx->RSSetViewports(1, &vp);

    // Compile HLSL Vertex & Pixel Shaders
    ID3DBlob* vsBlob = nullptr;
    ID3DBlob* psBlob = nullptr;
    ID3DBlob* errBlob = nullptr;

    hr = D3DCompile(kGhostHLSL, strlen(kGhostHLSL), "GhostVS", nullptr, nullptr, "VSMain", "vs_4_0", 0, 0, &vsBlob, &errBlob);
    if (SUCCEEDED(hr) && vsBlob) {
        g_d3dDevice->CreateVertexShader(vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), nullptr, &g_vs);
        D3D11_INPUT_ELEMENT_DESC ied[] = {
            { "POSITION", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 0,  D3D11_INPUT_PER_VERTEX_DATA, 0 },
            { "NORMAL",   0, DXGI_FORMAT_R32G32B32_FLOAT,    0, 16, D3D11_INPUT_PER_VERTEX_DATA, 0 },
            { "COLOR",    0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 28, D3D11_INPUT_PER_VERTEX_DATA, 0 },
            { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT,       0, 44, D3D11_INPUT_PER_VERTEX_DATA, 0 },
            { "TEXCOORD", 1, DXGI_FORMAT_R32G32_FLOAT,       0, 52, D3D11_INPUT_PER_VERTEX_DATA, 0 },
            { "TEXCOORD", 2, DXGI_FORMAT_R32G32_FLOAT,       0, 60, D3D11_INPUT_PER_VERTEX_DATA, 0 },
            { "TEXCOORD", 3, DXGI_FORMAT_R32G32_FLOAT,       0, 68, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        };
        g_d3dDevice->CreateInputLayout(ied, 7, vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), &g_inputLayout);
        vsBlob->Release();
    }
    if (errBlob) { errBlob->Release(); errBlob = nullptr; }

    hr = D3DCompile(kGhostHLSL, strlen(kGhostHLSL), "GhostPS", nullptr, nullptr, "PSMain", "ps_4_0", 0, 0, &psBlob, &errBlob);
    if (SUCCEEDED(hr) && psBlob) {
        g_d3dDevice->CreatePixelShader(psBlob->GetBufferPointer(), psBlob->GetBufferSize(), nullptr, &g_ps);
        psBlob->Release();
    }
    if (errBlob) { errBlob->Release(); errBlob = nullptr; }

    D3D11_BUFFER_DESC cbd = {};
    cbd.ByteWidth = sizeof(PerDrawCBData);
    cbd.Usage = D3D11_USAGE_DYNAMIC;
    cbd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    cbd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    g_d3dDevice->CreateBuffer(&cbd, nullptr, &g_cbPerDraw);

    return true;
}

static HRESULT __stdcall Bridge_Direct3D_CreateDevice(
    UINT Adapter, DWORD DeviceType, HWND hFocusWindow, DWORD BehaviorFlags,
    void* pPresentationParameters, void** ppReturnedDeviceInterface)
{
    (void)Adapter; (void)DeviceType; (void)hFocusWindow; (void)BehaviorFlags;

    InitD3D11HostDevice();

    // 1. Set _D3D__pDevice (0x006C0888) = &_D3D__Device (0x006C0890)
    *reinterpret_cast<uint32_t*>(ADDR_D3D_PDEVICE) = ADDR_D3D_DEVICE;
    if (ppReturnedDeviceInterface) {
        *ppReturnedDeviceInterface = reinterpret_cast<void*>(ADDR_D3D_DEVICE);
    }

    // 2. Initialize dummy pushbuffer at CDevice+0x00 and CDevice+0x04
    if (!g_dummyPushBuf) {
        g_dummyPushBuf = (uint32_t*)_aligned_malloc(65536, 4096);
        memset(g_dummyPushBuf, 0, 65536);
    }
    Bridge_D3DDevice_MakeSpace();

    // 3. Replicate CDevice::Init (0x006B6330) internal state setup
    *reinterpret_cast<uint32_t*>(0x006C03E8) |= 0x00FF7F7Fu;
    *reinterpret_cast<uint32_t*>(ADDR_D3D_DEVICE + 0x94C) = 0x40;
    *reinterpret_cast<uint32_t*>(ADDR_D3D_DEVICE + 0x794) = 0x006C2F20;
    memset(reinterpret_cast<void*>(0x006C2E08), 0, 0x46 * 4);
    *reinterpret_cast<uint32_t*>(0x006C2E0C) = 0x10;

    // 4. Copy presentation parameters to CDevice+0x19C8 and call InitializeFrameBuffers (0x006B5D90)
    if (pPresentationParameters) {
        memcpy(reinterpret_cast<void*>(ADDR_D3D_DEVICE + 0x19C8), pPresentationParameters, 0x3C);
        typedef int32_t (__thiscall *PFN_InitializeFrameBuffers)(void* thisPtr, void* pParams);
        PFN_InitializeFrameBuffers fnInitFB = reinterpret_cast<PFN_InitializeFrameBuffers>(0x006B5D90);
        fnInitFB(reinterpret_cast<void*>(ADDR_D3D_DEVICE), pPresentationParameters);

        *reinterpret_cast<uint32_t*>(ADDR_D3D_DEVICE + 0x1A04) = *reinterpret_cast<uint32_t*>(ADDR_D3D_DEVICE + 0x1A14);
        *reinterpret_cast<uint32_t*>(ADDR_D3D_DEVICE + 0x1A08) = *reinterpret_cast<uint32_t*>(ADDR_D3D_DEVICE + 0x1A20);

        typedef void (__stdcall *PFN_SetRenderTarget)(uint32_t pRT, uint32_t pZS);
        PFN_SetRenderTarget fnSetRT = reinterpret_cast<PFN_SetRenderTarget>(0x006B08D0);
        fnSetRT(*reinterpret_cast<uint32_t*>(ADDR_D3D_DEVICE + 0x1A14),
                *reinterpret_cast<uint32_t*>(ADDR_D3D_DEVICE + 0x1A20));
        Bridge_D3DDevice_MakeSpace();
    }

    // 5. Initialize default FVF vertex shaders (D3DFVF_XYZRHW=4, then D3DFVF_XYZ=2)
    Bridge_D3DDevice_SetVertexShader(0x004);
    Bridge_D3DDevice_SetVertexShader(0x002);

    // 6. Call InitializeD3dState@D3D (0x006B5B90) to populate _D3D__RenderState and _D3D__TextureState defaults
    typedef void (__stdcall *PFN_InitializeD3dState)();
    PFN_InitializeD3dState fnInitState = reinterpret_cast<PFN_InitializeD3dState>(0x006B5B90);
    fnInitState();
    Bridge_D3DDevice_MakeSpace();

    return S_OK;
}

// ============================================================================
// Initialize Xbox D3D8 Patches & Bridges
// ============================================================================

void InitXboxD3D8() {
    // 1. Apply the physical-address bitmask patches in Ghost.exe's D3D section
    PatchU32(0x006B3347, 0xFFFFFFFF); // _D3DDevice_CreateTexture2@28
    PatchU32(0x006B34F8, 0xFFFFFFFF); // _D3DDevice_CreateVertexBuffer2@4
    PatchU32(0x006B446E, 0xFFFFFFFF); // _D3DResource_Register@8
    PatchU32(0x006B4514, 0xFFFFFFFF); // CreateSurfaceOfTexture@D3D
    PatchU32(0x006B455E, 0xFFFFFFFF); // InitializeSurface@D3D

    PatchU32(0x006B424E, 0x00000000); // DestroyResource@D3D
    PatchU32(0x006B4287, 0x00000000); // DestroyResource@D3D

    PatchU32(0x006B3553, 0x00000000); // _D3DVertexBuffer_Lock2@8
    PatchU32(0x006B3576, 0x00000000); // _D3DPalette_Lock2@8
    PatchU32(0x006B6F3B, 0x00000000); // FindSurfaceWithinTexture@PixelJar
    PatchU32(0x006B719A, 0x00000000); // Lock2DSurface@PixelJar
    PatchU32(0x006B723B, 0x00000000); // Lock3DSurface@PixelJar

    // 2. Stub GPU busy-wait helpers & pushbuffer kickoff helpers
    PatchRet(0x006B4F00, 4, 0, false); // BlockOnResource@D3D
    PatchRet(0x006B4F80, 4, 0, false); // BlockOnNonSurfaceResource@D3D
    PatchRet(0x006B1C70, 8, 0, false); // SetTileNoWait@D3D
    PatchRet(0x006B2E50, 0, 0, false); // CommonSetPassthroughProgram@D3D
    PatchRet(0x006B4A20, 0, 0, false); // KickOff@CDevice@D3D
    PatchRet(0x006B4AD0, 0, 0, false); // ReentrantKickOffAndWait@CDevice@D3D

    // Bridge _D3DDevice_RunPushBuffer@8 (0x006BACD0) to execute recorded terrain/world pushbuffers
    InstallBridge(0x006BACD0, &Bridge_D3DDevice_RunPushBuffer,          "_D3DDevice_RunPushBuffer@8");

    // _D3DVertexBuffer_Lock2@8 (0x006B3510): mov eax, [esp+4]; mov eax, [eax+4]; ret 8
    static const uint8_t kVbLock2[10] = { 0x8B, 0x44, 0x24, 0x04, 0x8B, 0x40, 0x04, 0xC2, 0x08, 0x00 };
    PatchBytes(0x006B3510, kVbLock2, sizeof(kVbLock2));

    // 3. Bridge Dummy PushBuffer space functions
    InstallBridge(0x006B4E80, &Bridge_D3DDevice_MakeSpace,              "_D3DDevice_MakeSpace@0");
    InstallBridge(0x006B4D50, &Bridge_MakeRequestedSpace,               "?MakeRequestedSpace@D3D@@YGPCKKK@Z");

    // 4. Bridge Device Creation, Swap & Offscreen RenderTarget
    InstallBridge(0x006BD9E0, &Bridge_Direct3DCreate8,                  "_Direct3DCreate8@4");
    InstallBridge(0x006BCD20, &Bridge_Direct3D_CreateDevice,            "_Direct3D_CreateDevice@24");
    PatchRet(0x006BCD00, 8, 0, true);                                   // _Direct3D_SetPushBufferSize@8
    InstallBridge(0x006BD830, &Bridge_D3DDevice_Swap,                   "_D3DDevice_Swap@4");
    InstallBridge(0x006B77D0, &Bridge_D3DDevice_Clear,                  "_D3DDevice_Clear@24");
    InstallBridge(0x005F41F0, &Bridge_rlMain_SetRenderTarget,           "rlMain_SetRenderTarget");
    InstallBridge(0x005F4280, &Bridge_rlMain_SetGamma,                  "rlMain_SetGamma");
    InstallBridge(0x006B0310, &Bridge_D3DDevice_SetGammaRamp,           "_D3DDevice_SetGammaRamp@8");

    // 5. Bridge RenderState, TextureStageState, Transforms, Viewport, Material, Lights, Scissors
    constexpr uint32_t kAddrD3DDeviceSetTextureStateBorderColor   = 0x006AF600u;
    constexpr uint32_t kAddrD3DDeviceSetTextureStateColorKeyColor = 0x006AF640u;
    InstallBridge(0x006AEC10, &Bridge_D3DDevice_SetRenderState_Simple,  "@D3DDevice_SetRenderState_Simple@8");
    InstallBridge(0x006AEC40, &Bridge_D3DDevice_SetRenderStateNotInline,"_D3DDevice_SetRenderStateNotInline@8");
    InstallBridge(0x006AF680, &Bridge_D3DDevice_SetTextureStageStateNotInline, "_D3DDevice_SetTextureStageStateNotInline@12");
    InstallBridge(0x006AF480, &Bridge_D3DDevice_SetTextureState_TexCoordIndex, "_D3DDevice_SetTextureState_TexCoordIndex@8");
    InstallBridge(0x006AF590, &Bridge_D3DDevice_SetTextureState_BumpEnv,       "_D3DDevice_SetTextureState_BumpEnv@12");
    InstallBridge(kAddrD3DDeviceSetTextureStateBorderColor,   &Bridge_D3DDevice_SetTextureState_BorderColor,   "_D3DDevice_SetTextureState_BorderColor@8");
    InstallBridge(kAddrD3DDeviceSetTextureStateColorKeyColor, &Bridge_D3DDevice_SetTextureState_ColorKeyColor, "_D3DDevice_SetTextureState_ColorKeyColor@8");
    InstallBridge(0x006AEF90, &Bridge_D3DDevice_SetRenderState_FogColor,"_D3DDevice_SetRenderState_FogColor@4");
    InstallBridge(0x006AEFE0, &Bridge_D3DDevice_SetRenderState_CullMode,"_D3DDevice_SetRenderState_CullMode@4");
    InstallBridge(0x006AF0D0, &Bridge_D3DDevice_SetRenderState_TextureFactor, "_D3DDevice_SetRenderState_TextureFactor@4");
    InstallBridge(0x006AF220, &Bridge_D3DDevice_SetRenderState_ZBias,   "_D3DDevice_SetRenderState_ZBias@4");
    InstallBridge(0x006AF300, &Bridge_D3DDevice_SetRenderState_FillMode,"_D3DDevice_SetRenderState_FillMode@4");
    InstallBridge(0x006AFE60, &Bridge_D3DDevice_SetRenderState_ZEnable, "_D3DDevice_SetRenderState_ZEnable@4");
    InstallBridge(0x006AFEF0, &Bridge_D3DDevice_SetRenderState_StencilEnable, "_D3DDevice_SetRenderState_StencilEnable@4");
    InstallBridge(0x006AFF80, &Bridge_D3DDevice_SetRenderState_StencilFail,   "_D3DDevice_SetRenderState_StencilFail@4");

    InstallBridge(0x006B03E0, &Bridge_D3DDevice_SetTransform,           "_D3DDevice_SetTransform@8");
    InstallBridge(0x006B12B0, &Bridge_D3DDevice_SetViewport,            "_D3DDevice_SetViewport@4");
    InstallBridge(0x006B0500, &Bridge_D3DDevice_SetMaterial,            "_D3DDevice_SetMaterial@4");
    InstallBridge(0x006B14E0, &Bridge_D3DDevice_SetLight,               "_D3DDevice_SetLight@8");
    InstallBridge(0x006B17A0, &Bridge_D3DDevice_LightEnable,            "_D3DDevice_LightEnable@8");
    InstallBridge(0x006B1D50, &Bridge_D3DDevice_SetScissors,            "_D3DDevice_SetScissors@12");

    // 6. Bridge Textures, LockRect, LockBox, CopyRects, Overlay
    constexpr uint32_t kAddrD3DVolumeTextureLockBox = 0x006B3470u;
    InstallBridge(0x006B1900, &Bridge_D3DDevice_SetTexture,             "_D3DDevice_SetTexture@8");
    InstallBridge(0x006B33D0, &Bridge_D3DTexture_LockRect,              "_D3DTexture_LockRect@20");
    InstallBridge(kAddrD3DVolumeTextureLockBox, &Bridge_D3DVolumeTexture_LockBox, "_D3DVolumeTexture_LockBox@20");
    InstallBridge(0x006B45A0, &Bridge_D3DSurface_LockRect,              "_D3DSurface_LockRect@16");
    InstallBridge(0x006B0F40, &Bridge_D3DDevice_CopyRects,              "_D3DDevice_CopyRects@20");
    InstallBridge(0x006B3250, &Bridge_D3DDevice_EnableOverlay,          "_D3DDevice_EnableOverlay@4");
    InstallBridge(0x006B30B0, &Bridge_D3DDevice_GetOverlayUpdateStatus, "_D3DDevice_GetOverlayUpdateStatus@0");
    InstallBridge(0x006B30D0, &Bridge_D3DDevice_UpdateOverlay,          "_D3DDevice_UpdateOverlay@20");

    // 7. Bridge Vertex Shaders, Constants, Streams, Indices, Draw Calls, Immediate Mode
    InstallBridge(0x006B27F0, &Bridge_D3DDevice_CreateVertexShader,     "_D3DDevice_CreateVertexShader@16");
    InstallBridge(0x006B2F40, &Bridge_D3DDevice_SetVertexShader,        "_D3DDevice_SetVertexShader@4");
    InstallBridge(0x006B2930, &Bridge_D3DDevice_SetVertexShaderConstant1, "@D3DDevice_SetVertexShaderConstant1@8");
    InstallBridge(0x006B2990, &Bridge_D3DDevice_SetVertexShaderConstant4, "@D3DDevice_SetVertexShaderConstant4@8");
    InstallBridge(0x006B2A40, &Bridge_D3DDevice_SetVertexShaderConstantNotInline, "@D3DDevice_SetVertexShaderConstantNotInlineFast@12");
    InstallBridge(0x006B2B20, &Bridge_D3DDevice_SetVertexShaderConstantNotInline, "@D3DDevice_SetVertexShaderConstantNotInline@12");
    InstallBridge(0x006B2B60, &Bridge_D3DDevice_SetStreamSource,        "_D3DDevice_SetStreamSource@12");
    InstallBridge(0x006B1AB0, &Bridge_D3DDevice_SetIndices,             "_D3DDevice_SetIndices@8");
    InstallBridge(0x006B3620, &Bridge_D3DDevice_DrawVerticesUP,         "_D3DDevice_DrawVerticesUP@16");
    InstallBridge(0x006B3790, &Bridge_D3DDevice_DrawVertices,           "_D3DDevice_DrawVertices@12");
    InstallBridge(0x006B3830, &Bridge_D3DDevice_DrawIndexedVertices,    "_D3DDevice_DrawIndexedVertices@12");

    InstallBridge(0x006B3BD0, &Bridge_D3DDevice_Begin,                  "_D3DDevice_Begin@4");
    InstallBridge(0x006B3A90, &Bridge_D3DDevice_SetVertexData2f,        "_D3DDevice_SetVertexData2f@12");
    InstallBridge(0x006B3AD0, &Bridge_D3DDevice_SetVertexData4f,        "_D3DDevice_SetVertexData4f@20");
    InstallBridge(0x006B3B30, &Bridge_D3DDevice_SetVertexData4ub,       "_D3DDevice_SetVertexData4ub@20");
    InstallBridge(0x006B3B80, &Bridge_D3DDevice_SetVertexDataColor,     "_D3DDevice_SetVertexDataColor@8");
    InstallBridge(0x006B3C10, &Bridge_D3DDevice_End,                    "_D3DDevice_End@0");

    // 8. Bridge cShader::VertexSetup* to Ghost.exe's built-in CPU skinning (cMeshGroup::RenderSetup*)
    InstallBridge(0x00607AB0, &Bridge_Shader_VertexSetup,               "cShader::VertexSetup");
    InstallBridge(0x00602FE0, &Bridge_Shader_VertexSetup,               "cLightProjectShader::VertexSetup");
    InstallBridge(0x006085B0, &Bridge_SightShader_VertexSetup,          "cSightShader::VertexSetup");
    InstallBridge(0x00608A20, &Bridge_ShieldShader_VertexSetup,         "cShieldShader::VertexSetup");
    InstallBridge(0x00608C50, &Bridge_TeleportShader_VertexSetup,       "cTeleportShader::VertexSetup");
    InstallBridge(0x00609560, &Bridge_GhostSuitShader_VertexSetup,      "cGhostSuitShader::VertexSetup");
    InstallBridge(0x00609620, &Bridge_ScreenEffectShader_VertexSetup,   "cScreenEffectShader::VertexSetup");
    InstallBridge(0x00609830, &Bridge_CloakShader_VertexSetup,          "cCloakShader::VertexSetup");
    InstallBridge(0x00609E20, &Bridge_MembraneShader_VertexSetup,       "cMembraneShader::VertexSetup");
    InstallBridge(0x00609FE0, &Bridge_MotionBlur_VertexSetup,           "cMotionBlur::VertexSetup");
    InstallBridge(0x006127D0, &Bridge_Shader_VertexSetup,               "cBumpShader::VertexSetup");
    InstallBridge(0x00612920, &Bridge_BumpEnvShader_VertexSetup,        "cBumpEnvShader::VertexSetup");
    InstallBridge(0x00612A70, &Bridge_Shader_VertexSetup,               "cBumpEmbossShader::VertexSetup");
    InstallBridge(0x00616450, &Bridge_EnvmapShader_VertexSetup,         "cEnvmapShader::VertexSetup");
    InstallBridge(0x006165B0, &Bridge_EnvmapShader_VertexSetup,         "cMetalEnvmapShader::VertexSetup");
    InstallBridge(0x00616860, &Bridge_Shader_VertexSetup,               "cGlossEnvmapDetailShader::VertexSetup");
    InstallBridge(0x00616AD0, &Bridge_Shader_VertexSetup,               "cSpecularShader::VertexSetup");

    InstallBridge(0x00605750, &Bridge_Shader_VertexSetupRigid,          "cShader::VertexSetupRigid");
    InstallBridge(0x00605950, &Bridge_Shader_VertexSetupRigid,          "cShader::VertexSetupRigidBump");
    InstallBridge(0x00605B50, &Bridge_Shader_VertexSetupPrelit2,        "cShader::VertexSetupPrelitRigid");
    InstallBridge(0x00605CD0, &Bridge_Shader_VertexSetupWeightedSkin,   "cShader::VertexSetupWeightedSkin");
    InstallBridge(0x00607880, &Bridge_Shader_VertexSetupWeightedSkin,   "cShader::VertexSetupWeightedSkinBump");
    InstallBridge(0x00605EC0, &Bridge_Shader_VertexSetupPrelit4,        "cShader::VertexSetupPrelitWeightedSkin");
    InstallBridge(0x006060D0, &Bridge_Shader_VertexSetupSkinned,        "cShader::VertexSetupSkinned");
    InstallBridge(0x006062D0, &Bridge_Shader_VertexSetupSkinned,        "cShader::VertexSetupSkinnedBump");
    InstallBridge(0x006064F0, &Bridge_Shader_VertexSetupPrelit4,        "cShader::VertexSetupPrelitSkinned");

    // 9. Bridge PixelShader & Hardware Visibility Test (Occlusion Query) functions
    InstallBridge(0x006B3C60, &Bridge_D3DDevice_CreatePixelShader,      "_D3DDevice_CreatePixelShader@8");
    InstallBridge(0x006B3CB0, &Bridge_D3DDevice_SetPixelShaderProgram,  "_D3DDevice_SetPixelShaderProgram@4");
    InstallBridge(0x006B3CF0, &Bridge_D3DDevice_SetPixelShader,         "_D3DDevice_SetPixelShader@4");
    InstallBridge(0x006B3EF0, &Bridge_D3DDevice_SetPixelShaderConstant, "_D3DDevice_SetPixelShaderConstant@12");
    InstallBridge(0x006B1B70, &Bridge_D3DDevice_BeginVisibilityTest,    "_D3DDevice_BeginVisibilityTest@0");
    InstallBridge(0x006B1C10, &Bridge_D3DDevice_EndVisibilityTest,      "_D3DDevice_EndVisibilityTest@4");
    InstallBridge(0x006B05B0, &Bridge_D3DDevice_GetVisibilityTestResult,"_D3DDevice_GetVisibilityTestResult@12");
    PatchRet(0x006B05A0, 4, 0, false); // _D3DDevice_BlockOnFence@4
    PatchRet(0x006B0700, 0, 1, true);  // _D3DDevice_InsertFence@0
}
