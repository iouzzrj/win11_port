#include "ghost_runtime.h"
#include "ghost_addresses.h"
#include "xbox_d3d8_types.h"
#include <d3d11.h>
#include <vector>

// Xbox _XGUnswizzleRect@32 in Ghost.exe (0x0071FE62)
typedef void (__stdcall *PFN_XGUnswizzleRect)(
    const void* pSource, uint32_t Width, uint32_t Height, const RECT* pRect,
    void* pDest, uint32_t Pitch, const POINT* pPoint, uint32_t BytesPerPixel);
static PFN_XGUnswizzleRect g_pfnXGUnswizzleRect = reinterpret_cast<PFN_XGUnswizzleRect>(ADDR_XG_UNSWIZZLE_RECT);

// Lock2DSurface@PixelJar@D3D (0x006B7150) & Lock3DSurface@PixelJar@D3D (0x006B71F0)
typedef void (__stdcall *PFN_Lock2DSurface)(
    XboxPixelContainer* pContainer, uint32_t FaceType, UINT Level,
    XboxLockedRect* pLockedRect, const RECT* pRect, DWORD Flags);
static PFN_Lock2DSurface g_pfnLock2DSurface = reinterpret_cast<PFN_Lock2DSurface>(ADDR_PIXELJAR_LOCK_2D);

typedef void (__stdcall *PFN_Lock3DSurface)(
    XboxPixelContainer* pContainer, UINT Level,
    void* pLockedVolume, const void* pBox, DWORD Flags);
static PFN_Lock3DSurface g_pfnLock3DSurface = reinterpret_cast<PFN_Lock3DSurface>(ADDR_PIXELJAR_LOCK_3D);

XboxPixelContainer* g_boundTextures[4] = {};

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

void DecodeXboxTextureFormat(
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

ID3D11ShaderResourceView* GetOrCreateD3D11SRV(XboxPixelContainer* pTex, float* outUScale, float* outVScale) {
    if (outUScale) *outUScale = 1.0f;
    if (outVScale) *outVScale = 1.0f;
    if (!pTex || !pTex->Data || (uintptr_t)pTex->Data < 0x00400000) return nullptr;

    // Check if pTex is the Xbox main backbuffer (bound as a source texture by bloom/blur/refraction passes)
    XboxPixelContainer* mainBB0 = *reinterpret_cast<XboxPixelContainer**>(ADDR_MAIN_BACKBUFFER_PTR);
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
// SetTexture, Offscreen RenderTarget, LockRect, LockBox & CopyRects Bridges
// ============================================================================

static void __stdcall Bridge_D3DDevice_SetTexture(DWORD Stage, XboxPixelContainer* pTexture) {
    if (Stage < 4) {
        g_boundTextures[Stage] = pTexture;
        *reinterpret_cast<XboxPixelContainer**>(ADDR_D3D_DEVICE + 0x1A58 + Stage * 4) = pTexture;
    }
}

static void ApplyRenderTargetSurfaceD3D11(XboxPixelContainer* pRenderTarget, XboxPixelContainer* pZStencil) {
    (void)pZStencil;
    if (!g_d3dCtx) return;

    // Unbind PS shader resources before switching RTV to avoid D3D11 SRV/RTV hazard
    ID3D11ShaderResourceView* nullSrvs[4] = { nullptr, nullptr, nullptr, nullptr };
    g_d3dCtx->PSSetShaderResources(0, 4, nullSrvs);

    XboxPixelContainer* mainBB0 = *reinterpret_cast<XboxPixelContainer**>(ADDR_MAIN_BACKBUFFER_PTR);
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
void __cdecl Bridge_rlMain_SetRenderTarget(void* pTex, int bClear, int bFast) {
    (void)bFast;
    void* prevRT = *reinterpret_cast<void**>(ADDR_CUR_RENDERTARGET_PTR);
    if (pTex == prevRT) return;
    *reinterpret_cast<void**>(ADDR_CUR_RENDERTARGET_PTR) = pTex; // pCurRenderTarget
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
        XboxPixelContainer* mainBB0 = *reinterpret_cast<XboxPixelContainer**>(ADDR_MAIN_BACKBUFFER_PTR);
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
            PFN_XGSwizzleRect fnSwiz = reinterpret_cast<PFN_XGSwizzleRect>(ADDR_XG_SWIZZLE_RECT);
            fnSwiz(reinterpret_cast<const void*>(pSourceSurface->Data), spitch ? spitch : sw * bpp, nullptr,
                   reinterpret_cast<void*>(pDestinationSurface->Data), dw, dh, nullptr, bpp);
        } else {
            size_t bytes = (spitch ? spitch : sw * bpp) * sh;
            memcpy(reinterpret_cast<void*>(pDestinationSurface->Data), reinterpret_cast<const void*>(pSourceSurface->Data), bytes);
        }
        MarkTextureDirtyByData(pDestinationSurface->Data);
    }
}

void InitXboxD3D8TextureBridges() {
    constexpr uint32_t kAddrD3DVolumeTextureLockBox = 0x006B3470u;
    InstallBridge(0x005F41F0, &Bridge_rlMain_SetRenderTarget,                     "rlMain_SetRenderTarget");
    InstallBridge(0x006B1900, &Bridge_D3DDevice_SetTexture,                       "_D3DDevice_SetTexture@8");
    InstallBridge(0x006B33D0, &Bridge_D3DTexture_LockRect,                        "_D3DTexture_LockRect@20");
    InstallBridge(kAddrD3DVolumeTextureLockBox, &Bridge_D3DVolumeTexture_LockBox, "_D3DVolumeTexture_LockBox@20");
    InstallBridge(0x006B45A0, &Bridge_D3DSurface_LockRect,                        "_D3DSurface_LockRect@16");
    InstallBridge(0x006B0F40, &Bridge_D3DDevice_CopyRects,                        "_D3DDevice_CopyRects@20");
}
