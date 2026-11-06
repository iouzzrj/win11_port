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
static std::string DecompileXvuToHLSL(
    const XboxDeclElem* elems, uint32_t numElems, const DWORD* pFunction,
    const char* relPath = nullptr, uintptr_t declVa = 0)
{
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
    if (relPath && relPath[0]) {
        char hdr[512];
        snprintf(hdr, sizeof(hdr),
            "// ============================================================================\n"
            "// Decompiled Xbox NV2A Vertex Shader (.xvu -> D3D11 HLSL vs_4_0)\n"
            "// Source: %s\n"
            "// Declaration VA: 0x%08X | Instructions: %u\n"
            "// ============================================================================\n\n",
            relPath, (uint32_t)declVa, count);
        s += hdr;
    }
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
        s += "    if ((uStage0Ops.x == 22u && (uStage0Ops.z & 0x0Fu) == 0u) ||\n"
             "        (uStage1Ops.x == 22u && (uStage1Ops.z & 0x0Fu) == 0u) ||\n"
             "        (uStage2Ops.x == 22u && (uStage2Ops.z & 0x0Fu) == 0u) ||\n"
             "        (uStage3Ops.x == 22u && (uStage3Ops.z & 0x0Fu) == 0u)) {\n"
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

static void ParseXboxVertexDeclaration(
    const DWORD* pDeclaration, bool isFixedFunction,
    XboxDeclElem* outElems, uint32_t* outNumElems, XboxShaderDeclInfo* outInfo)
{
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
                        if (outElems) {
                            outElems[numElems].stream = (uint8_t)curStream;
                            outElems[numElems].reg    = (uint8_t)reg;
                            outElems[numElems].xType  = (uint8_t)xType;
                            outElems[numElems].offset = curOffset;
                        }
                        numElems++;
                        if (outInfo) outInfo->usesStream[curStream] = true;
                    }
                    if (outInfo) {
                        if (isFixedFunction) {
                            // Xbox fixed-function D3DVSDE_* register mapping (e.g. VertexSetupPrelitRigid at 0x00605B50)
                            if (curStream == 0) {
                                switch (reg) {
                                    case 0:  outInfo->hasPos  = true; outInfo->offPos  = curOffset; break;
                                    case 2:  outInfo->hasNorm = true; outInfo->offNorm = curOffset; break;
                                    case 3:  outInfo->hasDiff = true; outInfo->offDiff = curOffset; break;
                                    case 4:  outInfo->hasSpec = true; outInfo->offSpec = curOffset; break;
                                    case 9:  outInfo->hasUv0  = true; outInfo->offUv0  = curOffset; outInfo->dimUv[0] = compCount; break;
                                    case 10: outInfo->hasUv1  = true; outInfo->offUv1  = curOffset; outInfo->dimUv[1] = compCount; break;
                                    case 11: outInfo->hasUv2  = true; outInfo->offUv2  = curOffset; outInfo->dimUv[2] = compCount; break;
                                    case 12: outInfo->hasUv3  = true; outInfo->offUv3  = curOffset; outInfo->dimUv[3] = compCount; break;
                                    default: break;
                                }
                            } else if (curStream == 1 && reg == 3) {
                                outInfo->hasStream1Diff = true;
                                outInfo->offStream1Diff = curOffset;
                            }
                        } else if (curStream == 0) {
                            // Ghost.exe programmable D3DVSD register convention (0x007A980C..0x007A996C)
                            switch (reg) {
                                case 0:  outInfo->hasPos  = true; outInfo->offPos  = curOffset; break;
                                case 1:  outInfo->hasNorm = true; outInfo->offNorm = curOffset; break;
                                case 2:  outInfo->hasDiff = true; outInfo->offDiff = curOffset; break;
                                case 3:  outInfo->hasUv0  = true; outInfo->offUv0  = curOffset; outInfo->dimUv[0] = compCount; break;
                                case 4:  outInfo->hasUv1  = true; outInfo->offUv1  = curOffset; outInfo->dimUv[1] = compCount; break;
                                case 5:  outInfo->hasUv2  = true; outInfo->offUv2  = curOffset; outInfo->dimUv[2] = compCount; break;
                                case 6:  outInfo->hasUv3  = true; outInfo->offUv3  = curOffset; outInfo->dimUv[3] = compCount; break;
                                case 10: outInfo->hasRhw  = true; outInfo->offRhw  = curOffset; break;
                                default: break;
                            }
                        }
                    }
                    curOffset += (uint16_t)byteSize;
                }
                if (outInfo && curStream < 2) {
                    outInfo->streamStrides[curStream] = curOffset;
                }
            }
        }
    }
    if (outNumElems) *outNumElems = numElems;
}

// ============================================================================
// First-Run HLSL Generation & D3D11 Disk Shader Cache (GhostWin11_Data\shaders\)
// ============================================================================

static const char* kSharedPerDrawCBAndPSInHLSL =
    "cbuffer PerDrawCB : register(b0) {\n"
    "    float4 uWvpRow0;\n"
    "    float4 uWvpRow1;\n"
    "    float4 uWvpRow2;\n"
    "    float4 uWvpRow3;\n"
    "    float4 uViewport;\n"
    "    float4 uTFactor;\n"
    "    uint4  uStage0Ops;\n"
    "    uint4  uStage0Alpha;\n"
    "    uint4  uStage1Ops;\n"
    "    uint4  uStage1Alpha;\n"
    "    uint4  uStage2Ops;\n"
    "    uint4  uStage2Alpha;\n"
    "    uint4  uStage3Ops;\n"
    "    uint4  uStage3Alpha;\n"
    "    uint4  uFlags;\n"
    "    float4 uAlphaTest;\n"
    "    float4 uUvScale01;\n"
    "    float4 uUvScale23;\n"
    "    float4 uBumpParams;\n"
    "    float4 uFogParams;\n"
    "    float4 uFogColor;\n"
    "    float4 uPsConst0;\n"
    "    float4 uPsConst1;\n"
    "    float4 uPsConst2;\n"
    "    float4 uPsConst3;\n"
    "    float4 uPsConst4;\n"
    "    float4 uPsConst5;\n"
    "    float4 uPsConst6;\n"
    "    float4 uPsConst7;\n"
    "    float4 uWorldRow0;\n"
    "    float4 uWorldRow1;\n"
    "    float4 uWorldRow2;\n"
    "    float4 uWorldRow3;\n"
    "    uint4  uLightCount;\n"
    "    float4 uLightPosType[8];\n"
    "    float4 uLightDirRange[8];\n"
    "    float4 uLightAttenSpot[8];\n"
    "    float4 uLightSpotParams[8];\n"
    "    float4 uLightDiffuse[8];\n"
    "    float4 uLightAmbient[8];\n"
    "    float4 uLightSpecular[8];\n"
    "    float4 uEyePosWorld;\n"
    "    float4 uMatBackDiffAmb;\n"
    "    float4 uMatBackSpec;\n"
    "    uint4  uStageColorKeyOp;\n"
    "    float4 uStageColorKeyColor[4];\n"
    "    uint4  uStageColorSign;\n"
    "    float4 uBumpLuminance;\n"
    "    float4 uScissorRects[8];\n"
    "};\n\n"
    "struct PSIn {\n"
    "    float4 pos    : SV_POSITION;\n"
    "    float4 color  : COLOR0;\n"
    "    float4 uv0    : TEXCOORD0;\n"
    "    float4 uv1    : TEXCOORD1;\n"
    "    float4 uv2    : TEXCOORD2;\n"
    "    float4 uv3    : TEXCOORD3;\n"
    "    float2 eyeDist: TEXCOORD4;\n"
    "    float3 norm   : TEXCOORD5;\n"
    "};\n\n";

static const char* kSharedPsResourcesAndHelpersHLSL =
    "Texture2D    tex0        : register(t0);\n"
    "Texture2D    tex1        : register(t1);\n"
    "Texture2D    tex2        : register(t2);\n"
    "Texture2D    tex3        : register(t3);\n"
    "TextureCube  cubeTex0    : register(t4);\n"
    "TextureCube  cubeTex1    : register(t5);\n"
    "TextureCube  cubeTex2    : register(t6);\n"
    "TextureCube  cubeTex3    : register(t7);\n"
    "Texture3D    volTex0     : register(t8);\n"
    "Texture3D    volTex1     : register(t9);\n"
    "Texture3D    volTex2     : register(t10);\n"
    "Texture3D    volTex3     : register(t11);\n"
    "Texture2D    gammaLutTex : register(t12);\n"
    "SamplerState smp0        : register(s0);\n"
    "SamplerState smp1        : register(s1);\n"
    "SamplerState smp2        : register(s2);\n"
    "SamplerState smp3        : register(s3);\n\n"
    "float4 SampleStageTex(\n"
    "    uint stageIdx, Texture2D tex2d, TextureCube texCube, Texture3D texVol,\n"
    "    SamplerState smp, float4 uv, bool isCube, bool isVol)\n"
    "{\n"
    "    if (((uFlags.y >> stageIdx) & 1u) == 0u) {\n"
    "        return float4(1.0f, 1.0f, 1.0f, 1.0f);\n"
    "    }\n"
    "    float4 c;\n"
    "    if (isCube) {\n"
    "        c = texCube.Sample(smp, uv.xyz);\n"
    "    } else if (isVol) {\n"
    "        c = texVol.Sample(smp, uv.xyz);\n"
    "    } else {\n"
    "        c = tex2d.Sample(smp, uv.xy);\n"
    "    }\n"
    "    uint ckOp = uStageColorKeyOp[stageIdx];\n"
    "    if (ckOp != 0u) {\n"
    "        float4 ckCol = uStageColorKeyColor[stageIdx];\n"
    "        bool matchRgb = all(abs(c.rgb - ckCol.rgb) < (1.5f / 255.0f));\n"
    "        if (ckOp == 1u && matchRgb) {\n"
    "            c.a = 0.0f;\n"
    "        } else if (ckOp == 2u && matchRgb && abs(c.a - ckCol.a) < (1.5f / 255.0f)) {\n"
    "            c = float4(0.0f, 0.0f, 0.0f, 0.0f);\n"
    "        } else if (ckOp == 3u && matchRgb) {\n"
    "            discard;\n"
    "        }\n"
    "    }\n"
    "    if (((uFlags.y >> (16u + stageIdx)) & 1u) != 0u) {\n"
    "        if (c.a < (0.5f / 255.0f)) {\n"
    "            discard;\n"
    "        }\n"
    "    }\n"
    "    uint cSign = uStageColorSign[stageIdx];\n"
    "    if (cSign != 0u) {\n"
    "        if ((cSign & 0x20u) != 0u) c.r = c.r * 2.0f - 1.0f;\n"
    "        if ((cSign & 0x40u) != 0u) c.g = c.g * 2.0f - 1.0f;\n"
    "        if ((cSign & 0x80u) != 0u) c.b = c.b * 2.0f - 1.0f;\n"
    "        if ((cSign & 0x10u) != 0u) c.a = c.a * 2.0f - 1.0f;\n"
    "    }\n"
    "    return c;\n"
    "}\n\n"
    "float ComputeFogFactor(float d) {\n"
    "    if (uFogParams.x <= 0.5f || (uFlags.x & 1u) != 0u) return 1.0f;\n"
    "    float fogMode = uFogParams.x;\n"
    "    if (fogMode > 3.5f) {\n"
    "        return saturate(d);\n"
    "    } else if (fogMode > 2.5f) {\n"
    "        float span = max(uFogParams.z - uFogParams.y, 0.001f);\n"
    "        return saturate((uFogParams.z - d) / span);\n"
    "    } else if (fogMode < 1.5f) {\n"
    "        return saturate(exp(-d * uFogParams.w));\n"
    "    } else {\n"
    "        float df = d * uFogParams.w;\n"
    "        return saturate(exp(-(df * df)));\n"
    "    }\n"
    "}\n\n"
    "float ComputeInvProjQ(float4 uv, float fallbackW, bool isProj) {\n"
    "    if (!isProj) return 1.0f;\n"
    "    if (abs(uv.w) > 0.0001f && abs(uv.w - 1.0f) > 0.0001f) return 1.0f / uv.w;\n"
    "    if (abs(uv.w - 1.0f) <= 0.0001f && abs(uv.z) > 0.0001f) return 1.0f / uv.z;\n"
    "    if (abs(fallbackW) > 0.0001f) return 1.0f / fallbackW;\n"
    "    return 1.0f;\n"
    "}\n\n";

static const char* kSharedPsPrologueHLSL =
    "float4 PSMain(PSIn input) : SV_TARGET {\n"
    "    uint scissorCount = (uFlags.x >> 4u) & 0x0Fu;\n"
    "    if (scissorCount > 0u) {\n"
    "        bool insideAny = false;\n"
    "        [unroll]\n"
    "        for (uint si = 0u; si < 8u; ++si) {\n"
    "            if (si < scissorCount) {\n"
    "                float4 sr = uScissorRects[si];\n"
    "                if (input.pos.x >= sr.x && input.pos.x < sr.z &&\n"
    "                    input.pos.y >= sr.y && input.pos.y < sr.w) {\n"
    "                    insideAny = true;\n"
    "                }\n"
    "            }\n"
    "        }\n"
    "        bool scissorExcl = (uFlags.x & 4u) != 0u;\n"
    "        if (scissorExcl ? insideAny : !insideAny) {\n"
    "            discard;\n"
    "        }\n"
    "    }\n\n"
    "    float4 diffuse = input.color;\n"
    "    float4 specCol = float4(input.norm, 1.0f);\n"
    "    float4 current = diffuse;\n"
    "    float4 tempReg = float4(0.0f, 0.0f, 0.0f, 0.0f);\n\n"
    "    uint texMask  = uFlags.y & 0x0Fu;\n"
    "    uint cubeMask = (uFlags.y >> 4u)  & 0x0Fu;\n"
    "    uint projMask = (uFlags.y >> 8u)  & 0x0Fu;\n"
    "    uint volMask  = (uFlags.y >> 12u) & 0x0Fu;\n"
    "    uint tempMask = (uFlags.y >> 20u) & 0x0Fu;\n\n"
    "    float invProjQ0 = ComputeInvProjQ(input.uv0, input.eyeDist.y, (projMask & 1u) != 0u);\n"
    "    float invProjQ1 = ComputeInvProjQ(input.uv1, input.eyeDist.y, (projMask & 2u) != 0u);\n"
    "    float invProjQ2 = ComputeInvProjQ(input.uv2, input.eyeDist.y, (projMask & 4u) != 0u);\n"
    "    float invProjQ3 = ComputeInvProjQ(input.uv3, input.eyeDist.y, (projMask & 8u) != 0u);\n\n"
    "    float4 uv0 = ((projMask & 1u) != 0u) ? float4(input.uv0.xyz * invProjQ0, input.uv0.w) : input.uv0;\n"
    "    float4 uv1 = ((projMask & 2u) != 0u) ? float4(input.uv1.xyz * invProjQ1, input.uv1.w) : input.uv1;\n"
    "    float4 uv2 = ((projMask & 4u) != 0u) ? float4(input.uv2.xyz * invProjQ2, input.uv2.w) : input.uv2;\n"
    "    float4 uv3 = ((projMask & 8u) != 0u) ? float4(input.uv3.xyz * invProjQ3, input.uv3.w) : input.uv3;\n\n"
    "    float4 t0 = ((texMask & 1u) != 0u) ? SampleStageTex(0u, tex0, cubeTex0, volTex0, smp0, uv0, (cubeMask & 1u) != 0u, (volMask & 1u) != 0u) : float4(1.0f, 1.0f, 1.0f, 1.0f);\n"
    "    float4 t1 = ((texMask & 2u) != 0u) ? SampleStageTex(1u, tex1, cubeTex1, volTex1, smp1, uv1, (cubeMask & 2u) != 0u, (volMask & 2u) != 0u) : float4(1.0f, 1.0f, 1.0f, 1.0f);\n";

static const char* kSharedPsAlphaTestAndGammaHLSL =
    "    if (uFlags.z != 0u) {\n"
    "        uint fn = (uint)uAlphaTest.x;\n"
    "        float refVal = uAlphaTest.y;\n"
    "        bool passAlpha = true;\n"
    "        if (fn == 1u)      passAlpha = false;\n"
    "        else if (fn == 2u) passAlpha = (current.a <  refVal - 0.001f);\n"
    "        else if (fn == 3u) passAlpha = (abs(current.a - refVal) < 0.004f);\n"
    "        else if (fn == 4u) passAlpha = (current.a <= refVal + 0.001f);\n"
    "        else if (fn == 5u) passAlpha = (current.a >  refVal + 0.001f);\n"
    "        else if (fn == 6u) passAlpha = (abs(current.a - refVal) >= 0.004f);\n"
    "        else if (fn == 7u) passAlpha = (current.a >= refVal - 0.001f);\n"
    "        if (!passAlpha) discard;\n"
    "    }\n";

static const char* kSharedPsFogHLSL =
    "    if (uFogParams.x > 0.5f && (uFlags.x & 1u) == 0u) {\n"
    "        float fogAmt = 1.0f - fogFactor;\n"
    "        if (uFogColor.w > 0.5f) {\n"
    "            current.rgb *= fogFactor;\n"
    "        } else {\n"
    "            current.rgb = lerp(current.rgb, uFogColor.rgb, fogAmt);\n"
    "        }\n"
    "    }\n";

static const char* kSharedPsGammaEpilogueHLSL =
    "    if ((uFlags.x & 2u) != 0u) {\n"
    "        float3 c = saturate(current.rgb);\n"
    "        current.r = gammaLutTex.SampleLevel(smp0, float2(c.r * (255.0f / 256.0f) + (0.5f / 256.0f), 0.5f), 0).r;\n"
    "        current.g = gammaLutTex.SampleLevel(smp0, float2(c.g * (255.0f / 256.0f) + (0.5f / 256.0f), 0.5f), 0).g;\n"
    "        current.b = gammaLutTex.SampleLevel(smp0, float2(c.b * (255.0f / 256.0f) + (0.5f / 256.0f), 0.5f), 0).b;\n"
    "    } else if (abs(uAlphaTest.z - 1.0f) > 0.01f || abs(uAlphaTest.w - 1.0f) > 0.01f) {\n"
    "        float invG = 1.0f / max(uAlphaTest.z, 0.1f);\n"
    "        current.rgb = saturate(pow(max(current.rgb, 0.0f), invG) * uAlphaTest.w);\n"
    "    }\n"
    "    return current;\n"
    "}\n";

static std::string BuildFixedVsHLSL() {
    std::string s;
    s.reserve(8192);
    s += "// ============================================================================\n"
         "// StarCraft: Ghost (" GHOST_STR(GHOST_PROJECT_NAME) ") — Fixed-Function Vertex Shader (vs_4_0)\n"
         "// ============================================================================\n\n";
    s += kSharedPerDrawCBAndPSInHLSL;
    s += "struct VSIn {\n"
         "    float4 pos   : POSITION;\n"
         "    float3 norm  : NORMAL;\n"
         "    float4 color : COLOR0;\n"
         "    float2 uv0   : TEXCOORD0;\n"
         "    float2 uv1   : TEXCOORD1;\n"
         "    float2 uv2   : TEXCOORD2;\n"
         "    float2 uv3   : TEXCOORD3;\n"
         "    float4 uvw_w : TEXCOORD4;\n"
         "    float4 spec  : COLOR1;\n"
         "};\n\n"
         "PSIn VSMain(VSIn input) {\n"
         "    PSIn o;\n"
         "    bool isRhw = (uFlags.x & 1u) != 0u;\n"
         "    if (isRhw) {\n"
         "        float vpW = max(uViewport.z, 1.0f);\n"
         "        float vpH = max(uViewport.w, 1.0f);\n"
         "        float ndcX = ((input.pos.x - uViewport.x) / vpW) * 2.0f - 1.0f;\n"
         "        float ndcY = 1.0f - ((input.pos.y - uViewport.y) / vpH) * 2.0f;\n"
         "        o.pos = float4(ndcX, ndcY, saturate(input.pos.z), 1.0f);\n"
         "        o.eyeDist = float2(0.0f, 1.0f);\n"
         "    } else {\n"
         "        float4 clipPos = input.pos.x * uWvpRow0 +\n"
         "                         input.pos.y * uWvpRow1 +\n"
         "                         input.pos.z * uWvpRow2 +\n"
         "                         uWvpRow3;\n"
         "        o.pos = clipPos;\n"
         "        o.eyeDist = float2(max(clipPos.w, 0.0f), input.pos.w);\n"
         "    }\n"
         "    if (uLightCount.y != 0u && !isRhw) {\n"
         "        float3 wPos = input.pos.x * uWorldRow0.xyz +\n"
         "                      input.pos.y * uWorldRow1.xyz +\n"
         "                      input.pos.z * uWorldRow2.xyz +\n"
         "                      uWorldRow3.xyz;\n"
         "        float3 wNorm = input.norm.x * uWorldRow0.xyz +\n"
         "                       input.norm.y * uWorldRow1.xyz +\n"
         "                       input.norm.z * uWorldRow2.xyz;\n"
         "        float nLen = length(wNorm);\n"
         "        if (nLen > 0.0001f) wNorm /= nLen;\n"
         "        bool specEnable  = (uLightCount.z & 1u) != 0u;\n"
         "        bool localViewer = (uLightCount.z & 2u) != 0u;\n"
         "        float specPower  = max(uEyePosWorld.w, 0.001f);\n"
         "        float3 V = localViewer ? normalize(uEyePosWorld.xyz - wPos) : normalize(uEyePosWorld.xyz);\n"
         "        float3 totRgb  = input.color.rgb;\n"
         "        float3 totSpec = input.spec.rgb;\n"
         "        uint numLights = min(uLightCount.x, 8u);\n"
         "        for (uint l = 0u; l < numLights; ++l) {\n"
         "            float4 posType = uLightPosType[l];\n"
         "            float lType = posType.w;\n"
         "            float3 L = float3(0.0f, 0.0f, 0.0f);\n"
         "            float atten = 1.0f;\n"
         "            if (lType > 2.5f) {\n"
         "                L = posType.xyz;\n"
         "            } else {\n"
         "                float3 toLight = posType.xyz - wPos;\n"
         "                float dist = length(toLight);\n"
         "                float range = uLightDirRange[l].w;\n"
         "                if (range > 0.0f && dist > range) continue;\n"
         "                if (dist > 0.0001f) L = toLight / dist;\n"
         "                float4 attSpot = uLightAttenSpot[l];\n"
         "                float denom = attSpot.x + attSpot.y * dist + attSpot.z * dist * dist;\n"
         "                if (denom > 0.0001f) atten = 1.0f / denom;\n"
         "                if (lType > 1.5f) {\n"
         "                    float cosAlpha = -dot(L, uLightDirRange[l].xyz);\n"
         "                    float cosHalfPhi = attSpot.w;\n"
         "                    if (cosAlpha <= cosHalfPhi) continue;\n"
         "                    float cosHalfTheta = uLightSpotParams[l].x;\n"
         "                    if (cosAlpha < cosHalfTheta && (cosHalfTheta - cosHalfPhi) > 0.0001f) {\n"
         "                        atten *= pow(max((cosAlpha - cosHalfPhi) / (cosHalfTheta - cosHalfPhi), 0.0f), uLightSpotParams[l].y);\n"
         "                    }\n"
         "                }\n"
         "            }\n"
         "            float ndotl = max(dot(wNorm, L), 0.0f);\n"
         "            totRgb += atten * (uLightAmbient[l].rgb + ndotl * uLightDiffuse[l].rgb);\n"
         "            if (specEnable && ndotl > 0.0f) {\n"
         "                float3 H = normalize(L + V);\n"
         "                float ndoth = max(dot(wNorm, H), 0.0f);\n"
         "                totSpec += atten * pow(ndoth, specPower) * uLightSpecular[l].rgb;\n"
         "            }\n"
         "        }\n"
         "        o.color = float4(saturate(totRgb), input.color.a);\n"
         "        o.norm  = specEnable ? saturate(totSpec) : ((dot(abs(input.spec.rgb), float3(1.0f, 1.0f, 1.0f)) > 0.0f) ? input.spec.rgb : input.norm);\n"
         "    } else {\n"
         "        o.color = input.color;\n"
         "        o.norm  = (dot(abs(input.spec.rgb), float3(1.0f, 1.0f, 1.0f)) > 0.0f) ? input.spec.rgb : input.norm;\n"
         "    }\n"
         "    o.uv0 = float4(input.uv0 * uUvScale01.xy, input.uvw_w.x, 0.0f);\n"
         "    o.uv1 = float4(input.uv1 * uUvScale01.zw, input.uvw_w.y, 0.0f);\n"
         "    o.uv2 = float4(input.uv2 * uUvScale23.xy, input.uvw_w.z, 0.0f);\n"
         "    o.uv3 = float4(input.uv3 * uUvScale23.zw, input.uvw_w.w, 0.0f);\n"
         "    return o;\n"
         "}\n";
    return s;
}

static std::string BuildFixedPsHLSL() {
    std::string s;
    s.reserve(12288);
    s += "// ============================================================================\n"
         "// StarCraft: Ghost (" GHOST_STR(GHOST_PROJECT_NAME) ") — Fixed-Function Pixel Shader (ps_4_0)\n"
         "// ============================================================================\n\n";
    s += kSharedPerDrawCBAndPSInHLSL;
    s += kSharedPsResourcesAndHelpersHLSL;
    s += "float4 EvalArg(uint arg, float4 diffuse, float4 current, float4 tex, float4 tfactor, float4 spec, float4 tempReg) {\n"
         "    uint src = arg & 0x0Fu;\n"
         "    float4 v = current;\n"
         "    if (src == 0u)      v = diffuse;\n"
         "    else if (src == 1u) v = current;\n"
         "    else if (src == 2u) v = tex;\n"
         "    else if (src == 3u) v = tfactor;\n"
         "    else if (src == 4u) v = spec;\n"
         "    else if (src == 5u) v = tempReg;\n"
         "    if ((arg & 0x10u) != 0u) v = 1.0f - v;\n"
         "    if ((arg & 0x20u) != 0u) v = v.aaaa;\n"
         "    return v;\n"
         "}\n\n"
         "float3 EvalColorOp(uint op, uint arg2Src, float4 a1, float4 a2, float4 a0, float4 diffuse, float4 current, float4 tex, float4 tfactor) {\n"
         "    if (op == 2u)  return a1.rgb;\n"
         "    if (op == 3u)  return a2.rgb;\n"
         "    if (op == 4u)  return a1.rgb * a2.rgb;\n"
         "    if (op == 5u)  return saturate(a1.rgb * a2.rgb * 2.0f);\n"
         "    if (op == 6u)  return saturate(a1.rgb * a2.rgb * 4.0f);\n"
         "    if (op == 7u)  return saturate(a1.rgb + a2.rgb);\n"
         "    if (op == 8u)  return saturate(a1.rgb + a2.rgb - 0.5f);\n"
         "    if (op == 9u)  return saturate((a1.rgb + a2.rgb - 0.5f) * 2.0f);\n"
         "    if (op == 10u) return saturate(a1.rgb - a2.rgb);\n"
         "    if (op == 11u) return saturate(a1.rgb + a2.rgb - a1.rgb * a2.rgb);\n"
         "    if (op == 12u) return lerp(a2.rgb, a1.rgb, diffuse.a);\n"
         "    if (op == 13u) return lerp(a2.rgb, a1.rgb, current.a);\n"
         "    if (op == 14u) return lerp(a2.rgb, a1.rgb, tex.a);\n"
         "    if (op == 15u) return lerp(a2.rgb, a1.rgb, tfactor.a);\n"
         "    if (op == 16u) return saturate(a1.rgb + a2.rgb * (1.0f - tex.a));\n"
         "    if (op == 17u) return saturate(a1.rgb + a2.rgb * (1.0f - current.a));\n"
         "    if (op == 18u) return saturate(a1.rgb + a1.a * a2.rgb);\n"
         "    if (op == 19u) return saturate(a1.rgb * a2.rgb + a1.a);\n"
         "    if (op == 20u) return saturate(a1.rgb + (1.0f - a1.a) * a2.rgb);\n"
         "    if (op == 21u) return saturate((1.0f - a1.rgb) * a2.rgb + a1.a);\n"
         "    if (op == 22u) return saturate(dot((a1.rgb - 0.5f) * 2.0f, (a2.rgb - 0.5f) * 2.0f)).xxx;\n"
         "    if (op == 23u) return saturate(a0.rgb + a1.rgb * a2.rgb);\n"
         "    if (op == 24u) return lerp(a2.rgb, a1.rgb, a0.rgb);\n"
         "    if (op == 25u || op == 26u) return current.rgb;\n"
         "    return a1.rgb * a2.rgb;\n"
         "}\n\n"
         "float EvalAlphaOp(uint op, float a1, float a2, float a0, float4 diffuse, float4 current, float4 tex, float4 tfactor) {\n"
         "    if (op == 2u)  return a1;\n"
         "    if (op == 3u)  return a2;\n"
         "    if (op == 4u)  return a1 * a2;\n"
         "    if (op == 5u)  return saturate(a1 * a2 * 2.0f);\n"
         "    if (op == 6u)  return saturate(a1 * a2 * 4.0f);\n"
         "    if (op == 7u)  return saturate(a1 + a2);\n"
         "    if (op == 8u)  return saturate(a1 + a2 - 0.5f);\n"
         "    if (op == 9u)  return saturate((a1 + a2 - 0.5f) * 2.0f);\n"
         "    if (op == 10u) return saturate(a1 - a2);\n"
         "    if (op == 11u) return saturate(a1 + a2 - a1 * a2);\n"
         "    if (op == 12u) return lerp(a2, a1, diffuse.a);\n"
         "    if (op == 13u) return lerp(a2, a1, current.a);\n"
         "    if (op == 14u) return lerp(a2, a1, tex.a);\n"
         "    if (op == 15u) return lerp(a2, a1, tfactor.a);\n"
         "    if (op == 16u) return saturate(a1 + a2 * (1.0f - tex.a));\n"
         "    if (op == 17u) return saturate(a1 + a2 * (1.0f - current.a));\n"
         "    if (op == 23u) return saturate(a0 + a1 * a2);\n"
         "    if (op == 24u) return lerp(a2, a1, a0);\n"
         "    return a1 * a2;\n"
         "}\n\n"
         "bool OpUsesMissingTex(uint4 ops, bool hasStageTex) {\n"
         "    if (hasStageTex) return false;\n"
         "    uint op = ops.x;\n"
         "    if (op <= 1u || op == 25u || op == 26u) return false;\n"
         "    if (op == 14u || op == 16u) return true;\n"
         "    if (op != 3u && (ops.y & 0x0Fu) == 2u) return true;\n"
         "    if (op != 2u && op != 17u && (ops.z & 0x0Fu) == 2u) return true;\n"
         "    if ((op == 23u || op == 24u) && (ops.w & 0x0Fu) == 2u) return true;\n"
         "    return false;\n"
         "}\n\n"
         "void EvalStage(\n"
         "    uint4 colorOps, uint4 alphaOps, float4 diffuse, inout float4 current, inout float4 tempReg,\n"
         "    float4 tex, float4 tfactor, float4 spec, bool writeToTemp, bool hasStageTex, bool isStage0)\n"
         "{\n"
         "    bool missingColorTex = OpUsesMissingTex(colorOps, hasStageTex);\n"
         "    float3 outRgb;\n"
         "    if (missingColorTex) {\n"
         "        outRgb = isStage0 ? diffuse.rgb : current.rgb;\n"
         "    } else {\n"
         "        float4 c1 = EvalArg(colorOps.y, diffuse, current, tex, tfactor, spec, tempReg);\n"
         "        float4 c2 = EvalArg(colorOps.z, diffuse, current, tex, tfactor, spec, tempReg);\n"
         "        float4 c0 = EvalArg(colorOps.w, diffuse, current, tex, tfactor, spec, tempReg);\n"
         "        outRgb = EvalColorOp(colorOps.x, colorOps.z, c1, c2, c0, diffuse, current, tex, tfactor);\n"
         "    }\n"
         "    float outA = (colorOps.x == 22u && !missingColorTex)\n"
         "               ? outRgb.x\n"
         "               : (writeToTemp ? tempReg.a : current.a);\n"
         "    if (alphaOps.x > 1u && colorOps.x != 22u) {\n"
         "        if (OpUsesMissingTex(alphaOps, hasStageTex)) {\n"
         "            outA = isStage0 ? diffuse.a : current.a;\n"
         "        } else {\n"
         "            float4 a1 = EvalArg(alphaOps.y, diffuse, current, tex, tfactor, spec, tempReg);\n"
         "            float4 a2 = EvalArg(alphaOps.z, diffuse, current, tex, tfactor, spec, tempReg);\n"
         "            float4 a0 = EvalArg(alphaOps.w, diffuse, current, tex, tfactor, spec, tempReg);\n"
         "            outA = EvalAlphaOp(alphaOps.x, a1.a, a2.a, a0.a, diffuse, current, tex, tfactor);\n"
         "        }\n"
         "    }\n"
         "    if (writeToTemp) {\n"
         "        tempReg = float4(outRgb, outA);\n"
         "    } else {\n"
         "        current = float4(outRgb, outA);\n"
         "    }\n"
         "}\n\n";
    s += kSharedPsPrologueHLSL;
    s += "    if (uStage0Ops.x == 25u || uStage0Ops.x == 26u) {\n"
         "        float2 d = ((uStageColorSign[0] & 0x60u) != 0u) ? t0.rg : (t0.rg * 2.0f - 1.0f);\n"
         "        uv1.xy += float2(d.x * uBumpParams.x + d.y * uBumpParams.w, d.x * uBumpParams.y + d.y * uBumpParams.z);\n"
         "        if ((texMask & 2u) != 0u) {\n"
         "            t1 = SampleStageTex(1u, tex1, cubeTex1, volTex1, smp1, uv1, (cubeMask & 2u) != 0u, (volMask & 2u) != 0u);\n"
         "        }\n"
         "        if (uStage0Ops.x == 26u && (uBumpLuminance.x != 0.0f || uBumpLuminance.y != 0.0f)) {\n"
         "            t1.rgb *= saturate(t0.b * uBumpLuminance.x + uBumpLuminance.y);\n"
         "        }\n"
         "    }\n"
         "    if (uStage1Ops.x == 25u || uStage1Ops.x == 26u) {\n"
         "        float2 d = ((uStageColorSign[1] & 0x60u) != 0u) ? t1.rg : (t1.rg * 2.0f - 1.0f);\n"
         "        uv2.xy += float2(d.x * uBumpParams.x + d.y * uBumpParams.w, d.x * uBumpParams.y + d.y * uBumpParams.z);\n"
         "    }\n"
         "    float4 t2 = ((texMask & 4u) != 0u) ? SampleStageTex(2u, tex2, cubeTex2, volTex2, smp2, uv2, (cubeMask & 4u) != 0u, (volMask & 4u) != 0u) : float4(1.0f, 1.0f, 1.0f, 1.0f);\n"
         "    if (uStage1Ops.x == 26u && (uBumpLuminance.z != 0.0f || uBumpLuminance.w != 0.0f)) {\n"
         "        t2.rgb *= saturate(t1.b * uBumpLuminance.z + uBumpLuminance.w);\n"
         "    }\n"
         "    float4 t3 = ((texMask & 8u) != 0u) ? SampleStageTex(3u, tex3, cubeTex3, volTex3, smp3, uv3, (cubeMask & 8u) != 0u, (volMask & 8u) != 0u) : float4(1.0f, 1.0f, 1.0f, 1.0f);\n"
         "    float fogFactor = ComputeFogFactor(input.eyeDist.x);\n\n"
         "    if (uStage0Ops.x > 1u) {\n"
         "        EvalStage(uStage0Ops, uStage0Alpha, diffuse, current, tempReg, t0, uTFactor, specCol, (tempMask & 1u) != 0u, (texMask & 1u) != 0u, true);\n"
         "        if (uStage1Ops.x > 1u) {\n"
         "            EvalStage(uStage1Ops, uStage1Alpha, diffuse, current, tempReg, t1, uTFactor, specCol, (tempMask & 2u) != 0u, (texMask & 2u) != 0u, false);\n"
         "            if (uStage2Ops.x > 1u) {\n"
         "                EvalStage(uStage2Ops, uStage2Alpha, diffuse, current, tempReg, t2, uTFactor, specCol, (tempMask & 4u) != 0u, (texMask & 4u) != 0u, false);\n"
         "                if (uStage3Ops.x > 1u) {\n"
         "                    EvalStage(uStage3Ops, uStage3Alpha, diffuse, current, tempReg, t3, uTFactor, specCol, (tempMask & 8u) != 0u, (texMask & 8u) != 0u, false);\n"
         "                }\n"
         "            }\n"
         "        }\n"
         "    }\n"
         "    if ((uFlags.x & 8u) != 0u || (uLightCount.z & 1u) != 0u) {\n"
         "        current.rgb = saturate(current.rgb + specCol.rgb);\n"
         "    }\n\n";
    s += kSharedPsAlphaTestAndGammaHLSL;
    s += kSharedPsFogHLSL;
    s += kSharedPsGammaEpilogueHLSL;
    return s;
}

static std::string DecompileXpuToHLSL(const char* relPath, uint32_t pt, const XboxPixelShaderDef* pDef) {
    std::string s;
    s.reserve(8192);
    char line[512];
    snprintf(line, sizeof(line),
        "// ============================================================================\n"
        "// Decompiled Xbox NV2A Pixel Shader (.xpu -> D3D11 HLSL ps_4_0)\n"
        "// Source: %s (aPixelShaders[%u])\n",
        relPath ? relPath : "", pt);
    s += line;
    if (pDef) {
        uint32_t numStages = pDef->PSCombinerCount & 0xFu;
        snprintf(line, sizeof(line),
            "// PSB0 CombinerCount: 0x%08X (%u stages) | PSTextureModes: 0x%08X\n"
            "// PSDotMapping: 0x%08X | PSInputTexture: 0x%08X | PSCompareMode: 0x%08X\n"
            "// PSC0Mapping: 0x%08X | PSC1Mapping: 0x%08X | PSFinalCombinerConstants: 0x%08X\n",
            pDef->PSCombinerCount, numStages, pDef->PSTextureModes,
            pDef->PSDotMapping, pDef->PSInputTexture, pDef->PSCompareMode,
            pDef->PSC0Mapping, pDef->PSC1Mapping, pDef->PSFinalCombinerConstants);
        s += line;
        for (uint32_t st = 0; st < numStages && st < 8u; ++st) {
            snprintf(line, sizeof(line),
                "// Stage %u: RGBIn=0x%08X AlphaIn=0x%08X RGBOut=0x%08X AlphaOut=0x%08X C0=0x%08X C1=0x%08X\n",
                st, pDef->PSRGBInputs[st], pDef->PSAlphaInputs[st],
                pDef->PSRGBOutputs[st], pDef->PSAlphaOutputs[st],
                pDef->PSConstant0[st], pDef->PSConstant1[st]);
            s += line;
        }
        snprintf(line, sizeof(line),
            "// FinalCombiner: ABCD=0x%08X EFG=0x%08X FC0=0x%08X FC1=0x%08X\n",
            pDef->PSFinalCombinerInputsABCD, pDef->PSFinalCombinerInputsEFG,
            pDef->PSFinalCombinerConstant0, pDef->PSFinalCombinerConstant1);
        s += line;
    }
    s += "// ============================================================================\n\n";
    s += kSharedPerDrawCBAndPSInHLSL;
    s += kSharedPsResourcesAndHelpersHLSL;
    s += kSharedPsPrologueHLSL;

    if (pt == 0u || pt == 1u || pt == 2u) {
        s += "    // texbem t1, t0 BumpEnv UV perturbation\n"
             "    {\n"
             "        float2 d = ((uStageColorSign[0] & 0x60u) != 0u) ? t0.rg : (t0.rg * 2.0f - 1.0f);\n"
             "        uv1.xy += float2(d.x * uBumpParams.x + d.y * uBumpParams.w, d.x * uBumpParams.y + d.y * uBumpParams.z);\n"
             "        if ((texMask & 2u) != 0u) {\n"
             "            t1 = SampleStageTex(1u, tex1, cubeTex1, volTex1, smp1, uv1, (cubeMask & 2u) != 0u, (volMask & 2u) != 0u);\n"
             "        }\n"
             "        if (uBumpLuminance.x != 0.0f || uBumpLuminance.y != 0.0f) {\n"
             "            t1.rgb *= saturate(t0.b * uBumpLuminance.x + uBumpLuminance.y);\n"
             "        }\n"
             "    }\n";
    }
    s += "    float4 t2 = ((texMask & 4u) != 0u) ? SampleStageTex(2u, tex2, cubeTex2, volTex2, smp2, uv2, (cubeMask & 4u) != 0u, (volMask & 4u) != 0u) : float4(1.0f, 1.0f, 1.0f, 1.0f);\n"
         "    float4 t3 = ((texMask & 8u) != 0u) ? SampleStageTex(3u, tex3, cubeTex3, volTex3, smp3, uv3, (cubeMask & 8u) != 0u, (volMask & 8u) != 0u) : float4(1.0f, 1.0f, 1.0f, 1.0f);\n"
         "    float fogFactor = ComputeFogFactor(input.eyeDist.x);\n\n";

    switch (pt) {
        case 0u: // emBump.xpu
            s += "    current = t1;\n";
            break;
        case 1u: // lava.xpu
            s += "    current.rgb = saturate(t1.a * t2.rgb + t1.rgb);\n"
                 "    current.a   = saturate(t1.a * t2.b   + t1.b);\n";
            break;
        case 2u: // heatShimmer.xpu
            s += "    current.rgb = t1.rgb * t2.rgb;\n"
                 "    current.a   = t2.a * diffuse.a;\n";
            break;
        case 3u: // terrain_fog.xpu
            s += "    current.rgb = uFogColor.rgb;\n"
                 "    current.a   = (uFogParams.x > 0.5f) ? fogFactor : saturate(diffuse.a);\n";
            break;
        case 4u: // ghostsuit.xpu
            s += "    float3 r1 = saturate(t1.rgb + uPsConst1.rgb);\n"
                 "    current.rgb = (t0.a >= 0.5f) ? (uPsConst0.rgb * r1)   : (t0.rgb * diffuse.rgb);\n"
                 "    current.a   = (t0.a >= 0.5f) ? (uPsConst0.a   * r1.b) : (t0.a   * diffuse.a);\n";
            break;
        case 5u: // selfglow.xpu
            s += "    float4 r0 = saturate(uPsConst0 * t0.a + diffuse);\n"
                 "    current = r0 * t0;\n";
            break;
        case 6u: // bumpreflect.xpu
            s += "    float3 nTs = t0.rgb * 2.0f - 1.0f;\n"
                 "    float3 uvw = float3(dot(uv1.xyz, nTs), dot(uv2.xyz, nTs), dot(uv3.xyz, nTs));\n"
                 "    float3 Vw  = float3(uv1.w, uv2.w, uv3.w);\n"
                 "    float3 Np  = (dot(uvw, uvw) > 1e-6f) ? normalize(uvw) : float3(0.0f, 0.0f, 1.0f);\n"
                 "    float3 R   = (dot(Vw, Vw) > 1e-6f) ? reflect(-normalize(Vw), Np) : Np;\n"
                 "    float4 refl = t3;\n"
                 "    if ((texMask & 8u) != 0u) {\n"
                 "        refl = ((cubeMask & 8u) != 0u)\n"
                 "            ? cubeTex3.Sample(smp3, R)\n"
                 "            : tex3.Sample(smp3, float2(R.x * 0.5f + 0.5f, 0.5f - R.y * 0.5f));\n"
                 "    }\n"
                 "    current = refl;\n";
            break;
        case 7u: // waterNM.xpu
            s += "    current = (t0 + t1) * 0.5f;\n";
            break;
        case 8u: // screenHaze.xpu
            s += "    float r0a = dot(t1.rgb, uPsConst0.aaa);\n"
                 "    float r1a = dot(t2.rgb, uPsConst0.aaa);\n"
                 "    float4 r0 = (r0a >= 0.5f) ? t1 : float4(uPsConst0.rgb, uPsConst0.b);\n"
                 "    float4 r1 = (r1a >= 0.5f) ? t2 : float4(uPsConst0.rgb, uPsConst0.b);\n"
                 "    r1.rgb = saturate(r0.rgb * uPsConst1.rgb + r1.rgb * uPsConst1.rgb);\n"
                 "    r1.a   = saturate(r0.a   * uPsConst1.b   + r1.a   * uPsConst1.b);\n"
                 "    r0a = dot(t3.rgb, uPsConst0.aaa);\n"
                 "    r0 = (r0a >= 0.5f) ? t3 : float4(uPsConst0.rgb, uPsConst0.b);\n"
                 "    current.rgb = saturate(uPsConst1.rgb * r0.rgb + r1.rgb);\n"
                 "    current.a   = saturate(uPsConst1.a);\n";
            break;
        case 9u: // terrain.xpu
            s += "    float3 r0 = lerp(t2.rgb, t1.rgb, t0.a);\n"
                 "    r0 = saturate(r0 * t0.rgb * 2.0f);\n"
                 "    r0 = saturate(r0 * t3.rgb * 2.0f);\n"
                 "    current.rgb = r0 * diffuse.rgb;\n"
                 "    current.a   = 1.0f;\n";
            break;
        case 10u: // terrain_nodetail.xpu
            s += "    float3 r0 = lerp(t2.rgb, t1.rgb, t0.a);\n"
                 "    current.rgb = saturate(r0 * t0.rgb * 2.0f) * diffuse.rgb;\n"
                 "    current.a   = 1.0f;\n";
            break;
        case 11u:
        case 14u: // gaussBlurSource.xpu / gaussBlur_Source_ms.xpu
            s += "    float4 r0 = saturate(t0 - uPsConst0);\n"
                 "    float4 r1 = saturate(r0 * 2.0f);\n"
                 "    r1.rgb = saturate(uPsConst2.rgb - r1.rgb);\n"
                 "    r1.a   = saturate(uPsConst2.b   - r1.a);\n"
                 "    r1 = saturate((r1 - uPsConst2.a) * 2.0f);\n"
                 "    r1 = t1 * r1 * uPsConst1;\n"
                 "    current = saturate(r0 + r1);\n";
            break;
        case 12u:
        case 13u: // gaussBlur.xpu / gaussBlur_ms.xpu
            s += "    current = saturate(saturate(t0 * uPsConst0 + t1 * uPsConst1) + saturate(t2 * uPsConst2 + t3 * uPsConst3));\n";
            break;
        case 15u: // sightFinal.xpu
            s += "    current = ((texMask & 2u) != 0u) ? tex1.Sample(smp1, t0.gb) : t0;\n";
            break;
        case 16u: // cloak.xpu
            s += "    float3 r1 = saturate(t1.a * t1.rgb + t0.rgb);\n"
                 "    float3 r0 = lerp(t3.rgb, t3.rgb * diffuse.rgb, uPsConst1.a);\n"
                 "    r0 = lerp(r1, r0, uPsConst0.rgb);\n"
                 "    current.rgb = (t2.a >= 0.5f) ? r0 : r1;\n"
                 "    current.a   = uPsConst0.a;\n";
            break;
        case 17u: // cloakDissolve.xpu
            s += "    float3 r0 = saturate(t1.a * t1.rgb + t0.rgb);\n"
                 "    float3 r1 = lerp(t3.rgb, t3.rgb * diffuse.rgb, uPsConst1.a);\n"
                 "    r1 = ((t2.b - (uPsConst2.a - 0.5f)) >= 0.5f) ? r1 : uPsConst2.rgb;\n"
                 "    float3 blended = ((uPsConst2.a - (t2.b - 0.5f)) >= 0.5f) ? r0 : r1;\n"
                 "    current.rgb = lerp(blended, uPsConst3.rgb, uPsConst3.a);\n"
                 "    current.a   = uPsConst0.a;\n";
            break;
        case 18u: // cloakDissolveShadow.xpu
            s += "    float r0a = t0.b - (uPsConst1.a - 0.5f);\n"
                 "    current.rgb = ((r0a >= 0.5f) ? uPsConst1.rgb : uPsConst0.rgb) * uPsConst0.a;\n"
                 "    current.a   = saturate(r0a * uPsConst0.a);\n";
            break;
        case 19u: // desaturate.xpu
            s += "    float m = max(t0.r, max(t0.g, t0.b));\n"
                 "    current.rgb = saturate(m + uPsConst1.rgb) * uPsConst0.rgb;\n"
                 "    current.a   = m;\n";
            break;
        case 20u: // shield.xpu
            s += "    float4 s1 = ((texMask & 2u) != 0u) ? tex1.Sample(smp1, t0.ar) : t1;\n"
                 "    float4 r1 = t2 * uPsConst0.a;\n"
                 "    float4 r0 = saturate(s1 + r1) * r1.a;\n"
                 "    float r0a = saturate(t3.b - uPsConst0.b);\n"
                 "    r1 = (r0a >= 0.5f) ? float4(r0.rgb, r0a) : uPsConst1;\n"
                 "    current.rgb = r1.rgb;\n"
                 "    current.a   = r0a;\n";
            break;
        case 21u: // shieldblur.xpu
            s += "    float r0a = ((t0.a - 0.5f) + (t1.a - 0.5f)) * 0.5f;\n"
                 "    float r1a = ((t2.a - 0.5f) + (t3.a - 0.5f)) * 0.5f;\n"
                 "    r0a = saturate((r0a + r1a) * 0.5f + uPsConst0.a);\n"
                 "    current = float4(r0a, r0a, r0a, r0a);\n";
            break;
        case 22u: // shield_part1.xpu
            s += "    float r0a = clamp((t1.a - t0.a) + (t2.a - t0.a) + (t3.a - t0.a), -1.0f, 1.0f);\n"
                 "    float r1a = (uPsConst0.a - t0.a) * uPsConst1.a + r0a;\n"
                 "    r0a = saturate(r1a + uPsConst0.a);\n"
                 "    current = float4(r0a, r0a, r0a, 1.0f);\n";
            break;
        case 23u: // shield_part2.xpu
            s += "    float d = dot(t0.rgb, uPsConst2.rgb);\n"
                 "    float r0a = t1.a - t0.a;\n"
                 "    float4 r0 = saturate(float4(d + r0a, d + r0a, d + r0a, d + r0a));\n"
                 "    float4 r1 = clamp((float4(t0.rgb, t0.b) - uPsConst1) * 2.0f, -1.0f, 1.0f);\n"
                 "    r1.rgb = ((2.0f * r0.rgb - 1.0f) * uPsConst0.rgb + r1.rgb * uPsConst3.a) * 0.5f;\n"
                 "    r1.a   = ((2.0f * r0.a   - 1.0f) * uPsConst0.b   + r1.a   * uPsConst3.a) * 0.5f;\n"
                 "    r1.rgb = saturate(r1.rgb + uPsConst1.rgb);\n"
                 "    r1.a   = saturate(r1.a   + uPsConst1.b);\n"
                 "    float r0a2 = clamp((t0.a - uPsConst1.a) * 2.0f, -1.0f, 1.0f);\n"
                 "    r0a2 = ((2.0f * r1.a - 1.0f) * uPsConst0.a + r0a2) * 0.5f;\n"
                 "    current.rgb = r1.rgb;\n"
                 "    current.a   = saturate(r0a2 + uPsConst1.a);\n";
            break;
        case 24u: // dof.xpu
            s += "    float4 s1 = ((texMask & 2u) != 0u) ? tex1.Sample(smp1, t0.ar) : t1;\n"
                 "    float r0_rgb = saturate(((2.0f * saturate(s1.a) - 1.0f) * uPsConst0.a + uPsConst1.a) * 0.5f);\n"
                 "    float r1_rgb = saturate((2.0f * saturate(s1.a) - 1.0f) * uPsConst1.a);\n"
                 "    float r0_a   = (s1.a >= 0.5f) ? r0_rgb : saturate(s1.a * uPsConst1.a);\n"
                 "    current.rgb  = lerp(t2.rgb, t3.rgb, r1_rgb);\n"
                 "    current.a    = saturate(r0_a);\n";
            break;
        case 25u: // alphadissolve.xpu
            s += "    float r0a = t0.a - (saturate(uPsConst0.a) - 0.5f);\n"
                 "    current.rgb = (r0a >= 0.5f) ? t0.rgb : t1.rgb;\n"
                 "    current.a   = saturate(r0a);\n";
            break;
        case 26u: // alphadissolve_sep.xpu
            s += "    float r0a = t1.b - (saturate(uPsConst0.a) - 0.5f);\n"
                 "    current.rgb = (r0a >= 0.5f) ? t0.rgb : uPsConst1.rgb;\n"
                 "    current.a   = saturate(r0a);\n";
            break;
        default: // 27u: alphadissolve_sep_glo.xpu
            s += "    float4 r1 = float4(saturate(t1.bbb + diffuse.rgb) * t0.rgb, t0.a);\n"
                 "    float r0a = t2.b - (saturate(uPsConst0.a) - 0.5f);\n"
                 "    r1 = (r0a >= 0.5f) ? r1 : uPsConst1;\n"
                 "    r0a = t2.b - (saturate(uPsConst0.b) - 0.5f);\n"
                 "    current = (r0a >= 0.5f) ? r1 : uPsConst2;\n";
            break;
    }

    s += "\n";
    s += kSharedPsAlphaTestAndGammaHLSL;
    if (pt != 3u) { // Skip fog for terrain_fog.xpu which outputs fog directly
        s += kSharedPsFogHLSL;
    }
    s += kSharedPsGammaEpilogueHLSL;
    return s;
}

static void GetShaderCacheFilePaths(
    const char* shaderBaseName, char* outHlslPath, size_t hlslMax, char* outCsoPath, size_t csoMax)
{
    if (outHlslPath && hlslMax > 0) {
        snprintf(outHlslPath, hlslMax, "%s\\shaders\\%s.hlsl", g_appRootDir, shaderBaseName);
    }
    if (outCsoPath && csoMax > 0) {
        snprintf(outCsoPath, csoMax, "%s\\shaders\\%s.cso", g_appRootDir, shaderBaseName);
    }
}

bool LoadShaderCsoFromDisk(const char* shaderBaseName, void** outData, size_t* outSize) {
    if (!shaderBaseName || !outData || !outSize) return false;
    *outData = nullptr;
    *outSize = 0;

    char csoPath[MAX_PATH] = {};
    GetShaderCacheFilePaths(shaderBaseName, nullptr, 0, csoPath, sizeof(csoPath));
    FILE* f = fopen(csoPath, "rb");
    if (!f) return false;

    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0) {
        fclose(f);
        return false;
    }

    void* buf = malloc((size_t)sz);
    if (!buf || fread(buf, 1, (size_t)sz, f) != (size_t)sz) {
        if (buf) free(buf);
        fclose(f);
        return false;
    }
    fclose(f);
    *outData = buf;
    *outSize = (size_t)sz;
    return true;
}

struct ShaderCompileJob {
    std::string baseName;
    std::string hlslSource;
    bool        isPixel;
};

struct ShaderCompileBatchCtx {
    const std::vector<ShaderCompileJob>* jobs;
    volatile LONG                        nextIdx;
    volatile LONG                        doneCount;
    volatile LONG                        failCount;
    volatile LONG                        currentJobIdx;
    volatile LONG                        abortFlag;
};

static DWORD WINAPI ShaderCompileWorkerProc(LPVOID param) {
    ShaderCompileBatchCtx* ctx = reinterpret_cast<ShaderCompileBatchCtx*>(param);
    const size_t total = ctx->jobs->size();
    for (;;) {
        if (ctx->abortFlag != 0) break;
        LONG idx = InterlockedIncrement(&ctx->nextIdx) - 1;
        if (idx < 0 || (size_t)idx >= total) break;

        InterlockedExchange(&ctx->currentJobIdx, idx);
        const ShaderCompileJob& job = (*ctx->jobs)[(size_t)idx];
        char hlslPath[MAX_PATH] = {};
        char csoPath[MAX_PATH]  = {};
        GetShaderCacheFilePaths(job.baseName.c_str(), hlslPath, sizeof(hlslPath), csoPath, sizeof(csoPath));

        FILE* fHlsl = fopen(hlslPath, "wb");
        if (fHlsl) {
            fwrite(job.hlslSource.data(), 1, job.hlslSource.size(), fHlsl);
            fclose(fHlsl);
        }

        ID3DBlob* blob    = nullptr;
        ID3DBlob* errBlob = nullptr;
        HRESULT hr = D3DCompile(
            job.hlslSource.data(), job.hlslSource.size(), job.baseName.c_str(),
            nullptr, nullptr,
            job.isPixel ? "PSMain" : "VSMain",
            job.isPixel ? "ps_4_0" : "vs_4_0",
            D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &blob, &errBlob);
        if (SUCCEEDED(hr) && blob) {
            FILE* fCso = fopen(csoPath, "wb");
            if (fCso) {
                fwrite(blob->GetBufferPointer(), 1, blob->GetBufferSize(), fCso);
                fclose(fCso);
            } else {
                InterlockedIncrement(&ctx->failCount);
            }
            blob->Release();
        } else {
            InterlockedIncrement(&ctx->failCount);
        }
        if (errBlob) errBlob->Release();

        InterlockedIncrement(&ctx->doneCount);
    }
    return 0;
}

void ClearShaderCacheDirectory() {
    char shadersDir[MAX_PATH] = {};
    snprintf(shadersDir, sizeof(shadersDir), "%s\\shaders", g_appRootDir);
    EnsureDirExists(shadersDir);

    char searchPattern[MAX_PATH] = {};
    snprintf(searchPattern, sizeof(searchPattern), "%s\\*", shadersDir);

    WIN32_FIND_DATAA fd = {};
    HANDLE hFind = FindFirstFileA(searchPattern, &fd);
    if (hFind == INVALID_HANDLE_VALUE) return;

    do {
        if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0) {
            char filePath[MAX_PATH] = {};
            snprintf(filePath, sizeof(filePath), "%s\\%s", shadersDir, fd.cFileName);
            DeleteFileA(filePath);
        }
    } while (FindNextFileA(hFind, &fd));

    FindClose(hFind);
}

bool IsShaderCacheReadyOnDisk() {
    char shadersDir[MAX_PATH] = {};
    snprintf(shadersDir, sizeof(shadersDir), "%s\\shaders", g_appRootDir);
    EnsureDirExists(shadersDir);

    char fixedVsCso[MAX_PATH] = {};
    char fixedPsCso[MAX_PATH] = {};
    GetShaderCacheFilePaths("fixed_vs", nullptr, 0, fixedVsCso, sizeof(fixedVsCso));
    GetShaderCacheFilePaths("fixed_ps", nullptr, 0, fixedPsCso, sizeof(fixedPsCso));

    return IsShaderCacheCompiledInIni() &&
           GetFileAttributesA(fixedVsCso) != INVALID_FILE_ATTRIBUTES &&
           GetFileAttributesA(fixedPsCso) != INVALID_FILE_ATTRIBUTES;
}

bool CompileAllGameShadersToCache(PFN_ShaderCompileProgressCallback progressCb, void* userData) {
    ClearShaderCacheDirectory();

    std::vector<ShaderCompileJob> jobs;
    jobs.reserve(320);

    // 1. Fixed-function Pixel & Vertex Shaders (place fixed_ps first so Thread #0 starts it immediately)
    jobs.push_back({ "fixed_ps", BuildFixedPsHLSL(), true });
    jobs.push_back({ "fixed_vs", BuildFixedVsHLSL(), false });

    // 2. All .xpu Pixel Shaders from aPixelShaders[0..27] (0x007D1210) in VertexShaders/xbox
    for (uint32_t pt = 0; pt < 28u; ++pt) {
        const char* relPath = *reinterpret_cast<const char* const*>(0x007D1210u + pt * 8u);
        if (!relPath || reinterpret_cast<uintptr_t>(relPath) < kMinValidXboxAddress) continue;

        char fullPath[MAX_PATH] = {};
        snprintf(fullPath, sizeof(fullPath), "%s\\%s", g_gameRootDir, relPath);
        FILE* f = fopen(fullPath, "rb");
        if (!f) continue;

        uint32_t magic = 0;
        XboxPixelShaderDef def = {};
        bool ok = (fread(&magic, 1, 4, f) == 4 && magic == 0x30425350u &&
                   fread(&def, 1, sizeof(XboxPixelShaderDef), f) == sizeof(XboxPixelShaderDef));
        fclose(f);
        if (!ok) continue;

        const char* s1 = strrchr(relPath, '\\');
        const char* s2 = strrchr(relPath, '/');
        const char* baseName = (s1 > s2) ? (s1 + 1) : (s2 ? (s2 + 1) : relPath);
        jobs.push_back({ baseName, DecompileXpuToHLSL(relPath, pt, &def), true });
    }

    // 3. All .xvu Vertex Shaders from aVertexShaders[0..65] (0x007D0BE0) in VertexShaders/xbox
    std::vector<std::string> seenXvuNames;
    seenXvuNames.reserve(300);
    static const char* const kXvuSuffixes[5] = { ".xvu", "0.xvu", "1.xvu", "2.xvu", "3.xvu" };

    for (uint32_t vsType = 0; vsType < 66u; ++vsType) {
        uint32_t entryVa = ADDR_VERTEX_SHADERS_TABLE + vsType * 24u;
        const char* prefixRel = *reinterpret_cast<const char* const*>(entryVa + 0u);
        const DWORD* pDecl    = *reinterpret_cast<const DWORD* const*>(entryVa + 4u);
        if (!prefixRel || reinterpret_cast<uintptr_t>(prefixRel) < kMinValidXboxAddress ||
            !pDecl     || reinterpret_cast<uintptr_t>(pDecl)     < kMinValidXboxAddress)
        {
            continue;
        }

        XboxDeclElem elems[16] = {};
        uint32_t numElems = 0;
        ParseXboxVertexDeclaration(pDecl, false, elems, &numElems, nullptr);
        if (numElems == 0) continue;

        for (int sfxIdx = 0; sfxIdx < 5; ++sfxIdx) {
            char relFile[MAX_PATH]  = {};
            char fullFile[MAX_PATH] = {};
            snprintf(relFile, sizeof(relFile), "%s%s", prefixRel, kXvuSuffixes[sfxIdx]);
            snprintf(fullFile, sizeof(fullFile), "%s\\%s", g_gameRootDir, relFile);

            const char* s1 = strrchr(relFile, '\\');
            const char* s2 = strrchr(relFile, '/');
            const char* baseName = (s1 > s2) ? (s1 + 1) : (s2 ? (s2 + 1) : relFile);

            bool duplicate = false;
            for (size_t k = 0; k < seenXvuNames.size(); ++k) {
                if (_stricmp(seenXvuNames[k].c_str(), baseName) == 0) {
                    duplicate = true;
                    break;
                }
            }
            if (duplicate) continue;

            FILE* f = fopen(fullFile, "rb");
            if (!f) continue;
            fseek(f, 0, SEEK_END);
            long sz = ftell(f);
            fseek(f, 0, SEEK_SET);
            if (sz < 4) {
                fclose(f);
                continue;
            }
            std::vector<uint8_t> xvuBytes((size_t)sz);
            if (fread(xvuBytes.data(), 1, (size_t)sz, f) != (size_t)sz) {
                fclose(f);
                continue;
            }
            fclose(f);

            const DWORD* pFunction = reinterpret_cast<const DWORD*>(xvuBytes.data());
            if ((pFunction[0] & 0xFFFFu) != 0x2078u) continue;

            seenXvuNames.push_back(baseName);
            jobs.push_back({
                baseName,
                DecompileXvuToHLSL(elems, numElems, pFunction, relFile, reinterpret_cast<uintptr_t>(pDecl)),
                false
            });
        }
    }

    // Balance worker threads (3 threads: 1 for fixed_ps + 2 for the 307 .xpu/.xvu shaders)
    // so all 308 shaders progress evenly across the segmented progress bar.
    SYSTEM_INFO sysInfo = {};
    GetSystemInfo(&sysInfo);
    uint32_t numThreads = sysInfo.dwNumberOfProcessors;
    if (numThreads < 1u) numThreads = 1u;
    if (numThreads > 3u) numThreads = 3u;

    ShaderCompileBatchCtx batchCtx = { &jobs, 0, 0, 0, 0, 0 };
    HANDLE threads[3] = {};
    for (uint32_t t = 0; t < numThreads; ++t) {
        threads[t] = CreateThread(nullptr, 0, ShaderCompileWorkerProc, &batchCtx, 0, nullptr);
    }

    const uint32_t totalJobs = (uint32_t)jobs.size();
    bool cancelled = false;
    while (WaitForMultipleObjects(numThreads, threads, TRUE, 30) == WAIT_TIMEOUT) {
        if (progressCb) {
            LONG done   = batchCtx.doneCount;
            LONG curIdx = batchCtx.currentJobIdx;
            if (done < 0)                  done = 0;
            if ((uint32_t)done > totalJobs) done = (LONG)totalJobs;
            const char* curName = (curIdx >= 0 && (uint32_t)curIdx < totalJobs)
                ? jobs[(size_t)curIdx].baseName.c_str()
                : "fixed_ps";
            if (!progressCb((uint32_t)done, totalJobs, curName, userData)) {
                cancelled = true;
                InterlockedExchange(&batchCtx.abortFlag, 1);
            }
        }
    }

    for (uint32_t t = 0; t < numThreads; ++t) {
        if (threads[t]) CloseHandle(threads[t]);
    }

    if (cancelled) {
        return false;
    }
    if (progressCb && totalJobs > 0) {
        progressCb(totalJobs, totalJobs, jobs.back().baseName.c_str(), userData);
    }
    if (batchCtx.failCount == 0) {
        SetShaderCacheCompiledInIni(true);
    }
    return true;
}

static HRESULT __stdcall Bridge_D3DDevice_CreateVertexShader(
    const DWORD* pDeclaration, const DWORD* pFunction, DWORD* pHandle, DWORD Usage)
{
    (void)Usage;
    if (!pHandle) return E_INVALIDARG;

    XboxShaderDeclInfo info = {};
    info.vsType = -1;
    uint32_t lightVariant = 0u;

    uintptr_t hAddr = reinterpret_cast<uintptr_t>(pHandle);
    if (hAddr >= ADDR_VERTEX_SHADERS_TABLE && hAddr < ADDR_VERTEX_SHADERS_TABLE + 66 * 24) {
        uint32_t offset = (uint32_t)(hAddr - ADDR_VERTEX_SHADERS_TABLE);
        info.vsType = (int32_t)(offset / 24u);
        uint32_t rem = offset % 24u;
        if (rem >= 8u) lightVariant = (rem - 8u) / 4u;
    }

    XboxDeclElem elems[16] = {};
    uint32_t numElems = 0;
    ParseXboxVertexDeclaration(pDeclaration, (pFunction == nullptr), elems, &numElems, &info);

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
        void*  csoData = nullptr;
        size_t csoSize = 0;
        if (info.vsType >= 0 && info.vsType < 66) {
            const char* prefixRel = *reinterpret_cast<const char* const*>(ADDR_VERTEX_SHADERS_TABLE + (uint32_t)info.vsType * 24u);
            if (prefixRel && reinterpret_cast<uintptr_t>(prefixRel) >= kMinValidXboxAddress) {
                const char* s1 = strrchr(prefixRel, '\\');
                const char* s2 = strrchr(prefixRel, '/');
                const char* prefixBase = (s1 > s2) ? (s1 + 1) : (s2 ? (s2 + 1) : prefixRel);
                char xvuBaseName[128] = {};
                snprintf(xvuBaseName, sizeof(xvuBaseName), "%s%u.xvu", prefixBase, lightVariant);
                LoadShaderCsoFromDisk(xvuBaseName, &csoData, &csoSize);
            }
        }

        if (csoData && csoSize > 0) {
            g_d3dDevice->CreateVertexShader(csoData, csoSize, nullptr, &info.gpuVS);
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
            g_d3dDevice->CreateInputLayout(ied, iedCount, csoData, csoSize, &info.gpuInputLayout);
            info.isCompiledXvu = (info.gpuVS != nullptr && info.gpuInputLayout != nullptr);
            free(csoData);
        }
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
