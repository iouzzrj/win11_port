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
            s += "    o.eyeDist = float2(0.0f, 1.0f);\n";
        }
    } else {
        s += "    o.pos = float4(((r12.x - uViewport.x) / max(uViewport.z, 1.0f)) * 2.0f - 1.0f,\n"
             "                   1.0f - ((r12.y - uViewport.y) / max(uViewport.w, 1.0f)) * 2.0f,\n"
             "                   saturate(r12.z), 1.0f);\n"
             "    o.eyeDist = float2(0.0f, 1.0f);\n";
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
                                case 9:  info.hasUv0  = true; info.offUv0  = curOffset; break;
                                case 10: info.hasUv1  = true; info.offUv1  = curOffset; break;
                                case 11: info.hasUv2  = true; info.offUv2  = curOffset; break;
                                case 12: info.hasUv3  = true; info.offUv3  = curOffset; break;
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
                            case 3:  info.hasUv0  = true; info.offUv0  = curOffset; break;
                            case 4:  info.hasUv1  = true; info.offUv1  = curOffset; break;
                            case 5:  info.hasUv2  = true; info.offUv2  = curOffset; break;
                            case 6:  info.hasUv3  = true; info.offUv3  = curOffset; break;
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
const float*    g_curTangentBasis        = nullptr;
uint32_t        g_curTangentBaseVertex   = 0;
float           g_cloakDistort           = 16.0f;
float           g_cloakC35[4]            = { 1.0f, 0.0f, 0.0f, 0.5f };
float           g_shieldC35x             = 1.0f;
float           g_teleportC32[2]         = { 1.0f / 640.0f, 1.0f / 480.0f };
float           g_teleportC33x           = 0.0f;
float           g_motionBlurVec[4]       = { 0.0f, 0.0f, 0.0f, 1.0f };
float           g_embossScale            = 0.015f;
float           g_specularScale          = 1.0f;
float           g_detailUvScale[2]       = { 1.0f, 1.0f };
Mat44           g_lightProjMat0          = { {{1,0,0,0}, {0,1,0,0}, {0,0,1,0}, {0,0,0,1}} };
Mat44           g_lightProjMat1          = { {{1,0,0,0}, {0,1,0,0}, {0,0,1,0}, {0,0,0,1}} };

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
    g_curPrelitColors        = nullptr;
    g_curPrelitBaseVertex    = 0;
    g_curPrelitUseCustomBase = false;
    SetupTangentStreamForMesh(pMeshGroup, false);
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
    g_activeCustomVS = 0;
    uint32_t res = Host_RenderSetupWeightedSkin(pMeshGroup, pModelInst, lod, false);
    SetupPrelitStreamForMesh(pMeshGroup, pModelInst);
    return res;
}

static uint32_t __fastcall Bridge_MeshGroup_RenderSetupBlendWeightedSkin(uint8_t* pMeshGroup, void* edxUnused, uint8_t* pModelInst, uint32_t lod) {
    (void)edxUnused;
    g_activeCustomVS = 0;
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
