#include "ghost_runtime.h"
#include "ghost_addresses.h"
#include "xbox_d3d8_types.h"
#include <math.h>
#include <vector>

// ============================================================================
// Vertex Declarations, Shaders, Pixel Shaders & CPU Skinning Bridges
// ============================================================================

static std::vector<XboxShaderDeclInfo> g_vertexShaders;

uint32_t g_currentVsHandle   = 0x002;
uint32_t g_currentPsHandle   = 0;
int32_t  g_currentPsType     = -1; // -1 = fixed-function TSS, 0..27 = aPixelShaders[type]
float    g_psConstants[8][4] = {};

bool GetXboxVertexShaderDecl(uint32_t handle, XboxShaderDeclInfo* outDecl) {
    if (!outDecl) return false;
    if ((handle & 0x00010001u) == 0x00010001u) {
        uint32_t idx = (handle >> 4) - 1;
        if (idx < g_vertexShaders.size()) {
            *outDecl = g_vertexShaders[idx];
            return true;
        }
    }
    return false;
}

struct XboxPixelShaderObject {
    int32_t  engineType; // 0..27 if from aPixelShaders[0..27] at 0x007D1214, else 0
    uint8_t  defBytes[240];
};

static HRESULT __stdcall Bridge_D3DDevice_CreatePixelShader(const void* pShaderDef, DWORD* pHandle) {
    if (!pHandle) return E_INVALIDARG;
    XboxPixelShaderObject* obj = reinterpret_cast<XboxPixelShaderObject*>(calloc(1, sizeof(XboxPixelShaderObject)));
    if (!obj) return E_OUTOFMEMORY;
    obj->engineType = 0;
    if (pShaderDef) {
        memcpy(obj->defBytes, pShaderDef, 240);
    }
    uintptr_t hAddr = reinterpret_cast<uintptr_t>(pHandle);
    if (hAddr >= ADDR_PIXEL_SHADERS_TABLE && hAddr < ADDR_PIXEL_SHADERS_TABLE + 28 * 8) {
        obj->engineType = (int32_t)((hAddr - ADDR_PIXEL_SHADERS_TABLE) / 8);
    }
    if (obj->engineType >= 0 && obj->engineType < 28) {
        GetPixelShaderPermutation((uint32_t)(obj->engineType + 1));
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
    info.vsType = -1;

    uintptr_t hAddr = reinterpret_cast<uintptr_t>(pHandle);
    if (hAddr >= ADDR_VERTEX_SHADERS_TABLE && hAddr < ADDR_VERTEX_SHADERS_TABLE + 66 * 24) {
        info.vsType = (int32_t)((hAddr - ADDR_VERTEX_SHADERS_TABLE) / 24);
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
                }
            }
        }
    }

    g_vertexShaders.push_back(info);
    uint32_t idx = (uint32_t)g_vertexShaders.size();
    *pHandle = (idx << 4) | 0x00010001u;
    return S_OK;
}

int32_t g_activeCustomVS = 0; // 0=none, 1=Cloak, 2=ScreenEffect, 3=Membrane, 4=Sight, 5=Shield, 6=Teleport, 7=MotionBlur, 8=BumpEnv, 9=Envmap

void __stdcall Bridge_D3DDevice_SetVertexShader(uint32_t Handle) {
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
        memcpy(reinterpret_cast<void*>(ADDR_D3D_VS_CONSTANTS + xboxReg * 16), pConstantData, copyCnt * 16);
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

// Replacement for cMeshGroup::RenderSetupWeightedSkin (0x00606E50) and RenderSetupBlendWeightedSkin (0x00607390).
// Fixes the stack-aliasing bug in Ghost.exe's unused CPU fallback where Bone 1's normal overwrote Bone 0's normal
// ([ebp-0x54] / [ebp-0x1c]), causing vertices with w0=1, w1=0, w2=0 (e.g. Nova's face, abdomen, crotch) to get normal (0,0,0).
static uint32_t Host_RenderSetupWeightedSkin(uint8_t* pMeshGroup, uint8_t* pModelInst, uint32_t lod, bool isBlend) {
    if (!pMeshGroup) return 0;
    uint32_t numVerts = *reinterpret_cast<const uint32_t*>(pMeshGroup + 0x14 + lod * 16);
    void* pDynVB = *reinterpret_cast<void**>(ADDR_DYN_VB_PTR);
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
    const float* boneMats = *reinterpret_cast<const float* const*>(ADDR_CUR_BONE_MATS_PTR);

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

const uint32_t* g_curPrelitColors      = nullptr;
uint32_t        g_curPrelitBaseVertex  = 0;
const float*    g_curTangentBasis      = nullptr;
uint32_t        g_curTangentBaseVertex = 0;
float           g_cloakDistort         = 16.0f;
float           g_cloakC35[4]          = { 1.0f, 0.0f, 0.0f, 0.5f };
float           g_shieldC35x           = 1.0f;
float           g_teleportC32[2]       = { 1.0f / 640.0f, 1.0f / 480.0f };
float           g_teleportC33x         = 0.0f;
float           g_motionBlurVec[4]     = { 0.0f, 0.0f, 0.0f, 1.0f };
float           g_embossScale          = 0.015f;
float           g_specularScale        = 1.0f;
float           g_detailUvScale[2]     = { 1.0f, 1.0f };
Mat44           g_lightProjMat0        = { {{1,0,0,0}, {0,1,0,0}, {0,0,1,0}, {0,0,0,1}} };
Mat44           g_lightProjMat1        = { {{1,0,0,0}, {0,1,0,0}, {0,0,1,0}, {0,0,0,1}} };

static void SetupTangentStreamForMesh(const uint8_t* pMeshGroup, bool bindStream1) {
    g_curTangentBasis      = nullptr;
    g_curTangentBaseVertex = 0;
    if (!pMeshGroup) return;
    uint8_t lodIdx = pMeshGroup[0x50];
    if (lodIdx >= 3) return;
    const uint8_t* pModel = *reinterpret_cast<const uint8_t* const*>(pMeshGroup + 0x04);
    if (!pModel) return;
    XboxVertexBuffer* tangentVB = *reinterpret_cast<XboxVertexBuffer* const*>(pModel + lodIdx * 0x18 + 0x20);
    if (tangentVB && tangentVB->Data) {
        if (bindStream1) {
            Bridge_D3DDevice_SetStreamSource(1, tangentVB, 0x24);
        }
        g_curTangentBasis      = reinterpret_cast<const float*>(tangentVB->Data);
        g_curTangentBaseVertex = *reinterpret_cast<const uint32_t*>(pMeshGroup + 0x48);
    }
}

static void SetupPrelitStreamForMesh(const uint8_t* pMeshGroup, const uint8_t* pModelInst) {
    g_curPrelitColors     = nullptr;
    g_curPrelitBaseVertex = 0;
    SetupTangentStreamForMesh(pMeshGroup, false);
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
        g_curTangentBasis = nullptr;
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

static int __fastcall Bridge_BumpShader_VertexSetup(uint8_t* pShader, void* edxUnused, uint8_t* pMeshGroup, uint8_t* pModelInst) {
    int rc = Bridge_Shader_VertexSetup(pShader, edxUnused, pMeshGroup, pModelInst);
    SetupTangentStreamForMesh(pMeshGroup, true);
    g_activeCustomVS = kCustomVsBump;
    return rc;
}

static int __fastcall Bridge_BumpEnvShader_VertexSetup(uint8_t* pShader, void* edxUnused, uint8_t* pMeshGroup, uint8_t* pModelInst) {
    int rc = Bridge_Shader_VertexSetup(pShader, edxUnused, pMeshGroup, pModelInst);
    SetupTangentStreamForMesh(pMeshGroup, true);
    g_activeCustomVS = kCustomVsBumpEnv;
    return rc;
}

static int __fastcall Bridge_BumpEmbossShader_VertexSetup(uint8_t* pShader, void* edxUnused, uint8_t* pMeshGroup, uint8_t* pModelInst) {
    int rc = Bridge_Shader_VertexSetup(pShader, edxUnused, pMeshGroup, pModelInst);
    SetupTangentStreamForMesh(pMeshGroup, true);
    g_activeCustomVS = kCustomVsBumpEmboss;
    if (pShader) {
        float s = *reinterpret_cast<const float*>(pShader + 0x58);
        g_embossScale = (fabsf(s) > 0.0001f) ? (s * 0.05f) : 0.015f;
    }
    return rc;
}

static int __fastcall Bridge_EnvmapShader_VertexSetup(uint8_t* pShader, void* edxUnused, uint8_t* pMeshGroup, uint8_t* pModelInst) {
    int rc = Bridge_Shader_VertexSetup(pShader, edxUnused, pMeshGroup, pModelInst);
    g_activeCustomVS = kCustomVsEnvmap;
    return rc;
}

static int __fastcall Bridge_SpecularShader_VertexSetup(uint8_t* pShader, void* edxUnused, uint8_t* pMeshGroup, uint8_t* pModelInst) {
    int rc = Bridge_Shader_VertexSetup(pShader, edxUnused, pMeshGroup, pModelInst);
    g_activeCustomVS = kCustomVsSpecular;
    if (pShader) {
        float s = *reinterpret_cast<const float*>(pShader + 0x58);
        g_specularScale = (fabsf(s) > 0.0001f) ? s : 1.0f;
    }
    return rc;
}

static int __fastcall Bridge_GlossEnvmapDetailShader_VertexSetup(uint8_t* pShader, void* edxUnused, uint8_t* pMeshGroup, uint8_t* pModelInst) {
    int rc = Bridge_Shader_VertexSetup(pShader, edxUnused, pMeshGroup, pModelInst);
    g_activeCustomVS = kCustomVsGlossEnvmapDetail;
    if (pShader) {
        // cGlossEnvmapDetailShader is 0x5C bytes (NewInstance at 0x005FEDF0); +0x58 holds scalar detailScale
        float s = *reinterpret_cast<const float*>(pShader + 0x58);
        float val = (fabsf(s) > 0.0001f) ? s : 1.0f;
        g_detailUvScale[0] = val;
        g_detailUvScale[1] = val;
    }
    return rc;
}

static int __fastcall Bridge_LightProjectShader_VertexSetup(uint8_t* pShader, void* edxUnused, uint8_t* pMeshGroup, uint8_t* pModelInst) {
    int rc = Bridge_Shader_VertexSetup(pShader, edxUnused, pMeshGroup, pModelInst);
    g_activeCustomVS = kCustomVsLightProject;
    if (pShader) {
        memcpy(&g_lightProjMat0, pShader + 0xE0, sizeof(Mat44));
        memcpy(&g_lightProjMat1, pShader + 0x120, sizeof(Mat44));
    }
    return rc;
}

static int __fastcall Bridge_Shader_VertexSetupRigid(void* pShader, void* edxUnused, uint8_t* pMeshGroup, uint32_t lod, int bSetVS) {
    (void)pShader; (void)edxUnused; (void)bSetVS;
    g_activeCustomVS = 0;
    uint8_t* pModelInst = *reinterpret_cast<uint8_t**>(ADDR_CUR_MODEL_INST_PTR);
    SetupPrelitStreamForMesh(pMeshGroup, pModelInst);
    return (int)g_fnRenderSetupRigid(pMeshGroup, pModelInst, lod);
}

static int __fastcall Bridge_Shader_VertexSetupRigidBump(void* pShader, void* edxUnused, uint8_t* pMeshGroup, uint32_t lod, int bSetVS) {
    int rc = Bridge_Shader_VertexSetupRigid(pShader, edxUnused, pMeshGroup, lod, bSetVS);
    SetupTangentStreamForMesh(pMeshGroup, true);
    return rc;
}

static int __fastcall Bridge_Shader_VertexSetupWeightedSkin(void* pShader, void* edxUnused, uint8_t* pMeshGroup, uint32_t lod, int bSetVS) {
    (void)pShader; (void)edxUnused; (void)bSetVS;
    g_activeCustomVS = 0;
    uint8_t* pModelInst = *reinterpret_cast<uint8_t**>(ADDR_CUR_MODEL_INST_PTR);
    SetupPrelitStreamForMesh(pMeshGroup, pModelInst);
    return (int)Host_RenderSetupWeightedSkin(pMeshGroup, pModelInst, lod, false);
}

static int __fastcall Bridge_Shader_VertexSetupWeightedSkinBump(void* pShader, void* edxUnused, uint8_t* pMeshGroup, uint32_t lod, int bSetVS) {
    int rc = Bridge_Shader_VertexSetupWeightedSkin(pShader, edxUnused, pMeshGroup, lod, bSetVS);
    SetupTangentStreamForMesh(pMeshGroup, true);
    return rc;
}

static int __fastcall Bridge_Shader_VertexSetupSkinned(void* pShader, void* edxUnused, uint8_t* pMeshGroup, uint32_t lod, int bSetVS) {
    (void)pShader; (void)edxUnused; (void)bSetVS;
    g_activeCustomVS = 0;
    uint8_t* pModelInst = *reinterpret_cast<uint8_t**>(ADDR_CUR_MODEL_INST_PTR);
    SetupPrelitStreamForMesh(pMeshGroup, pModelInst);
    return (int)g_fnRenderSetupSkinned(pMeshGroup, pModelInst, lod);
}

static int __fastcall Bridge_Shader_VertexSetupSkinnedBump(void* pShader, void* edxUnused, uint8_t* pMeshGroup, uint32_t lod, int bSetVS) {
    int rc = Bridge_Shader_VertexSetupSkinned(pShader, edxUnused, pMeshGroup, lod, bSetVS);
    SetupTangentStreamForMesh(pMeshGroup, true);
    return rc;
}

static int __fastcall Bridge_Shader_VertexSetupPrelit2(void* pShader, void* edxUnused, void* pMeshGroup, void* pModelInst) {
    (void)pShader; (void)edxUnused; (void)pMeshGroup; (void)pModelInst;
    return 0; // Return 0 so caller falls back to standard rigid/skinned setup
}

static int __fastcall Bridge_Shader_VertexSetupPrelit4(void* pShader, void* edxUnused, void* pMeshGroup, void* pModelInst, uint32_t lod, int bSetVS) {
    (void)pShader; (void)edxUnused; (void)pMeshGroup; (void)pModelInst; (void)lod; (void)bSetVS;
    return 0;
}

void InitXboxD3D8ShaderBridges() {
    // 1. Bridge Vertex Shaders & Constants
    InstallBridge(0x006B27F0, &Bridge_D3DDevice_CreateVertexShader);               // _D3DDevice_CreateVertexShader@16
    InstallBridge(0x006B2F40, &Bridge_D3DDevice_SetVertexShader);                  // _D3DDevice_SetVertexShader@4
    InstallBridge(0x006B2930, &Bridge_D3DDevice_SetVertexShaderConstant1);         // @D3DDevice_SetVertexShaderConstant1@8
    InstallBridge(0x006B2990, &Bridge_D3DDevice_SetVertexShaderConstant4);         // @D3DDevice_SetVertexShaderConstant4@8
    InstallBridge(0x006B2A40, &Bridge_D3DDevice_SetVertexShaderConstantNotInline); // @D3DDevice_SetVertexShaderConstantNotInlineFast@12
    InstallBridge(0x006B2B20, &Bridge_D3DDevice_SetVertexShaderConstantNotInline); // @D3DDevice_SetVertexShaderConstantNotInline@12

    // 2. Bridge Pixel Shaders & Constants
    InstallBridge(0x006B3C60, &Bridge_D3DDevice_CreatePixelShader);                // _D3DDevice_CreatePixelShader@8
    InstallBridge(0x006B3CB0, &Bridge_D3DDevice_SetPixelShaderProgram);            // _D3DDevice_SetPixelShaderProgram@4
    InstallBridge(0x006B3CF0, &Bridge_D3DDevice_SetPixelShader);                   // _D3DDevice_SetPixelShader@4
    InstallBridge(0x006B3EF0, &Bridge_D3DDevice_SetPixelShaderConstant);           // _D3DDevice_SetPixelShaderConstant@12

    // 3. Bridge cShader::VertexSetup* to Ghost.exe's built-in CPU skinning (cMeshGroup::RenderSetup*)
    InstallBridge(0x00607AB0, &Bridge_Shader_VertexSetup);                         // cShader::VertexSetup
    InstallBridge(0x00602FE0, &Bridge_LightProjectShader_VertexSetup);             // cLightProjectShader::VertexSetup
    InstallBridge(0x006085B0, &Bridge_SightShader_VertexSetup);                    // cSightShader::VertexSetup
    InstallBridge(0x00608A20, &Bridge_ShieldShader_VertexSetup);                   // cShieldShader::VertexSetup
    InstallBridge(0x00608C50, &Bridge_TeleportShader_VertexSetup);                 // cTeleportShader::VertexSetup
    InstallBridge(0x00609560, &Bridge_GhostSuitShader_VertexSetup);                // cGhostSuitShader::VertexSetup
    InstallBridge(0x00609620, &Bridge_ScreenEffectShader_VertexSetup);             // cScreenEffectShader::VertexSetup
    InstallBridge(0x00609830, &Bridge_CloakShader_VertexSetup);                    // cCloakShader::VertexSetup
    InstallBridge(0x00609E20, &Bridge_MembraneShader_VertexSetup);                 // cMembraneShader::VertexSetup
    InstallBridge(0x00609FE0, &Bridge_MotionBlur_VertexSetup);                     // cMotionBlur::VertexSetup
    InstallBridge(0x006127D0, &Bridge_BumpShader_VertexSetup);                     // cBumpShader::VertexSetup
    InstallBridge(0x00612920, &Bridge_BumpEnvShader_VertexSetup);                  // cBumpEnvShader::VertexSetup
    InstallBridge(0x00612A70, &Bridge_BumpEmbossShader_VertexSetup);               // cBumpEmbossShader::VertexSetup
    InstallBridge(0x00616450, &Bridge_EnvmapShader_VertexSetup);                   // cEnvmapShader::VertexSetup
    InstallBridge(0x006165B0, &Bridge_EnvmapShader_VertexSetup);                   // cMetalEnvmapShader::VertexSetup
    InstallBridge(0x00616860, &Bridge_GlossEnvmapDetailShader_VertexSetup);        // cGlossEnvmapDetailShader::VertexSetup
    InstallBridge(0x00616AD0, &Bridge_SpecularShader_VertexSetup);                 // cSpecularShader::VertexSetup

    InstallBridge(0x00605750, &Bridge_Shader_VertexSetupRigid);                    // cShader::VertexSetupRigid
    InstallBridge(0x00605950, &Bridge_Shader_VertexSetupRigidBump);                // cShader::VertexSetupRigidBump
    InstallBridge(0x00605B50, &Bridge_Shader_VertexSetupPrelit2);                  // cShader::VertexSetupPrelitRigid
    InstallBridge(0x00605CD0, &Bridge_Shader_VertexSetupWeightedSkin);             // cShader::VertexSetupWeightedSkin
    InstallBridge(0x00607880, &Bridge_Shader_VertexSetupWeightedSkinBump);         // cShader::VertexSetupWeightedSkinBump
    InstallBridge(0x00605EC0, &Bridge_Shader_VertexSetupPrelit4);                  // cShader::VertexSetupPrelitWeightedSkin
    InstallBridge(0x006060D0, &Bridge_Shader_VertexSetupSkinned);                  // cShader::VertexSetupSkinned
    InstallBridge(0x006062D0, &Bridge_Shader_VertexSetupSkinnedBump);              // cShader::VertexSetupSkinnedBump
    InstallBridge(0x006064F0, &Bridge_Shader_VertexSetupPrelit4);                  // cShader::VertexSetupPrelitSkinned
}
