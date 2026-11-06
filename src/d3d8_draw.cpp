#include "ghost_runtime.h"
#include "ghost_addresses.h"
#include "xbox_d3d8_types.h"
#include <d3d11.h>
#include <math.h>
#include <stdio.h>
#include <string>
#include <vector>

// ============================================================================
// Stream Sources, Indices, Transforms, Viewport, Material, Lights & Scissors
// ============================================================================

struct BoundStream {
    const void* data;
    uint32_t    stride;
};
static BoundStream     g_streams[16]     = {};
static const uint16_t* g_indexData       = nullptr;
static uint32_t        g_baseVertexIndex = 0;

static Mat44        g_matView  = { {{1,0,0,0}, {0,1,0,0}, {0,0,1,0}, {0,0,0,1}} };
static Mat44        g_matProj  = { {{1,0,0,0}, {0,1,0,0}, {0,0,1,0}, {0,0,0,1}} };
static Mat44        g_matWorld = { {{1,0,0,0}, {0,1,0,0}, {0,0,1,0}, {0,0,0,1}} };
XboxViewport        g_viewport = { 0, 0, 640, 480, 0.0f, 1.0f };

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

void __stdcall Bridge_D3DDevice_SetStreamSource(uint32_t StreamNumber, XboxVertexBuffer* pStreamData, uint32_t Stride) {
    if (StreamNumber < 16) {
        g_streams[StreamNumber].data = (pStreamData && pStreamData->Data) ? reinterpret_cast<const void*>(pStreamData->Data) : nullptr;
        g_streams[StreamNumber].stride = Stride;
        *reinterpret_cast<uint32_t*>(ADDR_D3D_STREAM + StreamNumber * 12 + 0) = Stride;
        *reinterpret_cast<XboxVertexBuffer**>(ADDR_D3D_STREAM + StreamNumber * 12 + 8) = pStreamData;
        if (StreamNumber == 0) {
            g_curPrelitUseCustomBase = false;
        } else if (StreamNumber == 1 && Stride == 4) {
            g_curPrelitColors = (pStreamData && pStreamData->Data)
                ? reinterpret_cast<const uint32_t*>(pStreamData->Data)
                : nullptr;
            g_curPrelitBaseVertex    = 0;
            g_curPrelitUseCustomBase = false;
        }
    }
}

void __stdcall Bridge_D3DDevice_SetIndices(XboxVertexBuffer* pIndexData, uint32_t BaseVertexIndex) {
    g_indexData = (pIndexData && pIndexData->Data) ? reinterpret_cast<const uint16_t*>(pIndexData->Data) : nullptr;
    g_baseVertexIndex = BaseVertexIndex;
    *reinterpret_cast<const uint16_t**>(ADDR_D3D_INDEXDATA) = g_indexData;
    *reinterpret_cast<XboxVertexBuffer**>(ADDR_D3D_DEVICE + 0x7A0) = pIndexData;
    *reinterpret_cast<uint32_t*>(ADDR_D3D_DEVICE + 0x1C) = BaseVertexIndex;
}

void __stdcall Bridge_D3DDevice_SetTransform(uint32_t State, const Mat44* pMatrix) {
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

// _D3DDevice_SetViewport@4 (0x006B12B0)
// Matches Xbox D3D8 viewport clamping (0x006B12E7..0x006B14A9): clamps (X, Y) and
// (X + Width, Y + Height) to the active render target dimensions. This is critical
// because _D3DDevice_SetRenderTarget@8 (0x006B08D0 -> 0x006AFB00) passes Xbox's
// sentinel g_DefaultViewport at 0x006BE4B4 ({0, 0, 0x7FFFFFFF, 0x7FFFFFFF, 0.0f, 1.0f})
// expecting SetViewport to clamp Width/Height to the current render target (640x480).
void __stdcall Bridge_D3DDevice_SetViewport(const XboxViewport* pViewport) {
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
        g_viewport.Width  = static_cast<uint32_t>(right - x);
        g_viewport.Height = static_cast<uint32_t>(bottom - y);
        g_viewport.MinZ   = pViewport->MinZ;
        g_viewport.MaxZ   = (pViewport->MaxZ > pViewport->MinZ) ? pViewport->MaxZ : 1.0f;
    } else {
        g_viewport = { 0, 0, rtW, rtH, 0.0f, 1.0f };
    }

    memcpy(reinterpret_cast<void*>(ADDR_D3D_DEVICE + 0xEE0), &g_viewport, sizeof(XboxViewport));
    *reinterpret_cast<uint32_t*>(ADDR_D3D_DIRTYFLAGS) |= 0x100u;
    g_scissorEnabled   = false;
    g_scissorCount     = 0;
    g_scissorExclusive = false;

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

uint32_t g_scissorCount     = 0;
bool     g_scissorExclusive = false;
RECT     g_scissorRects[8]  = {};

static void __stdcall Bridge_D3DDevice_SetMaterial(const void* pMaterial) {
    if (pMaterial) {
        memcpy(reinterpret_cast<void*>(ADDR_DEVICE_MATERIAL), pMaterial, sizeof(XboxMaterial8));
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
    if (!g_d3dCtx) return;
    if (Count > 0 && pRects) {
        g_scissorCount     = (Count > 8u) ? 8u : Count;
        g_scissorExclusive = (bExclusive != FALSE);
        for (uint32_t i = 0; i < g_scissorCount; ++i) {
            g_scissorRects[i] = pRects[i];
        }
        memcpy(reinterpret_cast<void*>(ADDR_D3D_DEVICE + 0x1B80), g_scissorRects, g_scissorCount * sizeof(RECT));
        *reinterpret_cast<uint32_t*>(ADDR_D3D_DEVICE + 0x1C00) = g_scissorCount;
        *reinterpret_cast<uint32_t*>(ADDR_D3D_DEVICE + 0x1C04) = g_scissorExclusive ? 1u : 0u;

        g_scissorEnabled = true;
        if (g_scissorCount == 1u && !g_scissorExclusive) {
            g_d3dCtx->RSSetScissorRects(1, &g_scissorRects[0]);
        }
    } else {
        g_scissorCount     = 0;
        g_scissorExclusive = false;
        g_scissorEnabled   = false;
        *reinterpret_cast<uint32_t*>(ADDR_D3D_DEVICE + 0x1C00) = 0;
        *reinterpret_cast<uint32_t*>(ADDR_D3D_DEVICE + 0x1C04) = 0;
    }
}

// ============================================================================
// Vertex Layout Decoding & Draw Call Execution on D3D11
// ============================================================================

void UnpackD3DColor(uint32_t c, float* r, float* g, float* b, float* a) {
    *a = ((c >> 24) & 0xFFu) * (1.0f / 255.0f);
    *r = ((c >> 16) & 0xFFu) * (1.0f / 255.0f);
    *g = ((c >> 8)  & 0xFFu) * (1.0f / 255.0f);
    *b = (c         & 0xFFu) * (1.0f / 255.0f);
}

static uint32_t g_lastProjMask = 0;
static uint32_t g_gpuLightMode = 0; // 0 = off, 1 = defaultWorldLit (unit material), 2 = D3DMATERIAL8 lighting

static void DecodeVerticesToUnified(
    const uint8_t* srcData, uint32_t stride, uint32_t vertexCount,
    const uint16_t* optIndices, uint32_t baseVertexIndex,
    std::vector<UnifiedVertex>& outVerts, bool* outIsRhw)
{
    g_lastProjMask = 0;
    g_gpuLightMode = 0;
    XboxShaderDeclInfo decl = {};
    decl.vsType = -1;
    GetXboxVertexShaderDecl(g_currentVsHandle, &decl);

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
        if (fvf & 0x080u) { decl.hasSpec = true; decl.offSpec = off; off += 4; } // D3DFVF_SPECULAR
        uint32_t texCount = (fvf >> 8) & 0x0Fu;
        auto decodeFvfUvDim = [&](uint32_t stageIdx) -> uint8_t {
            uint32_t code = (fvf >> (16 + stageIdx * 2)) & 0x3u;
            if (code == 1u) return 3u;
            if (code == 2u) return 4u;
            if (code == 3u) return 1u;
            return 2u;
        };
        if (texCount >= 1) { uint8_t d = decodeFvfUvDim(0); decl.hasUv0 = true; decl.offUv0 = off; decl.dimUv[0] = d; off += d * 4; }
        if (texCount >= 2) { uint8_t d = decodeFvfUvDim(1); decl.hasUv1 = true; decl.offUv1 = off; decl.dimUv[1] = d; off += d * 4; }
        if (texCount >= 3) { uint8_t d = decodeFvfUvDim(2); decl.hasUv2 = true; decl.offUv2 = off; decl.dimUv[2] = d; off += d * 4; }
        if (texCount >= 4) { uint8_t d = decodeFvfUvDim(3); decl.hasUv3 = true; decl.offUv3 = off; decl.dimUv[3] = d; off += d * 4; }
    }

    *outIsRhw = decl.hasRhw;
    outVerts.resize(vertexCount);

    const uint32_t* xboxRS  = reinterpret_cast<const uint32_t*>(ADDR_D3D_RENDERSTATE);
    const uint32_t* xboxTSS = reinterpret_cast<const uint32_t*>(ADDR_D3D_TEXTURESTATE);
    const Mat44*    texMats = reinterpret_cast<const Mat44*>(ADDR_D3D_DEVICE + 0xC60 + 2 * 64); // D3DTS_TEXTURE0..3

    // Active Front & Back D3DMATERIAL8 in CDevice (+0xF00 = 0x006C1790, +0xF44 = 0x006C17D4)
    const XboxMaterial8& frontMat = g_hasMaterial
        ? *reinterpret_cast<const XboxMaterial8*>(ADDR_DEVICE_MATERIAL)
        : g_material;
    const XboxMaterial8& backMat  = *reinterpret_cast<const XboxMaterial8*>(ADDR_DEVICE_BACK_MATERIAL);

    // Xbox Fixed-Function Lighting RenderStates:
    // [102]=LIGHTING, [103]=SPECULARENABLE, [104]=LOCALVIEWER, [105]=COLORVERTEX,
    // [106..109]=BACKSPECULAR/DIFFUSE/AMBIENT/EMISSIVEMATERIALSOURCE,
    // [110..113]=SPECULAR/DIFFUSE/AMBIENT/EMISSIVEMATERIALSOURCE (0=MATERIAL, 1=COLOR1, 2=COLOR2),
    // [114]=BACKAMBIENT, [115]=AMBIENT, [141]=TWOSIDEDLIGHTING, [142]=NORMALIZENORMALS
    uint32_t lightingMode     = *reinterpret_cast<const uint32_t*>(ADDR_LIGHTING_MODE);
    bool     lightingEnabled  = (xboxRS[XBOX_D3DRS_LIGHTING] != 0);
    bool     specularEnabled  = (xboxRS[XBOX_D3DRS_SPECULARENABLE] != 0);
    bool     localViewer      = (xboxRS[XBOX_D3DRS_LOCALVIEWER] != 0);
    bool     colorVertex      = (xboxRS[XBOX_D3DRS_COLORVERTEX] != 0);
    uint32_t specMatSrc       = colorVertex ? xboxRS[XBOX_D3DRS_SPECULARMATERIALSOURCE] : 0u;
    uint32_t diffMatSrc       = colorVertex ? xboxRS[XBOX_D3DRS_DIFFUSEMATERIALSOURCE]  : 0u;
    uint32_t ambMatSrc        = colorVertex ? xboxRS[XBOX_D3DRS_AMBIENTMATERIALSOURCE]  : 0u;
    uint32_t emisMatSrc       = colorVertex ? xboxRS[XBOX_D3DRS_EMISSIVEMATERIALSOURCE] : 0u;
    uint32_t backSpecMatSrc   = colorVertex ? xboxRS[XBOX_D3DRS_BACKSPECULARMATERIALSOURCE] : 0u;
    uint32_t backDiffMatSrc   = colorVertex ? xboxRS[XBOX_D3DRS_BACKDIFFUSEMATERIALSOURCE]  : 0u;
    uint32_t backAmbMatSrc    = colorVertex ? xboxRS[XBOX_D3DRS_BACKAMBIENTMATERIALSOURCE]  : 0u;
    uint32_t backEmisMatSrc   = colorVertex ? xboxRS[XBOX_D3DRS_BACKEMISSIVEMATERIALSOURCE] : 0u;
    bool     twoSidedLighting = (xboxRS[XBOX_D3DRS_TWOSIDEDLIGHTING] != 0);
    bool     normalizeNormals = (xboxRS[XBOX_D3DRS_NORMALIZENORMALS] != 0);

    bool pureDynamicMode  = (lightingMode == 4) || (lightingEnabled && diffMatSrc == 0u && emisMatSrc == 0u); // SetLighting(4)
    const bool hasPrelitColors = (g_curPrelitColors != nullptr) && (decl.hasStream1Diff || g_curPrelitUseCustomBase);
    bool prelitOnlyMode   = (lightingMode == 2 && hasPrelitColors);
    float ambR = 0.0f, ambG = 0.0f, ambB = 0.0f, ambA = 1.0f;
    UnpackD3DColor(xboxRS[XBOX_D3DRS_AMBIENT], &ambR, &ambG, &ambB, &ambA);
    if (ambA <= 0.001f) ambA = 1.0f;
    float backAmbR = 0.0f, backAmbG = 0.0f, backAmbB = 0.0f, backAmbA = 1.0f;
    UnpackD3DColor(xboxRS[XBOX_D3DRS_BACKAMBIENT], &backAmbR, &backAmbG, &backAmbB, &backAmbA);

    Mat44 worldView = MulMat44(g_matWorld, g_matView);
    Mat44 wvp       = MulMat44(worldView, g_matProj);

    // Precompute active lights and per-draw invariant directional/spotlight parameters
    struct ActiveLightPrecomp {
        const XboxLight8* lt;
        float dirX, dirY, dirZ;
        float cosHalfPhi;
        float cosHalfTheta;
    };
    ActiveLightPrecomp activeLights[16];
    int numActiveLights = 0;
    for (int l = 0; l < 16; ++l) {
        if (!g_lightEnabled[l]) continue;
        const XboxLight8& lt = g_lights[l];
        ActiveLightPrecomp& al = activeLights[numActiveLights++];
        al.lt = &lt;
        al.dirX = 0.0f; al.dirY = 0.0f; al.dirZ = 0.0f;
        al.cosHalfPhi = 0.0f; al.cosHalfTheta = 0.0f;
        if (lt.Type == 3) { // D3DLIGHT_DIRECTIONAL
            float lx = -lt.Direction[0], ly = -lt.Direction[1], lz = -lt.Direction[2];
            float lLen = sqrtf(lx * lx + ly * ly + lz * lz);
            if (lLen > 0.0001f) { lx /= lLen; ly /= lLen; lz /= lLen; }
            al.dirX = lx; al.dirY = ly; al.dirZ = lz;
        } else if (lt.Type == 2) { // D3DLIGHT_SPOT
            float sdx = lt.Direction[0], sdy = lt.Direction[1], sdz = lt.Direction[2];
            float sdLen = sqrtf(sdx * sdx + sdy * sdy + sdz * sdz);
            if (sdLen > 0.0001f) { sdx /= sdLen; sdy /= sdLen; sdz /= sdLen; }
            al.dirX = sdx; al.dirY = sdy; al.dirZ = sdz;
            al.cosHalfPhi   = cosf(lt.Phi * 0.5f);
            al.cosHalfTheta = cosf(lt.Theta * 0.5f);
        }
    }

    // Intra-draw Post-T&L vertex cache (simulates Xbox NV2A hardware indexed vertex cache)
    uint32_t postTnLTag[1024];
    uint32_t postTnLOutIdx[1024];
    const bool usePostTnLCache = (optIndices != nullptr && vertexCount > 3);
    if (usePostTnLCache) {
        memset(postTnLTag, 0xFF, sizeof(postTnLTag));
    }

    const bool hasPerVertexMaterialSource = (decl.vsType != 1) && (
        (diffMatSrc == 1u && decl.hasDiff) ||
        (diffMatSrc == 2u && decl.hasSpec) ||
        (ambMatSrc  == 1u && (decl.hasDiff || hasPrelitColors)) ||
        (ambMatSrc  == 2u && decl.hasSpec) ||
        (specularEnabled && ((specMatSrc == 1u && (decl.hasDiff || hasPrelitColors)) || (specMatSrc == 2u && decl.hasSpec))) ||
        twoSidedLighting);

    const bool offloadLightsToGpu    = (numActiveLights > 0 && !hasPerVertexMaterialSource);
    const bool needCpuWorldTransform = !decl.hasRhw && (numActiveLights > 0 && !offloadLightsToGpu);

    const uint8_t* pActiveCam = *reinterpret_cast<const uint8_t* const*>(ADDR_ACTIVE_CAMERA_PTR);
    const float*   camPosWorld = pActiveCam ? reinterpret_cast<const float*>(pActiveCam + 0x30) : reinterpret_cast<const float*>(0x008ACC38);
    float nonLocalVx = -g_matView.m[0][2], nonLocalVy = -g_matView.m[1][2], nonLocalVz = -g_matView.m[2][2];
    float nlLen = sqrtf(nonLocalVx * nonLocalVx + nonLocalVy * nonLocalVy + nonLocalVz * nonLocalVz);
    if (nlLen > 0.0001f) { nonLocalVx /= nlLen; nonLocalVy /= nlLen; nonLocalVz /= nlLen; }

    for (uint32_t i = 0; i < vertexCount; ++i) {
        if (usePostTnLCache) {
            uint32_t rawIdx = (uint32_t)optIndices[i];
            uint32_t slot   = rawIdx & 1023u;
            if (postTnLTag[slot] == rawIdx) {
                outVerts[i] = outVerts[postTnLOutIdx[slot]];
                continue;
            }
            postTnLTag[slot]    = rawIdx;
            postTnLOutIdx[slot] = i;
        }
        uint32_t vIdx = optIndices ? ((uint32_t)optIndices[i] + baseVertexIndex) : (i + baseVertexIndex);
        const uint8_t* vPtr = srcData + vIdx * stride;
        UnifiedVertex& dst = outVerts[i];
        dst.w0 = 0.0f; dst.w1 = 0.0f; dst.w2 = 0.0f; dst.w3 = 0.0f;
        dst.sr = 0.0f; dst.sg = 0.0f; dst.sb = 0.0f; dst.sa = 1.0f;

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
            if (normalizeNormals) {
                float nLen = sqrtf(dst.nx * dst.nx + dst.ny * dst.ny + dst.nz * dst.nz);
                if (nLen > 0.0001f) { dst.nx /= nLen; dst.ny /= nLen; dst.nz /= nLen; }
            }
        } else {
            dst.nx = 0.0f; dst.ny = 1.0f; dst.nz = 0.0f;
        }

        float wx = 0.0f, wy = 1.0f, wz = 0.0f;
        float wpx = dst.x, wpy = dst.y, wpz = dst.z;
        if (needCpuWorldTransform) {
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

        float c1R = 1.0f, c1G = 1.0f, c1B = 1.0f, c1A = 1.0f;
        bool hasColor1 = false;
        if (decl.hasDiff) {
            uint32_t c = *reinterpret_cast<const uint32_t*>(vPtr + decl.offDiff);
            UnpackD3DColor(c, &c1R, &c1G, &c1B, &c1A);
            hasColor1 = true;
        } else if (hasPrelitColors) {
            uint32_t preIdx = g_curPrelitUseCustomBase
                ? (g_curPrelitBaseVertex + (optIndices ? (uint32_t)optIndices[i] : i))
                : vIdx;
            UnpackD3DColor(g_curPrelitColors[preIdx], &c1R, &c1G, &c1B, &c1A);
            hasColor1 = true;
        }

        float c2R = 0.0f, c2G = 0.0f, c2B = 0.0f, c2A = 1.0f;
        bool hasColor2 = false;
        if (decl.hasSpec) {
            uint32_t sc = *reinterpret_cast<const uint32_t*>(vPtr + decl.offSpec);
            UnpackD3DColor(sc, &c2R, &c2G, &c2B, &c2A);
            hasColor2 = true;
            dst.sr = c2R; dst.sg = c2G; dst.sb = c2B; dst.sa = c2A;
        }

        auto evalDynamicLights = [&](float nxW, float nyW, float nzW,
                                     float matDiffR, float matDiffG, float matDiffB,
                                     float matAmbR,  float matAmbG,  float matAmbB,
                                     float matSpecR, float matSpecG, float matSpecB, float matPower,
                                     float& totR, float& totG, float& totB,
                                     float& specOutR, float& specOutG, float& specOutB)
        {
            float vx = nonLocalVx, vy = nonLocalVy, vz = nonLocalVz;
            if (specularEnabled && localViewer) {
                vx = camPosWorld[0] - wpx;
                vy = camPosWorld[1] - wpy;
                vz = camPosWorld[2] - wpz;
                float vLen = sqrtf(vx * vx + vy * vy + vz * vz);
                if (vLen > 0.0001f) { vx /= vLen; vy /= vLen; vz /= vLen; }
            }
            float spPow = (matPower > 0.0f) ? matPower : 16.0f;

            for (int alIdx = 0; alIdx < numActiveLights; ++alIdx) {
                const ActiveLightPrecomp& al = activeLights[alIdx];
                const XboxLight8& lt = *al.lt;
                float lx = 0.0f, ly = 0.0f, lz = 0.0f, atten = 1.0f;
                if (lt.Type == 3) { // D3DLIGHT_DIRECTIONAL
                    lx = al.dirX; ly = al.dirY; lz = al.dirZ;
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
                        float cosAlpha = -(lx * al.dirX + ly * al.dirY + lz * al.dirZ);
                        if (cosAlpha <= al.cosHalfPhi) continue;
                        if (cosAlpha < al.cosHalfTheta && (al.cosHalfTheta - al.cosHalfPhi) > 0.0001f) {
                            atten *= powf((cosAlpha - al.cosHalfPhi) / (al.cosHalfTheta - al.cosHalfPhi), lt.Falloff > 0.0f ? lt.Falloff : 1.0f);
                        }
                    }
                } else {
                    continue;
                }
                float ndotl = nxW * lx + nyW * ly + nzW * lz;
                if (ndotl < 0.0f) ndotl = 0.0f;
                totR += atten * (lt.Ambient[0] * matAmbR + ndotl * lt.Diffuse[0] * matDiffR);
                totG += atten * (lt.Ambient[1] * matAmbG + ndotl * lt.Diffuse[1] * matDiffG);
                totB += atten * (lt.Ambient[2] * matAmbB + ndotl * lt.Diffuse[2] * matDiffB);
                if (specularEnabled && ndotl > 0.0f) {
                    float hx = lx + vx, hy = ly + vy, hz = lz + vz;
                    float hLen = sqrtf(hx * hx + hy * hy + hz * hz);
                    if (hLen > 0.0001f) { hx /= hLen; hy /= hLen; hz /= hLen; }
                    float ndoth = nxW * hx + nyW * hy + nzW * hz;
                    if (ndoth > 0.0f) {
                        float sp = atten * powf(ndoth, spPow);
                        specOutR += sp * lt.Specular[0] * matSpecR;
                        specOutG += sp * lt.Specular[1] * matSpecG;
                        specOutB += sp * lt.Specular[2] * matSpecB;
                    }
                }
            }
        };

        if (decl.hasDiff && !pureDynamicMode && (decl.vsType == 1 || !lightingEnabled || prelitOnlyMode || lightingMode == 0 || !decl.hasNorm || decl.hasRhw)) {
            dst.r = c1R; dst.g = c1G; dst.b = c1B; dst.a = c1A;
            if (!decl.hasRhw && decl.hasNorm && decl.vsType == 1 && !prelitOnlyMode && lightingMode != 0) { // defaultWorldLit0..3.xvu
                float totR = dst.r + ambR;
                float totG = dst.g + ambG;
                float totB = dst.b + ambB;
                if (offloadLightsToGpu) {
                    g_gpuLightMode = 1u;
                    dst.r = totR;
                    dst.g = totG;
                    dst.b = totB;
                } else {
                    float spR = c2R, spG = c2G, spB = c2B;
                    evalDynamicLights(wx, wy, wz, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 16.0f, totR, totG, totB, spR, spG, spB);
                    dst.r = totR < 0.0f ? 0.0f : (totR > 1.0f ? 1.0f : totR);
                    dst.g = totG < 0.0f ? 0.0f : (totG > 1.0f ? 1.0f : totG);
                    dst.b = totB < 0.0f ? 0.0f : (totB > 1.0f ? 1.0f : totB);
                    if (specularEnabled) { dst.sr = spR; dst.sg = spG; dst.sb = spB; }
                }
            }
        } else if (decl.hasNorm && !decl.hasRhw) {
            if (lightingMode != 0 && (lightingEnabled || hasPrelitColors)) {
                auto selectMatComp = [&](uint32_t src, const float* matCol, float* outR, float* outG, float* outB, float* outA) {
                    if (src == 1u && hasColor1) {
                        *outR = c1R; *outG = c1G; *outB = c1B;
                        if (outA) *outA = c1A;
                    } else if (src == 2u && hasColor2) {
                        *outR = c2R; *outG = c2G; *outB = c2B;
                        if (outA) *outA = c2A;
                    } else {
                        *outR = matCol[0]; *outG = matCol[1]; *outB = matCol[2];
                        if (outA) *outA = matCol[3];
                    }
                };

                // For CPU-skinned meshes (Host_RenderSetupWeightedSkin -> !decl.hasDiff && hasPrelitColors),
                // prelit colors are baked lighting added to the material emissive/ambient base while matDiff uses frontMat.Diffuse.
                uint32_t effDiffSrc = (!decl.hasDiff && hasPrelitColors) ? 0u : diffMatSrc;
                uint32_t effEmisSrc = (!pureDynamicMode && !decl.hasDiff && hasPrelitColors) ? 1u : emisMatSrc;

                float matDiffR = 1.0f, matDiffG = 1.0f, matDiffB = 1.0f, matDiffA = 1.0f;
                float matAmbR  = 1.0f, matAmbG  = 1.0f, matAmbB  = 1.0f;
                float matEmisR = 0.0f, matEmisG = 0.0f, matEmisB = 0.0f;
                float matSpecR = 0.0f, matSpecG = 0.0f, matSpecB = 0.0f;

                selectMatComp(effDiffSrc, frontMat.Diffuse,  &matDiffR, &matDiffG, &matDiffB, &matDiffA);
                selectMatComp(ambMatSrc,  frontMat.Ambient,  &matAmbR,  &matAmbG,  &matAmbB,  nullptr);
                selectMatComp(effEmisSrc, frontMat.Emissive, &matEmisR, &matEmisG, &matEmisB, nullptr);
                selectMatComp(specMatSrc, frontMat.Specular, &matSpecR, &matSpecG, &matSpecB, nullptr);

                float totR = (!prelitOnlyMode && lightingEnabled) ? (ambR * matAmbR + matEmisR) : matEmisR;
                float totG = (!prelitOnlyMode && lightingEnabled) ? (ambG * matAmbG + matEmisG) : matEmisG;
                float totB = (!prelitOnlyMode && lightingEnabled) ? (ambB * matAmbB + matEmisB) : matEmisB;

                if (!prelitOnlyMode && lightingEnabled && offloadLightsToGpu) {
                    g_gpuLightMode = 2u;
                    dst.r = totR;
                    dst.g = totG;
                    dst.b = totB;
                } else {
                    float spR = c2R, spG = c2G, spB = c2B;
                    if (!prelitOnlyMode && lightingEnabled) {
                        evalDynamicLights(wx, wy, wz, matDiffR, matDiffG, matDiffB, matAmbR, matAmbG, matAmbB, matSpecR, matSpecG, matSpecB, frontMat.Power, totR, totG, totB, spR, spG, spB);
                        if (twoSidedLighting) {
                            float vx = camPosWorld[0] - wpx, vy = camPosWorld[1] - wpy, vz = camPosWorld[2] - wpz;
                            if ((wx * vx + wy * vy + wz * vz) < 0.0f) {
                                float bDiffR = 1.0f, bDiffG = 1.0f, bDiffB = 1.0f, bDiffA = 1.0f;
                                float bAmbR  = 1.0f, bAmbG  = 1.0f, bAmbB  = 1.0f;
                                float bEmisR = 0.0f, bEmisG = 0.0f, bEmisB = 0.0f;
                                float bSpecR = 0.0f, bSpecG = 0.0f, bSpecB = 0.0f;
                                selectMatComp(backDiffMatSrc, backMat.Diffuse,  &bDiffR, &bDiffG, &bDiffB, &bDiffA);
                                selectMatComp(backAmbMatSrc,  backMat.Ambient,  &bAmbR,  &bAmbG,  &bAmbB,  nullptr);
                                selectMatComp(backEmisMatSrc, backMat.Emissive, &bEmisR, &bEmisG, &bEmisB, nullptr);
                                selectMatComp(backSpecMatSrc, backMat.Specular, &bSpecR, &bSpecG, &bSpecB, nullptr);
                                totR = backAmbR * bAmbR + bEmisR;
                                totG = backAmbG * bAmbG + bEmisG;
                                totB = backAmbB * bAmbB + bEmisB;
                                spR = c2R; spG = c2G; spB = c2B;
                                evalDynamicLights(-wx, -wy, -wz, bDiffR, bDiffG, bDiffB, bAmbR, bAmbG, bAmbB, bSpecR, bSpecG, bSpecB, backMat.Power, totR, totG, totB, spR, spG, spB);
                                matDiffA = bDiffA;
                            }
                        }
                    }
                    dst.r = totR < 0.0f ? 0.0f : (totR > 1.0f ? 1.0f : totR);
                    dst.g = totG < 0.0f ? 0.0f : (totG > 1.0f ? 1.0f : totG);
                    dst.b = totB < 0.0f ? 0.0f : (totB > 1.0f ? 1.0f : totB);
                    if (specularEnabled) { dst.sr = spR; dst.sg = spG; dst.sb = spB; }
                }
                if (effDiffSrc != 0u && hasColor1) {
                    dst.a = c1A;
                } else if (g_hasMaterial && frontMat.Diffuse[3] > 0.001f && frontMat.Diffuse[3] <= 1.0f) {
                    dst.a = frontMat.Diffuse[3];
                } else {
                    dst.a = (ambA > 0.001f && ambA <= 1.0f) ? ambA : 1.0f;
                }
            } else {
                dst.r = hasColor1 ? c1R : 1.0f;
                dst.g = hasColor1 ? c1G : 1.0f;
                dst.b = hasColor1 ? c1B : 1.0f;
                dst.a = hasColor1 ? c1A : ((ambA > 0.001f && ambA <= 1.0f) ? ambA : 1.0f);
            }
        } else {
            dst.r = hasColor1 ? c1R : 1.0f;
            dst.g = hasColor1 ? c1G : 1.0f;
            dst.b = hasColor1 ? c1B : 1.0f;
            dst.a = hasColor1 ? c1A : 1.0f;
        }

        // Read raw UVW0..3 from vertex stream
        float rawU[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
        float rawV[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
        float rawW[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
        bool  has3DUv[4] = { false, false, false, false };
        if (decl.hasUv0) {
            const float* uv = reinterpret_cast<const float*>(vPtr + decl.offUv0);
            uint8_t d = decl.dimUv[0] ? decl.dimUv[0] : 2u;
            rawU[0] = uv[0];
            rawV[0] = (d >= 2u) ? uv[1] : 0.0f;
            rawW[0] = (d >= 3u) ? uv[2] : 0.0f;
            has3DUv[0] = (d >= 3u);
        }
        if (decl.hasUv1) {
            const float* uv = reinterpret_cast<const float*>(vPtr + decl.offUv1);
            uint8_t d = decl.dimUv[1] ? decl.dimUv[1] : 2u;
            rawU[1] = uv[0];
            rawV[1] = (d >= 2u) ? uv[1] : 0.0f;
            rawW[1] = (d >= 3u) ? uv[2] : 0.0f;
            has3DUv[1] = (d >= 3u);
        } else {
            rawU[1] = rawU[0]; rawV[1] = rawV[0]; rawW[1] = rawW[0]; has3DUv[1] = has3DUv[0];
        }
        if (decl.hasUv2) {
            const float* uv = reinterpret_cast<const float*>(vPtr + decl.offUv2);
            uint8_t d = decl.dimUv[2] ? decl.dimUv[2] : 2u;
            rawU[2] = uv[0];
            rawV[2] = (d >= 2u) ? uv[1] : 0.0f;
            rawW[2] = (d >= 3u) ? uv[2] : 0.0f;
            has3DUv[2] = (d >= 3u);
        } else {
            rawU[2] = rawU[0]; rawV[2] = rawV[0]; rawW[2] = rawW[0]; has3DUv[2] = has3DUv[0];
        }
        if (decl.hasUv3) {
            const float* uv = reinterpret_cast<const float*>(vPtr + decl.offUv3);
            uint8_t d = decl.dimUv[3] ? decl.dimUv[3] : 2u;
            rawU[3] = uv[0];
            rawV[3] = (d >= 2u) ? uv[1] : 0.0f;
            rawW[3] = (d >= 3u) ? uv[2] : 0.0f;
            has3DUv[3] = (d >= 3u);
        } else {
            rawU[3] = rawU[0]; rawV[3] = rawV[0]; rawW[3] = rawW[0]; has3DUv[3] = has3DUv[0];
        }

        // Standard 4-stage UV routing: D3DTSS_TEXCOORDINDEX (xboxTSS[s*32 + 28]) & D3DTSS_TEXTURETRANSFORMFLAGS (xboxTSS[s*32 + 21])
        float camNx = 0.0f, camNy = 0.0f, camNz = 1.0f;
        float camPx = 0.0f, camPy = 0.0f, camPz = 1.0f;
        bool  camComputed = false;

        bool stageNeeds3D[4] = { false, false, false, false };
        for (uint32_t s = 0; s < 4; ++s) {
            if (g_boundTextures[s]) {
                uint32_t fmt = g_boundTextures[s]->Format;
                bool isCubeTex = (fmt & 0x00000004u) != 0u;
                bool isVolTex  = (((fmt >> 16) & 0x0Fu) > 2u) && (((fmt >> 28) & 0x0Fu) > 0u);
                stageNeeds3D[s] = isCubeTex || isVolTex;
            }
        }

        float finalU[4], finalV[4], finalW[4];
        for (uint32_t s = 0; s < 4; ++s) {
            uint32_t tci = xboxTSS[s * XBOX_D3DTSS_MAX + XBOX_D3DTSS_TEXCOORDINDEX];
            uint32_t ttf = xboxTSS[s * XBOX_D3DTSS_MAX + XBOX_D3DTSS_TEXTURETRANSFORMFLAGS];
            uint32_t srcIdx = tci & 0x03u;
            uint32_t genMode = tci & 0xFFFF0000u;

            float tu = rawU[srcIdx];
            float tv = rawV[srcIdx];
            float tw = has3DUv[srcIdx] ? rawW[srcIdx] : 1.0f;
            bool  is3DCoord = has3DUv[srcIdx];

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
                if ((ttf & kXboxTtffProjectedFlag) != 0u) { // D3DTTFF_PROJECTED (per-pixel division in PSMain)
                    float denom = (dimCount == 3u) ? outZ : outW;
                    tu = outU;
                    tv = outV;
                    tw = outZ;
                    dst.w = (fabsf(denom) > 0.0001f) ? denom : 1.0f;
                    g_lastProjMask |= (1u << s);
                } else if (stageNeeds3D[s]) {
                    tu = outU;
                    tv = outV;
                    tw = outZ;
                } else if (is3DCoord && genMode != 0u) {
                    // Non-projected 3D reflection/normal vector mapped onto a 2D texture:
                    float rLen = sqrtf(outU * outU + outV * outV + outZ * outZ);
                    if (rLen > 0.0001f) {
                        outU /= rLen;
                        outV /= rLen;
                        outZ /= rLen;
                    }
                    tu = outU * 0.5f + 0.5f;
                    tv = 0.5f - outV * 0.5f;
                    tw = outZ;
                } else {
                    tu = outU;
                    tv = outV;
                    tw = outZ;
                }
            } else if (stageNeeds3D[s]) {
                if (!is3DCoord) {
                    tu = tu * 2.0f - 1.0f;
                    tv = 1.0f - tv * 2.0f;
                    tw = 1.0f;
                }
            } else if (is3DCoord && genMode != 0u) {
                tu = tu * 0.5f + 0.5f;
                tv = 0.5f - tv * 0.5f;
            }

            finalU[s] = tu;
            finalV[s] = tv;
            finalW[s] = tw;
        }

        dst.u0 = finalU[0]; dst.v0 = finalV[0]; dst.w0 = finalW[0];
        dst.u1 = finalU[1]; dst.v1 = finalV[1]; dst.w1 = finalW[1];
        dst.u2 = finalU[2]; dst.v2 = finalV[2]; dst.w2 = finalW[2];
        dst.u3 = finalU[3]; dst.v3 = finalV[3]; dst.w3 = finalW[3];
    }
}

static void SubmitUnifiedVerticesD3D11(uint32_t xboxPrim, std::vector<UnifiedVertex>& verts, bool isRhw) {
    if (!g_d3dDevice || !g_d3dCtx || !g_vs || verts.empty()) return;

    const uint32_t* xboxRS  = reinterpret_cast<const uint32_t*>(ADDR_D3D_RENDERSTATE);
    const uint32_t* xboxTSS = reinterpret_cast<const uint32_t*>(ADDR_D3D_TEXTURESTATE);

    D3D11_PRIMITIVE_TOPOLOGY topo = D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
    static std::vector<UnifiedVertex> expanded;
    expanded.clear();
    const UnifiedVertex* drawPtr = verts.data();
    uint32_t drawCount = (uint32_t)verts.size();

    switch (xboxPrim) {
        case 1: { // POINTLIST
            float pointSize = 1.0f, pointMin = 0.0f, pointMax = 64.0f;
            memcpy(&pointSize, &xboxRS[XBOX_D3DRS_POINTSIZE], 4);
            memcpy(&pointMin,  &xboxRS[XBOX_D3DRS_POINTSIZE_MIN], 4);
            memcpy(&pointMax,  &xboxRS[XBOX_D3DRS_POINTSIZE_MAX], 4);
            bool pointSprite = (xboxRS[XBOX_D3DRS_POINTSPRITEENABLE] != 0);
            bool pointScale  = (xboxRS[XBOX_D3DRS_POINTSCALEENABLE] != 0);

            if (pointSize > 1.0f || pointSprite || pointScale) {
                float vpW = (float)(g_viewport.Width  ? g_viewport.Width  : 640);
                float vpH = (float)(g_viewport.Height ? g_viewport.Height : 480);
                float scaleA = 1.0f, scaleB = 0.0f, scaleC = 0.0f;
                memcpy(&scaleA, &xboxRS[XBOX_D3DRS_POINTSCALE_A], 4);
                memcpy(&scaleB, &xboxRS[XBOX_D3DRS_POINTSCALE_B], 4);
                memcpy(&scaleC, &xboxRS[XBOX_D3DRS_POINTSCALE_C], 4);

                Mat44 wv  = MulMat44(g_matWorld, g_matView);
                Mat44 wvp = MulMat44(wv, g_matProj);
                float rx = wv.m[0][0], ry = wv.m[1][0], rz = wv.m[2][0];
                float ux = wv.m[0][1], uy = wv.m[1][1], uz = wv.m[2][1];
                float rLenSq = rx * rx + ry * ry + rz * rz;
                float uLenSq = ux * ux + uy * uy + uz * uz;
                if (rLenSq > 0.0001f) { rx /= rLenSq; ry /= rLenSq; rz /= rLenSq; }
                if (uLenSq > 0.0001f) { ux /= uLenSq; uy /= uLenSq; uz /= uLenSq; }
                float projX = (fabsf(g_matProj.m[0][0]) > 0.0001f) ? fabsf(g_matProj.m[0][0]) : 1.0f;
                float projY = (fabsf(g_matProj.m[1][1]) > 0.0001f) ? fabsf(g_matProj.m[1][1]) : 1.0f;

                expanded.resize(drawCount * 6);
                for (uint32_t p = 0; p < drawCount; ++p) {
                    const UnifiedVertex& v = verts[p];
                    float sz = pointSize;
                    if (!isRhw && pointScale) {
                        float ex = v.x * wv.m[0][0] + v.y * wv.m[1][0] + v.z * wv.m[2][0] + wv.m[3][0];
                        float ey = v.x * wv.m[0][1] + v.y * wv.m[1][1] + v.z * wv.m[2][1] + wv.m[3][1];
                        float ez = v.x * wv.m[0][2] + v.y * wv.m[1][2] + v.z * wv.m[2][2] + wv.m[3][2];
                        float de = sqrtf(ex * ex + ey * ey + ez * ez);
                        float denom = scaleA + scaleB * de + scaleC * de * de;
                        if (denom > 0.0001f) sz = vpH * pointSize * sqrtf(1.0f / denom);
                    }
                    float maxLim = (pointMax > pointMin && pointMax > 0.0f) ? pointMax : 64.0f;
                    if (sz < pointMin) sz = pointMin;
                    if (sz > maxLim)   sz = maxLim;
                    if (sz < 1.0f)     sz = 1.0f;

                    UnifiedVertex c[4] = { v, v, v, v };
                    if (isRhw) {
                        float half = sz * 0.5f;
                        c[0].x = v.x - half; c[0].y = v.y - half;
                        c[1].x = v.x + half; c[1].y = v.y - half;
                        c[2].x = v.x + half; c[2].y = v.y + half;
                        c[3].x = v.x - half; c[3].y = v.y + half;
                    } else {
                        float clipW = v.x * wvp.m[0][3] + v.y * wvp.m[1][3] + v.z * wvp.m[2][3] + wvp.m[3][3];
                        if (clipW < 0.01f) clipW = 0.01f;
                        float dR = (sz / vpW) * clipW / projX;
                        float dU = (sz / vpH) * clipW / projY;
                        c[0].x = v.x - rx * dR + ux * dU; c[0].y = v.y - ry * dR + uy * dU; c[0].z = v.z - rz * dR + uz * dU;
                        c[1].x = v.x + rx * dR + ux * dU; c[1].y = v.y + ry * dR + uy * dU; c[1].z = v.z + rz * dR + uz * dU;
                        c[2].x = v.x + rx * dR - ux * dU; c[2].y = v.y + ry * dR - uy * dU; c[2].z = v.z + rz * dR - uz * dU;
                        c[3].x = v.x - rx * dR - ux * dU; c[3].y = v.y - ry * dR - uy * dU; c[3].z = v.z - rz * dR - uz * dU;
                    }
                    if (pointSprite) {
                        c[0].u0 = 0.0f; c[0].v0 = 0.0f;
                        c[1].u0 = 1.0f; c[1].v0 = 0.0f;
                        c[2].u0 = 1.0f; c[2].v0 = 1.0f;
                        c[3].u0 = 0.0f; c[3].v0 = 1.0f;
                    }
                    expanded[p * 6 + 0] = c[0];
                    expanded[p * 6 + 1] = c[1];
                    expanded[p * 6 + 2] = c[2];
                    expanded[p * 6 + 3] = c[0];
                    expanded[p * 6 + 4] = c[2];
                    expanded[p * 6 + 5] = c[3];
                }
                drawPtr = expanded.data();
                drawCount = (uint32_t)expanded.size();
                topo = D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
            } else {
                topo = D3D11_PRIMITIVE_TOPOLOGY_POINTLIST;
            }
            break;
        }
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

    float uScale[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
    float vScale[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
    bool  isCube[4] = { false, false, false, false };
    bool  isVol[4]  = { false, false, false, false };
    ID3D11ShaderResourceView* stageSrvs[4] = {
        GetOrCreateD3D11SRV(g_boundTextures[0], &uScale[0], &vScale[0], &isCube[0], &isVol[0]),
        GetOrCreateD3D11SRV(g_boundTextures[1], &uScale[1], &vScale[1], &isCube[1], &isVol[1]),
        GetOrCreateD3D11SRV(g_boundTextures[2], &uScale[2], &vScale[2], &isCube[2], &isVol[2]),
        GetOrCreateD3D11SRV(g_boundTextures[3], &uScale[3], &vScale[3], &isCube[3], &isVol[3])
    };
    ID3D11ShaderResourceView* srvs[13] = {};
    for (int s = 0; s < 4; ++s) {
        if (!stageSrvs[s]) continue;
        if (isCube[s])     srvs[4 + s] = stageSrvs[s];
        else if (isVol[s]) srvs[8 + s] = stageSrvs[s];
        else               srvs[0 + s] = stageSrvs[s];
    }
    srvs[12] = g_gammaLutSRV;

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

    const bool     hasProgPs        = (g_currentPsHandle != 0 && g_currentPsType >= 0);
    const uint32_t psTexModes       = xboxRS[XBOX_D3DRS_PSTEXTUREMODES];
    bool           ffCascadeActive  = true;
    uint32_t*      stageOpsPtrs[4]   = { cb.stage0Ops,   cb.stage1Ops,   cb.stage2Ops,   cb.stage3Ops   };
    uint32_t*      stageAlphaPtrs[4] = { cb.stage0Alpha, cb.stage1Alpha, cb.stage2Alpha, cb.stage3Alpha };
    uint32_t       texMask          = 0;
    uint32_t       cubeMask         = 0;
    uint32_t       volMask          = 0;
    uint32_t       projMask         = g_lastProjMask & 0x0Fu;
    uint32_t       alphaKillMask    = 0;
    uint32_t       tempResultMask   = 0;
    for (int s = 0; s < 4; ++s) {
        stageOpsPtrs[s][0]   = xboxTSS[s * XBOX_D3DTSS_MAX + XBOX_D3DTSS_COLOROP];
        stageOpsPtrs[s][1]   = xboxTSS[s * XBOX_D3DTSS_MAX + XBOX_D3DTSS_COLORARG1];
        stageOpsPtrs[s][2]   = xboxTSS[s * XBOX_D3DTSS_MAX + XBOX_D3DTSS_COLORARG2];
        stageOpsPtrs[s][3]   = xboxTSS[s * XBOX_D3DTSS_MAX + XBOX_D3DTSS_COLORARG0];
        stageAlphaPtrs[s][0] = xboxTSS[s * XBOX_D3DTSS_MAX + XBOX_D3DTSS_ALPHAOP];
        stageAlphaPtrs[s][1] = xboxTSS[s * XBOX_D3DTSS_MAX + XBOX_D3DTSS_ALPHAARG1];
        stageAlphaPtrs[s][2] = xboxTSS[s * XBOX_D3DTSS_MAX + XBOX_D3DTSS_ALPHAARG2];
        stageAlphaPtrs[s][3] = xboxTSS[s * XBOX_D3DTSS_MAX + XBOX_D3DTSS_ALPHAARG0];

        if (!hasProgPs && stageOpsPtrs[s][0] <= 1u) {
            ffCascadeActive = false;
        }
        const uint32_t stagePsMode    = (psTexModes >> (s * 5)) & 0x1Fu;
        const bool     stageEnabled   = hasProgPs ? (stagePsMode != 0u) : ffCascadeActive;
        const bool     stageTexActive = (stageSrvs[s] != nullptr) && stageEnabled;
        if (stageTexActive) {
            texMask |= (1u << s);
            if (isCube[s]) cubeMask |= (1u << s);
            if (isVol[s])  volMask  |= (1u << s);
            if (hasProgPs && (stagePsMode == 1u || stagePsMode == 2u)) {
                projMask |= (1u << s);
            }
            if ((xboxTSS[s * XBOX_D3DTSS_MAX + XBOX_D3DTSS_ALPHAKILL] & 0x04u) != 0u) {
                alphaKillMask |= (1u << s);
            }
        }
        uint32_t resArg = xboxTSS[s * XBOX_D3DTSS_MAX + XBOX_D3DTSS_RESULTARG] & 0x0Fu;
        if (resArg == 0x05u || resArg == 0x0Du) { // D3DTA_TEMP
            tempResultMask |= (1u << s);
        }
        cb.stageColorKeyOp[s] = stageTexActive ? (xboxTSS[s * XBOX_D3DTSS_MAX + XBOX_D3DTSS_COLORKEYOP] & 0x03u) : 0u;
        UnpackD3DColor(
            xboxTSS[s * XBOX_D3DTSS_MAX + XBOX_D3DTSS_COLORKEYCOLOR],
            &cb.stageColorKeyColor[s][0], &cb.stageColorKeyColor[s][1],
            &cb.stageColorKeyColor[s][2], &cb.stageColorKeyColor[s][3]);
        cb.stageColorSign[s] = xboxTSS[s * XBOX_D3DTSS_MAX + XBOX_D3DTSS_COLORSIGN];
    }

    bool hwScissor = g_scissorEnabled && !g_scissorExclusive && (g_scissorCount == 1u);
    uint32_t scCount = (g_scissorEnabled && !hwScissor) ? g_scissorCount : 0u;
    if (scCount > 8u) scCount = 8u;
    for (uint32_t si = 0; si < scCount; ++si) {
        cb.scissorRects[si][0] = (float)g_scissorRects[si].left;
        cb.scissorRects[si][1] = (float)g_scissorRects[si].top;
        cb.scissorRects[si][2] = (float)g_scissorRects[si].right;
        cb.scissorRects[si][3] = (float)g_scissorRects[si].bottom;
    }

    cb.flags[0] = (isRhw ? 1u : 0u)
                | (g_hasGammaRamp ? 2u : 0u)
                | ((g_scissorEnabled && g_scissorExclusive) ? 4u : 0u)
                | ((!isRhw && xboxRS[XBOX_D3DRS_SPECULARENABLE] != 0u) ? 8u : 0u)
                | ((scCount & 0x0Fu) << 4);
    cb.flags[1] = texMask
                | (cubeMask << 4)
                | ((projMask & 0x0Fu) << 8)
                | (volMask << 12)
                | (alphaKillMask << 16)
                | (tempResultMask << 20);
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

    // BumpEnv matrix & luminance scale/offset from Stage 1 (or Stage 0 if Stage 1's BumpEnvMat is zero)
    const uint32_t* bumpMat1 = &xboxTSS[1 * XBOX_D3DTSS_MAX + XBOX_D3DTSS_BUMPENVMAT00];
    const uint32_t* bumpMat0 = &xboxTSS[0 * XBOX_D3DTSS_MAX + XBOX_D3DTSS_BUMPENVMAT00];
    bool useStage1Bump = (bumpMat1[0] || bumpMat1[1] || bumpMat1[2] || bumpMat1[3]);
    const uint32_t* chosenBump = useStage1Bump ? bumpMat1 : bumpMat0;
    memcpy(&cb.bumpParams[0], chosenBump, 16);
    if (cb.bumpParams[0] == 0.0f && cb.bumpParams[1] == 0.0f && cb.bumpParams[2] == 0.0f && cb.bumpParams[3] == 0.0f) {
        cb.bumpParams[0] = 0.04f;
        cb.bumpParams[2] = 0.04f;
    }
    uint32_t bumpStage0 = useStage1Bump ? 1u : 0u;
    memcpy(&cb.bumpLuminance[0], &xboxTSS[bumpStage0 * XBOX_D3DTSS_MAX + XBOX_D3DTSS_BUMPENVLSCALE], 4);
    memcpy(&cb.bumpLuminance[1], &xboxTSS[bumpStage0 * XBOX_D3DTSS_MAX + XBOX_D3DTSS_BUMPENVLOFFSET], 4);
    memcpy(&cb.bumpLuminance[2], &xboxTSS[2 * XBOX_D3DTSS_MAX + XBOX_D3DTSS_BUMPENVLSCALE], 4);
    memcpy(&cb.bumpLuminance[3], &xboxTSS[2 * XBOX_D3DTSS_MAX + XBOX_D3DTSS_BUMPENVLOFFSET], 4);

    // Hardware Distance & Atmospheric Fog (xboxRS[92]=FOGENABLE, [93]=FOGTABLEMODE, [94]=FOGSTART, [95]=FOGEND, [96]=FOGDENSITY, [138]=FOGCOLOR)
    if (xboxRS[XBOX_D3DRS_FOGENABLE] != 0 && !isRhw) {
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
    memcpy(cb.psConst4, g_psConstants[4], 16);
    memcpy(cb.psConst5, g_psConstants[5], 16);
    memcpy(cb.psConst6, g_psConstants[6], 16);
    memcpy(cb.psConst7, g_psConstants[7], 16);

    if (g_gpuLightMode != 0u && !isRhw) {
        for (int i = 0; i < 4; ++i) {
            cb.worldRow0[i] = g_matWorld.m[0][i];
            cb.worldRow1[i] = g_matWorld.m[1][i];
            cb.worldRow2[i] = g_matWorld.m[2][i];
            cb.worldRow3[i] = g_matWorld.m[3][i];
        }
        const XboxMaterial8* devFrontMat = reinterpret_cast<const XboxMaterial8*>(ADDR_DEVICE_MATERIAL);
        const XboxMaterial8& frontMat = (devFrontMat->Diffuse[0] != 0.0f || devFrontMat->Diffuse[1] != 0.0f ||
                                         devFrontMat->Diffuse[2] != 0.0f || devFrontMat->Diffuse[3] != 0.0f ||
                                         devFrontMat->Ambient[0] != 0.0f || devFrontMat->Power != 0.0f || !g_hasMaterial)
                                        ? *devFrontMat : g_material;

        float matDiffR = (g_gpuLightMode == 2u) ? frontMat.Diffuse[0]  : 1.0f;
        float matDiffG = (g_gpuLightMode == 2u) ? frontMat.Diffuse[1]  : 1.0f;
        float matDiffB = (g_gpuLightMode == 2u) ? frontMat.Diffuse[2]  : 1.0f;
        float matAmbR  = (g_gpuLightMode == 2u) ? frontMat.Ambient[0]  : 1.0f;
        float matAmbG  = (g_gpuLightMode == 2u) ? frontMat.Ambient[1]  : 1.0f;
        float matAmbB  = (g_gpuLightMode == 2u) ? frontMat.Ambient[2]  : 1.0f;
        float matSpecR = (g_gpuLightMode == 2u) ? frontMat.Specular[0] : 1.0f;
        float matSpecG = (g_gpuLightMode == 2u) ? frontMat.Specular[1] : 1.0f;
        float matSpecB = (g_gpuLightMode == 2u) ? frontMat.Specular[2] : 1.0f;

        const uint8_t* pCam = *reinterpret_cast<const uint8_t* const*>(ADDR_ACTIVE_CAMERA_PTR);
        const float* camPos = pCam ? reinterpret_cast<const float*>(pCam + 0x30) : reinterpret_cast<const float*>(0x008ACC38);
        bool localViewer = (xboxRS[XBOX_D3DRS_LOCALVIEWER] != 0);
        if (localViewer) {
            cb.eyePosWorld[0] = camPos[0];
            cb.eyePosWorld[1] = camPos[1];
            cb.eyePosWorld[2] = camPos[2];
        } else {
            cb.eyePosWorld[0] = -g_matView.m[0][2];
            cb.eyePosWorld[1] = -g_matView.m[1][2];
            cb.eyePosWorld[2] = -g_matView.m[2][2];
        }
        cb.eyePosWorld[3] = (frontMat.Power > 0.0f) ? frontMat.Power : 16.0f;

        uint32_t numGpuLights = 0u;
        for (int l = 0; l < 16 && numGpuLights < 8u; ++l) {
            if (!g_lightEnabled[l]) continue;
            const XboxLight8& lt = g_lights[l];
            if (lt.Type < 1u || lt.Type > 3u) continue;
            uint32_t idx = numGpuLights++;
            if (lt.Type == 3u) { // D3DLIGHT_DIRECTIONAL
                float lx = -lt.Direction[0], ly = -lt.Direction[1], lz = -lt.Direction[2];
                float lLen = sqrtf(lx * lx + ly * ly + lz * lz);
                if (lLen > 0.0001f) { lx /= lLen; ly /= lLen; lz /= lLen; }
                cb.lightPosType[idx][0] = lx;
                cb.lightPosType[idx][1] = ly;
                cb.lightPosType[idx][2] = lz;
                cb.lightPosType[idx][3] = 3.0f;
            } else { // POINT (1) or SPOT (2)
                cb.lightPosType[idx][0]  = lt.Position[0];
                cb.lightPosType[idx][1]  = lt.Position[1];
                cb.lightPosType[idx][2]  = lt.Position[2];
                cb.lightPosType[idx][3]  = (float)lt.Type;
                cb.lightDirRange[idx][3] = lt.Range;
                cb.lightAttenSpot[idx][0] = lt.Attenuation0;
                cb.lightAttenSpot[idx][1] = lt.Attenuation1;
                cb.lightAttenSpot[idx][2] = lt.Attenuation2;
                if (lt.Type == 2u) {
                    float sdx = lt.Direction[0], sdy = lt.Direction[1], sdz = lt.Direction[2];
                    float sdLen = sqrtf(sdx * sdx + sdy * sdy + sdz * sdz);
                    if (sdLen > 0.0001f) { sdx /= sdLen; sdy /= sdLen; sdz /= sdLen; }
                    cb.lightDirRange[idx][0]   = sdx;
                    cb.lightDirRange[idx][1]   = sdy;
                    cb.lightDirRange[idx][2]   = sdz;
                    cb.lightAttenSpot[idx][3]  = cosf(lt.Phi * 0.5f);
                    cb.lightSpotParams[idx][0] = cosf(lt.Theta * 0.5f);
                    cb.lightSpotParams[idx][1] = (lt.Falloff > 0.0f) ? lt.Falloff : 1.0f;
                }
            }
            cb.lightDiffuse[idx][0]  = lt.Diffuse[0]  * matDiffR;
            cb.lightDiffuse[idx][1]  = lt.Diffuse[1]  * matDiffG;
            cb.lightDiffuse[idx][2]  = lt.Diffuse[2]  * matDiffB;
            cb.lightAmbient[idx][0]  = lt.Ambient[0]  * matAmbR;
            cb.lightAmbient[idx][1]  = lt.Ambient[1]  * matAmbG;
            cb.lightAmbient[idx][2]  = lt.Ambient[2]  * matAmbB;
            cb.lightSpecular[idx][0] = lt.Specular[0] * matSpecR;
            cb.lightSpecular[idx][1] = lt.Specular[1] * matSpecG;
            cb.lightSpecular[idx][2] = lt.Specular[2] * matSpecB;
        }
        cb.lightCount[0] = numGpuLights;
        cb.lightCount[1] = (numGpuLights > 0u) ? 1u : 0u;
        cb.lightCount[2] = ((xboxRS[XBOX_D3DRS_SPECULARENABLE] != 0) ? 1u : 0u)
                         | (localViewer ? 2u : 0u)
                         | ((xboxRS[XBOX_D3DRS_TWOSIDEDLIGHTING] != 0) ? 4u : 0u);
    }

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
    // Disable backface culling on RHW 2D UI quads; use Xbox CULLMODE=[147] and FRONTFACE=[146] for 3D
    uint32_t cull = isRhw ? 0 : xboxRS[XBOX_D3DRS_CULLMODE];
    ID3D11RasterizerState* rs = GetRastState(
        cull, xboxRS[XBOX_D3DRS_FILLMODE], xboxRS[XBOX_D3DRS_ZBIAS], hwScissor, xboxRS[XBOX_D3DRS_FRONTFACE]);

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
        GetStageSamplerState(0),
        GetStageSamplerState(1),
        GetStageSamplerState(2),
        GetStageSamplerState(3)
    };

    float blendFactor[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
    if (xboxRS[XBOX_D3DRS_BLENDCOLOR] != 0u ||
        xboxRS[XBOX_D3DRS_SRCBLEND]  >= 0x8001u ||
        xboxRS[XBOX_D3DRS_DESTBLEND] >= 0x8001u ||
        (xboxRS[XBOX_D3DRS_SRCBLEND]  >= 14u && xboxRS[XBOX_D3DRS_SRCBLEND]  <= 17u) ||
        (xboxRS[XBOX_D3DRS_DESTBLEND] >= 14u && xboxRS[XBOX_D3DRS_DESTBLEND] <= 17u))
    {
        UnpackD3DColor(xboxRS[XBOX_D3DRS_BLENDCOLOR], &blendFactor[0], &blendFactor[1], &blendFactor[2], &blendFactor[3]);
        if (xboxRS[XBOX_D3DRS_SRCBLEND]  == 0x8003u || xboxRS[XBOX_D3DRS_SRCBLEND]  == 0x8004u ||
            xboxRS[XBOX_D3DRS_DESTBLEND] == 0x8003u || xboxRS[XBOX_D3DRS_DESTBLEND] == 0x8004u)
        {
            blendFactor[0] = blendFactor[1] = blendFactor[2] = blendFactor[3];
        }
    }
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
    ID3D11PixelShader* activePS = GetPixelShaderPermutation(cb.flags[3]);
    g_d3dCtx->PSSetShader(activePS, nullptr, 0);
    g_d3dCtx->PSSetConstantBuffers(0, 1, &g_cbPerDraw);

    g_d3dCtx->PSSetShaderResources(0, 13, srvs);
    g_d3dCtx->PSSetSamplers(0, 4, smps);

    g_d3dCtx->Draw(drawCount, startVertex);
    if (g_curOffscreenRT == nullptr) {
        g_backBufferDirty = true;
    }
}

static void SubmitXvuDrawD3D11(
    const XboxShaderDeclInfo& decl, uint32_t xboxPrim,
    const uint8_t* stream0Data, uint32_t stream0Stride,
    uint32_t vertexCount, const uint16_t* optIndices, uint32_t baseVertexIndex)
{
    if (!g_d3dDevice || !g_d3dCtx || !decl.gpuVS || !decl.gpuInputLayout || !g_cbVSConstants || !stream0Data || vertexCount == 0) return;

    const uint32_t* xboxRS  = reinterpret_cast<const uint32_t*>(ADDR_D3D_RENDERSTATE);
    const uint32_t* xboxTSS = reinterpret_cast<const uint32_t*>(ADDR_D3D_TEXTURESTATE);

    D3D11_PRIMITIVE_TOPOLOGY topo = D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
    static std::vector<uint16_t> drawIndices;
    drawIndices.clear();
    bool useIB = false;

    uint32_t firstVert = baseVertexIndex;
    uint32_t numVerts  = vertexCount;

    if (optIndices) {
        uint16_t minIdx = 0xFFFFu;
        uint16_t maxIdx = 0u;
        for (uint32_t i = 0; i < vertexCount; ++i) {
            uint16_t idx = optIndices[i];
            if (idx < minIdx) minIdx = idx;
            if (idx > maxIdx) maxIdx = idx;
        }
        if (minIdx > maxIdx) return;
        firstVert = baseVertexIndex + minIdx;
        numVerts  = (uint32_t)(maxIdx - minIdx) + 1u;
        useIB     = true;

        switch (xboxPrim) {
            case 1: // POINTLIST
                topo = D3D11_PRIMITIVE_TOPOLOGY_POINTLIST;
                drawIndices.resize(vertexCount);
                for (uint32_t i = 0; i < vertexCount; ++i) drawIndices[i] = (uint16_t)(optIndices[i] - minIdx);
                break;
            case 2: // LINELIST
                if (vertexCount < 2) return;
                topo = D3D11_PRIMITIVE_TOPOLOGY_LINELIST;
                drawIndices.resize(vertexCount);
                for (uint32_t i = 0; i < vertexCount; ++i) drawIndices[i] = (uint16_t)(optIndices[i] - minIdx);
                break;
            case 3: // LINELOOP
                if (vertexCount < 2) return;
                topo = D3D11_PRIMITIVE_TOPOLOGY_LINESTRIP;
                drawIndices.resize(vertexCount + 1);
                for (uint32_t i = 0; i < vertexCount; ++i) drawIndices[i] = (uint16_t)(optIndices[i] - minIdx);
                drawIndices[vertexCount] = (uint16_t)(optIndices[0] - minIdx);
                break;
            case 4: // LINESTRIP
                if (vertexCount < 2) return;
                topo = D3D11_PRIMITIVE_TOPOLOGY_LINESTRIP;
                drawIndices.resize(vertexCount);
                for (uint32_t i = 0; i < vertexCount; ++i) drawIndices[i] = (uint16_t)(optIndices[i] - minIdx);
                break;
            case 5: // TRIANGLELIST
                if (vertexCount < 3) return;
                topo = D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
                drawIndices.resize(vertexCount);
                for (uint32_t i = 0; i < vertexCount; ++i) drawIndices[i] = (uint16_t)(optIndices[i] - minIdx);
                break;
            case 6: // TRIANGLESTRIP
            case 9: // QUADSTRIP
                if (vertexCount < 3) return;
                topo = D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP;
                drawIndices.resize(vertexCount);
                for (uint32_t i = 0; i < vertexCount; ++i) drawIndices[i] = (uint16_t)(optIndices[i] - minIdx);
                break;
            case 7: // TRIANGLEFAN
            case 10: { // POLYGON
                if (vertexCount < 3) return;
                topo = D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
                uint32_t numTris = vertexCount - 2;
                drawIndices.resize(numTris * 3);
                uint16_t i0 = (uint16_t)(optIndices[0] - minIdx);
                for (uint32_t t = 0; t < numTris; ++t) {
                    drawIndices[t * 3 + 0] = i0;
                    drawIndices[t * 3 + 1] = (uint16_t)(optIndices[t + 1] - minIdx);
                    drawIndices[t * 3 + 2] = (uint16_t)(optIndices[t + 2] - minIdx);
                }
                break;
            }
            case 8: { // QUADLIST
                uint32_t numQuads = vertexCount / 4;
                if (numQuads == 0) return;
                topo = D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
                drawIndices.resize(numQuads * 6);
                for (uint32_t q = 0; q < numQuads; ++q) {
                    uint16_t q0 = (uint16_t)(optIndices[q * 4 + 0] - minIdx);
                    uint16_t q1 = (uint16_t)(optIndices[q * 4 + 1] - minIdx);
                    uint16_t q2 = (uint16_t)(optIndices[q * 4 + 2] - minIdx);
                    uint16_t q3 = (uint16_t)(optIndices[q * 4 + 3] - minIdx);
                    drawIndices[q * 6 + 0] = q0;
                    drawIndices[q * 6 + 1] = q1;
                    drawIndices[q * 6 + 2] = q2;
                    drawIndices[q * 6 + 3] = q0;
                    drawIndices[q * 6 + 4] = q2;
                    drawIndices[q * 6 + 5] = q3;
                }
                break;
            }
            default:
                return;
        }
    } else {
        switch (xboxPrim) {
            case 1: // POINTLIST
                topo = D3D11_PRIMITIVE_TOPOLOGY_POINTLIST;
                break;
            case 2: // LINELIST
                if (numVerts < 2) return;
                topo = D3D11_PRIMITIVE_TOPOLOGY_LINELIST;
                break;
            case 3: // LINELOOP
                if (numVerts < 2) return;
                topo  = D3D11_PRIMITIVE_TOPOLOGY_LINESTRIP;
                useIB = true;
                drawIndices.resize(numVerts + 1);
                for (uint32_t i = 0; i < numVerts; ++i) drawIndices[i] = (uint16_t)i;
                drawIndices[numVerts] = 0;
                break;
            case 4: // LINESTRIP
                if (numVerts < 2) return;
                topo = D3D11_PRIMITIVE_TOPOLOGY_LINESTRIP;
                break;
            case 5: // TRIANGLELIST
                if (numVerts < 3) return;
                topo = D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
                break;
            case 6: // TRIANGLESTRIP
            case 9: // QUADSTRIP
                if (numVerts < 3) return;
                topo = D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP;
                break;
            case 7: // TRIANGLEFAN
            case 10: { // POLYGON
                if (numVerts < 3) return;
                topo  = D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
                useIB = true;
                uint32_t numTris = numVerts - 2;
                drawIndices.resize(numTris * 3);
                for (uint32_t t = 0; t < numTris; ++t) {
                    drawIndices[t * 3 + 0] = 0;
                    drawIndices[t * 3 + 1] = (uint16_t)(t + 1);
                    drawIndices[t * 3 + 2] = (uint16_t)(t + 2);
                }
                break;
            }
            case 8: { // QUADLIST
                uint32_t numQuads = numVerts / 4;
                if (numQuads == 0) return;
                topo  = D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
                useIB = true;
                drawIndices.resize(numQuads * 6);
                for (uint32_t q = 0; q < numQuads; ++q) {
                    drawIndices[q * 6 + 0] = (uint16_t)(q * 4 + 0);
                    drawIndices[q * 6 + 1] = (uint16_t)(q * 4 + 1);
                    drawIndices[q * 6 + 2] = (uint16_t)(q * 4 + 2);
                    drawIndices[q * 6 + 3] = (uint16_t)(q * 4 + 0);
                    drawIndices[q * 6 + 4] = (uint16_t)(q * 4 + 2);
                    drawIndices[q * 6 + 5] = (uint16_t)(q * 4 + 3);
                }
                break;
            }
            default:
                return;
        }
    }

    static ID3D11Buffer* s_dynStreamVB[2] = { nullptr, nullptr };
    static uint32_t      s_dynStreamCap[2] = { 0, 0 };
    static uint32_t      s_dynStreamOff[2] = { 0, 0 };
    UINT strides[2] = { 0, 0 };
    UINT byteOff[2] = { 0, 0 };

    auto uploadStreamSlice = [&](uint32_t slot, const uint8_t* basePtr, uint32_t stride, uint32_t firstV, uint32_t countV) -> bool {
        if (stride == 0 || countV == 0) return false;
        uint32_t bytesNeeded = countV * stride;
        uint32_t alignedBytes = (bytesNeeded + 15u) & ~15u;
        if (!s_dynStreamVB[slot] || alignedBytes > s_dynStreamCap[slot]) {
            if (s_dynStreamVB[slot]) { s_dynStreamVB[slot]->Release(); s_dynStreamVB[slot] = nullptr; }
            s_dynStreamCap[slot] = (alignedBytes > 2 * 1024 * 1024) ? (alignedBytes * 2) : (2 * 1024 * 1024);
            s_dynStreamOff[slot] = 0;
            D3D11_BUFFER_DESC bd = {};
            bd.ByteWidth      = s_dynStreamCap[slot];
            bd.Usage          = D3D11_USAGE_DYNAMIC;
            bd.BindFlags      = D3D11_BIND_VERTEX_BUFFER;
            bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
            if (FAILED(g_d3dDevice->CreateBuffer(&bd, nullptr, &s_dynStreamVB[slot]))) return false;
        }
        D3D11_MAP mapMode = D3D11_MAP_WRITE_NO_OVERWRITE;
        if (s_dynStreamOff[slot] == 0 || (s_dynStreamOff[slot] + alignedBytes) > s_dynStreamCap[slot]) {
            mapMode = D3D11_MAP_WRITE_DISCARD;
            s_dynStreamOff[slot] = 0;
        }
        D3D11_MAPPED_SUBRESOURCE mapped = {};
        if (FAILED(g_d3dCtx->Map(s_dynStreamVB[slot], 0, mapMode, 0, &mapped))) return false;
        uint8_t* dst = reinterpret_cast<uint8_t*>(mapped.pData) + s_dynStreamOff[slot];
        if (basePtr) {
            memcpy(dst, basePtr + firstV * stride, bytesNeeded);
        } else {
            memset(dst, 0, bytesNeeded);
        }
        g_d3dCtx->Unmap(s_dynStreamVB[slot], 0);
        strides[slot] = stride;
        byteOff[slot] = s_dynStreamOff[slot];
        s_dynStreamOff[slot] += alignedBytes;
        return true;
    };

    uint32_t s0 = (stream0Stride != 0) ? stream0Stride : decl.streamStrides[0];
    if (!uploadStreamSlice(0, stream0Data, s0, firstVert, numVerts)) return;

    uint32_t numStreams = 1u;
    if (decl.usesStream[1]) {
        uint32_t s1 = (decl.streamStrides[1] != 0) ? decl.streamStrides[1] : g_streams[1].stride;
        const uint8_t* p1 = (g_streams[1].stride == s1) ? reinterpret_cast<const uint8_t*>(g_streams[1].data) : nullptr;
        if (!uploadStreamSlice(1, p1, s1, firstVert, numVerts)) return;
        numStreams = 2u;
    }

    static ID3D11Buffer* s_dynIB    = nullptr;
    static uint32_t      s_dynIBCap = 0;
    static uint32_t      s_dynIBOff = 0;
    uint32_t startIndex   = 0;
    uint32_t drawIdxCount = (uint32_t)drawIndices.size();
    if (useIB) {
        if (drawIdxCount == 0) return;
        if (!s_dynIB || drawIdxCount > s_dynIBCap) {
            if (s_dynIB) { s_dynIB->Release(); s_dynIB = nullptr; }
            s_dynIBCap = (drawIdxCount > 131072u) ? (drawIdxCount * 2u) : 131072u;
            s_dynIBOff = 0;
            D3D11_BUFFER_DESC ibd = {};
            ibd.ByteWidth      = s_dynIBCap * sizeof(uint16_t);
            ibd.Usage          = D3D11_USAGE_DYNAMIC;
            ibd.BindFlags      = D3D11_BIND_INDEX_BUFFER;
            ibd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
            if (FAILED(g_d3dDevice->CreateBuffer(&ibd, nullptr, &s_dynIB))) return;
        }
        D3D11_MAP ibMapMode = D3D11_MAP_WRITE_NO_OVERWRITE;
        if (s_dynIBOff == 0 || (s_dynIBOff + drawIdxCount) > s_dynIBCap) {
            ibMapMode  = D3D11_MAP_WRITE_DISCARD;
            s_dynIBOff = 0;
        }
        D3D11_MAPPED_SUBRESOURCE mappedIB = {};
        if (FAILED(g_d3dCtx->Map(s_dynIB, 0, ibMapMode, 0, &mappedIB))) return;
        memcpy(reinterpret_cast<uint16_t*>(mappedIB.pData) + s_dynIBOff, drawIndices.data(), drawIdxCount * sizeof(uint16_t));
        g_d3dCtx->Unmap(s_dynIB, 0);
        startIndex = s_dynIBOff;
        s_dynIBOff += drawIdxCount;
    }

    // Update Xbox NV2A hardware viewport constants c[58] (scale) and c[59] (offset)
    float vpX    = (float)g_viewport.X;
    float vpY    = (float)g_viewport.Y;
    float vpW    = (float)(g_viewport.Width  ? g_viewport.Width  : 640);
    float vpH    = (float)(g_viewport.Height ? g_viewport.Height : 480);
    float vpMinZ = g_viewport.MinZ;
    float vpMaxZ = (g_viewport.MaxZ > g_viewport.MinZ) ? g_viewport.MaxZ : 1.0f;
    float* vsConsts = reinterpret_cast<float*>(ADDR_D3D_VS_CONSTANTS);
    vsConsts[58 * 4 + 0] =  vpW * 0.5f;
    vsConsts[58 * 4 + 1] = -vpH * 0.5f;
    vsConsts[58 * 4 + 2] =  vpMaxZ - vpMinZ;
    vsConsts[58 * 4 + 3] =  1.0f;
    vsConsts[59 * 4 + 0] =  vpX + vpW * 0.5f;
    vsConsts[59 * 4 + 1] =  vpY + vpH * 0.5f;
    vsConsts[59 * 4 + 2] =  vpMinZ;
    vsConsts[59 * 4 + 3] =  0.0f;

    D3D11_MAPPED_SUBRESOURCE mappedVS = {};
    if (SUCCEEDED(g_d3dCtx->Map(g_cbVSConstants, 0, D3D11_MAP_WRITE_DISCARD, 0, &mappedVS))) {
        memcpy(mappedVS.pData, reinterpret_cast<const void*>(ADDR_D3D_VS_CONSTANTS), 192 * 16);
        g_d3dCtx->Unmap(g_cbVSConstants, 0);
    }

    float uScale[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
    float vScale[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
    bool  isCube[4] = { false, false, false, false };
    bool  isVol[4]  = { false, false, false, false };
    ID3D11ShaderResourceView* stageSrvs[4] = {
        GetOrCreateD3D11SRV(g_boundTextures[0], &uScale[0], &vScale[0], &isCube[0], &isVol[0]),
        GetOrCreateD3D11SRV(g_boundTextures[1], &uScale[1], &vScale[1], &isCube[1], &isVol[1]),
        GetOrCreateD3D11SRV(g_boundTextures[2], &uScale[2], &vScale[2], &isCube[2], &isVol[2]),
        GetOrCreateD3D11SRV(g_boundTextures[3], &uScale[3], &vScale[3], &isCube[3], &isVol[3])
    };
    ID3D11ShaderResourceView* srvs[13] = {};
    for (int s = 0; s < 4; ++s) {
        if (!stageSrvs[s]) continue;
        if (isCube[s])     srvs[4 + s] = stageSrvs[s];
        else if (isVol[s]) srvs[8 + s] = stageSrvs[s];
        else               srvs[0 + s] = stageSrvs[s];
    }
    srvs[12] = g_gammaLutSRV;

    PerDrawCBData cb = {};
    Mat44 wvp = MulMat44(MulMat44(g_matWorld, g_matView), g_matProj);
    for (int i = 0; i < 4; ++i) {
        cb.wvpRow0[i] = wvp.m[0][i];
        cb.wvpRow1[i] = wvp.m[1][i];
        cb.wvpRow2[i] = wvp.m[2][i];
        cb.wvpRow3[i] = wvp.m[3][i];
    }
    cb.viewport[0] = vpX;
    cb.viewport[1] = vpY;
    cb.viewport[2] = vpW;
    cb.viewport[3] = vpH;

    UnpackD3DColor(xboxRS[XBOX_D3DRS_TEXTUREFACTOR], &cb.tfactor[0], &cb.tfactor[1], &cb.tfactor[2], &cb.tfactor[3]);

    const bool     hasProgPs        = (g_currentPsHandle != 0 && g_currentPsType >= 0);
    const uint32_t psTexModes       = xboxRS[XBOX_D3DRS_PSTEXTUREMODES];
    bool           ffCascadeActive  = true;
    uint32_t*      stageOpsPtrs[4]   = { cb.stage0Ops,   cb.stage1Ops,   cb.stage2Ops,   cb.stage3Ops   };
    uint32_t*      stageAlphaPtrs[4] = { cb.stage0Alpha, cb.stage1Alpha, cb.stage2Alpha, cb.stage3Alpha };
    uint32_t       texMask          = 0;
    uint32_t       cubeMask         = 0;
    uint32_t       volMask          = 0;
    uint32_t       projMask         = 0;
    uint32_t       alphaKillMask    = 0;
    uint32_t       tempResultMask   = 0;
    for (int s = 0; s < 4; ++s) {
        stageOpsPtrs[s][0]   = xboxTSS[s * XBOX_D3DTSS_MAX + XBOX_D3DTSS_COLOROP];
        stageOpsPtrs[s][1]   = xboxTSS[s * XBOX_D3DTSS_MAX + XBOX_D3DTSS_COLORARG1];
        stageOpsPtrs[s][2]   = xboxTSS[s * XBOX_D3DTSS_MAX + XBOX_D3DTSS_COLORARG2];
        stageOpsPtrs[s][3]   = xboxTSS[s * XBOX_D3DTSS_MAX + XBOX_D3DTSS_COLORARG0];
        stageAlphaPtrs[s][0] = xboxTSS[s * XBOX_D3DTSS_MAX + XBOX_D3DTSS_ALPHAOP];
        stageAlphaPtrs[s][1] = xboxTSS[s * XBOX_D3DTSS_MAX + XBOX_D3DTSS_ALPHAARG1];
        stageAlphaPtrs[s][2] = xboxTSS[s * XBOX_D3DTSS_MAX + XBOX_D3DTSS_ALPHAARG2];
        stageAlphaPtrs[s][3] = xboxTSS[s * XBOX_D3DTSS_MAX + XBOX_D3DTSS_ALPHAARG0];

        if (!hasProgPs && stageOpsPtrs[s][0] <= 1u) {
            ffCascadeActive = false;
        }
        const uint32_t stagePsMode    = (psTexModes >> (s * 5)) & 0x1Fu;
        const bool     stageEnabled   = hasProgPs ? (stagePsMode != 0u) : ffCascadeActive;
        const bool     stageTexActive = (stageSrvs[s] != nullptr) && stageEnabled;
        if (stageTexActive) {
            texMask |= (1u << s);
            if (isCube[s]) cubeMask |= (1u << s);
            if (isVol[s])  volMask  |= (1u << s);
            const bool isStageProj = hasProgPs
                ? (stagePsMode == 1u || stagePsMode == 2u)
                : ((xboxTSS[s * XBOX_D3DTSS_MAX + XBOX_D3DTSS_TEXTURETRANSFORMFLAGS] & 0x0100u) != 0u);
            if (isStageProj) {
                projMask |= (1u << s);
            }
            if ((xboxTSS[s * XBOX_D3DTSS_MAX + XBOX_D3DTSS_ALPHAKILL] & 0x04u) != 0u) {
                alphaKillMask |= (1u << s);
            }
        }
        uint32_t resArg = xboxTSS[s * XBOX_D3DTSS_MAX + XBOX_D3DTSS_RESULTARG] & 0x0Fu;
        if (resArg == 0x05u || resArg == 0x0Du) { // D3DTA_TEMP
            tempResultMask |= (1u << s);
        }
        cb.stageColorKeyOp[s] = stageTexActive ? (xboxTSS[s * XBOX_D3DTSS_MAX + XBOX_D3DTSS_COLORKEYOP] & 0x03u) : 0u;
        UnpackD3DColor(
            xboxTSS[s * XBOX_D3DTSS_MAX + XBOX_D3DTSS_COLORKEYCOLOR],
            &cb.stageColorKeyColor[s][0], &cb.stageColorKeyColor[s][1],
            &cb.stageColorKeyColor[s][2], &cb.stageColorKeyColor[s][3]);
        cb.stageColorSign[s] = xboxTSS[s * XBOX_D3DTSS_MAX + XBOX_D3DTSS_COLORSIGN];
    }

    bool hwScissor = g_scissorEnabled && !g_scissorExclusive && (g_scissorCount == 1u);
    uint32_t scCount = (g_scissorEnabled && !hwScissor) ? g_scissorCount : 0u;
    if (scCount > 8u) scCount = 8u;
    for (uint32_t si = 0; si < scCount; ++si) {
        cb.scissorRects[si][0] = (float)g_scissorRects[si].left;
        cb.scissorRects[si][1] = (float)g_scissorRects[si].top;
        cb.scissorRects[si][2] = (float)g_scissorRects[si].right;
        cb.scissorRects[si][3] = (float)g_scissorRects[si].bottom;
    }

    cb.flags[0] = (decl.hasRhw ? 1u : 0u)
                | (g_hasGammaRamp ? 2u : 0u)
                | ((g_scissorEnabled && g_scissorExclusive) ? 4u : 0u)
                | ((!decl.hasRhw && xboxRS[XBOX_D3DRS_SPECULARENABLE] != 0u) ? 8u : 0u)
                | ((scCount & 0x0Fu) << 4);
    cb.flags[1] = texMask
                | (cubeMask << 4)
                | ((projMask & 0x0Fu) << 8)
                | (volMask << 12)
                | (alphaKillMask << 16)
                | (tempResultMask << 20);
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

    const uint32_t* bumpMat1 = &xboxTSS[1 * XBOX_D3DTSS_MAX + XBOX_D3DTSS_BUMPENVMAT00];
    const uint32_t* bumpMat0 = &xboxTSS[0 * XBOX_D3DTSS_MAX + XBOX_D3DTSS_BUMPENVMAT00];
    bool useStage1Bump = (bumpMat1[0] || bumpMat1[1] || bumpMat1[2] || bumpMat1[3]);
    const uint32_t* chosenBump = useStage1Bump ? bumpMat1 : bumpMat0;
    memcpy(&cb.bumpParams[0], chosenBump, 16);
    if (cb.bumpParams[0] == 0.0f && cb.bumpParams[1] == 0.0f && cb.bumpParams[2] == 0.0f && cb.bumpParams[3] == 0.0f) {
        cb.bumpParams[0] = 0.04f;
        cb.bumpParams[2] = 0.04f;
    }
    uint32_t bumpStage0 = useStage1Bump ? 1u : 0u;
    memcpy(&cb.bumpLuminance[0], &xboxTSS[bumpStage0 * XBOX_D3DTSS_MAX + XBOX_D3DTSS_BUMPENVLSCALE], 4);
    memcpy(&cb.bumpLuminance[1], &xboxTSS[bumpStage0 * XBOX_D3DTSS_MAX + XBOX_D3DTSS_BUMPENVLOFFSET], 4);
    memcpy(&cb.bumpLuminance[2], &xboxTSS[2 * XBOX_D3DTSS_MAX + XBOX_D3DTSS_BUMPENVLSCALE], 4);
    memcpy(&cb.bumpLuminance[3], &xboxTSS[2 * XBOX_D3DTSS_MAX + XBOX_D3DTSS_BUMPENVLOFFSET], 4);

    if (xboxRS[XBOX_D3DRS_FOGENABLE] != 0 && !decl.hasRhw) {
        uint32_t fogMode = xboxRS[XBOX_D3DRS_FOGTABLEMODE];
        if (fogMode == 0) {
            fogMode = decl.writesOFog ? 4u : 3u;
        }
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
    memcpy(cb.psConst4, g_psConstants[4], 16);
    memcpy(cb.psConst5, g_psConstants[5], 16);
    memcpy(cb.psConst6, g_psConstants[6], 16);
    memcpy(cb.psConst7, g_psConstants[7], 16);

    D3D11_MAPPED_SUBRESOURCE mappedCB = {};
    if (SUCCEEDED(g_d3dCtx->Map(g_cbPerDraw, 0, D3D11_MAP_WRITE_DISCARD, 0, &mappedCB))) {
        memcpy(mappedCB.pData, &cb, sizeof(cb));
        g_d3dCtx->Unmap(g_cbPerDraw, 0);
    }

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

    ID3D11BlendState* bs = GetBlendState(
        xboxRS[XBOX_D3DRS_ALPHABLENDENABLE], xboxRS[XBOX_D3DRS_SRCBLEND],
        xboxRS[XBOX_D3DRS_DESTBLEND], xboxRS[XBOX_D3DRS_BLENDOP], cwMask);
    ID3D11DepthStencilState* ds = GetDepthState(
        xboxRS[XBOX_D3DRS_ZENABLE], xboxRS[XBOX_D3DRS_ZWRITEENABLE], xboxRS[XBOX_D3DRS_ZFUNC],
        xboxRS[XBOX_D3DRS_STENCILENABLE], xboxRS[XBOX_D3DRS_STENCILFAIL],
        xboxRS[XBOX_D3DRS_STENCILZFAIL], xboxRS[XBOX_D3DRS_STENCILPASS],
        xboxRS[XBOX_D3DRS_STENCILFUNC], xboxRS[XBOX_D3DRS_STENCILMASK], xboxRS[XBOX_D3DRS_STENCILWRITEMASK]);
    uint32_t cull = decl.hasRhw ? 0 : xboxRS[XBOX_D3DRS_CULLMODE];
    ID3D11RasterizerState* rs = GetRastState(
        cull, xboxRS[XBOX_D3DRS_FILLMODE], xboxRS[XBOX_D3DRS_ZBIAS], hwScissor, xboxRS[XBOX_D3DRS_FRONTFACE]);

    D3D11_VIEWPORT vp11 = {};
    vp11.TopLeftX = vpX;
    vp11.TopLeftY = vpY;
    vp11.Width    = vpW;
    vp11.Height   = vpH;
    vp11.MinDepth = decl.hasRhw ? 0.0f : vpMinZ;
    vp11.MaxDepth = decl.hasRhw ? 1.0f : vpMaxZ;
    g_d3dCtx->RSSetViewports(1, &vp11);

    ID3D11SamplerState* smps[4] = {
        GetStageSamplerState(0),
        GetStageSamplerState(1),
        GetStageSamplerState(2),
        GetStageSamplerState(3)
    };

    float blendFactor[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
    if (xboxRS[XBOX_D3DRS_BLENDCOLOR] != 0u ||
        xboxRS[XBOX_D3DRS_SRCBLEND]  >= 0x8001u ||
        xboxRS[XBOX_D3DRS_DESTBLEND] >= 0x8001u ||
        (xboxRS[XBOX_D3DRS_SRCBLEND]  >= 14u && xboxRS[XBOX_D3DRS_SRCBLEND]  <= 17u) ||
        (xboxRS[XBOX_D3DRS_DESTBLEND] >= 14u && xboxRS[XBOX_D3DRS_DESTBLEND] <= 17u))
    {
        UnpackD3DColor(xboxRS[XBOX_D3DRS_BLENDCOLOR], &blendFactor[0], &blendFactor[1], &blendFactor[2], &blendFactor[3]);
        if (xboxRS[XBOX_D3DRS_SRCBLEND]  == 0x8003u || xboxRS[XBOX_D3DRS_SRCBLEND]  == 0x8004u ||
            xboxRS[XBOX_D3DRS_DESTBLEND] == 0x8003u || xboxRS[XBOX_D3DRS_DESTBLEND] == 0x8004u)
        {
            blendFactor[0] = blendFactor[1] = blendFactor[2] = blendFactor[3];
        }
    }
    g_d3dCtx->OMSetBlendState(bs, blendFactor, 0xFFFFFFFFu);
    g_d3dCtx->OMSetDepthStencilState(ds, xboxRS[XBOX_D3DRS_STENCILREF] & 0xFFu);
    g_d3dCtx->RSSetState(rs);

    g_d3dCtx->IASetInputLayout(decl.gpuInputLayout);
    g_d3dCtx->IASetVertexBuffers(0, numStreams, s_dynStreamVB, strides, byteOff);
    g_d3dCtx->IASetPrimitiveTopology(topo);

    ID3D11Buffer* cbVBs[2] = { g_cbPerDraw, g_cbVSConstants };
    g_d3dCtx->VSSetShader(decl.gpuVS, nullptr, 0);
    g_d3dCtx->VSSetConstantBuffers(0, 2, cbVBs);
    ID3D11PixelShader* activePS = GetPixelShaderPermutation(cb.flags[3]);
    g_d3dCtx->PSSetShader(activePS, nullptr, 0);
    g_d3dCtx->PSSetConstantBuffers(0, 1, &g_cbPerDraw);

    g_d3dCtx->PSSetShaderResources(0, 13, srvs);
    g_d3dCtx->PSSetSamplers(0, 4, smps);

    if (useIB) {
        g_d3dCtx->IASetIndexBuffer(s_dynIB, DXGI_FORMAT_R16_UINT, 0);
        g_d3dCtx->DrawIndexed(drawIdxCount, startIndex, 0);
    } else {
        g_d3dCtx->Draw(numVerts, 0);
    }

    if (g_curOffscreenRT == nullptr) {
        g_backBufferDirty = true;
    }
}

static void __stdcall Bridge_D3DDevice_DrawVerticesUP(
    DWORD PrimitiveType, UINT VertexCount, const void* pVertexStreamZeroData, UINT VertexStreamZeroStride)
{
    g_curPrelitUseCustomBase = false;
    if (!pVertexStreamZeroData || VertexCount == 0 || VertexStreamZeroStride == 0) return;

    XboxShaderDeclInfo decl = {};
    if (GetXboxVertexShaderDecl(g_currentVsHandle, &decl) && decl.isCompiledXvu && decl.gpuVS && decl.gpuInputLayout) {
        SubmitXvuDrawD3D11(
            decl, PrimitiveType,
            reinterpret_cast<const uint8_t*>(pVertexStreamZeroData),
            VertexStreamZeroStride, VertexCount, nullptr, 0);
        return;
    }

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
    if (!g_streams[0].data || g_streams[0].stride == 0 || VertexCount == 0) return;

    XboxShaderDeclInfo decl = {};
    if (GetXboxVertexShaderDecl(g_currentVsHandle, &decl) && decl.isCompiledXvu && decl.gpuVS && decl.gpuInputLayout) {
        SubmitXvuDrawD3D11(
            decl, PrimitiveType,
            reinterpret_cast<const uint8_t*>(g_streams[0].data),
            g_streams[0].stride, VertexCount, nullptr, StartVertex);
        g_curPrelitUseCustomBase = false;
        return;
    }

    static std::vector<UnifiedVertex> verts;
    verts.clear();
    bool isRhw = false;
    DecodeVerticesToUnified(
        reinterpret_cast<const uint8_t*>(g_streams[0].data),
        g_streams[0].stride, VertexCount, nullptr, StartVertex, verts, &isRhw);
    g_curPrelitUseCustomBase = false;
    SubmitUnifiedVerticesD3D11(PrimitiveType, verts, isRhw);
}

static void __stdcall Bridge_D3DDevice_DrawIndexedVertices(
    DWORD PrimitiveType, UINT VertexCount, const uint16_t* pIndexData)
{
    if (!g_streams[0].data || g_streams[0].stride == 0 || !pIndexData || VertexCount == 0) return;

    XboxShaderDeclInfo decl = {};
    bool hasDecl = GetXboxVertexShaderDecl(g_currentVsHandle, &decl);
    if (hasDecl && decl.isCompiledXvu && decl.gpuVS && decl.gpuInputLayout) {
        SubmitXvuDrawD3D11(
            decl, PrimitiveType,
            reinterpret_cast<const uint8_t*>(g_streams[0].data),
            g_streams[0].stride, VertexCount, pIndexData, g_baseVertexIndex);
        g_curPrelitUseCustomBase = false;
        return;
    }

    static std::vector<UnifiedVertex> verts;
    verts.clear();
    bool isRhw = false;
    DecodeVerticesToUnified(
        reinterpret_cast<const uint8_t*>(g_streams[0].data),
        g_streams[0].stride, VertexCount, pIndexData, g_baseVertexIndex, verts, &isRhw);
    g_curPrelitUseCustomBase = false;
    SubmitUnifiedVerticesD3D11(PrimitiveType, verts, isRhw);
}

// _D3DDevice_RunPushBuffer@8 (0x006BACD0) - executes terrain & world pushbuffers with optional pFixup
static void __stdcall Bridge_D3DDevice_RunPushBuffer(XboxPushBuffer* pPushBuffer, void* pFixup) {
    if (!pPushBuffer || !pPushBuffer->Data || pPushBuffer->Size < 8) return;

    const uint8_t* pbBytes = reinterpret_cast<const uint8_t*>(pPushBuffer->Data);
    uint32_t       pbSize  = pPushBuffer->Size;

    // Apply XboxPushBufferFixup patches (_D3DPushBuffer_Fixup@12 at 0x006BAC30)
    static std::vector<uint8_t> patchedPb;
    if (pFixup != nullptr) {
        const XboxPushBufferFixup* fixup = reinterpret_cast<const XboxPushBufferFixup*>(pFixup);
        if (fixup->Data != 0u && fixup->Run < fixup->Size) {
            patchedPb.assign(pbBytes, pbBytes + pbSize);
            const uint8_t* fBase = reinterpret_cast<const uint8_t*>(fixup->Data);
            const uint8_t* fPtr  = fBase + fixup->Run;
            const uint8_t* fEnd  = fBase + fixup->Size;
            while (fPtr + 8 <= fEnd) {
                uint32_t patchBytes = *reinterpret_cast<const uint32_t*>(fPtr + 0);
                if (patchBytes == 0xFFFFFFFFu) break;
                uint32_t pbOffset   = *reinterpret_cast<const uint32_t*>(fPtr + 4);
                fPtr += 8;
                if (fPtr + patchBytes > fEnd) break;
                if (pbOffset + patchBytes <= patchedPb.size()) {
                    memcpy(patchedPb.data() + pbOffset, fPtr, patchBytes);
                }
                fPtr += patchBytes;
            }
            pbBytes = patchedPb.data();
        }
    }

    const uint32_t* p    = reinterpret_cast<const uint32_t*>(pbBytes);
    const uint32_t* pEnd = p + (pbSize / 4);

    uint32_t curPrim      = 0;
    uint32_t vsConstLoad  = 0;
    uint32_t* xboxRS      = reinterpret_cast<uint32_t*>(ADDR_D3D_RENDERSTATE);
    const uint32_t* rsEnc = reinterpret_cast<const uint32_t*>(ADDR_SIMPLE_RS_ENCODE);
    static std::vector<uint16_t> curIndices;
    static std::vector<uint32_t> curInlineDwords;
    curIndices.clear();
    curInlineDwords.clear();

    while (p < pEnd) {
        uint32_t hdr = *p;
        if (hdr == 0u) {
            ++p;
            continue;
        }

        uint32_t hdrType = hdr & 0xE0030003u;
        if (hdrType != 0x00000000u && hdrType != 0x40000000u) {
            break; // End of pushbuffer or jump command
        }

        bool     nonInc     = (hdrType == 0x40000000u);
        uint32_t dwordCount = (hdr >> 18) & 0x7FFu;
        uint32_t baseMethod = hdr & 0x1FFFCu;
        if (p + 1 + dwordCount > pEnd) break;

        if (baseMethod == 0x1800u && nonInc) { // NV097_ARRAY_ELEMENT16
            const uint16_t* idxPtr = reinterpret_cast<const uint16_t*>(p + 1);
            curIndices.insert(curIndices.end(), idxPtr, idxPtr + dwordCount * 2);
            p += 1 + dwordCount;
            continue;
        }
        if (baseMethod == 0x1808u && nonInc) { // NV097_ARRAY_ELEMENT32
            for (uint32_t i = 0; i < dwordCount; ++i) {
                curIndices.push_back((uint16_t)(p[1 + i] & 0xFFFFu));
            }
            p += 1 + dwordCount;
            continue;
        }
        if (baseMethod == 0x1818u && nonInc) { // NV097_INLINE_ARRAY
            curInlineDwords.insert(curInlineDwords.end(), p + 1, p + 1 + dwordCount);
            p += 1 + dwordCount;
            continue;
        }

        for (uint32_t i = 0; i < dwordCount; ++i) {
            uint32_t m   = nonInc ? baseMethod : (baseMethod + i * 4u);
            uint32_t val = p[1 + i];

            if (m == 0x17FCu) { // NV097_SET_BEGIN_END
                if (val == 0u) {
                    if (curPrim != 0u) {
                        if (!curIndices.empty()) {
                            Bridge_D3DDevice_DrawIndexedVertices(curPrim, (UINT)curIndices.size(), curIndices.data());
                        } else if (!curInlineDwords.empty() && g_streams[0].stride > 0) {
                            uint32_t vCount = ((uint32_t)curInlineDwords.size() * 4u) / g_streams[0].stride;
                            if (vCount > 0) {
                                Bridge_D3DDevice_DrawVerticesUP(curPrim, vCount, curInlineDwords.data(), g_streams[0].stride);
                            }
                        }
                    }
                    curIndices.clear();
                    curInlineDwords.clear();
                    curPrim = 0u;
                } else {
                    curPrim = val;
                    curIndices.clear();
                    curInlineDwords.clear();
                }
            } else if (m == 0x1800u) { // NV097_ARRAY_ELEMENT16 (Type 0 single dword)
                curIndices.push_back((uint16_t)(val & 0xFFFFu));
                curIndices.push_back((uint16_t)((val >> 16) & 0xFFFFu));
            } else if (m == 0x1808u) { // NV097_ARRAY_ELEMENT32 (Type 0 single dword)
                curIndices.push_back((uint16_t)(val & 0xFFFFu));
            } else if (m == 0x1810u) { // NV097_DRAW_ARRAYS
                uint32_t startV = val & 0xFFFFFFu;
                uint32_t countV = ((val >> 24) & 0xFFu) + 1u;
                for (uint32_t v = 0; v < countV; ++v) {
                    curIndices.push_back((uint16_t)(startV + v));
                }
            } else if (m == 0x1818u) { // NV097_INLINE_ARRAY (Type 0)
                curInlineDwords.push_back(val);
            } else if (m >= 0x1720u && m < 0x1760u) { // NV097_SET_VERTEX_DATA_ARRAY_OFFSET(0..15)
                uint32_t slot = (m - 0x1720u) >> 2;
                if (slot < 16u && val != 0u) {
                    g_streams[slot].data = reinterpret_cast<const void*>(val);
                }
            } else if (m >= 0x1760u && m < 0x17A0u) { // NV097_SET_VERTEX_DATA_ARRAY_FORMAT(0..15)
                uint32_t slot   = (m - 0x1760u) >> 2;
                uint32_t stride = (val >> 8) & 0xFFu;
                if (slot < 16u && stride != 0u) {
                    g_streams[slot].stride = stride;
                }
            } else if (m == 0x1E94u) { // NV097_SET_TRANSFORM_CONSTANT_LOAD
                vsConstLoad = val;
            } else if (m >= 0x0B80u && m < 0x0C00u) { // NV097_SET_TRANSFORM_CONSTANT
                uint32_t compIdx = ((m - 0x0B80u) >> 2);
                uint32_t regIdx  = vsConstLoad + (compIdx >> 2);
                uint32_t chanIdx = compIdx & 3u;
                if (regIdx < 192u) {
                    reinterpret_cast<uint32_t*>(ADDR_D3D_VS_CONSTANTS)[regIdx * 4u + chanIdx] = val;
                }
                if (chanIdx == 3u && compIdx == 31u) {
                    vsConstLoad += 8u;
                }
            } else {
                // Match against Xbox _D3D__SimpleRenderStateEncode[0..91] (0x007502A0)
                uint32_t encodedHdr = 0x00040000u | m;
                for (uint32_t rs = 0; rs < 92u; ++rs) {
                    if (rsEnc[rs] == encodedHdr) {
                        xboxRS[rs] = val;
                        break;
                    }
                }
            }
        }
        p += 1 + dwordCount;
    }

    if (curPrim != 0u && !curIndices.empty()) {
        Bridge_D3DDevice_DrawIndexedVertices(curPrim, (UINT)curIndices.size(), curIndices.data());
        curIndices.clear();
    }
}

// ============================================================================
// Immediate Mode: D3DDevice_Begin / SetVertexData* / D3DDevice_End
// ============================================================================

static constexpr int      kXboxVsdeAutoCommit        = -1;
static constexpr int      kXboxVsdePosition          = 0;  // D3DVSDE_POSITION
static constexpr int      kXboxVsdeNormal            = 2;  // D3DVSDE_NORMAL
static constexpr int      kXboxVsdeDiffuse           = 3;  // D3DVSDE_DIFFUSE
static constexpr int      kXboxVsdeSpecular          = 4;  // D3DVSDE_SPECULAR
static constexpr int      kXboxVsdeTexCoord0         = 9;  // D3DVSDE_TEXCOORD0
static constexpr int      kXboxVsdeTexCoord1         = 10; // D3DVSDE_TEXCOORD1
static constexpr int      kXboxVsdeTexCoord2         = 11; // D3DVSDE_TEXCOORD2
static constexpr int      kXboxVsdeTexCoord3         = 12; // D3DVSDE_TEXCOORD3
static constexpr uint32_t kXboxTexStageCount         = 4u;
static constexpr uint32_t kXboxTssStride             = XBOX_D3DTSS_MAX;
static constexpr uint32_t kXboxTssTtfOffset          = XBOX_D3DTSS_TEXTURETRANSFORMFLAGS;
static constexpr uint32_t kXboxTssTciOffset          = XBOX_D3DTSS_TEXCOORDINDEX;
static constexpr uint32_t kXboxTtffProjected         = 0x100u;
static constexpr uint32_t kXboxDeviceTexMatrixOffset = 0xCE0u; // CDevice+0xC60 + 2*64 (D3DTS_TEXTURE0..3)

static uint32_t                   g_immPrimType = 0;
static UnifiedVertex              g_immCur      = { 0, 0, 0, 1.0f, 0, 0, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1.0f };
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
        g_immCur.w0 = 0.0f;
        if (!g_immHasUv1) { g_immCur.u1 = x; g_immCur.v1 = y; g_immCur.w1 = 0.0f; }
        if (!g_immHasUv2) { g_immCur.u2 = x; g_immCur.v2 = y; g_immCur.w2 = 0.0f; }
        if (!g_immHasUv3) { g_immCur.u3 = x; g_immCur.v3 = y; g_immCur.w3 = 0.0f; }
    } else if (Register == kXboxVsdeTexCoord1) {
        g_immCur.u1 = x;
        g_immCur.v1 = y;
        g_immCur.w1 = 0.0f;
        g_immHasUv1 = true;
    } else if (Register == kXboxVsdeTexCoord2) {
        g_immCur.u2 = x;
        g_immCur.v2 = y;
        g_immCur.w2 = 0.0f;
        g_immHasUv2 = true;
    } else if (Register == kXboxVsdeTexCoord3) {
        g_immCur.u3 = x;
        g_immCur.v3 = y;
        g_immCur.w3 = 0.0f;
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
    } else if (Register == kXboxVsdeDiffuse) {
        g_immCur.r = x;
        g_immCur.g = y;
        g_immCur.b = z;
        g_immCur.a = w;
    } else if (Register == kXboxVsdeSpecular) {
        g_immCur.sr = x;
        g_immCur.sg = y;
        g_immCur.sb = z;
        g_immCur.sa = w;
    } else if (Register == kXboxVsdeTexCoord0) {
        g_immCur.u0 = x;
        g_immCur.v0 = y;
        g_immCur.w0 = z;
        if (!g_immHasUv1) { g_immCur.u1 = x; g_immCur.v1 = y; g_immCur.w1 = z; }
        if (!g_immHasUv2) { g_immCur.u2 = x; g_immCur.v2 = y; g_immCur.w2 = z; }
        if (!g_immHasUv3) { g_immCur.u3 = x; g_immCur.v3 = y; g_immCur.w3 = z; }
    } else if (Register == kXboxVsdeTexCoord1) {
        g_immCur.u1 = x;
        g_immCur.v1 = y;
        g_immCur.w1 = z;
        g_immHasUv1 = true;
    } else if (Register == kXboxVsdeTexCoord2) {
        g_immCur.u2 = x;
        g_immCur.v2 = y;
        g_immCur.w2 = z;
        g_immHasUv2 = true;
    } else if (Register == kXboxVsdeTexCoord3) {
        g_immCur.u3 = x;
        g_immCur.v3 = y;
        g_immCur.w3 = z;
        g_immHasUv3 = true;
    }
}

static void __stdcall Bridge_D3DDevice_SetVertexData4ub(int Register, uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    if (Register == kXboxVsdeDiffuse || Register == kXboxVsdePosition) {
        g_immCur.r = r * (1.0f / 255.0f);
        g_immCur.g = g * (1.0f / 255.0f);
        g_immCur.b = b * (1.0f / 255.0f);
        g_immCur.a = a * (1.0f / 255.0f);
    } else if (Register == kXboxVsdeSpecular) {
        g_immCur.sr = r * (1.0f / 255.0f);
        g_immCur.sg = g * (1.0f / 255.0f);
        g_immCur.sb = b * (1.0f / 255.0f);
        g_immCur.sa = a * (1.0f / 255.0f);
    }
}

static void __stdcall Bridge_D3DDevice_SetVertexDataColor(int Register, DWORD Color) {
    if (Register == kXboxVsdeDiffuse || Register == kXboxVsdePosition) {
        UnpackD3DColor(Color, &g_immCur.r, &g_immCur.g, &g_immCur.b, &g_immCur.a);
    } else if (Register == kXboxVsdeSpecular) {
        UnpackD3DColor(Color, &g_immCur.sr, &g_immCur.sg, &g_immCur.sb, &g_immCur.sa);
    }
}

static void __stdcall Bridge_D3DDevice_End() {
    if (g_immVerts.empty()) return;
    g_lastProjMask = 0;
    g_gpuLightMode = 0;
    bool isRhw = ((g_currentVsHandle & 0x00010001u) == 0) && ((g_currentVsHandle & 0x004u) != 0);

    // Route Stage 0..3 UVs according to D3DTSS_TEXCOORDINDEX and D3DTSS_TEXTURETRANSFORMFLAGS
    const uint32_t* xboxTSS = reinterpret_cast<const uint32_t*>(ADDR_D3D_TEXTURESTATE);
    const Mat44*    texMats = reinterpret_cast<const Mat44*>(ADDR_D3D_DEVICE + kXboxDeviceTexMatrixOffset);
    for (size_t vi = 0; vi < g_immVerts.size(); ++vi) {
        UnifiedVertex& v = g_immVerts[vi];
        float rawU[kXboxTexStageCount] = { v.u0, v.u1, v.u2, v.u3 };
        float rawV[kXboxTexStageCount] = { v.v0, v.v1, v.v2, v.v3 };
        float rawW[kXboxTexStageCount] = { v.w0, v.w1, v.w2, v.w3 };
        float finalU[kXboxTexStageCount];
        float finalV[kXboxTexStageCount];
        float finalW[kXboxTexStageCount];

        for (uint32_t s = 0; s < kXboxTexStageCount; ++s) {
            uint32_t tci    = xboxTSS[s * kXboxTssStride + kXboxTssTciOffset];
            uint32_t ttf    = xboxTSS[s * kXboxTssStride + kXboxTssTtfOffset];
            uint32_t srcIdx = tci & 0x03u;
            float tu = rawU[srcIdx];
            float tv = rawV[srcIdx];
            float tw = rawW[srcIdx];

            uint32_t dimCount = ttf & 0xFFu;
            if (!isRhw && dimCount > 0 && dimCount <= 4) {
                const Mat44& tm = texMats[s];
                float inZ  = (dimCount >= 3) ? tw : 1.0f;
                float outU = tu * tm.m[0][0] + tv * tm.m[1][0] + inZ * tm.m[2][0] + tm.m[3][0];
                float outV = tu * tm.m[0][1] + tv * tm.m[1][1] + inZ * tm.m[2][1] + tm.m[3][1];
                float outZ = tu * tm.m[0][2] + tv * tm.m[1][2] + inZ * tm.m[2][2] + tm.m[3][2];
                float outW = tu * tm.m[0][3] + tv * tm.m[1][3] + inZ * tm.m[2][3] + tm.m[3][3];
                if (ttf & kXboxTtffProjected) {
                    float denom = (dimCount == 3) ? outZ : outW;
                    v.w = (fabsf(denom) > 0.0001f) ? denom : 1.0f;
                    g_lastProjMask |= (1u << s);
                }
                tu = outU;
                tv = outV;
                tw = outZ;
            }
            finalU[s] = tu;
            finalV[s] = tv;
            finalW[s] = tw;
        }
        v.u0 = finalU[0]; v.v0 = finalV[0]; v.w0 = finalW[0];
        v.u1 = finalU[1]; v.v1 = finalV[1]; v.w1 = finalW[1];
        v.u2 = finalU[2]; v.v2 = finalV[2]; v.w2 = finalW[2];
        v.u3 = finalU[3]; v.v3 = finalV[3]; v.w3 = finalW[3];
    }

    SubmitUnifiedVerticesD3D11(g_immPrimType, g_immVerts, isRhw);
    g_immVerts.clear();
}

void InitXboxD3D8DrawBridges() {
    // 1. Bridge _D3DDevice_RunPushBuffer@8 (0x006BACD0) to execute recorded terrain/world pushbuffers
    InstallBridge(0x006BACD0, &Bridge_D3DDevice_RunPushBuffer);      // _D3DDevice_RunPushBuffer@8

    // 2. Bridge Transforms, Viewport, Material, Lights, Scissors
    InstallBridge(0x006B03E0, &Bridge_D3DDevice_SetTransform);       // _D3DDevice_SetTransform@8
    InstallBridge(0x006B12B0, &Bridge_D3DDevice_SetViewport);        // _D3DDevice_SetViewport@4
    InstallBridge(0x006B0500, &Bridge_D3DDevice_SetMaterial);        // _D3DDevice_SetMaterial@4
    InstallBridge(0x006B14E0, &Bridge_D3DDevice_SetLight);           // _D3DDevice_SetLight@8
    InstallBridge(0x006B17A0, &Bridge_D3DDevice_LightEnable);        // _D3DDevice_LightEnable@8
    InstallBridge(0x006B1D50, &Bridge_D3DDevice_SetScissors);        // _D3DDevice_SetScissors@12

    // 3. Bridge Streams, Indices & Draw Calls
    InstallBridge(0x006B2B60, &Bridge_D3DDevice_SetStreamSource);    // _D3DDevice_SetStreamSource@12
    InstallBridge(0x006B1AB0, &Bridge_D3DDevice_SetIndices);         // _D3DDevice_SetIndices@8
    InstallBridge(0x006B3620, &Bridge_D3DDevice_DrawVerticesUP);     // _D3DDevice_DrawVerticesUP@16
    InstallBridge(0x006B3790, &Bridge_D3DDevice_DrawVertices);       // _D3DDevice_DrawVertices@12
    InstallBridge(0x006B3830, &Bridge_D3DDevice_DrawIndexedVertices);// _D3DDevice_DrawIndexedVertices@12

    // 4. Bridge Immediate Mode (Begin / SetVertexData* / End)
    InstallBridge(0x006B3BD0, &Bridge_D3DDevice_Begin);              // _D3DDevice_Begin@4
    InstallBridge(0x006B3A90, &Bridge_D3DDevice_SetVertexData2f);    // _D3DDevice_SetVertexData2f@12
    InstallBridge(0x006B3AD0, &Bridge_D3DDevice_SetVertexData4f);    // _D3DDevice_SetVertexData4f@20
    InstallBridge(0x006B3B30, &Bridge_D3DDevice_SetVertexData4ub);   // _D3DDevice_SetVertexData4ub@20
    InstallBridge(0x006B3B80, &Bridge_D3DDevice_SetVertexDataColor); // _D3DDevice_SetVertexDataColor@8
    InstallBridge(0x006B3C10, &Bridge_D3DDevice_End);                // _D3DDevice_End@0
}
