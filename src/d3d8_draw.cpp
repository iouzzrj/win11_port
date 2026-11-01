#include "ghost_runtime.h"
#include "ghost_addresses.h"
#include "xbox_d3d8_types.h"
#include <d3d11.h>
#include <math.h>
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
        if (StreamNumber == 1) {
            g_curTangentBasis = (pStreamData && pStreamData->Data && Stride == 0x24)
                ? reinterpret_cast<const float*>(pStreamData->Data)
                : nullptr;
            g_curTangentBaseVertex = 0;
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

void UnpackD3DColor(uint32_t c, float* r, float* g, float* b, float* a) {
    *a = ((c >> 24) & 0xFFu) * (1.0f / 255.0f);
    *r = ((c >> 16) & 0xFFu) * (1.0f / 255.0f);
    *g = ((c >> 8)  & 0xFFu) * (1.0f / 255.0f);
    *b = (c         & 0xFFu) * (1.0f / 255.0f);
}

static uint32_t g_lastProjMask = 0;
static uint32_t g_gpuLightMode = 0; // 0 = off, 1 = defaultWorldLit (unit material), 2 = g_material lighting

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
    const float*    vsC126  = reinterpret_cast<const float*>(ADDR_D3D_VS_CONSTANTS + 126 * 16);

    // Xbox D3DRS_LIGHTING is index 102 (0x006C0788); AMBIENTMATERIALSOURCE is 111 (0x006C07AC);
    // DIFFUSEMATERIALSOURCE is 113 (0x006C07B4); D3DRS_AMBIENT is 115 (0x006C07BC);
    // cRenderState::m_lightingMode is at 0x008AA460 (0=unlit, 1=dyn+prelit, 2=prelit, 3=prelit+dyn, 4=pure-dyn)
    uint32_t lightingMode = *reinterpret_cast<const uint32_t*>(ADDR_LIGHTING_MODE);
    bool lightingEnabled  = (xboxRS[102] != 0);
    bool pureDynamicMode  = (lightingMode == 4) || (lightingEnabled && xboxRS[111] == 0 && xboxRS[113] == 0); // SetLighting(4)
    bool prelitOnlyMode   = (lightingMode == 2 && g_curPrelitColors != nullptr);
    float ambR = 0.0f, ambG = 0.0f, ambB = 0.0f, ambA = 1.0f;
    UnpackD3DColor(xboxRS[115], &ambR, &ambG, &ambB, &ambA);
    if (ambA <= 0.001f) ambA = 1.0f;

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

    const bool isCustom3DVS = !decl.hasRhw && (
        g_activeCustomVS != 0 ||
        (decl.vsType >= 0x0Bu && decl.vsType <= 0x32u) ||
        (decl.vsType >= 0x38u && decl.vsType <= 0x41u));

    const bool isCustomNormalOverride = !decl.hasRhw && (
        ((g_activeCustomVS == kCustomVsBumpEnv || (decl.vsType >= 0x13 && decl.vsType <= 0x16)) && g_currentPsHandle != 0 && g_currentPsType == 6) ||
        ((g_activeCustomVS == kCustomVsBump || (decl.vsType >= 0x10 && decl.vsType <= 0x12)) && g_activeCustomVS != kCustomVsBumpEmboss) ||
        (g_activeCustomVS == kCustomVsSpecular || (decl.vsType >= 0x3Fu && decl.vsType <= 0x41u)));

    const bool offloadLightsToGpu = (numActiveLights > 0 && !isCustomNormalOverride);
    const bool needCpuWorldTransform = !decl.hasRhw && (isCustom3DVS || (numActiveLights > 0 && !offloadLightsToGpu));

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

        auto evalDynamicLights = [&](float matDiffR, float matDiffG, float matDiffB, float matAmbR, float matAmbG, float matAmbB, float& totR, float& totG, float& totB) {
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
                    evalDynamicLights(1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, totR, totG, totB);
                    dst.r = totR < 0.0f ? 0.0f : (totR > 1.0f ? 1.0f : totR);
                    dst.g = totG < 0.0f ? 0.0f : (totG > 1.0f ? 1.0f : totG);
                    dst.b = totB < 0.0f ? 0.0f : (totB > 1.0f ? 1.0f : totB);
                }
            }
        } else if (decl.hasNorm && !decl.hasRhw) {
            if (lightingMode != 0 && (lightingEnabled || g_curPrelitColors != nullptr)) {
                float matDiffR = g_hasMaterial ? g_material.Diffuse[0] : 1.0f;
                float matDiffG = g_hasMaterial ? g_material.Diffuse[1] : 1.0f;
                float matDiffB = g_hasMaterial ? g_material.Diffuse[2] : 1.0f;
                float matAmbR  = g_hasMaterial ? g_material.Ambient[0] : 1.0f;
                float matAmbG  = g_hasMaterial ? g_material.Ambient[1] : 1.0f;
                float matAmbB  = g_hasMaterial ? g_material.Ambient[2] : 1.0f;

                float totR = (!prelitOnlyMode && lightingEnabled) ? (ambR * matAmbR + (g_hasMaterial ? g_material.Emissive[0] : 0.0f)) : (g_hasMaterial ? g_material.Emissive[0] : 0.0f);
                float totG = (!prelitOnlyMode && lightingEnabled) ? (ambG * matAmbG + (g_hasMaterial ? g_material.Emissive[1] : 0.0f)) : (g_hasMaterial ? g_material.Emissive[1] : 0.0f);
                float totB = (!prelitOnlyMode && lightingEnabled) ? (ambB * matAmbB + (g_hasMaterial ? g_material.Emissive[2] : 0.0f)) : (g_hasMaterial ? g_material.Emissive[2] : 0.0f);

                if (!pureDynamicMode && g_curPrelitColors != nullptr) {
                    uint32_t preIdx = g_curPrelitBaseVertex + (optIndices ? (uint32_t)optIndices[i] : i);
                    float preR = 0.0f, preG = 0.0f, preB = 0.0f, preA = 1.0f;
                    UnpackD3DColor(g_curPrelitColors[preIdx], &preR, &preG, &preB, &preA);
                    totR += preR;
                    totG += preG;
                    totB += preB;
                }

                if (!prelitOnlyMode && lightingEnabled && offloadLightsToGpu) {
                    g_gpuLightMode = 2u;
                    dst.r = totR;
                    dst.g = totG;
                    dst.b = totB;
                } else {
                    if (!prelitOnlyMode && lightingEnabled) {
                        evalDynamicLights(matDiffR, matDiffG, matDiffB, matAmbR, matAmbG, matAmbB, totR, totG, totB);
                    }
                    dst.r = totR < 0.0f ? 0.0f : (totR > 1.0f ? 1.0f : totR);
                    dst.g = totG < 0.0f ? 0.0f : (totG > 1.0f ? 1.0f : totG);
                    dst.b = totB < 0.0f ? 0.0f : (totB > 1.0f ? 1.0f : totB);
                }
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

        // Custom 3D Programmable Vertex Shaders (.xvu 0x0C..0x32, 0x3D..0x41 and specialized c*Shader::VertexSetup bridges)
        if (!decl.hasRhw && (g_activeCustomVS != 0 ||
            (decl.vsType >= 0x0Cu && decl.vsType <= 0x32u) ||
            (decl.vsType >= 0x3Du && decl.vsType <= 0x41u)))
        {
            const uint8_t* pCam = *reinterpret_cast<const uint8_t* const*>(ADDR_ACTIVE_CAMERA_PTR);
            const float* camPos = pCam ? reinterpret_cast<const float*>(pCam + 0x30) : reinterpret_cast<const float*>(0x008ACC38);
            float vx = camPos[0] - wpx, vy = camPos[1] - wpy, vz = camPos[2] - wpz;
            float vLen = sqrtf(vx * vx + vy * vy + vz * vz);
            if (vLen > 0.0001f) { vx /= vLen; vy /= vLen; vz /= vLen; }
            float ndotv = vx * wx + vy * wy + vz * wz;
            if (ndotv < 0.0f) ndotv = 0.0f; else if (ndotv > 1.0f) ndotv = 1.0f;

            auto computeWorldTangentBasis = [&](float& twx, float& twy, float& twz, float& bwx, float& bwy, float& bwz) {
                if (g_curTangentBasis != nullptr) {
                    uint32_t tbIdx = g_curTangentBaseVertex + (optIndices ? (uint32_t)optIndices[i] : i);
                    const float* tb = g_curTangentBasis + tbIdx * 9;
                    twx = tb[0] * g_matWorld.m[0][0] + tb[1] * g_matWorld.m[1][0] + tb[2] * g_matWorld.m[2][0];
                    twy = tb[0] * g_matWorld.m[0][1] + tb[1] * g_matWorld.m[1][1] + tb[2] * g_matWorld.m[2][1];
                    twz = tb[0] * g_matWorld.m[0][2] + tb[1] * g_matWorld.m[1][2] + tb[2] * g_matWorld.m[2][2];
                    float tLen = sqrtf(twx * twx + twy * twy + twz * twz);
                    if (tLen > 0.0001f) { twx /= tLen; twy /= tLen; twz /= tLen; }

                    bwx = tb[3] * g_matWorld.m[0][0] + tb[4] * g_matWorld.m[1][0] + tb[5] * g_matWorld.m[2][0];
                    bwy = tb[3] * g_matWorld.m[0][1] + tb[4] * g_matWorld.m[1][1] + tb[5] * g_matWorld.m[2][1];
                    bwz = tb[3] * g_matWorld.m[0][2] + tb[4] * g_matWorld.m[1][2] + tb[5] * g_matWorld.m[2][2];
                    float bLen = sqrtf(bwx * bwx + bwy * bwy + bwz * bwz);
                    if (bLen > 0.0001f) { bwx /= bLen; bwy /= bLen; bwz /= bLen; }
                } else {
                    float upX = (fabsf(wy) < 0.99f) ? 0.0f : 1.0f;
                    float upY = (fabsf(wy) < 0.99f) ? 1.0f : 0.0f;
                    float upZ = 0.0f;
                    twx = upY * wz - upZ * wy;
                    twy = upZ * wx - upX * wz;
                    twz = upX * wy - upY * wx;
                    float tLen = sqrtf(twx * twx + twy * twy + twz * twz);
                    if (tLen > 0.0001f) { twx /= tLen; twy /= tLen; twz /= tLen; }
                    bwx = wy * twz - wz * twy;
                    bwy = wz * twx - wx * twz;
                    bwz = wx * twy - wy * twx;
                }
            };

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
            } else if (g_activeCustomVS == kCustomVsBumpEnv || (decl.vsType >= 0x13 && decl.vsType <= 0x16)) { // cBumpEnvShader (BumpReflection*.xvu)
                if (g_currentPsHandle != 0 && g_currentPsType == 6) { // bumpreflect.xpu (index 6 in aPixelShaders: texm3x3pad / texm3x3vspec)
                    float twx = 1.0f, twy = 0.0f, twz = 0.0f;
                    float bwx = 0.0f, bwy = 1.0f, bwz = 0.0f;
                    computeWorldTangentBasis(twx, twy, twz, bwx, bwy, bwz);
                    dst.u0 = rawU[0]; dst.v0 = rawV[0];
                    dst.u1 = twx;     dst.v1 = twy;
                    dst.u2 = twz;     dst.v2 = bwx;
                    dst.u3 = bwy;     dst.v3 = bwz;
                    dst.nx = vx;      dst.ny = vy;      dst.nz = vz;
                } else {
                    float rx = 2.0f * ndotv * wx - vx;
                    float ry = 2.0f * ndotv * wy - vy;
                    dst.u0 = rawU[0];          dst.v0 = rawV[0];
                    dst.u1 = rx * 0.5f + 0.5f; dst.v1 = 0.5f - ry * 0.5f;
                    dst.u2 = rawU[0];          dst.v2 = rawV[0];
                    dst.u3 = rx * 0.5f + 0.5f; dst.v3 = 0.5f - ry * 0.5f;
                }
                continue;
            } else if (g_activeCustomVS == kCustomVsBump || g_activeCustomVS == kCustomVsBumpEmboss ||
                       (decl.vsType >= 0x10 && decl.vsType <= 0x12)) { // cBumpShader / cBumpEmbossShader (BumpDiffuse*.xvu)
                float twx = 1.0f, twy = 0.0f, twz = 0.0f;
                float bwx = 0.0f, bwy = 1.0f, bwz = 0.0f;
                computeWorldTangentBasis(twx, twy, twz, bwx, bwy, bwz);

                float lx = 0.57735f, ly = 0.57735f, lz = 0.57735f;
                if (numActiveLights > 0) {
                    const ActiveLightPrecomp& al = activeLights[0];
                    const XboxLight8& lt = *al.lt;
                    if (lt.Type == 3) {
                        lx = al.dirX; ly = al.dirY; lz = al.dirZ;
                    } else {
                        lx = lt.Position[0] - wpx; ly = lt.Position[1] - wpy; lz = lt.Position[2] - wpz;
                        float lLen = sqrtf(lx * lx + ly * ly + lz * lz);
                        if (lLen > 0.0001f) { lx /= lLen; ly /= lLen; lz /= lLen; }
                    }
                }
                float ltX = lx * twx + ly * twy + lz * twz;
                float ltY = lx * bwx + ly * bwy + lz * bwz;
                float ltZ = lx * wx  + ly * wy  + lz * wz;

                if (g_activeCustomVS == kCustomVsBumpEmboss) {
                    dst.u0 = rawU[0];                        dst.v0 = rawV[0];
                    dst.u1 = rawU[0] + ltX * g_embossScale;  dst.v1 = rawV[0] + ltY * g_embossScale;
                    dst.u2 = rawU[0];                        dst.v2 = rawV[0];
                    dst.u3 = rawU[0];                        dst.v3 = rawV[0];
                } else {
                    // BumpRigid0..3.xvu outputs biased tangent-space light vector in oD1 (SPECULAR = dst.nx..nz)
                    // while preserving vertex lighting in oD0 (DIFFUSE = dst.r..b) for SetMacroMode(0x39).
                    float biasedX = ltX * 0.5f + 0.5f;
                    float biasedY = ltY * 0.5f + 0.5f;
                    float biasedZ = ltZ * 0.5f + 0.5f;
                    dst.nx = biasedX;
                    dst.ny = biasedY;
                    dst.nz = biasedZ;
                    for (int s = 0; s < 4; ++s) {
                        uint32_t cop = xboxTSS[s * XBOX_D3DTSS_MAX + XBOX_D3DTSS_COLOROP];
                        uint32_t ca2 = xboxTSS[s * XBOX_D3DTSS_MAX + XBOX_D3DTSS_COLORARG2] & 0x0Fu;
                        if (cop == 25u && ca2 == 0u) {
                            dst.r = biasedX;
                            dst.g = biasedY;
                            dst.b = biasedZ;
                            break;
                        }
                    }
                    dst.u0 = rawU[0]; dst.v0 = rawV[0];
                    dst.u1 = rawU[0]; dst.v1 = rawV[0];
                    dst.u2 = rawU[0]; dst.v2 = rawV[0];
                    dst.u3 = rawU[0]; dst.v3 = rawV[0];
                }
                continue;
            } else if (g_activeCustomVS == kCustomVsSpecular || (decl.vsType >= 0x3Fu && decl.vsType <= 0x41u)) { // cSpecularShader (specular*.xvu)
                // specularRigid1.xvu computes H = normalize(L + V), spec = pow(max(dot(N, H), 0), c[131].w) * lightColor,
                // writes spec to oD1 (SPECULAR = dst.nx..nz), and writes base UV v3 to oT0, oT1, oT2 for SetMacroMode(0x3A).
                float specExp = (g_specularScale >= 1.0f) ? g_specularScale : 16.0f;
                float specScale = (g_specularScale > 0.0f && g_specularScale < 1.0f) ? g_specularScale : 1.0f;
                float specR = 0.0f, specG = 0.0f, specB = 0.0f;
                for (int alIdx = 0; alIdx < numActiveLights; ++alIdx) {
                    const ActiveLightPrecomp& al = activeLights[alIdx];
                    const XboxLight8& lt = *al.lt;
                    float lx = 0.0f, ly = 0.0f, lz = 0.0f;
                    if (lt.Type == 3) {
                        lx = al.dirX; ly = al.dirY; lz = al.dirZ;
                    } else {
                        lx = lt.Position[0] - wpx; ly = lt.Position[1] - wpy; lz = lt.Position[2] - wpz;
                        float lLen = sqrtf(lx * lx + ly * ly + lz * lz);
                        if (lLen > 0.0001f) { lx /= lLen; ly /= lLen; lz /= lLen; }
                    }
                    float hx = lx + vx, hy = ly + vy, hz = lz + vz;
                    float hLen = sqrtf(hx * hx + hy * hy + hz * hz);
                    if (hLen > 0.0001f) { hx /= hLen; hy /= hLen; hz /= hLen; }
                    float ndoth = wx * hx + wy * hy + wz * hz;
                    if (ndoth > 0.0f) {
                        float sp = powf(ndoth, specExp) * specScale;
                        specR += sp * lt.Diffuse[0];
                        specG += sp * lt.Diffuse[1];
                        specB += sp * lt.Diffuse[2];
                    }
                }
                dst.nx = (specR > 1.0f) ? 1.0f : specR;
                dst.ny = (specG > 1.0f) ? 1.0f : specG;
                dst.nz = (specB > 1.0f) ? 1.0f : specB;
                dst.u0 = rawU[0]; dst.v0 = rawV[0];
                dst.u1 = rawU[0]; dst.v1 = rawV[0];
                dst.u2 = rawU[0]; dst.v2 = rawV[0];
                dst.u3 = rawU[0]; dst.v3 = rawV[0];
                continue;
            } else if (g_activeCustomVS == kCustomVsGlossEnvmapDetail ||
                       (decl.vsType >= 0x0Cu && decl.vsType <= 0x0Fu)) { // cGlossEnvmapDetailShader (EnvmapSkinned*.xvu, SetMacroMode(0x41))
                // Stage 0 = envmap (reflection UV), Stage 1 = base diffuse+gloss (rawU[0]), Stage 2 = detail map (rawU[0] * scale)
                float rx = 2.0f * ndotv * wx - vx;
                float ry = 2.0f * ndotv * wy - vy;
                dst.u0 = rx * 0.5f + 0.5f;               dst.v0 = 0.5f - ry * 0.5f;
                dst.u1 = rawU[0];                        dst.v1 = rawV[0];
                dst.u2 = rawU[0] * g_detailUvScale[0];   dst.v2 = rawV[0] * g_detailUvScale[1];
                dst.u3 = rawU[0];                        dst.v3 = rawV[0];
                continue;
            } else if (g_activeCustomVS == kCustomVsLightProject ||
                       (decl.vsType >= 0x3Du && decl.vsType <= 0x3Eu)) { // cLightProjectShader (ProjLight*.xvu)
                // ProjLightSkinned0.xvu transforms world position r6 = (wpx, wpy, wpz, 1):
                // oT0.xy = dp4(r6, c[126..127]) (non-projective distance ramp),
                // oT1.xy = dp4(r6, c[122..123]), q = dp4(r6, c[124]) (per-pixel projective light cookie).
                float pu = 0.0f, pv = 0.0f, pz = 1.0f;
                if (g_activeCustomVS == kCustomVsLightProject) {
                    const Mat44& m0 = g_lightProjMat0;
                    const Mat44& m1 = g_lightProjMat1;
                    dst.u0 = wpx * m0.m[0][0] + wpy * m0.m[1][0] + wpz * m0.m[2][0] + m0.m[3][0];
                    dst.v0 = wpx * m0.m[0][1] + wpy * m0.m[1][1] + wpz * m0.m[2][1] + m0.m[3][1];
                    pu = wpx * m1.m[0][0] + wpy * m1.m[1][0] + wpz * m1.m[2][0] + m1.m[3][0];
                    pv = wpx * m1.m[0][1] + wpy * m1.m[1][1] + wpz * m1.m[2][1] + m1.m[3][1];
                    pz = wpx * m1.m[0][2] + wpy * m1.m[1][2] + wpz * m1.m[2][2] + m1.m[3][2];
                    if (fabsf(pz) <= 0.0001f) {
                        pz = wpx * m1.m[0][3] + wpy * m1.m[1][3] + wpz * m1.m[2][3] + m1.m[3][3];
                    }
                } else {
                    const float* vsC122 = reinterpret_cast<const float*>(ADDR_D3D_VS_CONSTANTS + 122 * 16);
                    dst.u0 = wpx * vsC126[0 * 4 + 0] + wpy * vsC126[0 * 4 + 1] + wpz * vsC126[0 * 4 + 2] + vsC126[0 * 4 + 3];
                    dst.v0 = wpx * vsC126[1 * 4 + 0] + wpy * vsC126[1 * 4 + 1] + wpz * vsC126[1 * 4 + 2] + vsC126[1 * 4 + 3];
                    pu = wpx * vsC122[0 * 4 + 0] + wpy * vsC122[0 * 4 + 1] + wpz * vsC122[0 * 4 + 2] + vsC122[0 * 4 + 3];
                    pv = wpx * vsC122[1 * 4 + 0] + wpy * vsC122[1 * 4 + 1] + wpz * vsC122[1 * 4 + 2] + vsC122[1 * 4 + 3];
                    pz = wpx * vsC122[2 * 4 + 0] + wpy * vsC122[2 * 4 + 1] + wpz * vsC122[2 * 4 + 2] + vsC122[2 * 4 + 3];
                }
                dst.u1 = pu;
                dst.v1 = pv;
                dst.w  = (fabsf(pz) > 0.0001f) ? pz : 0.0001f;
                g_lastProjMask |= (1u << 1);
                dst.u2 = rawU[0]; dst.v2 = rawV[0];
                dst.u3 = rawU[0]; dst.v3 = rawV[0];
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
                if ((ttf & kXboxTtffProjectedFlag) != 0u) { // D3DTTFF_PROJECTED (per-pixel division in PSMain)
                    float denom = (dimCount == 3u) ? outZ : outW;
                    tu = outU;
                    tv = outV;
                    dst.w = (fabsf(denom) > 0.0001f) ? denom : 1.0f;
                    g_lastProjMask |= (1u << s);
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
                const uint8_t* pCam = *reinterpret_cast<const uint8_t* const*>(ADDR_ACTIVE_CAMERA_PTR);
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

static void SubmitUnifiedVerticesD3D11(uint32_t xboxPrim, std::vector<UnifiedVertex>& verts, bool isRhw) {
    if (!g_d3dDevice || !g_d3dCtx || !g_vs || !g_ps || verts.empty()) return;

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
    ID3D11ShaderResourceView* srvs[4] = {
        GetOrCreateD3D11SRV(g_boundTextures[0], &uScale[0], &vScale[0], &isCube[0]),
        GetOrCreateD3D11SRV(g_boundTextures[1], &uScale[1], &vScale[1], &isCube[1]),
        GetOrCreateD3D11SRV(g_boundTextures[2], &uScale[2], &vScale[2], &isCube[2]),
        GetOrCreateD3D11SRV(g_boundTextures[3], &uScale[3], &vScale[3], &isCube[3])
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

    uint32_t texMask  = 0;
    uint32_t cubeMask = 0;
    for (int s = 0; s < 4; ++s) {
        if (srvs[s]) {
            texMask |= (1u << s);
            if (isCube[s]) cubeMask |= (1u << s);
        }
    }

    cb.flags[0] = isRhw ? 1u : 0u;
    cb.flags[1] = texMask | (cubeMask << 4) | ((g_lastProjMask & 0x0Fu) << 8);
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
        float matDiffR = (g_gpuLightMode == 2u && g_hasMaterial) ? g_material.Diffuse[0] : 1.0f;
        float matDiffG = (g_gpuLightMode == 2u && g_hasMaterial) ? g_material.Diffuse[1] : 1.0f;
        float matDiffB = (g_gpuLightMode == 2u && g_hasMaterial) ? g_material.Diffuse[2] : 1.0f;
        float matAmbR  = (g_gpuLightMode == 2u && g_hasMaterial) ? g_material.Ambient[0] : 1.0f;
        float matAmbG  = (g_gpuLightMode == 2u && g_hasMaterial) ? g_material.Ambient[1] : 1.0f;
        float matAmbB  = (g_gpuLightMode == 2u && g_hasMaterial) ? g_material.Ambient[2] : 1.0f;

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
            cb.lightDiffuse[idx][0] = lt.Diffuse[0] * matDiffR;
            cb.lightDiffuse[idx][1] = lt.Diffuse[1] * matDiffG;
            cb.lightDiffuse[idx][2] = lt.Diffuse[2] * matDiffB;
            cb.lightAmbient[idx][0] = lt.Ambient[0] * matAmbR;
            cb.lightAmbient[idx][1] = lt.Ambient[1] * matAmbG;
            cb.lightAmbient[idx][2] = lt.Ambient[2] * matAmbB;
        }
        cb.lightCount[0] = numGpuLights;
        cb.lightCount[1] = (numGpuLights > 0u) ? 1u : 0u;
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
    ID3D11PixelShader* activePS = GetPixelShaderPermutation(cb.flags[3]);
    g_d3dCtx->PSSetShader(activePS ? activePS : g_ps, nullptr, 0);
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
    if (!g_streams[0].data || g_streams[0].stride == 0 || VertexCount == 0) return;
    static std::vector<UnifiedVertex> verts;
    verts.clear();
    bool isRhw = false;
    DecodeVerticesToUnified(
        reinterpret_cast<const uint8_t*>(g_streams[0].data),
        g_streams[0].stride, VertexCount, nullptr, StartVertex, verts, &isRhw);
    SubmitUnifiedVerticesD3D11(PrimitiveType, verts, isRhw);
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

static constexpr int      kXboxVsdeAutoCommit        = -1;
static constexpr int      kXboxVsdePosition          = 0;  // D3DVSDE_POSITION
static constexpr int      kXboxVsdeNormal            = 2;  // D3DVSDE_NORMAL
static constexpr int      kXboxVsdeDiffuse           = 3;  // D3DVSDE_DIFFUSE
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
                    v.w = (fabsf(denom) > 0.0001f) ? denom : 1.0f;
                    g_lastProjMask |= (1u << s);
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
