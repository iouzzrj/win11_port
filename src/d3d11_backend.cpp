#include "ghost_runtime.h"
#include "ghost_addresses.h"
#include "xbox_d3d8_types.h"
#include "ghost_bot.h"
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

ID3D11Device*             g_d3dDevice         = nullptr;
ID3D11DeviceContext*      g_d3dCtx            = nullptr;
IDXGISwapChain*           g_swapChain         = nullptr;
ID3D11Texture2D*          g_backBufferTex     = nullptr;
ID3D11Texture2D*          g_backBufferCopyTex = nullptr;
ID3D11ShaderResourceView* g_backBufferCopySRV = nullptr;
bool                      g_backBufferDirty   = true;
ID3D11RenderTargetView*   g_rtv               = nullptr;
ID3D11Texture2D*          g_depthTex          = nullptr;
ID3D11DepthStencilView*   g_dsv               = nullptr;
ID3D11RenderTargetView*   g_curActiveRTV      = nullptr;
ID3D11DepthStencilView*   g_curActiveDSV      = nullptr;
void*                     g_curOffscreenRT    = nullptr;
float                     g_gamma             = 1.0f;
float                     g_brightness        = 1.0f;

ID3D11VertexShader*       g_vs                = nullptr;
ID3D11PixelShader*        g_ps                = nullptr;
ID3D11InputLayout*        g_inputLayout       = nullptr;
ID3D11Buffer*             g_cbPerDraw         = nullptr;
ID3D11Buffer*             g_cbVSConstants     = nullptr;
ID3D11Buffer*             g_dynVB             = nullptr;
uint32_t                  g_dynVBCapacity     = 0;

volatile uint32_t         g_swapFrameCount    = 0;
static uint32_t*          g_dummyPushBuf      = nullptr;

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
// HLSL Unified Vertex & Pixel Shaders (generated from shaders/ghost.hlsl)
// ============================================================================

static const char* kGhostHLSL =
#include "ghost_hlsl.inc"
;

// ============================================================================
// RenderState, BlendState, DepthStencilState, RasterizerState, SamplerState Cache
// ============================================================================

static D3D11_COMPARISON_FUNC MapXboxCmpFuncD3D11(uint32_t v) {
    uint32_t idx = v;
    if (v >= 0x200 && v <= 0x207) idx = (v - 0x200) + 1;
    if (idx >= 1 && idx <= 8) return (D3D11_COMPARISON_FUNC)idx;
    return D3D11_COMPARISON_LESS_EQUAL;
}

uint32_t MapXboxCmpFuncInt(uint32_t v) {
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

ID3D11BlendState* GetBlendState(uint32_t alphaBlendEnable, uint32_t srcBlend, uint32_t dstBlend, uint32_t blendOp, uint32_t colorWrite) {
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

ID3D11DepthStencilState* GetDepthState(
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
bool g_scissorEnabled = false;

ID3D11RasterizerState* GetRastState(uint32_t cullMode, uint32_t fillMode, uint32_t zBias, bool scissor) {
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
    desc.SlopeScaledDepthBias = -((float)(int32_t)zBias) * 0.25f;
    desc.DepthClipEnable = TRUE;
    desc.ScissorEnable = scissor ? TRUE : FALSE;

    ID3D11RasterizerState* st = nullptr;
    g_d3dDevice->CreateRasterizerState(&desc, &st);
    g_rastCache.push_back({ key, st });
    return st;
}

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

ID3D11SamplerState* GetSamplerState(uint32_t addrU, uint32_t addrV, uint32_t magFilter, uint32_t borderColor) {
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
    uint32_t* rs = reinterpret_cast<uint32_t*>(ADDR_D3D_RENDERSTATE);
    float fBias = -static_cast<float>(static_cast<int32_t>(Value));
    float fSlope = fBias * 0.25f;
    uint32_t enable = (Value != 0) ? 1u : 0u;
    memcpy(&rs[77], &fSlope, 4); // D3DRS_POLYGONOFFSETZSLOPESCALE (0x006C0724)
    memcpy(&rs[78], &fBias, 4);  // D3DRS_POLYGONOFFSETZOFFSET (0x006C0728)
    rs[79] = enable;             // D3DRS_POINTOFFSETENABLE (0x006C072C)
    rs[80] = enable;             // D3DRS_WIREFRAMEOFFSETENABLE (0x006C0730)
    rs[81] = enable;             // D3DRS_SOLIDOFFSETENABLE (0x006C0734)
    rs[XBOX_D3DRS_ZBIAS] = Value;// D3DRS_ZBIAS (0x006C0844)
}

static void __stdcall Bridge_D3DDevice_SetRenderState_StencilEnable(uint32_t Value) {
    reinterpret_cast<uint32_t*>(ADDR_D3D_RENDERSTATE)[XBOX_D3DRS_STENCILENABLE] = Value;
}

static void __stdcall Bridge_D3DDevice_SetRenderState_StencilFail(uint32_t Value) {
    reinterpret_cast<uint32_t*>(ADDR_D3D_RENDERSTATE)[XBOX_D3DRS_STENCILFAIL] = Value;
}

// ============================================================================
// Hardware Occlusion Queries (BeginVisibilityTest / EndVisibilityTest / GetVisibilityTestResult)
// Used by cEffectGlow::RenderVisCheck (0x0055FD10) & cEffectLensFlare::RenderVisCheck (0x00566F30)
// ============================================================================

static constexpr uint32_t kMaxXboxVisibilityQueries = 1024u;

struct XboxVisibilityQuerySlot {
    ID3D11Query* query;
    bool         pending;
    UINT         lastPixelsPassed;
};

static XboxVisibilityQuerySlot g_visQuerySlots[kMaxXboxVisibilityQueries] = {};
static ID3D11Query*            g_activeVisQuery = nullptr;
static ID3D11Query*            g_visQueryFreePool[kMaxXboxVisibilityQueries * 2] = {};
static uint32_t                g_visQueryFreeCount = 0;

static ID3D11Query* AcquireVisibilityQueryObject() {
    if (g_visQueryFreeCount > 0) {
        return g_visQueryFreePool[--g_visQueryFreeCount];
    }
    if (!g_d3dDevice) return nullptr;
    D3D11_QUERY_DESC qd = {};
    qd.Query = D3D11_QUERY_OCCLUSION;
    qd.MiscFlags = 0;
    ID3D11Query* q = nullptr;
    if (SUCCEEDED(g_d3dDevice->CreateQuery(&qd, &q))) {
        return q;
    }
    return nullptr;
}

static void RecycleVisibilityQueryObject(ID3D11Query* q) {
    if (!q) return;
    if (g_visQueryFreeCount < (kMaxXboxVisibilityQueries * 2)) {
        g_visQueryFreePool[g_visQueryFreeCount++] = q;
    } else {
        q->Release();
    }
}

static HRESULT __stdcall Bridge_D3DDevice_BeginVisibilityTest() {
    if (!g_d3dDevice || !g_d3dCtx) return S_OK;
    if (g_activeVisQuery) {
        g_d3dCtx->End(g_activeVisQuery);
        RecycleVisibilityQueryObject(g_activeVisQuery);
        g_activeVisQuery = nullptr;
    }
    g_activeVisQuery = AcquireVisibilityQueryObject();
    if (g_activeVisQuery) {
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
            slot.lastPixelsPassed = (passed > 0xFFFFFFFFULL) ? 0xFFFFFFFFu : static_cast<UINT>(passed);
        }
        RecycleVisibilityQueryObject(slot.query);
    }
    slot.query   = g_activeVisQuery;
    slot.pending = true;
    g_activeVisQuery = nullptr;
    return S_OK;
}

static HRESULT __stdcall Bridge_D3DDevice_GetVisibilityTestResult(
    DWORD Index, UINT* pResult, uint64_t* pTime)
{
    static constexpr HRESULT kXboxD3DErrTestIncomplete = static_cast<HRESULT>(0x8876086Au);
    uint32_t slotIdx = Index % kMaxXboxVisibilityQueries;
    XboxVisibilityQuerySlot& slot = g_visQuerySlots[slotIdx];

    if (!slot.query || !g_d3dCtx) {
        return kXboxD3DErrTestIncomplete;
    }

    uintptr_t flagsOrTimePtr = reinterpret_cast<uintptr_t>(pTime);
    bool waitForResult = (flagsOrTimePtr == 1u); // D3DVISIBILITY_WAIT

    if (slot.pending) {
        uint64_t passed = 0;
        UINT getDataFlags = waitForResult ? 0u : D3D11_ASYNC_GETDATA_DONOTFLUSH;
        HRESULT hr = g_d3dCtx->GetData(slot.query, &passed, sizeof(passed), getDataFlags);
        if (hr == S_FALSE && waitForResult) {
            for (int spin = 0; spin < 5000 && hr == S_FALSE; ++spin) {
                SwitchToThread();
                hr = g_d3dCtx->GetData(slot.query, &passed, sizeof(passed), 0);
            }
        }
        if (hr == S_OK) {
            slot.lastPixelsPassed = (passed > 0xFFFFFFFFULL) ? 0xFFFFFFFFu : static_cast<UINT>(passed);
            slot.pending = false;
        } else if (hr == S_FALSE && !waitForResult) {
            return kXboxD3DErrTestIncomplete;
        } else {
            slot.pending = false;
        }
    }

    if (pResult) {
        *pResult = slot.lastPixelsPassed;
    }
    if (flagsOrTimePtr >= 0x10000u) {
        *pTime = 1ULL;
    }
    return S_OK;
}

// ============================================================================
// Clear, Top-Right In-Game OSD Overlay (FPS), Swap (Present) & Gamma
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

static ID3D11Texture2D*          g_osdTex           = nullptr;
static ID3D11ShaderResourceView* g_osdSRV           = nullptr;
static ID3D11Buffer*             g_osdVB            = nullptr;
static uint32_t                  g_osdTexW          = 0;
static uint32_t                  g_osdTexH          = 0;
static wchar_t                   g_lastOsdText[256] = { 0 };
static HFONT                     g_hOsdFont         = nullptr;

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

void DrawOsdOverlayOnBackbuffer() {
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
    ID3D11PixelShader* osdPS = GetPixelShaderPermutation(0u);
    g_d3dCtx->PSSetShader(osdPS ? osdPS : g_ps, nullptr, 0);
    g_d3dCtx->PSSetConstantBuffers(0, 1, &g_cbPerDraw);

    ID3D11ShaderResourceView* srvs[4] = { g_osdSRV, nullptr, nullptr, nullptr };
    ID3D11SamplerState*       smps[4] = { smp, smp, smp, smp };
    g_d3dCtx->PSSetShaderResources(0, 4, srvs);
    g_d3dCtx->PSSetSamplers(0, 4, smps);

    g_d3dCtx->Draw(4, 0);

    ID3D11ShaderResourceView* nullSrv = nullptr;
    g_d3dCtx->PSSetShaderResources(0, 1, &nullSrv);
}

static void __stdcall Bridge_D3DDevice_Swap(DWORD Flags) {
    (void)Flags;
    g_swapFrameCount++;
    g_curOffscreenRT = nullptr;
    g_curActiveRTV   = g_rtv;
    g_curActiveDSV   = g_dsv;
    g_backBufferDirty = true;

    DrawOsdOverlayOnBackbuffer();
    GhostBot_On3DSwapFrame();

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

static constexpr uint32_t kNumPsPermutations = 29u; // 0 = fixed-function, 1..28 = aPixelShaders[0..27]
static ID3D11PixelShader* g_psPermutations[kNumPsPermutations] = {};

ID3D11PixelShader* GetPixelShaderPermutation(uint32_t psMode) {
    if (!g_d3dDevice || psMode >= kNumPsPermutations) return g_ps;
    if (g_psPermutations[psMode]) return g_psPermutations[psMode];

    char modeBuf[16];
    snprintf(modeBuf, sizeof(modeBuf), "%uu", psMode);
    const D3D_SHADER_MACRO macros[] = {
        { "FIXED_PS_MODE", modeBuf },
        { nullptr, nullptr }
    };
    ID3DBlob* psBlob  = nullptr;
    ID3DBlob* errBlob = nullptr;
    HRESULT hr = D3DCompile(
        kGhostHLSL, strlen(kGhostHLSL), "GhostPSPerm",
        macros, nullptr, "PSMain", "ps_4_0",
        D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &psBlob, &errBlob);
    if (SUCCEEDED(hr) && psBlob) {
        g_d3dDevice->CreatePixelShader(
            psBlob->GetBufferPointer(), psBlob->GetBufferSize(),
            nullptr, &g_psPermutations[psMode]);
        psBlob->Release();
    }
    if (errBlob) errBlob->Release();
    if (!g_psPermutations[psMode]) {
        g_psPermutations[psMode] = g_ps;
    }
    return g_psPermutations[psMode];
}

static void* __stdcall Bridge_Direct3DCreate8(UINT SDKVersion) {
    (void)SDKVersion;
    return reinterpret_cast<void*>(ADDR_D3D_DEVICE);
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

    hr = D3DCompile(kGhostHLSL, strlen(kGhostHLSL), "GhostVS", nullptr, nullptr, "VSMain", "vs_4_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &vsBlob, &errBlob);
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

    hr = D3DCompile(kGhostHLSL, strlen(kGhostHLSL), "GhostPS", nullptr, nullptr, "PSMain", "ps_4_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &psBlob, &errBlob);
    if (SUCCEEDED(hr) && psBlob) {
        g_d3dDevice->CreatePixelShader(psBlob->GetBufferPointer(), psBlob->GetBufferSize(), nullptr, &g_ps);
        psBlob->Release();
    }
    if (errBlob) { errBlob->Release(); errBlob = nullptr; }

    // Pre-compile fixed-function pixel shader permutation (mode 0)
    GetPixelShaderPermutation(0u);

    D3D11_BUFFER_DESC cbd = {};
    cbd.ByteWidth = sizeof(PerDrawCBData);
    cbd.Usage = D3D11_USAGE_DYNAMIC;
    cbd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    cbd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    g_d3dDevice->CreateBuffer(&cbd, nullptr, &g_cbPerDraw);

    D3D11_BUFFER_DESC vscbd = {};
    vscbd.ByteWidth = 192 * 16;
    vscbd.Usage = D3D11_USAGE_DYNAMIC;
    vscbd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    vscbd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    g_d3dDevice->CreateBuffer(&vscbd, nullptr, &g_cbVSConstants);

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
    *reinterpret_cast<uint32_t*>(ADDR_D3D_DIRTYFLAGS) |= 0x00FF7F7Fu;
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

    // _D3DVertexBuffer_Lock2@8 (0x006B3510): mov eax, [esp+4]; mov eax, [eax+4]; ret 8
    static const uint8_t kVbLock2[10] = { 0x8B, 0x44, 0x24, 0x04, 0x8B, 0x40, 0x04, 0xC2, 0x08, 0x00 };
    PatchBytes(0x006B3510, kVbLock2, sizeof(kVbLock2));

    // 3. Bridge Dummy PushBuffer space functions
    InstallBridge(0x006B4E80, &Bridge_D3DDevice_MakeSpace);                 // _D3DDevice_MakeSpace@0
    InstallBridge(0x006B4D50, &Bridge_MakeRequestedSpace);                  // ?MakeRequestedSpace@D3D@@YGPCKKK@Z

    // 4. Bridge Device Creation, Swap & Gamma
    InstallBridge(0x006BD9E0, &Bridge_Direct3DCreate8);                     // _Direct3DCreate8@4
    InstallBridge(0x006BCD20, &Bridge_Direct3D_CreateDevice);               // _Direct3D_CreateDevice@24
    PatchRet(0x006BCD00, 8, 0, true);                                       // _Direct3D_SetPushBufferSize@8
    InstallBridge(0x006BD830, &Bridge_D3DDevice_Swap);                      // _D3DDevice_Swap@4
    InstallBridge(0x006B77D0, &Bridge_D3DDevice_Clear);                     // _D3DDevice_Clear@24
    InstallBridge(0x005F4280, &Bridge_rlMain_SetGamma);                     // rlMain_SetGamma
    InstallBridge(0x006B0310, &Bridge_D3DDevice_SetGammaRamp);              // _D3DDevice_SetGammaRamp@8

    // 5. Bridge RenderState & TextureStageState functions
    constexpr uint32_t kAddrD3DDeviceSetTextureStateBorderColor   = 0x006AF600u;
    constexpr uint32_t kAddrD3DDeviceSetTextureStateColorKeyColor = 0x006AF640u;
    InstallBridge(0x006AEC10, &Bridge_D3DDevice_SetRenderState_Simple);             // @D3DDevice_SetRenderState_Simple@8
    InstallBridge(0x006AEC40, &Bridge_D3DDevice_SetRenderStateNotInline);           // _D3DDevice_SetRenderStateNotInline@8
    InstallBridge(0x006AF680, &Bridge_D3DDevice_SetTextureStageStateNotInline);     // _D3DDevice_SetTextureStageStateNotInline@12
    InstallBridge(0x006AF480, &Bridge_D3DDevice_SetTextureState_TexCoordIndex);     // _D3DDevice_SetTextureState_TexCoordIndex@8
    InstallBridge(0x006AF590, &Bridge_D3DDevice_SetTextureState_BumpEnv);           // _D3DDevice_SetTextureState_BumpEnv@12
    InstallBridge(kAddrD3DDeviceSetTextureStateBorderColor,   &Bridge_D3DDevice_SetTextureState_BorderColor);   // _D3DDevice_SetTextureState_BorderColor@8
    InstallBridge(kAddrD3DDeviceSetTextureStateColorKeyColor, &Bridge_D3DDevice_SetTextureState_ColorKeyColor); // _D3DDevice_SetTextureState_ColorKeyColor@8
    InstallBridge(0x006AEF90, &Bridge_D3DDevice_SetRenderState_FogColor);           // _D3DDevice_SetRenderState_FogColor@4
    InstallBridge(0x006AEFE0, &Bridge_D3DDevice_SetRenderState_CullMode);           // _D3DDevice_SetRenderState_CullMode@4
    InstallBridge(0x006AF0D0, &Bridge_D3DDevice_SetRenderState_TextureFactor);      // _D3DDevice_SetRenderState_TextureFactor@4
    InstallBridge(0x006AF220, &Bridge_D3DDevice_SetRenderState_ZBias);              // _D3DDevice_SetRenderState_ZBias@4
    InstallBridge(0x006AF300, &Bridge_D3DDevice_SetRenderState_FillMode);           // _D3DDevice_SetRenderState_FillMode@4
    InstallBridge(0x006AFE60, &Bridge_D3DDevice_SetRenderState_ZEnable);            // _D3DDevice_SetRenderState_ZEnable@4
    InstallBridge(0x006AFEF0, &Bridge_D3DDevice_SetRenderState_StencilEnable);      // _D3DDevice_SetRenderState_StencilEnable@4
    InstallBridge(0x006AFF80, &Bridge_D3DDevice_SetRenderState_StencilFail);        // _D3DDevice_SetRenderState_StencilFail@4

    // 6. Bridge Hardware Visibility Test (Occlusion Query) & Fence functions
    InstallBridge(0x006B1B70, &Bridge_D3DDevice_BeginVisibilityTest);               // _D3DDevice_BeginVisibilityTest@0
    InstallBridge(0x006B1C10, &Bridge_D3DDevice_EndVisibilityTest);                 // _D3DDevice_EndVisibilityTest@4
    InstallBridge(0x006B05B0, &Bridge_D3DDevice_GetVisibilityTestResult);           // _D3DDevice_GetVisibilityTestResult@12
    PatchRet(0x006B05A0, 4, 0, false); // _D3DDevice_BlockOnFence@4
    PatchRet(0x006B0700, 0, 1, true);  // _D3DDevice_InsertFence@0

    // 7. Initialize Textures, Shaders, Draw Calls & Video Subsystems
    InitXboxD3D8TextureBridges();
    InitXboxD3D8ShaderBridges();
    InitXboxD3D8DrawBridges();
    InitXboxVideo();
}
