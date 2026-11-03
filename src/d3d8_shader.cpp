#include "ghost_runtime.h"
#include "ghost_addresses.h"
#include "xbox_d3d8_types.h"
#include <d3d11.h>
#include <d3dcompiler.h>
#include <math.h>
#include <stdio.h>
#include <string>
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
        uint32_t rawIdx = (handle & 0xFFFFu) >> 4;
        if (rawIdx > 0 && (rawIdx - 1) < g_vertexShaders.size()) {
            *outDecl = g_vertexShaders[rawIdx - 1];
            return true;
        }
    }
    return false;
}

// Matches the 0xFC-byte Xbox D3D8 pixel shader struct created by _D3DDevice_CreatePixelShader@8 (0x006B3C60):
//   +0x00: uint32_t RefCount (= 1)
//   +0x04: uint32_t Flags    (= 1 for CreatePixelShader, 0 for SetPixelShaderProgram wrapper at CDevice+0x924)
//   +0x08: XboxPixelShaderDef* pPSDef (points to +0x0C, or external D3DPIXELSHADERDEF*)
//   +0x0C: XboxPixelShaderDef def (240 bytes)
struct XboxPixelShaderObject {
    uint32_t           refCount;
    uint32_t           flags;
    XboxPixelShaderDef* pPSDef;
    XboxPixelShaderDef def;
    int32_t            engineType; // 0..27 if from aPixelShaders[0..27] at 0x007D1214, else -1
};

static const XboxPixelShaderDef* GetHandlePSDef(uint32_t handle, int32_t* outEngineType = nullptr) {
    if (outEngineType) *outEngineType = -1;
    if (handle < 0x00400000u) return nullptr;
    if (handle == (ADDR_D3D_DEVICE + 0x924u)) {
        return *reinterpret_cast<const XboxPixelShaderDef* const*>(handle + 0x08u);
    }
    const XboxPixelShaderObject* obj = reinterpret_cast<const XboxPixelShaderObject*>(handle);
    if (outEngineType) *outEngineType = obj->engineType;
    return obj->pPSDef ? obj->pPSDef : &obj->def;
}

static uint32_t PackFloat4ToD3DColor(const float* rgba) {
    auto toByte = [](float v) -> uint32_t {
        if (v <= 0.0f) return 0u;
        if (v >= 1.0f) return 255u;
        return static_cast<uint32_t>(v * 255.0f + 0.5f);
    };
    uint32_t r = toByte(rgba[0]);
    uint32_t g = toByte(rgba[1]);
    uint32_t b = toByte(rgba[2]);
    uint32_t a = toByte(rgba[3]);
    return (a << 24) | (r << 16) | (g << 8) | b;
}

static HRESULT __stdcall Bridge_D3DDevice_CreatePixelShader(const void* pShaderDef, DWORD* pHandle) {
    if (!pHandle) return E_INVALIDARG;
    XboxPixelShaderObject* obj = reinterpret_cast<XboxPixelShaderObject*>(calloc(1, sizeof(XboxPixelShaderObject)));
    if (!obj) return E_OUTOFMEMORY;
    obj->refCount   = 1u;
    obj->flags      = 1u;
    obj->pPSDef     = &obj->def;
    obj->engineType = -1;
    if (pShaderDef) {
        memcpy(&obj->def, pShaderDef, sizeof(XboxPixelShaderDef));
    }
    uintptr_t hAddr = reinterpret_cast<uintptr_t>(pHandle);
    if (hAddr >= ADDR_PIXEL_SHADERS_TABLE && hAddr < ADDR_PIXEL_SHADERS_TABLE + 28 * 8) {
        obj->engineType = static_cast<int32_t>((hAddr - ADDR_PIXEL_SHADERS_TABLE) / 8);
    }
    if (obj->engineType >= 0 && obj->engineType < 28) {
        GetPixelShaderPermutation(static_cast<uint32_t>(obj->engineType + 1));
    }
    *pHandle = reinterpret_cast<DWORD>(obj);
    return S_OK;
}

static HRESULT __stdcall Bridge_D3DDevice_SetPixelShader(DWORD Handle) {
    g_currentPsHandle = Handle;
    *reinterpret_cast<uint32_t*>(ADDR_D3D_DEVICE + 0x784) = Handle;
    uint32_t* xboxRS = reinterpret_cast<uint32_t*>(ADDR_D3D_RENDERSTATE);

    if (Handle != 0 && Handle >= 0x00400000u) {
        int32_t engType = -1;
        const XboxPixelShaderDef* pDef = GetHandlePSDef(Handle, &engType);
        g_currentPsType = (engType >= 0 && engType < 28) ? engType : 28;
        if (pDef) {
            const uint32_t* dwords = reinterpret_cast<const uint32_t*>(pDef);
            // Synchronize D3DPIXELSHADERDEF into _D3D__RenderState[0..56] & PSTextureModes (136),
            // matching _D3DDevice_SetPixelShader@4 (0x006B3CF0).
            for (uint32_t i = 0; i <= 53; ++i) {
                xboxRS[i] = dwords[i];
            }
            xboxRS[XBOX_D3DRS_PSTEXTUREMODES] = dwords[54];
            *reinterpret_cast<uint32_t*>(ADDR_D3D_DEVICE + 0x790) = dwords[54];
            xboxRS[55] = dwords[55]; // PSDotMapping
            xboxRS[56] = dwords[56]; // PSInputTexture

            // Load embedded def cN constants from D3DPIXELSHADERDEF unconditionally (including 0x00000000!):
            // PSC0Mapping=dwords[57], PSC1Mapping=dwords[58], PSFinalCombinerConstants=dwords[59].
            uint32_t c0Map = dwords[57];
            uint32_t c1Map = dwords[58];
            uint32_t fcMap = dwords[59];
            for (int s = 0; s < 8; ++s) {
                uint32_t r0 = (c0Map >> (s * 4)) & 0xFu;
                if (r0 < 8u) {
                    uint32_t c = dwords[10 + s];
                    UnpackD3DColor(c, &g_psConstants[r0][0], &g_psConstants[r0][1], &g_psConstants[r0][2], &g_psConstants[r0][3]);
                }
                uint32_t r1 = (c1Map >> (s * 4)) & 0xFu;
                if (r1 < 8u) {
                    uint32_t c = dwords[18 + s];
                    UnpackD3DColor(c, &g_psConstants[r1][0], &g_psConstants[r1][1], &g_psConstants[r1][2], &g_psConstants[r1][3]);
                }
            }
            uint32_t fc0Reg = fcMap & 0xFu;
            if (fc0Reg < 8u) {
                uint32_t c = dwords[43];
                UnpackD3DColor(c, &g_psConstants[fc0Reg][0], &g_psConstants[fc0Reg][1], &g_psConstants[fc0Reg][2], &g_psConstants[fc0Reg][3]);
            }
            uint32_t fc1Reg = (fcMap >> 4) & 0xFu;
            if (fc1Reg < 8u) {
                uint32_t c = dwords[44];
                UnpackD3DColor(c, &g_psConstants[fc1Reg][0], &g_psConstants[fc1Reg][1], &g_psConstants[fc1Reg][2], &g_psConstants[fc1Reg][3]);
            }
        }
    } else {
        g_currentPsType = -1;
        xboxRS[XBOX_D3DRS_PSCOMBINERCOUNT] = 0u;
    }
    return S_OK;
}

// _D3DDevice_SetPixelShaderProgram@4 (0x006B3CB0):
// Takes a direct pointer to a D3DPIXELSHADERDEF (or NULL), wraps it in CDevice+0x924, and calls SetPixelShader.
static void __stdcall Bridge_D3DDevice_SetPixelShaderProgram(const XboxPixelShaderDef* pPSDef) {
    if (pPSDef) {
        *reinterpret_cast<uint32_t*>(ADDR_D3D_DEVICE + 0x924) = 1u;
        *reinterpret_cast<uint32_t*>(ADDR_D3D_DEVICE + 0x928) = 0u;
        *reinterpret_cast<const XboxPixelShaderDef**>(ADDR_D3D_DEVICE + 0x92C) = pPSDef;
        Bridge_D3DDevice_SetPixelShader(ADDR_D3D_DEVICE + 0x924u);
    } else {
        Bridge_D3DDevice_SetPixelShader(0u);
    }
}

static HRESULT __stdcall Bridge_D3DDevice_SetPixelShaderConstant(DWORD Register, const void* pConstantData, DWORD ConstantCount) {
    if (pConstantData && Register < 8 && ConstantCount > 0) {
        DWORD cnt = ConstantCount;
        if (Register + cnt > 8) cnt = 8 - Register;
        memcpy(&g_psConstants[Register][0], pConstantData, cnt * 16);
        memcpy(reinterpret_cast<void*>(ADDR_D3D_DEVICE + 0x894 + Register * 16), pConstantData, cnt * 16);

        // Also synchronize mapped register combiner constant slots in _D3D__RenderState[10..25, 43..44],
        // matching _D3DDevice_SetPixelShaderConstant@12 (0x006B3EF0).
        const XboxPixelShaderDef* pDef = GetHandlePSDef(g_currentPsHandle);
        if (pDef) {
            uint32_t* xboxRS = reinterpret_cast<uint32_t*>(ADDR_D3D_RENDERSTATE);
            uint32_t c0Map = pDef->PSC0Mapping;
            uint32_t c1Map = pDef->PSC1Mapping;
            uint32_t fcMap = pDef->PSFinalCombinerConstants;
            for (DWORD r = Register; r < Register + cnt; ++r) {
                uint32_t packed = PackFloat4ToD3DColor(g_psConstants[r]);
                for (int s = 0; s < 8; ++s) {
                    if (((c0Map >> (s * 4)) & 0xFu) == r) xboxRS[10 + s] = packed;
                    if (((c1Map >> (s * 4)) & 0xFu) == r) xboxRS[18 + s] = packed;
                }
                if ((fcMap & 0xFu) == r)         xboxRS[43] = packed;
                if (((fcMap >> 4) & 0xFu) == r)  xboxRS[44] = packed;
            }
        }
    }
    return S_OK;
}

// ============================================================================
// General NV2A Register Combiner (D3DPIXELSHADERDEF / _D3D__RenderState[0..56, 136])
// Decompiler to HLSL ps_4_0 & Constant Populator
// ============================================================================

void PopulateRegisterCombinerConstants(PerDrawCBData* cb) {
    if (!cb) return;
    const uint32_t* xboxRS = reinterpret_cast<const uint32_t*>(ADDR_D3D_RENDERSTATE);
    const XboxPixelShaderDef* pDef = GetHandlePSDef(g_currentPsHandle);

    uint32_t combCountFlags = xboxRS[XBOX_D3DRS_PSCOMBINERCOUNT];
    bool sepC0 = (combCountFlags & 0x1000u)  != 0u;
    bool sepC1 = (combCountFlags & 0x10000u) != 0u;
    uint32_t c0Map = pDef ? pDef->PSC0Mapping : 0xFFFFFFFFu;
    uint32_t c1Map = pDef ? pDef->PSC1Mapping : 0xFFFFFFFFu;
    uint32_t fcMap = pDef ? pDef->PSFinalCombinerConstants : 0xFFFFFFFFu;

    for (int s = 0; s < 8; ++s) {
        int effS0 = sepC0 ? s : 0;
        uint32_t r0 = (c0Map >> (effS0 * 4)) & 0xFu;
        if (r0 < 8u && PackFloat4ToD3DColor(g_psConstants[r0]) == xboxRS[10 + effS0]) {
            memcpy(cb->rcStageC0[s], g_psConstants[r0], 16);
        } else {
            UnpackD3DColor(xboxRS[10 + effS0], &cb->rcStageC0[s][0], &cb->rcStageC0[s][1], &cb->rcStageC0[s][2], &cb->rcStageC0[s][3]);
        }

        int effS1 = sepC1 ? s : 0;
        uint32_t r1 = (c1Map >> (effS1 * 4)) & 0xFu;
        if (r1 < 8u && PackFloat4ToD3DColor(g_psConstants[r1]) == xboxRS[18 + effS1]) {
            memcpy(cb->rcStageC1[s], g_psConstants[r1], 16);
        } else {
            UnpackD3DColor(xboxRS[18 + effS1], &cb->rcStageC1[s][0], &cb->rcStageC1[s][1], &cb->rcStageC1[s][2], &cb->rcStageC1[s][3]);
        }
    }

    uint32_t fc0Reg = fcMap & 0xFu;
    if (fc0Reg < 8u && PackFloat4ToD3DColor(g_psConstants[fc0Reg]) == xboxRS[43]) {
        memcpy(cb->rcFinalConst[0], g_psConstants[fc0Reg], 16);
    } else {
        UnpackD3DColor(xboxRS[43], &cb->rcFinalConst[0][0], &cb->rcFinalConst[0][1], &cb->rcFinalConst[0][2], &cb->rcFinalConst[0][3]);
    }

    uint32_t fc1Reg = (fcMap >> 4) & 0xFu;
    if (fc1Reg < 8u && PackFloat4ToD3DColor(g_psConstants[fc1Reg]) == xboxRS[44]) {
        memcpy(cb->rcFinalConst[1], g_psConstants[fc1Reg], 16);
    } else {
        UnpackD3DColor(xboxRS[44], &cb->rcFinalConst[1][0], &cb->rcFinalConst[1][1], &cb->rcFinalConst[1][2], &cb->rcFinalConst[1][3]);
    }
}

struct CachedRegCombinerPS {
    uint64_t           hash;
    ID3D11PixelShader* ps;
};
static std::vector<CachedRegCombinerPS> g_rcPsCache;

static void FormatRcInputExpr(
    uint8_t byteVal, bool isAlpha, int stageIdx, bool isFinalCombiner,
    char* outBuf, size_t outSize)
{
    uint32_t reg  = byteVal & 0x0Fu;
    uint32_t chan = (byteVal >> 4) & 0x01u;
    uint32_t map  = (byteVal >> 5) & 0x07u;

    char regStr[64];
    switch (reg) {
        case 0x0u: snprintf(regStr, sizeof(regStr), "float4(0.0f,0.0f,0.0f,0.0f)"); break;
        case 0x1u:
            if (isFinalCombiner) snprintf(regStr, sizeof(regStr), "uRcFinalConst[0]");
            else                 snprintf(regStr, sizeof(regStr), "uRcStageC0[%d]", stageIdx);
            break;
        case 0x2u:
            if (isFinalCombiner) snprintf(regStr, sizeof(regStr), "uRcFinalConst[1]");
            else                 snprintf(regStr, sizeof(regStr), "uRcStageC1[%d]", stageIdx);
            break;
        case 0x3u: snprintf(regStr, sizeof(regStr), "fogReg");  break;
        case 0x4u: snprintf(regStr, sizeof(regStr), "v0");      break;
        case 0x5u: snprintf(regStr, sizeof(regStr), "v1");      break;
        case 0x8u: snprintf(regStr, sizeof(regStr), "t0");      break;
        case 0x9u: snprintf(regStr, sizeof(regStr), "t1");      break;
        case 0xAu: snprintf(regStr, sizeof(regStr), "t2");      break;
        case 0xBu: snprintf(regStr, sizeof(regStr), "t3");      break;
        case 0xCu: snprintf(regStr, sizeof(regStr), "r0");      break;
        case 0xDu: snprintf(regStr, sizeof(regStr), "r1");      break;
        case 0xEu: snprintf(regStr, sizeof(regStr), "sumReg");  break;
        case 0xFu: snprintf(regStr, sizeof(regStr), "efProd");  break;
        default:   snprintf(regStr, sizeof(regStr), "float4(0.0f,0.0f,0.0f,0.0f)"); break;
    }

    char compStr[80];
    if (isAlpha) {
        snprintf(compStr, sizeof(compStr), "(%s).%s", regStr, chan ? "a" : "b");
    } else {
        snprintf(compStr, sizeof(compStr), "(%s).%s", regStr, chan ? "aaa" : "rgb");
    }

    switch (map) {
        case 0u: // UNSIGNED_IDENTITY: max(0, x)
            snprintf(outBuf, outSize, "max(%s, 0.0f)", compStr);
            break;
        case 1u: // UNSIGNED_INVERT: 1 - clamp(x, 0, 1)
            snprintf(outBuf, outSize, "(1.0f - saturate(%s))", compStr);
            break;
        case 2u: // EXPAND_NORMAL: 2 * max(0, x) - 1
            snprintf(outBuf, outSize, "(2.0f * max(%s, 0.0f) - 1.0f)", compStr);
            break;
        case 3u: // EXPAND_NEGATE: 1 - 2 * max(0, x)
            snprintf(outBuf, outSize, "(1.0f - 2.0f * max(%s, 0.0f))", compStr);
            break;
        case 4u: // HALFBIAS_NORMAL: max(0, x) - 0.5
            snprintf(outBuf, outSize, "(max(%s, 0.0f) - 0.5f)", compStr);
            break;
        case 5u: // HALFBIAS_NEGATE: 0.5 - max(0, x)
            snprintf(outBuf, outSize, "(0.5f - max(%s, 0.0f))", compStr);
            break;
        case 6u: // SIGNED_IDENTITY: x
            snprintf(outBuf, outSize, "(%s)", compStr);
            break;
        case 7u: // SIGNED_NEGATE: -x
            snprintf(outBuf, outSize, "(-(%s))", compStr);
            break;
        default:
            snprintf(outBuf, outSize, "(%s)", compStr);
            break;
    }
}

static const char* GetRcDestRegName(uint32_t dstNibble) {
    switch (dstNibble & 0x0Fu) {
        case 0x4u: return "v0";
        case 0x5u: return "v1";
        case 0x8u: return "t0";
        case 0x9u: return "t1";
        case 0xAu: return "t2";
        case 0xBu: return "t3";
        case 0xCu: return "r0";
        case 0xDu: return "r1";
        default:   return nullptr;
    }
}

static void FormatRcScaleBias(const char* expr, uint32_t outWord, char* outBuf, size_t outSize) {
    uint32_t scale = (outWord >> 15) & 0x03u;
    uint32_t bias  = (outWord >> 17) & 0x01u;
    const char* scaleMul = "";
    if (scale == 1u)      scaleMul = " * 2.0f";
    else if (scale == 2u) scaleMul = " * 4.0f";
    else if (scale == 3u) scaleMul = " * 0.5f";

    if (bias != 0u) {
        snprintf(outBuf, outSize, "clamp(((%s) - 0.5f)%s, -1.0f, 1.0f)", expr, scaleMul);
    } else {
        snprintf(outBuf, outSize, "clamp((%s)%s, -1.0f, 1.0f)", expr, scaleMul);
    }
}

// Decompiles the active NV2A Register Combiner state in _D3D__RenderState[0..56, 136] into HLSL ps_4_0
static std::string DecompileRegisterCombinersToHLSL(const uint32_t* xboxRS) {
    uint32_t psTexModes = xboxRS[XBOX_D3DRS_PSTEXTUREMODES];
    uint32_t dotMapping = xboxRS[XBOX_D3DRS_PSDOTMAPPING];
    uint32_t inputTex   = xboxRS[XBOX_D3DRS_PSINPUTTEXTURE];
    uint32_t cmpMode    = xboxRS[XBOX_D3DRS_PSCOMPAREMODE];
    uint32_t combCount  = xboxRS[XBOX_D3DRS_PSCOMBINERCOUNT] & 0x0Fu;
    if (combCount == 0u) combCount = 1u;
    if (combCount > 8u)  combCount = 8u;
    bool muxMsb = (xboxRS[XBOX_D3DRS_PSCOMBINERCOUNT] & 0x0100u) != 0u;

    std::string s;
    s.reserve(8192);
    s += "cbuffer PerDrawCB : register(b0) {\n"
         "    float4 uWvpRow0; float4 uWvpRow1; float4 uWvpRow2; float4 uWvpRow3;\n"
         "    float4 uViewport; float4 uTFactor;\n"
         "    uint4  uStage0Ops; uint4 uStage0Alpha;\n"
         "    uint4  uStage1Ops; uint4 uStage1Alpha;\n"
         "    uint4  uStage2Ops; uint4 uStage2Alpha;\n"
         "    uint4  uStage3Ops; uint4  uStage3Alpha;\n"
         "    uint4  uFlags; float4 uAlphaTest;\n"
         "    float4 uUvScale01; float4 uUvScale23;\n"
         "    float4 uBumpParams; float4 uFogParams; float4 uFogColor;\n"
         "    float4 uPsConst0; float4 uPsConst1; float4 uPsConst2; float4 uPsConst3;\n"
         "    float4 uPsConst4; float4 uPsConst5; float4 uPsConst6; float4 uPsConst7;\n"
         "    float4 uWorldRow0; float4 uWorldRow1; float4 uWorldRow2; float4 uWorldRow3;\n"
         "    uint4  uLightCount;\n"
         "    float4 uLightPosType[8]; float4 uLightDirRange[8];\n"
         "    float4 uLightAttenSpot[8]; float4 uLightSpotParams[8];\n"
         "    float4 uLightDiffuse[8]; float4 uLightAmbient[8]; float4 uLightSpecular[8];\n"
         "    float4 uEyePosWorld; float4 uMatBackDiffAmb; float4 uMatBackSpec;\n"
         "    uint4  uStageColorKeyOp; float4 uStageColorKeyColor[4];\n"
         "    uint4  uStageColorSign; float4 uBumpLuminance;\n"
         "    float4 uScissorRects[8];\n"
         "    float4 uRcStageC0[8]; float4 uRcStageC1[8]; float4 uRcFinalConst[2];\n"
         "};\n"
         "Texture2D    tex0 : register(t0); Texture2D    tex1 : register(t1);\n"
         "Texture2D    tex2 : register(t2); Texture2D    tex3 : register(t3);\n"
         "TextureCube  cubeTex0 : register(t4); TextureCube  cubeTex1 : register(t5);\n"
         "TextureCube  cubeTex2 : register(t6); TextureCube  cubeTex3 : register(t7);\n"
         "Texture3D    volTex0  : register(t8); Texture3D    volTex1  : register(t9);\n"
         "Texture3D    volTex2  : register(t10); Texture3D   volTex3  : register(t11);\n"
         "Texture2D    gammaLutTex : register(t12);\n"
         "SamplerState smp0 : register(s0); SamplerState smp1 : register(s1);\n"
         "SamplerState smp2 : register(s2); SamplerState smp3 : register(s3);\n"
         "struct PSIn {\n"
         "    float4 pos    : SV_POSITION;\n"
         "    float4 color  : COLOR0;\n"
         "    float4 uv0    : TEXCOORD0;\n"
         "    float4 uv1    : TEXCOORD1;\n"
         "    float4 uv2    : TEXCOORD2;\n"
         "    float4 uv3    : TEXCOORD3;\n"
         "    float2 eyeDist: TEXCOORD4;\n"
         "    float3 norm   : TEXCOORD5;\n"
         "};\n"
         "float4 SampleStageTex(uint stageIdx, Texture2D tex2d, TextureCube texCube, Texture3D texVol, SamplerState smp, float4 uv, bool isCube, bool isVol) {\n"
         "    float4 c;\n"
         "    if (isCube) { c = texCube.Sample(smp, uv.xyz); }\n"
         "    else if (isVol) { c = texVol.Sample(smp, uv.xyz); }\n"
         "    else { c = tex2d.Sample(smp, uv.xy); }\n"
         "    uint ckOp = uStageColorKeyOp[stageIdx];\n"
         "    if (ckOp != 0u) {\n"
         "        float4 ckCol = uStageColorKeyColor[stageIdx];\n"
         "        bool matchRgb = all(abs(c.rgb - ckCol.rgb) < (1.5f / 255.0f));\n"
         "        if (ckOp == 1u && matchRgb) c.a = 0.0f;\n"
         "        else if (ckOp == 2u && matchRgb && abs(c.a - ckCol.a) < (1.5f / 255.0f)) c = 0.0f;\n"
         "        else if (ckOp == 3u && matchRgb) discard;\n"
         "    }\n"
         "    if (((uFlags.y >> (16u + stageIdx)) & 1u) != 0u && c.a < (0.5f / 255.0f)) discard;\n"
         "    uint cSign = uStageColorSign[stageIdx];\n"
         "    if (cSign != 0u) {\n"
         "        if ((cSign & 0x20u) != 0u) c.r = c.r * 2.0f - 1.0f;\n"
         "        if ((cSign & 0x40u) != 0u) c.g = c.g * 2.0f - 1.0f;\n"
         "        if ((cSign & 0x80u) != 0u) c.b = c.b * 2.0f - 1.0f;\n"
         "        if ((cSign & 0x10u) != 0u) c.a = c.a * 2.0f - 1.0f;\n"
         "    }\n"
         "    return c;\n"
         "}\n"
         "float3 MapDotVec(float4 t, uint dm) {\n"
         "    if (dm == 0u) return t.rgb;\n"
         "    if (dm == 1u) return (t.rgb - 128.0f / 255.0f) * (255.0f / 127.0f);\n"
         "    return t.rgb * 2.0f - 1.0f;\n"
         "}\n"
         "float4 PSMain(PSIn input) : SV_TARGET {\n"
         "    uint scissorCount = (uFlags.x >> 4u) & 0x0Fu;\n"
         "    if (scissorCount > 0u) {\n"
         "        bool insideAny = false;\n"
         "        [unroll] for (uint si = 0u; si < 8u; ++si) {\n"
         "            if (si < scissorCount) {\n"
         "                float4 sr = uScissorRects[si];\n"
         "                if (input.pos.x >= sr.x && input.pos.x < sr.z && input.pos.y >= sr.y && input.pos.y < sr.w) insideAny = true;\n"
         "            }\n"
         "        }\n"
         "        if (((uFlags.x & 4u) != 0u) ? insideAny : !insideAny) discard;\n"
         "    }\n"
         "    uint texMask  = uFlags.y & 0x0Fu;\n"
         "    uint cubeMask = (uFlags.y >> 4u) & 0x0Fu;\n"
         "    uint projMask = (uFlags.y >> 8u) & 0x0Fu;\n"
         "    uint volMask  = (uFlags.y >> 12u) & 0x0Fu;\n"
         "    float4 uv0 = input.uv0, uv1 = input.uv1, uv2 = input.uv2, uv3 = input.uv3;\n"
         "    float4 t0 = 0.0f, t1 = 0.0f, t2 = 0.0f, t3 = 0.0f;\n"
         "    float3 dotVec = 0.0f;\n";

    char line[512];
    // 1. Emit 4-stage NV2A Texture Addressing Modes (PSTextureModes)
    for (int st = 0; st < 4; ++st) {
        uint32_t tm = (psTexModes >> (st * 5)) & 0x1Fu;
        uint32_t inSt = 0u;
        if (st == 1)      inSt = 0u;
        else if (st == 2) inSt = (inputTex >> 16) & 1u;
        else if (st == 3) inSt = (inputTex >> 20) & 3u;
        if (inSt >= (uint32_t)st) inSt = (st > 0) ? (uint32_t)(st - 1) : 0u;
        uint32_t dm = (st > 0) ? ((dotMapping >> ((st - 1) * 4)) & 0x07u) : 0u;
        uint32_t cm = (cmpMode >> (st * 4)) & 0x0Fu;

        switch (tm) {
            case 0x00u: // PS_TEXTUREMODES_NONE
                snprintf(line, sizeof(line), "    t%d = float4(0.0f, 0.0f, 0.0f, 0.0f);\n", st);
                s += line;
                break;
            case 0x01u: // PS_TEXTUREMODES_PROJECT2D
            case 0x02u: // PS_TEXTUREMODES_PROJECT3D
            case 0x03u: // PS_TEXTUREMODES_CUBEMAP
                snprintf(line, sizeof(line),
                    "    if ((texMask & %uu) != 0u) {\n"
                    "        float4 uvs = uv%d;\n"
                    "        if ((projMask & %uu) != 0u && abs(uvs.w) > 1e-5f) uvs.xyz /= uvs.w;\n"
                    "        t%d = SampleStageTex(%uu, tex%d, cubeTex%d, volTex%d, smp%d, uvs, (cubeMask & %uu) != 0u || %uu == 3u, (volMask & %uu) != 0u || %uu == 2u);\n"
                    "    }\n",
                    1u << st, st, 1u << st, st, st, st, st, st, st, 1u << st, tm, 1u << st, tm);
                s += line;
                break;
            case 0x04u: // PS_TEXTUREMODES_PASSTHRU
                snprintf(line, sizeof(line), "    t%d = saturate(uv%d);\n", st, st);
                s += line;
                break;
            case 0x05u: // PS_TEXTUREMODES_CLIPPLANE
                snprintf(line, sizeof(line),
                    "    if (((%uu & 1u) != 0u ? (uv%d.x < 0.0f) : (uv%d.x >= 0.0f)) ||\n"
                    "        ((%uu & 2u) != 0u ? (uv%d.y < 0.0f) : (uv%d.y >= 0.0f)) ||\n"
                    "        ((%uu & 4u) != 0u ? (uv%d.z < 0.0f) : (uv%d.z >= 0.0f)) ||\n"
                    "        ((%uu & 8u) != 0u ? (uv%d.w < 0.0f) : (uv%d.w >= 0.0f))) discard;\n",
                    cm, st, st, cm, st, st, cm, st, st, cm, st, st);
                s += line;
                break;
            case 0x06u: // PS_TEXTUREMODES_BUMPENVMAP
            case 0x07u: // PS_TEXTUREMODES_BUMPENVMAPLUMINANCE
                snprintf(line, sizeof(line),
                    "    if ((texMask & %uu) != 0u) {\n"
                    "        float2 bd = MapDotVec(t%u, %uu).xy;\n"
                    "        float4 buv = uv%d;\n"
                    "        buv.xy += float2(bd.x * uBumpParams.x + bd.y * uBumpParams.z, bd.x * uBumpParams.y + bd.y * uBumpParams.w);\n"
                    "        t%d = SampleStageTex(%uu, tex%d, cubeTex%d, volTex%d, smp%d, buv, (cubeMask & %uu) != 0u, (volMask & %uu) != 0u);\n"
                    "        if (%uu == 7u) t%d.rgb *= saturate(t%u.b * uBumpLuminance.x + uBumpLuminance.y);\n"
                    "    }\n",
                    1u << st, inSt, dm, st, st, st, st, st, st, st, 1u << st, 1u << st, tm, st, inSt);
                s += line;
                break;
            case 0x0Fu: // PS_TEXTUREMODES_DPNDNT_AR (texreg2ar)
                snprintf(line, sizeof(line),
                    "    if ((texMask & %uu) != 0u) t%d = SampleStageTex(%uu, tex%d, cubeTex%d, volTex%d, smp%d, float4(t%u.ar, 0.0f, 0.0f), false, false);\n",
                    1u << st, st, st, st, st, st, st, inSt);
                s += line;
                break;
            case 0x10u: // PS_TEXTUREMODES_DPNDNT_GB (texreg2gb)
                snprintf(line, sizeof(line),
                    "    if ((texMask & %uu) != 0u) t%d = SampleStageTex(%uu, tex%d, cubeTex%d, volTex%d, smp%d, float4(t%u.gb, 0.0f, 0.0f), false, false);\n",
                    1u << st, st, st, st, st, st, st, inSt);
                s += line;
                break;
            case 0x11u: // PS_TEXTUREMODES_DOTPRODUCT (texm3x2pad / texm3x3pad)
                snprintf(line, sizeof(line),
                    "    { float dp = dot(uv%d.xyz, MapDotVec(t%u, %uu)); dotVec.%s = dp; t%d = dp.xxxx; }\n",
                    st, inSt, dm, (st == 1) ? "x" : ((st == 2) ? "y" : "z"), st);
                s += line;
                break;
            case 0x09u: // PS_TEXTUREMODES_DOT_ST (texm3x2tex)
                snprintf(line, sizeof(line),
                    "    { float dp = dot(uv%d.xyz, MapDotVec(t%u, %uu)); dotVec.y = dp;\n"
                    "      if ((texMask & %uu) != 0u) t%d = SampleStageTex(%uu, tex%d, cubeTex%d, volTex%d, smp%d, float4(dotVec.xy, 0.0f, 0.0f), false, false); }\n",
                    st, inSt, dm, 1u << st, st, st, st, st, st, st);
                s += line;
                break;
            case 0x0Du: // PS_TEXTUREMODES_DOT_STR_3D
            case 0x0Eu: // PS_TEXTUREMODES_DOT_STR_CUBE (texm3x3tex)
                snprintf(line, sizeof(line),
                    "    { float dp = dot(uv%d.xyz, MapDotVec(t%u, %uu)); dotVec.z = dp;\n"
                    "      if ((texMask & %uu) != 0u) t%d = SampleStageTex(%uu, tex%d, cubeTex%d, volTex%d, smp%d, float4(dotVec.xyz, 1.0f), (cubeMask & %uu) != 0u || %uu == 14u, (volMask & %uu) != 0u || %uu == 13u); }\n",
                    st, inSt, dm, 1u << st, st, st, st, st, st, st, 1u << st, tm, 1u << st, tm);
                s += line;
                break;
            case 0x0Bu: // PS_TEXTUREMODES_DOT_RFLCT_DIFF
            case 0x0Cu: // PS_TEXTUREMODES_DOT_RFLCT_SPEC (texm3x3vspec)
            case 0x12u: // PS_TEXTUREMODES_DOT_RFLCT_SPEC_CONST (texm3x3spec)
                snprintf(line, sizeof(line),
                    "    { float dp = dot(uv%d.xyz, MapDotVec(t%u, %uu));\n"
                    "      if (%d == 2) dotVec.y = dp; else dotVec.z = dp;\n"
                    "      float3 Np = (dot(dotVec, dotVec) > 1e-6f) ? normalize(dotVec) : float3(0.0f, 0.0f, 1.0f);\n"
                    "      float3 Vw = (%uu == 18u) ? uRcStageC0[0].xyz : float3(uv1.w, uv2.w, uv3.w);\n"
                    "      float3 R  = (%uu == 11u) ? Np : ((dot(Vw, Vw) > 1e-6f) ? reflect(-normalize(Vw), Np) : Np);\n"
                    "      if ((texMask & %uu) != 0u) t%d = ((cubeMask & %uu) != 0u) ? cubeTex%d.Sample(smp%d, R) : tex%d.Sample(smp%d, float2(R.x * 0.5f + 0.5f, 0.5f - R.y * 0.5f)); }\n",
                    st, inSt, dm, st, tm, tm, 1u << st, st, 1u << st, st, st, st, st);
                s += line;
                break;
            default:
                snprintf(line, sizeof(line),
                    "    if ((texMask & %uu) != 0u) t%d = SampleStageTex(%uu, tex%d, cubeTex%d, volTex%d, smp%d, uv%d, (cubeMask & %uu) != 0u, (volMask & %uu) != 0u);\n",
                    1u << st, st, st, st, st, st, st, st, 1u << st, 1u << st);
                s += line;
                break;
        }
    }

    // 2. Initialize Register Combiner registers
    s += "    float fogFactor = 1.0f;\n"
         "    if (uFogParams.x > 0.5f && (uFlags.x & 1u) == 0u) {\n"
         "        float d = input.eyeDist.x;\n"
         "        if (uFogParams.x > 3.5f) fogFactor = saturate(d);\n"
         "        else if (uFogParams.x > 2.5f) fogFactor = saturate((uFogParams.z - d) / max(uFogParams.z - uFogParams.y, 0.001f));\n"
         "        else if (uFogParams.x < 1.5f) fogFactor = saturate(exp(-d * uFogParams.w));\n"
         "        else { float df = d * uFogParams.w; fogFactor = saturate(exp(-(df * df))); }\n"
         "    }\n"
         "    float4 fogReg = float4(uFogColor.rgb, fogFactor);\n"
         "    float4 v0 = input.color;\n"
         "    float4 v1 = float4(input.norm, 1.0f);\n"
         "    float4 r0 = float4(t0.a, t0.a, t0.a, t0.a);\n"
         "    float4 r1 = float4(0.0f, 0.0f, 0.0f, 0.0f);\n"
         "    float4 sumReg = float4(0.0f, 0.0f, 0.0f, 0.0f);\n"
         "    float4 efProd = float4(0.0f, 0.0f, 0.0f, 0.0f);\n";

    // 3. Emit General Combiner Stages 0 .. combCount-1
    for (uint32_t st = 0; st < combCount; ++st) {
        uint32_t rgbIn    = xboxRS[XBOX_D3DRS_PSRGBINPUTS0    + st];
        uint32_t alphaIn  = xboxRS[XBOX_D3DRS_PSALPHAINPUTS0  + st];
        uint32_t rgbOut   = xboxRS[XBOX_D3DRS_PSRGBOUTPUTS0   + st];
        uint32_t alphaOut = xboxRS[XBOX_D3DRS_PSALPHAOUTPUTS0 + st];

        char cA[160], cB[160], cC[160], cD[160];
        FormatRcInputExpr((uint8_t)((rgbIn >> 24) & 0xFFu), false, (int)st, false, cA, sizeof(cA));
        FormatRcInputExpr((uint8_t)((rgbIn >> 16) & 0xFFu), false, (int)st, false, cB, sizeof(cB));
        FormatRcInputExpr((uint8_t)((rgbIn >> 8)  & 0xFFu), false, (int)st, false, cC, sizeof(cC));
        FormatRcInputExpr((uint8_t)( rgbIn        & 0xFFu), false, (int)st, false, cD, sizeof(cD));

        char aA[160], aB[160], aC[160], aD[160];
        FormatRcInputExpr((uint8_t)((alphaIn >> 24) & 0xFFu), true, (int)st, false, aA, sizeof(aA));
        FormatRcInputExpr((uint8_t)((alphaIn >> 16) & 0xFFu), true, (int)st, false, aB, sizeof(aB));
        FormatRcInputExpr((uint8_t)((alphaIn >> 8)  & 0xFFu), true, (int)st, false, aC, sizeof(aC));
        FormatRcInputExpr((uint8_t)( alphaIn        & 0xFFu), true, (int)st, false, aD, sizeof(aD));

        bool abDot  = ((rgbOut >> 12) & 1u) != 0u;
        bool cdDot  = ((rgbOut >> 13) & 1u) != 0u;
        bool rgbMux = ((rgbOut >> 14) & 1u) != 0u;
        bool alpMux = ((alphaOut >> 14) & 1u) != 0u;

        s += "    {\n";
        snprintf(line, sizeof(line), "        float3 rgbA = %s, rgbB = %s, rgbC = %s, rgbD = %s;\n", cA, cB, cC, cD);
        s += line;
        snprintf(line, sizeof(line), "        float  alpA = %s, alpB = %s, alpC = %s, alpD = %s;\n", aA, aB, aC, aD);
        s += line;
        snprintf(line, sizeof(line), "        float3 rawAB_rgb = %s;\n", abDot ? "dot(rgbA, rgbB).xxx" : "(rgbA * rgbB)");
        s += line;
        snprintf(line, sizeof(line), "        float3 rawCD_rgb = %s;\n", cdDot ? "dot(rgbC, rgbD).xxx" : "(rgbC * rgbD)");
        s += line;
        if (rgbMux) {
            snprintf(line, sizeof(line), "        float3 rawSum_rgb = (%s) ? rawCD_rgb : rawAB_rgb;\n",
                     muxMsb ? "(r0.a >= 0.5f)" : "(((int)(saturate(r0.a) * 255.0f + 0.5f) & 1) != 0)");
        } else {
            snprintf(line, sizeof(line), "        float3 rawSum_rgb = rawAB_rgb + rawCD_rgb;\n");
        }
        s += line;

        s += "        float rawAB_a = alpA * alpB;\n"
             "        float rawCD_a = alpC * alpD;\n";
        if (alpMux) {
            snprintf(line, sizeof(line), "        float rawSum_a = (%s) ? rawCD_a : rawAB_a;\n",
                     muxMsb ? "(r0.a >= 0.5f)" : "(((int)(saturate(r0.a) * 255.0f + 0.5f) & 1) != 0)");
        } else {
            snprintf(line, sizeof(line), "        float rawSum_a = rawAB_a + rawCD_a;\n");
        }
        s += line;

        char scaledAB_rgb[128], scaledCD_rgb[128], scaledSum_rgb[128];
        FormatRcScaleBias("rawAB_rgb",  rgbOut, scaledAB_rgb,  sizeof(scaledAB_rgb));
        FormatRcScaleBias("rawCD_rgb",  rgbOut, scaledCD_rgb,  sizeof(scaledCD_rgb));
        FormatRcScaleBias("rawSum_rgb", rgbOut, scaledSum_rgb, sizeof(scaledSum_rgb));

        char scaledAB_a[128], scaledCD_a[128], scaledSum_a[128];
        FormatRcScaleBias("rawAB_a",  alphaOut, scaledAB_a,  sizeof(scaledAB_a));
        FormatRcScaleBias("rawCD_a",  alphaOut, scaledCD_a,  sizeof(scaledCD_a));
        FormatRcScaleBias("rawSum_a", alphaOut, scaledSum_a, sizeof(scaledSum_a));

        if (const char* dst = GetRcDestRegName(rgbOut & 0xFu)) {
            snprintf(line, sizeof(line), "        %s.rgb = %s;\n", dst, scaledAB_rgb);
            s += line;
        }
        if (const char* dst = GetRcDestRegName((rgbOut >> 4) & 0xFu)) {
            snprintf(line, sizeof(line), "        %s.rgb = %s;\n", dst, scaledCD_rgb);
            s += line;
        }
        if (const char* dst = GetRcDestRegName((rgbOut >> 8) & 0xFu)) {
            snprintf(line, sizeof(line), "        %s.rgb = %s;\n", dst, scaledSum_rgb);
            s += line;
        }

        if (const char* dst = GetRcDestRegName(alphaOut & 0xFu)) {
            snprintf(line, sizeof(line), "        %s.a = %s;\n", dst, scaledAB_a);
            s += line;
        }
        if (const char* dst = GetRcDestRegName((alphaOut >> 4) & 0xFu)) {
            snprintf(line, sizeof(line), "        %s.a = %s;\n", dst, scaledCD_a);
            s += line;
        }
        if (const char* dst = GetRcDestRegName((alphaOut >> 8) & 0xFu)) {
            snprintf(line, sizeof(line), "        %s.a = %s;\n", dst, scaledSum_a);
            s += line;
        }
        s += "    }\n";
    }

    // 4. Emit Final Combiner (PSFinalCombinerInputsABCD = RS[8], PSFinalCombinerInputsEFG = RS[9])
    uint32_t fcABCD = xboxRS[XBOX_D3DRS_PSFINALCOMBINERINPUTSABCD];
    uint32_t fcEFG  = xboxRS[XBOX_D3DRS_PSFINALCOMBINERINPUTSEFG];
    if (fcABCD != 0u || fcEFG != 0u) {
        bool clampSum = (fcEFG & 0x80u) != 0u;
        snprintf(line, sizeof(line), "    sumReg = %s;\n", clampSum ? "saturate(r0 + v1)" : "(r0 + v1)");
        s += line;

        char fcE[160], fcF[160], fcA[160], fcB[160], fcC[160], fcD[160], fcG[160];
        FormatRcInputExpr((uint8_t)((fcEFG  >> 24) & 0xFFu), false, 8, true, fcE, sizeof(fcE));
        FormatRcInputExpr((uint8_t)((fcEFG  >> 16) & 0xFFu), false, 8, true, fcF, sizeof(fcF));
        snprintf(line, sizeof(line), "    efProd = float4((%s) * (%s), 1.0f);\n", fcE, fcF);
        s += line;

        FormatRcInputExpr((uint8_t)((fcABCD >> 24) & 0xFFu), false, 8, true, fcA, sizeof(fcA));
        FormatRcInputExpr((uint8_t)((fcABCD >> 16) & 0xFFu), false, 8, true, fcB, sizeof(fcB));
        FormatRcInputExpr((uint8_t)((fcABCD >> 8)  & 0xFFu), false, 8, true, fcC, sizeof(fcC));
        FormatRcInputExpr((uint8_t)( fcABCD        & 0xFFu), false, 8, true, fcD, sizeof(fcD));
        FormatRcInputExpr((uint8_t)((fcEFG  >> 8)  & 0xFFu), true,  8, true, fcG, sizeof(fcG));

        snprintf(line, sizeof(line),
            "    float3 finalA = %s, finalB = %s, finalC = %s, finalD = %s;\n"
            "    float4 current;\n"
            "    current.rgb = saturate(finalA * finalB + (1.0f - finalA) * finalC + finalD);\n"
            "    current.a   = saturate(%s);\n",
            fcA, fcB, fcC, fcD, fcG);
        s += line;
    } else {
        s += "    float4 current = saturate(r0);\n";
    }

    // 5. Alpha Test, Fog (if not already consumed in final combiner), and Gamma Ramp LUT
    bool fcUsesFog =
        (((fcABCD >> 24) & 0xFu) == 0x3u) ||
        (((fcABCD >> 16) & 0xFu) == 0x3u) ||
        (((fcABCD >> 8)  & 0xFu) == 0x3u) ||
        (( fcABCD        & 0xFu) == 0x3u) ||
        (((fcEFG  >> 8)  & 0xFu) == 0x3u);

    s += "    if (uFlags.z != 0u) {\n"
         "        uint fn = (uint)uAlphaTest.x; float refVal = uAlphaTest.y; bool passAlpha = true;\n"
         "        if (fn == 1u) passAlpha = false;\n"
         "        else if (fn == 2u) passAlpha = (current.a < refVal - 0.001f);\n"
         "        else if (fn == 3u) passAlpha = (abs(current.a - refVal) < 0.004f);\n"
         "        else if (fn == 4u) passAlpha = (current.a <= refVal + 0.001f);\n"
         "        else if (fn == 5u) passAlpha = (current.a > refVal + 0.001f);\n"
         "        else if (fn == 6u) passAlpha = (abs(current.a - refVal) >= 0.004f);\n"
         "        else if (fn == 7u) passAlpha = (current.a >= refVal - 0.001f);\n"
         "        if (!passAlpha) discard;\n"
         "    }\n";

    if (!fcUsesFog) {
        s += "    if (uFogParams.x > 0.5f && (uFlags.x & 1u) == 0u) {\n"
             "        if (uFogColor.w > 0.5f) current.rgb *= fogFactor;\n"
             "        else current.rgb = lerp(current.rgb, uFogColor.rgb, 1.0f - fogFactor);\n"
             "    }\n";
    }

    s += "    if ((uFlags.x & 2u) != 0u) {\n"
         "        int3 lutIdx = clamp(int3(saturate(current.rgb) * 255.0f + 0.5f), 0, 255);\n"
         "        current.rgb = float3(gammaLutTex.Load(int3(lutIdx.r, 0, 0)).r,\n"
         "                             gammaLutTex.Load(int3(lutIdx.g, 0, 0)).g,\n"
         "                             gammaLutTex.Load(int3(lutIdx.b, 0, 0)).b);\n"
         "    } else if (uAlphaTest.z > 0.1f && (abs(uAlphaTest.z - 1.0f) > 0.01f || abs(uAlphaTest.w - 1.0f) > 0.01f)) {\n"
         "        current.rgb = pow(max(current.rgb, 0.0f), 1.0f / uAlphaTest.z) * uAlphaTest.w;\n"
         "    }\n"
         "    return current;\n"
         "}\n";
    return s;
}

static bool IsRegisterCombinerStateUnmodifiedFromDef(const uint32_t* xboxRS, const XboxPixelShaderDef* pDef) {
    if (!pDef) return false;
    const uint32_t* dwords = reinterpret_cast<const uint32_t*>(pDef);
    for (int i = 0; i <= 9; ++i)   if (xboxRS[i] != dwords[i]) return false;
    for (int i = 26; i <= 42; ++i) if (xboxRS[i] != dwords[i]) return false;
    for (int i = 45; i <= 53; ++i) if (xboxRS[i] != dwords[i]) return false;
    if (xboxRS[XBOX_D3DRS_PSTEXTUREMODES] != dwords[54]) return false;
    if (xboxRS[55] != dwords[55] || xboxRS[56] != dwords[56]) return false;
    return true;
}

ID3D11PixelShader* GetActiveRegisterCombinerPS() {
    if (!g_d3dDevice) return g_ps;
    const uint32_t* xboxRS = reinterpret_cast<const uint32_t*>(ADDR_D3D_RENDERSTATE);

    if (g_currentPsHandle != 0) {
        int32_t engType = -1;
        const XboxPixelShaderDef* pDef = GetHandlePSDef(g_currentPsHandle, &engType);
        if (engType >= 0 && engType < 28 && IsRegisterCombinerStateUnmodifiedFromDef(xboxRS, pDef)) {
            return GetPixelShaderPermutation(static_cast<uint32_t>(engType + 1));
        }
    } else if ((xboxRS[XBOX_D3DRS_PSCOMBINERCOUNT] & 0x0Fu) == 0u) {
        return GetPixelShaderPermutation(0u);
    }

    // Compute 64-bit FNV-1a hash of the active NV2A Register Combiner structure in _D3D__RenderState
    uint64_t h = 14695981039346656037ULL;
    auto mix = [&](uint32_t v) {
        h ^= v;
        h *= 1099511628211ULL;
    };
    uint32_t combCount = xboxRS[XBOX_D3DRS_PSCOMBINERCOUNT] & 0x0Fu;
    if (combCount == 0u) combCount = 1u;
    if (combCount > 8u)  combCount = 8u;
    for (uint32_t i = 0; i < combCount; ++i) {
        mix(xboxRS[XBOX_D3DRS_PSALPHAINPUTS0  + i]);
        mix(xboxRS[XBOX_D3DRS_PSALPHAOUTPUTS0 + i]);
        mix(xboxRS[XBOX_D3DRS_PSRGBINPUTS0    + i]);
        mix(xboxRS[XBOX_D3DRS_PSRGBOUTPUTS0   + i]);
    }
    mix(xboxRS[XBOX_D3DRS_PSFINALCOMBINERINPUTSABCD]);
    mix(xboxRS[XBOX_D3DRS_PSFINALCOMBINERINPUTSEFG]);
    mix(xboxRS[XBOX_D3DRS_PSCOMPAREMODE]);
    mix(xboxRS[XBOX_D3DRS_PSCOMBINERCOUNT]);
    mix(xboxRS[XBOX_D3DRS_PSTEXTUREMODES]);
    mix(xboxRS[XBOX_D3DRS_PSDOTMAPPING]);
    mix(xboxRS[XBOX_D3DRS_PSINPUTTEXTURE]);

    for (size_t i = 0; i < g_rcPsCache.size(); ++i) {
        if (g_rcPsCache[i].hash == h) {
            return g_rcPsCache[i].ps;
        }
    }

    std::string hlsl = DecompileRegisterCombinersToHLSL(xboxRS);
    ID3DBlob* psBlob  = nullptr;
    ID3DBlob* errBlob = nullptr;
    ID3D11PixelShader* compiledPS = nullptr;
    HRESULT hr = D3DCompile(
        hlsl.data(), hlsl.size(), "XboxRegCombinerPS",
        nullptr, nullptr, "PSMain", "ps_4_0",
        D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &psBlob, &errBlob);
    if (SUCCEEDED(hr) && psBlob) {
        g_d3dDevice->CreatePixelShader(
            psBlob->GetBufferPointer(), psBlob->GetBufferSize(),
            nullptr, &compiledPS);
        psBlob->Release();
    }
    if (errBlob) errBlob->Release();
    if (!compiledPS) compiledPS = g_ps;
    g_rcPsCache.push_back({ h, compiledPS });
    return compiledPS;
}

struct XboxDeclElem {
    uint8_t  stream;
    uint8_t  reg;
    uint8_t  xType;
    uint16_t offset;
};

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

static DXGI_FORMAT MapXboxDeclTypeToDXGI(uint8_t xType) {
    switch (xType) {
        case 0x12: return DXGI_FORMAT_R32_FLOAT;
        case 0x22: return DXGI_FORMAT_R32G32_FLOAT;
        case 0x32: return DXGI_FORMAT_R32G32B32_FLOAT;
        case 0x42: return DXGI_FORMAT_R32G32B32A32_FLOAT;
        case 0x40: return DXGI_FORMAT_B8G8R8A8_UNORM;
        case 0x41: return DXGI_FORMAT_R8G8B8A8_UINT;
        case 0x25: return DXGI_FORMAT_R16G16_SINT;
        case 0x45: return DXGI_FORMAT_R16G16B16A16_SINT;
        case 0x24: return DXGI_FORMAT_R16G16_SNORM;
        case 0x44: return DXGI_FORMAT_R16G16B16A16_SNORM;
        default:   return DXGI_FORMAT_R32G32B32_FLOAT;
    }
}

static void FormatXvuSwizzle(uint32_t swz, char* outBuf) {
    static const char kComp[4] = { 'x', 'y', 'z', 'w' };
    uint32_t x = (swz >> 6) & 3u;
    uint32_t y = (swz >> 4) & 3u;
    uint32_t z = (swz >> 2) & 3u;
    uint32_t w = swz & 3u;
    if (x == 0u && y == 1u && z == 2u && w == 3u) {
        outBuf[0] = '\0';
        return;
    }
    outBuf[0] = '.';
    outBuf[1] = kComp[x];
    outBuf[2] = kComp[y];
    outBuf[3] = kComp[z];
    outBuf[4] = kComp[w];
    outBuf[5] = '\0';
}

static void FormatXvuWriteMask(uint32_t mask, char* outBuf) {
    static const char kComp[4] = { 'x', 'y', 'z', 'w' };
    int pos = 0;
    for (int i = 0; i < 4; ++i) {
        if (mask & (1u << (3 - i))) {
            outBuf[pos++] = kComp[i];
        }
    }
    outBuf[pos] = '\0';
}

static void FormatXvuSrcOperand(
    uint32_t src, uint32_t reg, uint32_t neg, uint32_t swz,
    uint32_t va, uint32_t cIdx, uint32_t rel, char* outBuf, size_t outSize)
{
    char base[64];
    if (src == 1u) {
        snprintf(base, sizeof(base), "r%u", (reg <= 12u) ? reg : 0u);
    } else if (src == 2u) {
        snprintf(base, sizeof(base), "v%u", va & 15u);
    } else if (src == 3u) {
        if (rel != 0u) {
            snprintf(base, sizeof(base), "c[clamp(a0x + %u, 0, 191)]", cIdx);
        } else {
            snprintf(base, sizeof(base), "c[%u]", (cIdx < 192u) ? cIdx : 191u);
        }
    } else {
        snprintf(base, sizeof(base), "float4(0.0f, 0.0f, 0.0f, 0.0f)");
    }

    char swzStr[8];
    FormatXvuSwizzle(swz, swzStr);
    if (neg != 0u) {
        snprintf(outBuf, outSize, "(-%s%s)", base, swzStr);
    } else {
        snprintf(outBuf, outSize, "%s%s", base, swzStr);
    }
}

// Decompiles 128-bit Xbox NV2A .xvu vertex shader microcode into D3D11 HLSL (vs_4_0)
static std::string DecompileXvuToHLSL(const XboxDeclElem* elems, uint32_t numElems, const DWORD* pFunction) {
    uint32_t count = (pFunction[0] >> 16) & 0xFFFFu;
    bool hasC58     = false;
    bool writesOD1  = false;
    bool writesOFog = false;

    for (uint32_t i = 0; i < count; ++i) {
        uint32_t w1 = pFunction[1 + i * 4 + 1];
        uint32_t w2 = pFunction[1 + i * 4 + 2];
        uint32_t w3 = pFunction[1 + i * 4 + 3];
        uint32_t cIdx = (w1 >> 13) & 0xFFu;
        uint32_t aSrc = (w2 >> 26) & 3u;
        uint32_t bSrc = (w2 >> 11) & 3u;
        uint32_t cSrc = (w3 >> 28) & 3u;
        uint32_t rel  = (w3 >> 1)  & 1u;
        if ((aSrc == 3u || bSrc == 3u || cSrc == 3u) && rel == 0u && (cIdx == 58u || cIdx == 59u)) {
            hasC58 = true;
        }
        uint32_t outMask  = (w3 >> 12) & 0xFu;
        uint32_t outWrite = (w3 >> 11) & 1u;
        uint32_t outReg   = (w3 >> 3)  & 0xFFu;
        if (outWrite != 0u && outMask != 0u) {
            if (outReg == 4u) writesOD1 = true;
            if (outReg == 5u) writesOFog = true;
        }
    }

    std::string s;
    s.reserve(8192);
    s += "cbuffer PerDrawCB : register(b0) {\n"
         "    float4 uWvp0; float4 uWvp1; float4 uWvp2; float4 uWvp3;\n"
         "    float4 uViewport; float4 uTFactor;\n"
         "    uint4 uStage0Ops; uint4 uStage0Alpha;\n"
         "    uint4 uStage1Ops; uint4 uStage1Alpha;\n"
         "    uint4 uStage2Ops; uint4 uStage2Alpha;\n"
         "    uint4 uStage3Ops; uint4 uStage3Alpha;\n"
         "    uint4 uFlags;\n"
         "    float4 uAlphaTest;\n"
         "    float4 uUvScale01;\n"
         "    float4 uUvScale23;\n"
         "};\n"
         "cbuffer VSConstantsCB : register(b1) {\n"
         "    float4 c[192];\n"
         "};\n"
         "struct VSIn {\n";

    bool declaredReg[16] = {};
    char line[512];
    for (uint32_t i = 0; i < numElems; ++i) {
        uint8_t r = elems[i].reg;
        if (r >= 16 || declaredReg[r]) continue;
        declaredReg[r] = true;
        const char* hlslType = "float3";
        switch (elems[i].xType) {
            case 0x12: hlslType = "float";  break;
            case 0x22:
            case 0x24: hlslType = "float2"; break;
            case 0x25: hlslType = "int2";   break;
            case 0x32: hlslType = "float3"; break;
            case 0x41: hlslType = "uint4";  break;
            case 0x45: hlslType = "int4";   break;
            default:   hlslType = "float4"; break;
        }
        snprintf(line, sizeof(line), "    %s raw_v%u : ATTR%u;\n", hlslType, r, r);
        s += line;
    }
    if (numElems == 0) {
        s += "    uint vid : SV_VertexID;\n";
    }
    s += "};\n"
         "struct VSOut {\n"
         "    float4 pos     : SV_POSITION;\n"
         "    float4 color   : COLOR0;\n"
         "    float4 uv0     : TEXCOORD0;\n"
         "    float4 uv1     : TEXCOORD1;\n"
         "    float4 uv2     : TEXCOORD2;\n"
         "    float4 uv3     : TEXCOORD3;\n"
         "    float2 eyeDist : TEXCOORD4;\n"
         "    float3 norm    : TEXCOORD5;\n"
         "};\n"
         "float nv_rcp(float x) { return (abs(x) > 1e-12f) ? clamp(1.0f / x, -1e12f, 1e12f) : 1e12f; }\n"
         "float nv_rsq(float x) { return (x > 1e-12f) ? clamp(rsqrt(x), 0.0f, 1e12f) : 0.0f; }\n"
         "float nv_rcc(float x) { float t = (abs(x) > 1e-12f) ? (1.0f / x) : 0.0f; return (t >= 0.0f) ? clamp(t, 5.42101086e-20f, 1.884467e19f) : clamp(t, -1.884467e19f, -5.42101086e-20f); }\n"
         "float4 nv_lit(float4 v) { float x = max(v.x, 0.0f); float y = (v.x > 0.0f) ? pow(max(v.y, 0.0f), clamp(v.w, -128.0f, 128.0f)) : 0.0f; return float4(1.0f, x, y, 1.0f); }\n"
         "VSOut VSMain(VSIn In) {\n";

    bool unpackedReg[16] = {};
    for (uint32_t i = 0; i < numElems; ++i) {
        uint8_t r = elems[i].reg;
        if (r >= 16 || unpackedReg[r]) continue;
        unpackedReg[r] = true;
        switch (elems[i].xType) {
            case 0x12:
                snprintf(line, sizeof(line), "    float4 v%u = float4(In.raw_v%u, 0.0f, 0.0f, 1.0f);\n", r, r);
                break;
            case 0x22:
            case 0x24:
            case 0x25:
                snprintf(line, sizeof(line), "    float4 v%u = float4(In.raw_v%u, 0.0f, 1.0f);\n", r, r);
                break;
            case 0x32:
                snprintf(line, sizeof(line), "    float4 v%u = float4(In.raw_v%u, 1.0f);\n", r, r);
                break;
            default:
                snprintf(line, sizeof(line), "    float4 v%u = float4(In.raw_v%u);\n", r, r);
                break;
        }
        s += line;
    }
    for (uint32_t r = 0; r < 16; ++r) {
        if (!unpackedReg[r]) {
            snprintf(line, sizeof(line), "    float4 v%u = float4(0.0f, 0.0f, 0.0f, 1.0f);\n", r);
            s += line;
        }
    }

    s += "    float4 r0 = 0, r1 = 0, r2 = 0, r3 = 0, r4 = 0, r5 = 0, r6 = 0, r7 = 0, r8 = 0, r9 = 0, r10 = 0, r11 = 0;\n"
         "    float4 r12 = float4(0.0f, 0.0f, 0.0f, 1.0f);\n"
         "    int a0x = 0;\n"
         "    float4 clipPos = float4(0.0f, 0.0f, 0.0f, 1.0f);\n"
         "    float4 oD0 = float4(1.0f, 1.0f, 1.0f, 1.0f), oD1 = 0, oFog = float4(1.0f, 0.0f, 0.0f, 0.0f), oPts = float4(1.0f, 0.0f, 0.0f, 0.0f), oB0 = 0, oB1 = 0;\n"
         "    float4 oT0 = float4(0.0f, 0.0f, 0.0f, 1.0f), oT1 = float4(0.0f, 0.0f, 0.0f, 1.0f), oT2 = float4(0.0f, 0.0f, 0.0f, 1.0f), oT3 = float4(0.0f, 0.0f, 0.0f, 1.0f);\n"
         "    float4 macVal = 0, iluVal = 0;\n";

    for (uint32_t i = 0; i < count; ++i) {
        uint32_t w1 = pFunction[1 + i * 4 + 1];
        uint32_t w2 = pFunction[1 + i * 4 + 2];
        uint32_t w3 = pFunction[1 + i * 4 + 3];

        uint32_t ilu  = (w1 >> 25) & 0x7u;
        uint32_t mac  = (w1 >> 21) & 0xFu;
        uint32_t cIdx = (w1 >> 13) & 0xFFu;
        uint32_t va   = (w1 >> 9)  & 0xFu;

        uint32_t aNeg = (w1 >> 8)  & 1u;
        uint32_t aSwz =  w1        & 0xFFu;
        uint32_t aR   = (w2 >> 28) & 0xFu;
        uint32_t aSrc = (w2 >> 26) & 3u;

        uint32_t bNeg = (w2 >> 25) & 1u;
        uint32_t bSwz = (w2 >> 17) & 0xFFu;
        uint32_t bR   = (w2 >> 13) & 0xFu;
        uint32_t bSrc = (w2 >> 11) & 3u;

        uint32_t cNeg = (w2 >> 10) & 1u;
        uint32_t cSwz = (w2 >> 2)  & 0xFFu;
        uint32_t cR   = ((w2 & 3u) << 2) | ((w3 >> 30) & 3u);
        uint32_t cSrc = (w3 >> 28) & 3u;

        uint32_t macMask  = (w3 >> 24) & 0xFu;
        uint32_t macR     = (w3 >> 20) & 0xFu;
        uint32_t iluMask  = (w3 >> 16) & 0xFu;
        uint32_t outMask  = (w3 >> 12) & 0xFu;
        uint32_t outWrite = (w3 >> 11) & 1u;
        uint32_t outReg   = (w3 >> 3)  & 0xFFu;
        uint32_t outMux   = (w3 >> 2)  & 1u;
        uint32_t rel      = (w3 >> 1)  & 1u;

        char sA[128], sB[128], sC[128];
        FormatXvuSrcOperand(aSrc, aR, aNeg, aSwz, va, cIdx, rel, sA, sizeof(sA));
        FormatXvuSrcOperand(bSrc, bR, bNeg, bSwz, va, cIdx, rel, sB, sizeof(sB));
        FormatXvuSrcOperand(cSrc, cR, cNeg, cSwz, va, cIdx, rel, sC, sizeof(sC));

        if (mac == 13u) {
            snprintf(line, sizeof(line), "    a0x = (int)floor((%s).x);\n", sA);
            s += line;
        } else if (mac != 0u) {
            switch (mac) {
                case 1u:  snprintf(line, sizeof(line), "    macVal = %s;\n", sA); break;
                case 2u:  snprintf(line, sizeof(line), "    macVal = (%s * %s);\n", sA, sB); break;
                case 3u:  snprintf(line, sizeof(line), "    macVal = (%s + %s);\n", sA, sC); break;
                case 4u:  snprintf(line, sizeof(line), "    macVal = (%s * %s + %s);\n", sA, sB, sC); break;
                case 5u:  snprintf(line, sizeof(line), "    macVal = dot((%s).xyz, (%s).xyz).xxxx;\n", sA, sB); break;
                case 6u:  snprintf(line, sizeof(line), "    macVal = dot(float4((%s).xyz, 1.0f), %s).xxxx;\n", sA, sB); break;
                case 7u:  snprintf(line, sizeof(line), "    macVal = dot(%s, %s).xxxx;\n", sA, sB); break;
                case 8u:  snprintf(line, sizeof(line), "    macVal = float4(1.0f, (%s).y * (%s).y, (%s).z, (%s).w);\n", sA, sB, sA, sB); break;
                case 9u:  snprintf(line, sizeof(line), "    macVal = min(%s, %s);\n", sA, sB); break;
                case 10u: snprintf(line, sizeof(line), "    macVal = max(%s, %s);\n", sA, sB); break;
                case 11u: snprintf(line, sizeof(line), "    macVal = ((%s < %s) ? float4(1,1,1,1) : float4(0,0,0,0));\n", sA, sB); break;
                case 12u: snprintf(line, sizeof(line), "    macVal = ((%s >= %s) ? float4(1,1,1,1) : float4(0,0,0,0));\n", sA, sB); break;
                default:  snprintf(line, sizeof(line), "    macVal = float4(0,0,0,0);\n"); break;
            }
            s += line;
        }

        if (ilu != 0u) {
            switch (ilu) {
                case 1u: snprintf(line, sizeof(line), "    iluVal = %s;\n", sC); break;
                case 2u: snprintf(line, sizeof(line), "    iluVal = nv_rcp((%s).x).xxxx;\n", sC); break;
                case 3u: snprintf(line, sizeof(line), "    iluVal = nv_rcc((%s).x).xxxx;\n", sC); break;
                case 4u: snprintf(line, sizeof(line), "    iluVal = nv_rsq((%s).x).xxxx;\n", sC); break;
                case 5u: snprintf(line, sizeof(line), "    iluVal = exp2((%s).x).xxxx;\n", sC); break;
                case 6u: snprintf(line, sizeof(line), "    iluVal = log2(max(abs((%s).x), 1e-12f)).xxxx;\n", sC); break;
                case 7u: snprintf(line, sizeof(line), "    iluVal = nv_lit(%s);\n", sC); break;
                default: snprintf(line, sizeof(line), "    iluVal = float4(0,0,0,0);\n"); break;
            }
            s += line;
        }

        char maskStr[8];
        if (mac != 0u && mac != 13u && macMask != 0u && macR <= 12u) {
            FormatXvuWriteMask(macMask, maskStr);
            snprintf(line, sizeof(line), "    r%u.%s = macVal.%s;\n", macR, maskStr, maskStr);
            s += line;
        }
        if (ilu != 0u && iluMask != 0u) {
            uint32_t iluR = (mac == 0u && macR <= 12u) ? macR : 1u;
            FormatXvuWriteMask(iluMask, maskStr);
            snprintf(line, sizeof(line), "    r%u.%s = iluVal.%s;\n", iluR, maskStr, maskStr);
            s += line;
        }
        if (outWrite != 0u && outMask != 0u) {
            const char* outName = nullptr;
            switch (outReg) {
                case 0u:  outName = "r12";  break;
                case 3u:  outName = "oD0";  break;
                case 4u:  outName = "oD1";  break;
                case 5u:  outName = "oFog"; break;
                case 6u:  outName = "oPts"; break;
                case 7u:  outName = "oB0";  break;
                case 8u:  outName = "oB1";  break;
                case 9u:  outName = "oT0";  break;
                case 10u: outName = "oT1";  break;
                case 11u: outName = "oT2";  break;
                case 12u: outName = "oT3";  break;
                default:  break;
            }
            if (outName) {
                const char* srcVal = (outMux != 0u) ? "iluVal" : "macVal";
                FormatXvuWriteMask(outMask, maskStr);
                snprintf(line, sizeof(line), "    %s.%s = %s.%s;\n", outName, maskStr, srcVal, maskStr);
                s += line;
                if (outReg == 0u && hasC58 && !(rel == 0u && (cIdx == 58u || cIdx == 59u))) {
                    snprintf(line, sizeof(line), "    clipPos.%s = %s.%s;\n", maskStr, srcVal, maskStr);
                    s += line;
                }
            }
        }
    }

    s += "    VSOut o = (VSOut)0;\n";
    if (hasC58) {
        s += "    o.pos = clipPos;\n";
        if (writesOFog) {
            s += "    o.eyeDist = float2(max(oFog.x, 0.0f), 1.0f);\n";
        } else {
            s += "    o.eyeDist = float2(max(clipPos.w, 0.0f), 1.0f);\n";
        }
    } else {
        s += "    o.pos = float4(((r12.x - uViewport.x) / max(uViewport.z, 1.0f)) * 2.0f - 1.0f,\n"
             "                   1.0f - ((r12.y - uViewport.y) / max(uViewport.w, 1.0f)) * 2.0f,\n"
             "                   saturate(r12.z), 1.0f);\n";
        if (writesOFog) {
            s += "    o.eyeDist = float2(max(oFog.x, 0.0f), 1.0f);\n";
        } else {
            s += "    o.eyeDist = float2(0.0f, 1.0f);\n";
        }
    }
    s += "    o.color = saturate(oD0);\n"
         "    o.uv0   = float4(oT0.xy * uUvScale01.xy, oT0.zw);\n"
         "    o.uv1   = float4(oT1.xy * uUvScale01.zw, oT1.zw);\n"
         "    o.uv2   = float4(oT2.xy * uUvScale23.xy, oT2.zw);\n"
         "    o.uv3   = float4(oT3.xy * uUvScale23.zw, oT3.zw);\n"
         "    o.norm  = saturate(oD1.xyz);\n";

    if (writesOD1) {
        s += "    if ((uStage0Ops.x == 25u && (uStage0Ops.z & 0x0Fu) == 0u) ||\n"
             "        (uStage1Ops.x == 25u && (uStage1Ops.z & 0x0Fu) == 0u) ||\n"
             "        (uStage2Ops.x == 25u && (uStage2Ops.z & 0x0Fu) == 0u) ||\n"
             "        (uStage3Ops.x == 25u && (uStage3Ops.z & 0x0Fu) == 0u)) {\n"
             "        o.color = float4(saturate(oD1.xyz), saturate(oD0.w));\n"
             "    }\n";
    }

    s += "    return o;\n"
         "}\n";
    return s;
}

static uint8_t GetXboxDeclTypeComponentCount(uint32_t xType) {
    switch (xType) {
        case 0x12: return 1; // FLOAT1
        case 0x22:
        case 0x24:
        case 0x25: return 2; // FLOAT2 / SHORT2 / NORMSHORT2
        case 0x32: return 3; // FLOAT3
        default:   return 4; // FLOAT4 / D3DCOLOR / SHORT4
    }
}

static HRESULT __stdcall Bridge_D3DDevice_CreateVertexShader(
    const DWORD* pDeclaration, const DWORD* pFunction, DWORD* pHandle, DWORD Usage)
{
    (void)Usage;
    if (!pHandle) return E_INVALIDARG;

    XboxShaderDeclInfo info = {};
    info.vsType = -1;

    uintptr_t hAddr = reinterpret_cast<uintptr_t>(pHandle);
    if (hAddr >= ADDR_VERTEX_SHADERS_TABLE && hAddr < ADDR_VERTEX_SHADERS_TABLE + 66 * 24) {
        info.vsType = (int32_t)((hAddr - ADDR_VERTEX_SHADERS_TABLE) / 24);
    }

    XboxDeclElem elems[16] = {};
    uint32_t numElems = 0;

    if (pDeclaration) {
        uint16_t curStream = 0;
        uint16_t curOffset = 0;
        for (int i = 0; i < 64 && pDeclaration[i] != 0xFFFFFFFF; ++i) {
            DWORD tok = pDeclaration[i];
            DWORD tokType = (tok >> 29) & 0x7;
            if (tokType == 1) {
                curStream = (uint16_t)(tok & 0x0F);
                curOffset = 0;
            } else if (tokType == 2) {
                if (tok & 0x10000000) {
                    DWORD skipDwords = (tok >> 16) & 0x0F;
                    curOffset += (uint16_t)(skipDwords * 4);
                } else {
                    uint32_t reg = tok & 0x1F;
                    uint32_t xType = (tok >> 16) & 0xFF;
                    uint32_t byteSize = GetXboxDeclTypeByteSize(xType);
                    uint8_t compCount = GetXboxDeclTypeComponentCount(xType);
                    if (curStream < 2 && reg < 16 && numElems < 16) {
                        elems[numElems].stream = (uint8_t)curStream;
                        elems[numElems].reg    = (uint8_t)reg;
                        elems[numElems].xType  = (uint8_t)xType;
                        elems[numElems].offset = curOffset;
                        numElems++;
                        info.usesStream[curStream] = true;
                    }
                    if (pFunction == nullptr) {
                        // Xbox fixed-function D3DVSDE_* register mapping (e.g. VertexSetupPrelitRigid at 0x00605B50)
                        if (curStream == 0) {
                            switch (reg) {
                                case 0:  info.hasPos  = true; info.offPos  = curOffset; break;
                                case 2:  info.hasNorm = true; info.offNorm = curOffset; break;
                                case 3:  info.hasDiff = true; info.offDiff = curOffset; break;
                                case 4:  info.hasSpec = true; info.offSpec = curOffset; break;
                                case 9:  info.hasUv0  = true; info.offUv0  = curOffset; info.dimUv[0] = compCount; break;
                                case 10: info.hasUv1  = true; info.offUv1  = curOffset; info.dimUv[1] = compCount; break;
                                case 11: info.hasUv2  = true; info.offUv2  = curOffset; info.dimUv[2] = compCount; break;
                                case 12: info.hasUv3  = true; info.offUv3  = curOffset; info.dimUv[3] = compCount; break;
                                default: break;
                            }
                        } else if (curStream == 1 && reg == 3) {
                            info.hasStream1Diff = true;
                            info.offStream1Diff = curOffset;
                        }
                    } else if (curStream == 0) {
                        // Ghost.exe programmable D3DVSD register convention (0x007A980C..0x007A996C)
                        switch (reg) {
                            case 0:  info.hasPos  = true; info.offPos  = curOffset; break;
                            case 1:  info.hasNorm = true; info.offNorm = curOffset; break;
                            case 2:  info.hasDiff = true; info.offDiff = curOffset; break;
                            case 3:  info.hasUv0  = true; info.offUv0  = curOffset; info.dimUv[0] = compCount; break;
                            case 4:  info.hasUv1  = true; info.offUv1  = curOffset; info.dimUv[1] = compCount; break;
                            case 5:  info.hasUv2  = true; info.offUv2  = curOffset; info.dimUv[2] = compCount; break;
                            case 6:  info.hasUv3  = true; info.offUv3  = curOffset; info.dimUv[3] = compCount; break;
                            case 10: info.hasRhw  = true; info.offRhw  = curOffset; break;
                            default: break;
                        }
                    }
                    curOffset += (uint16_t)byteSize;
                }
                if (curStream < 2) {
                    info.streamStrides[curStream] = curOffset;
                }
            }
        }
    }

    if (pFunction && (pFunction[0] & 0xFFFFu) == 0x2078u) {
        uint32_t instCount = (pFunction[0] >> 16) & 0xFFFFu;
        for (uint32_t i = 0; i < instCount; ++i) {
            uint32_t w3 = pFunction[1 + i * 4 + 3];
            uint32_t outMask  = (w3 >> 12) & 0xFu;
            uint32_t outWrite = (w3 >> 11) & 1u;
            uint32_t outReg   = (w3 >> 3)  & 0xFFu;
            if (outWrite != 0u && outMask != 0u && outReg == 5u) {
                info.writesOFog = true;
                break;
            }
        }
    }

    if (pFunction && (pFunction[0] & 0xFFFFu) == 0x2078u && g_d3dDevice && numElems > 0) {
        std::string hlsl = DecompileXvuToHLSL(elems, numElems, pFunction);
        ID3DBlob* vsBlob  = nullptr;
        ID3DBlob* errBlob = nullptr;
        HRESULT hr = D3DCompile(
            hlsl.data(), hlsl.size(), "XvuVS",
            nullptr, nullptr, "VSMain", "vs_4_0",
            D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &vsBlob, &errBlob);
        if (SUCCEEDED(hr) && vsBlob) {
            g_d3dDevice->CreateVertexShader(
                vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(),
                nullptr, &info.gpuVS);
            D3D11_INPUT_ELEMENT_DESC ied[16] = {};
            uint32_t iedCount = 0;
            bool seenReg[16] = {};
            for (uint32_t i = 0; i < numElems; ++i) {
                uint8_t r = elems[i].reg;
                if (r >= 16 || seenReg[r]) continue;
                seenReg[r] = true;
                ied[iedCount].SemanticName         = "ATTR";
                ied[iedCount].SemanticIndex        = r;
                ied[iedCount].Format               = MapXboxDeclTypeToDXGI(elems[i].xType);
                ied[iedCount].InputSlot            = elems[i].stream;
                ied[iedCount].AlignedByteOffset    = elems[i].offset;
                ied[iedCount].InputSlotClass       = D3D11_INPUT_PER_VERTEX_DATA;
                ied[iedCount].InstanceDataStepRate = 0;
                iedCount++;
            }
            g_d3dDevice->CreateInputLayout(
                ied, iedCount,
                vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(),
                &info.gpuInputLayout);
            info.isCompiledXvu = (info.gpuVS != nullptr && info.gpuInputLayout != nullptr);
            vsBlob->Release();
        }
        if (errBlob) errBlob->Release();
    }

    g_vertexShaders.push_back(info);
    uint32_t idx = (uint32_t)g_vertexShaders.size();
    *pHandle = (idx << 4) | 0x00010001u;
    return S_OK;
}

void __stdcall Bridge_D3DDevice_SetVertexShader(uint32_t Handle) {
    g_currentVsHandle = Handle;
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
// CPU Skinning Bridges (cMeshGroup::RenderSetupWeightedSkin / BlendWeightedSkin)
// ============================================================================

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
    *reinterpret_cast<uint32_t*>(0x008AA468) = 0x112;
    Bridge_D3DDevice_SetVertexShader(0x112);

    XboxVertexBuffer* dynVbObj = *reinterpret_cast<XboxVertexBuffer**>(reinterpret_cast<uint8_t*>(pDynVB) + 0x10);
    *reinterpret_cast<XboxVertexBuffer**>(0x008AA480) = dynVbObj;
    Bridge_D3DDevice_SetStreamSource(0, dynVbObj, 0x20);

    XboxVertexBuffer* idxBuf = *reinterpret_cast<XboxVertexBuffer* const*>(pModel + 0x7C);
    Bridge_D3DDevice_SetIndices(idxBuf, outBaseVert);
    return 0;
}

const uint32_t* g_curPrelitColors        = nullptr;
uint32_t        g_curPrelitBaseVertex    = 0;
bool            g_curPrelitUseCustomBase = false;

static void SetupPrelitStreamForMesh(const uint8_t* pMeshGroup, const uint8_t* pModelInst) {
    g_curPrelitColors        = nullptr;
    g_curPrelitBaseVertex    = 0;
    g_curPrelitUseCustomBase = false;
    if (pMeshGroup && pModelInst && (pModelInst[0x14] & 1) != 0) {
        uint8_t lodIdx = pMeshGroup[0x50];
        if (lodIdx < 3) {
            XboxVertexBuffer* prelitVB = *reinterpret_cast<XboxVertexBuffer* const*>(pModelInst + 0xB0 + lodIdx * 4);
            if (prelitVB && prelitVB->Data) {
                g_curPrelitColors        = reinterpret_cast<const uint32_t*>(prelitVB->Data);
                g_curPrelitBaseVertex    = *reinterpret_cast<const uint32_t*>(pMeshGroup + 0x48);
                g_curPrelitUseCustomBase = true;
            }
        }
    }
}

static uint32_t __fastcall Bridge_MeshGroup_RenderSetupWeightedSkin(uint8_t* pMeshGroup, void* edxUnused, uint8_t* pModelInst, uint32_t lod) {
    (void)edxUnused;
    uint32_t res = Host_RenderSetupWeightedSkin(pMeshGroup, pModelInst, lod, false);
    SetupPrelitStreamForMesh(pMeshGroup, pModelInst);
    return res;
}

static uint32_t __fastcall Bridge_MeshGroup_RenderSetupBlendWeightedSkin(uint8_t* pMeshGroup, void* edxUnused, uint8_t* pModelInst, uint32_t lod) {
    (void)edxUnused;
    uint32_t res = Host_RenderSetupWeightedSkin(pMeshGroup, pModelInst, lod, true);
    SetupPrelitStreamForMesh(pMeshGroup, pModelInst);
    return res;
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

    // 3. Bridge CPU weighted-skin & facial morph-target fallbacks (with Bone 0 normal-aliasing fix & prelit stream setup).
    // All 25 cShader::VertexSetup* functions run natively in Ghost.exe, executing compiled .xvu shaders on the GPU!
    InstallBridge(0x00606E50, &Bridge_MeshGroup_RenderSetupWeightedSkin);          // cMeshGroup::RenderSetupWeightedSkin
    InstallBridge(0x00607390, &Bridge_MeshGroup_RenderSetupBlendWeightedSkin);     // cMeshGroup::RenderSetupBlendWeightedSkin
}
