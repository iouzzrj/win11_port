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
    bool                      isCube;
    bool                      isVolume;
    uint32_t                  width;
    uint32_t                  height;
    uint32_t                  depth;
    uint32_t                  mipLevels;
    uint32_t                  texWidth;
    uint32_t                  texHeight;
    ID3D11Texture2D*          tex2D;
    ID3D11Texture3D*          tex3D;
    ID3D11ShaderResourceView* srv;
    ID3D11RenderTargetView*   rtv;
};

static std::vector<CachedTexture> g_texCache;

static void MarkTextureDirtyByData(uint32_t dataPtr) {
    for (size_t i = 0; i < g_texCache.size(); ++i) {
        CachedTexture& c = g_texCache[i];
        uint32_t endPtr = c.dataPtr + static_cast<uint32_t>(c.maxReadableBytes ? c.maxReadableBytes : 1u);
        if ((dataPtr >= c.dataPtr && dataPtr < endPtr) ||
            reinterpret_cast<uint32_t>(c.xboxTex) == dataPtr) {
            c.dirty = true;
            c.isRenderTarget = false;
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

static void DecodeSingleXboxSurfaceToBGRA8(
    const uint8_t* srcData, uint32_t w, uint32_t h, uint32_t xboxFmt,
    bool isSwizzled, uint32_t linearPitch, bool isVolumeSwizzled, uint32_t depth, uint32_t zSlice,
    uint32_t* dstPixels)
{
    if (xboxFmt == 0x0C || xboxFmt == 0x0E || xboxFmt == 0x0F) {
        uint32_t blockBytes = (xboxFmt == 0x0C) ? 8u : 16u;
        uint32_t blocksW = (w + 3u) / 4u;
        uint32_t blocksH = (h + 3u) / 4u;
        for (uint32_t by = 0; by < blocksH; ++by) {
            for (uint32_t bx = 0; bx < blocksW; ++bx) {
                const uint8_t* blk = srcData + (by * blocksW + bx) * blockBytes;
                DecodeDXTBlockToBGRA8(blk, xboxFmt, dstPixels, w, h, bx, by);
            }
        }
        return;
    }

    uint32_t bpp = 4;
    switch (xboxFmt) {
        case 0x00: case 0x01: case 0x0B: case 0x13: case 0x19: case 0x2E: bpp = 1; break;
        case 0x02: case 0x03: case 0x04: case 0x05: case 0x11: case 0x12:
        case 0x1A: case 0x1B: case 0x1C: case 0x1D: case 0x20: case 0x24:
        case 0x25: case 0x27: case 0x28: case 0x2D: bpp = 2; break;
        default: bpp = 4; break;
    }

    std::vector<uint8_t> unswizzled;
    const uint8_t* linearSrc = srcData;
    uint32_t srcPitch = linearPitch ? linearPitch : (w * bpp);

    if (isSwizzled) {
        unswizzled.resize((size_t)w * h * bpp);
        if (isVolumeSwizzled && depth > 1u) {
            UnswizzleBoxSliceToLinear(srcData, w, h, depth, zSlice, bpp, unswizzled.data());
        } else {
            g_pfnXGUnswizzleRect(srcData, w, h, nullptr, unswizzled.data(), w * bpp, nullptr, bpp);
        }
        linearSrc = unswizzled.data();
        srcPitch = w * bpp;
    }

    for (uint32_t y = 0; y < h; ++y) {
        const uint8_t* sRow = linearSrc + (size_t)y * srcPitch;
        uint32_t* dRow = &dstPixels[(size_t)y * w];
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

// Computes the byte size of a single mip level in an Xbox swizzled/DXT container,
// matching FindSurfaceWithinTexture@PixelJar@D3D (0x006B7030..0x006B7058).
static size_t ComputeXboxMipByteSize(uint32_t logW, uint32_t logH, uint32_t logD, uint32_t xboxFmt, uint32_t bitsPerPixel) {
    bool isDxt = (xboxFmt == 0x0Cu || xboxFmt == 0x0Eu || xboxFmt == 0x0Fu);
    uint32_t minLog = isDxt ? 2u : 0u;
    uint32_t effW = (logW > minLog) ? logW : minLog;
    uint32_t effH = (logH > minLog) ? logH : minLog;
    return ((size_t)1u << (effW + effH + logD)) * bitsPerPixel / 8u;
}

static void GenerateBoxMip2D(const uint32_t* src, uint32_t srcW, uint32_t srcH, uint32_t* dst, uint32_t dstW, uint32_t dstH) {
    for (uint32_t y = 0; y < dstH; ++y) {
        uint32_t sy0 = (y * 2 < srcH) ? (y * 2) : (srcH - 1);
        uint32_t sy1 = (y * 2 + 1 < srcH) ? (y * 2 + 1) : sy0;
        for (uint32_t x = 0; x < dstW; ++x) {
            uint32_t sx0 = (x * 2 < srcW) ? (x * 2) : (srcW - 1);
            uint32_t sx1 = (x * 2 + 1 < srcW) ? (x * 2 + 1) : sx0;
            uint32_t p00 = src[sy0 * srcW + sx0];
            uint32_t p10 = src[sy0 * srcW + sx1];
            uint32_t p01 = src[sy1 * srcW + sx0];
            uint32_t p11 = src[sy1 * srcW + sx1];
            uint32_t b = ((p00 & 0xFFu) + (p10 & 0xFFu) + (p01 & 0xFFu) + (p11 & 0xFFu) + 2u) >> 2;
            uint32_t g = (((p00 >> 8) & 0xFFu) + ((p10 >> 8) & 0xFFu) + ((p01 >> 8) & 0xFFu) + ((p11 >> 8) & 0xFFu) + 2u) >> 2;
            uint32_t r = (((p00 >> 16) & 0xFFu) + ((p10 >> 16) & 0xFFu) + ((p01 >> 16) & 0xFFu) + ((p11 >> 16) & 0xFFu) + 2u) >> 2;
            uint32_t a = (((p00 >> 24) & 0xFFu) + ((p10 >> 24) & 0xFFu) + ((p01 >> 24) & 0xFFu) + ((p11 >> 24) & 0xFFu) + 2u) >> 2;
            dst[y * dstW + x] = (a << 24) | (r << 16) | (g << 8) | b;
        }
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
    if (!isSwizzled) {
        isCubeMap = false;
        isVolume  = false;
        depth     = 1u;
        levels    = 1u;
    }

    uint32_t maxDim = (w > h) ? w : h;
    if (depth > maxDim) maxDim = depth;
    uint32_t maxPossibleMips = 1u;
    while ((1u << maxPossibleMips) <= maxDim) ++maxPossibleMips;
    if (levels > maxPossibleMips) levels = maxPossibleMips;
    if (levels == 0) levels = 1u;

    const uint8_t* srcData = reinterpret_cast<const uint8_t*>(pTex->Data);
    size_t maxReadable = GetReadableBytesAt(srcData);
    if (maxReadable < 16) return;

    bool isDxt = (xboxFmt == 0x0C || xboxFmt == 0x0E || xboxFmt == 0x0F);
    uint32_t bpp = 4;
    switch (xboxFmt) {
        case 0x00: case 0x01: case 0x0B: case 0x13: case 0x19: case 0x2E: bpp = 1; break;
        case 0x02: case 0x03: case 0x04: case 0x05: case 0x11: case 0x12:
        case 0x1A: case 0x1B: case 0x1C: case 0x1D: case 0x20: case 0x24:
        case 0x25: case 0x27: case 0x28: case 0x2D: bpp = 2; break;
        default: bpp = 4; break;
    }
    uint32_t bitsPerPixel = isDxt ? ((xboxFmt == 0x0C) ? 4u : 8u) : (bpp * 8u);
    size_t baseSliceBytes = isDxt
        ? ((size_t)((w + 3u) / 4u) * ((h + 3u) / 4u) * ((xboxFmt == 0x0C) ? 8u : 16u))
        : ((size_t)(linearPitch ? linearPitch : (w * bpp)) * h);
    if (baseSliceBytes > maxReadable) return;

    uint32_t baseLogW = (pTex->Format >> 20) & 0x0Fu;
    uint32_t baseLogH = (pTex->Format >> 24) & 0x0Fu;
    uint32_t baseLogD = isVolume ? ((pTex->Format >> 28) & 0x0Fu) : 0u;

    entry.width     = w;
    entry.height    = h;
    entry.depth     = depth;
    entry.mipLevels = levels;
    entry.isCube    = isCubeMap;
    entry.isVolume  = (isVolume && depth > 1u);

    // ------------------------------------------------------------------------
    // Case 1: True Hardware Cubemap (ID3D11Texture2D ArraySize=6, TEXTURECUBE)
    // ------------------------------------------------------------------------
    if (isCubeMap) {
        size_t faceStride = ComputeXboxCubemapFaceStride(pTex->Format, xboxFmt, levels, bitsPerPixel);
        bool faceNonZero[kXboxCubemapFaceCount] = {};
        int firstNonZeroFace = -1;

        for (uint32_t face = 0; face < kXboxCubemapFaceCount; ++face) {
            size_t faceOff = (size_t)face * faceStride;
            if (faceOff + baseSliceBytes <= maxReadable) {
                const uint8_t* faceSrc = srcData + faceOff;
                for (size_t k = 0; k + 4 <= baseSliceBytes; k += 4) {
                    if (*reinterpret_cast<const uint32_t*>(faceSrc + k) != 0u) {
                        faceNonZero[face] = true;
                        break;
                    }
                }
                if (faceNonZero[face] && firstNonZeroFace < 0) {
                    firstNonZeroFace = (int)face;
                }
            }
        }
        if (firstNonZeroFace < 0) {
            firstNonZeroFace = 0;
            faceNonZero[0] = true;
        }

        uint32_t totalSubres = kXboxCubemapFaceCount * levels;
        std::vector<std::vector<uint32_t>> subresPixels(totalSubres);
        std::vector<D3D11_SUBRESOURCE_DATA> initData(totalSubres);

        for (uint32_t face = 0; face < kXboxCubemapFaceCount; ++face) {
            uint32_t srcFaceIdx = faceNonZero[face] ? face : (uint32_t)firstNonZeroFace;
            size_t faceBaseOff = (size_t)srcFaceIdx * faceStride;
            size_t mipOff = 0;
            uint32_t logW = baseLogW, logH = baseLogH;

            for (uint32_t m = 0; m < levels; ++m) {
                uint32_t mipW = (w >> m) ? (w >> m) : 1u;
                uint32_t mipH = (h >> m) ? (h >> m) : 1u;
                uint32_t subIdx = face * levels + m;
                subresPixels[subIdx].assign((size_t)mipW * mipH, 0xFFFFFFFFu);

                size_t mipBytes = ComputeXboxMipByteSize(logW, logH, 0u, xboxFmt, bitsPerPixel);
                if (faceBaseOff + mipOff + mipBytes <= maxReadable) {
                    DecodeSingleXboxSurfaceToBGRA8(
                        srcData + faceBaseOff + mipOff, mipW, mipH, xboxFmt,
                        isSwizzled, 0u, false, 1u, 0u, subresPixels[subIdx].data());
                } else if (m > 0) {
                    uint32_t prevW = (w >> (m - 1)) ? (w >> (m - 1)) : 1u;
                    uint32_t prevH = (h >> (m - 1)) ? (h >> (m - 1)) : 1u;
                    GenerateBoxMip2D(subresPixels[face * levels + (m - 1)].data(), prevW, prevH,
                                     subresPixels[subIdx].data(), mipW, mipH);
                }

                initData[subIdx].pSysMem = subresPixels[subIdx].data();
                initData[subIdx].SysMemPitch = mipW * 4u;
                initData[subIdx].SysMemSlicePitch = 0u;

                mipOff += mipBytes;
                if (logW > 0) --logW;
                if (logH > 0) --logH;
            }
        }

        if (!entry.tex2D || entry.tex3D || entry.texWidth != w || entry.texHeight != h) {
            if (entry.rtv)   { entry.rtv->Release();   entry.rtv = nullptr; }
            if (entry.srv)   { entry.srv->Release();   entry.srv = nullptr; }
            if (entry.tex2D) { entry.tex2D->Release(); entry.tex2D = nullptr; }
            if (entry.tex3D) { entry.tex3D->Release(); entry.tex3D = nullptr; }
            entry.texWidth  = w;
            entry.texHeight = h;

            D3D11_TEXTURE2D_DESC td = {};
            td.Width            = w;
            td.Height           = h;
            td.MipLevels        = levels;
            td.ArraySize        = kXboxCubemapFaceCount;
            td.Format           = DXGI_FORMAT_B8G8R8A8_UNORM;
            td.SampleDesc.Count = 1;
            td.Usage            = D3D11_USAGE_DEFAULT;
            td.BindFlags        = D3D11_BIND_SHADER_RESOURCE;
            td.MiscFlags        = D3D11_RESOURCE_MISC_TEXTURECUBE;

            if (SUCCEEDED(g_d3dDevice->CreateTexture2D(&td, initData.data(), &entry.tex2D))) {
                D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
                srvDesc.Format                      = DXGI_FORMAT_B8G8R8A8_UNORM;
                srvDesc.ViewDimension               = D3D11_SRV_DIMENSION_TEXTURECUBE;
                srvDesc.TextureCube.MostDetailedMip = 0;
                srvDesc.TextureCube.MipLevels       = levels;
                g_d3dDevice->CreateShaderResourceView(entry.tex2D, &srvDesc, &entry.srv);
            }
        } else {
            for (uint32_t face = 0; face < kXboxCubemapFaceCount; ++face) {
                for (uint32_t m = 0; m < levels; ++m) {
                    uint32_t mipW = (w >> m) ? (w >> m) : 1u;
                    uint32_t subIdx = face * levels + m;
                    g_d3dCtx->UpdateSubresource(entry.tex2D, subIdx, nullptr, subresPixels[subIdx].data(), mipW * 4u, 0u);
                }
            }
        }
        entry.dirty = false;
        return;
    }

    // ------------------------------------------------------------------------
    // Case 2: True Hardware 3D Volume Texture (ID3D11Texture3D, TEXTURE3D)
    // ------------------------------------------------------------------------
    if (entry.isVolume) {
        std::vector<std::vector<uint32_t>> mipPixels(levels);
        std::vector<D3D11_SUBRESOURCE_DATA> initData(levels);
        size_t mipOff = 0;
        uint32_t logW = baseLogW, logH = baseLogH, logD = baseLogD;

        for (uint32_t m = 0; m < levels; ++m) {
            uint32_t mipW = (w >> m) ? (w >> m) : 1u;
            uint32_t mipH = (h >> m) ? (h >> m) : 1u;
            uint32_t mipD = (depth >> m) ? (depth >> m) : 1u;
            mipPixels[m].assign((size_t)mipW * mipH * mipD, 0xFFFFFFFFu);

            size_t mipBytes = ComputeXboxMipByteSize(logW, logH, logD, xboxFmt, bitsPerPixel);
            size_t sliceBytesMip = ComputeXboxMipByteSize(logW, logH, 0u, xboxFmt, bitsPerPixel);

            if (mipOff + mipBytes <= maxReadable) {
                const uint8_t* mipSrc = srcData + mipOff;
                for (uint32_t z = 0; z < mipD; ++z) {
                    const uint8_t* zSrc = (isDxt || !isSwizzled) ? (mipSrc + (size_t)z * sliceBytesMip) : mipSrc;
                    uint32_t* dstSlice = &mipPixels[m][(size_t)z * mipW * mipH];
                    DecodeSingleXboxSurfaceToBGRA8(
                        zSrc, mipW, mipH, xboxFmt, isSwizzled, 0u,
                        !isDxt && isSwizzled, mipD, z, dstSlice);
                }
            }

            initData[m].pSysMem          = mipPixels[m].data();
            initData[m].SysMemPitch      = mipW * 4u;
            initData[m].SysMemSlicePitch = mipW * mipH * 4u;

            mipOff += mipBytes;
            if (logW > 0) --logW;
            if (logH > 0) --logH;
            if (logD > 0) --logD;
        }

        if (!entry.tex3D || entry.tex2D || entry.texWidth != w || entry.texHeight != h) {
            if (entry.rtv)   { entry.rtv->Release();   entry.rtv = nullptr; }
            if (entry.srv)   { entry.srv->Release();   entry.srv = nullptr; }
            if (entry.tex2D) { entry.tex2D->Release(); entry.tex2D = nullptr; }
            if (entry.tex3D) { entry.tex3D->Release(); entry.tex3D = nullptr; }
            entry.texWidth  = w;
            entry.texHeight = h;

            D3D11_TEXTURE3D_DESC td3 = {};
            td3.Width     = w;
            td3.Height    = h;
            td3.Depth     = depth;
            td3.MipLevels = levels;
            td3.Format    = DXGI_FORMAT_B8G8R8A8_UNORM;
            td3.Usage     = D3D11_USAGE_DEFAULT;
            td3.BindFlags = D3D11_BIND_SHADER_RESOURCE;

            if (SUCCEEDED(g_d3dDevice->CreateTexture3D(&td3, initData.data(), &entry.tex3D))) {
                D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
                srvDesc.Format                    = DXGI_FORMAT_B8G8R8A8_UNORM;
                srvDesc.ViewDimension             = D3D11_SRV_DIMENSION_TEXTURE3D;
                srvDesc.Texture3D.MostDetailedMip = 0;
                srvDesc.Texture3D.MipLevels       = levels;
                g_d3dDevice->CreateShaderResourceView(entry.tex3D, &srvDesc, &entry.srv);
            }
        } else {
            for (uint32_t m = 0; m < levels; ++m) {
                uint32_t mipW = (w >> m) ? (w >> m) : 1u;
                uint32_t mipH = (h >> m) ? (h >> m) : 1u;
                g_d3dCtx->UpdateSubresource(entry.tex3D, m, nullptr, mipPixels[m].data(), mipW * 4u, mipW * mipH * 4u);
            }
        }
        entry.dirty = false;
        return;
    }

    // ------------------------------------------------------------------------
    // Case 3: Standard 2D Texture with Full Mip Chain (ID3D11Texture2D)
    // ------------------------------------------------------------------------
    std::vector<std::vector<uint32_t>> mipPixels(levels);
    std::vector<D3D11_SUBRESOURCE_DATA> initData(levels);
    size_t mipOff = 0;
    uint32_t logW = baseLogW, logH = baseLogH;

    for (uint32_t m = 0; m < levels; ++m) {
        uint32_t mipW = (w >> m) ? (w >> m) : 1u;
        uint32_t mipH = (h >> m) ? (h >> m) : 1u;
        mipPixels[m].assign((size_t)mipW * mipH, 0xFFFFFFFFu);

        size_t mipBytes = isSwizzled
            ? ComputeXboxMipByteSize(logW, logH, 0u, xboxFmt, bitsPerPixel)
            : baseSliceBytes;

        if (mipOff + mipBytes <= maxReadable) {
            DecodeSingleXboxSurfaceToBGRA8(
                srcData + mipOff, mipW, mipH, xboxFmt,
                isSwizzled, (m == 0) ? linearPitch : 0u, false, 1u, 0u, mipPixels[m].data());
        } else if (m > 0) {
            uint32_t prevW = (w >> (m - 1)) ? (w >> (m - 1)) : 1u;
            uint32_t prevH = (h >> (m - 1)) ? (h >> (m - 1)) : 1u;
            GenerateBoxMip2D(mipPixels[m - 1].data(), prevW, prevH, mipPixels[m].data(), mipW, mipH);
        }

        initData[m].pSysMem          = mipPixels[m].data();
        initData[m].SysMemPitch      = mipW * 4u;
        initData[m].SysMemSlicePitch = 0u;

        mipOff += mipBytes;
        if (logW > 0) --logW;
        if (logH > 0) --logH;
    }

    if (!entry.tex2D || entry.tex3D || entry.texWidth != w || entry.texHeight != h) {
        if (entry.rtv)   { entry.rtv->Release();   entry.rtv = nullptr; }
        if (entry.srv)   { entry.srv->Release();   entry.srv = nullptr; }
        if (entry.tex2D) { entry.tex2D->Release(); entry.tex2D = nullptr; }
        if (entry.tex3D) { entry.tex3D->Release(); entry.tex3D = nullptr; }
        entry.texWidth  = w;
        entry.texHeight = h;

        D3D11_TEXTURE2D_DESC td = {};
        td.Width            = w;
        td.Height           = h;
        td.MipLevels        = levels;
        td.ArraySize        = 1;
        td.Format           = DXGI_FORMAT_B8G8R8A8_UNORM;
        td.SampleDesc.Count = 1;
        td.Usage            = D3D11_USAGE_DEFAULT;
        td.BindFlags        = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;

        if (SUCCEEDED(g_d3dDevice->CreateTexture2D(&td, initData.data(), &entry.tex2D))) {
            g_d3dDevice->CreateShaderResourceView(entry.tex2D, nullptr, &entry.srv);
            g_d3dDevice->CreateRenderTargetView(entry.tex2D, nullptr, &entry.rtv);
        }
    } else if (entry.tex2D) {
        for (uint32_t m = 0; m < levels; ++m) {
            uint32_t mipW = (w >> m) ? (w >> m) : 1u;
            g_d3dCtx->UpdateSubresource(entry.tex2D, m, nullptr, mipPixels[m].data(), mipW * 4u, 0u);
        }
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
    entry.isRenderTarget = true;
    entry.dirty = false;
    entry.width = w;
    entry.height = h;
    entry.texWidth = w;
    entry.texHeight = h;

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

ID3D11ShaderResourceView* GetOrCreateD3D11SRV(
    XboxPixelContainer* pTex, float* outUScale, float* outVScale, bool* outIsCube, bool* outIsVolume)
{
    if (outUScale)   *outUScale   = 1.0f;
    if (outVScale)   *outVScale   = 1.0f;
    if (outIsCube)   *outIsCube   = false;
    if (outIsVolume) *outIsVolume = false;
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
            if (outIsCube)   *outIsCube   = c.isCube;
            if (outIsVolume) *outIsVolume = c.isVolume;
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
                        if (outIsCube)   *outIsCube   = c.isCube;
                        if (outIsVolume) *outIsVolume = c.isVolume;
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
    if (outIsCube)   *outIsCube   = entry.isCube;
    if (outIsVolume) *outIsVolume = entry.isVolume;
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

    // Unbind PS shader resources (2D t0..t3, Cube t4..t7, Volume t8..t11) before switching RTV
    ID3D11ShaderResourceView* nullSrvs[12] = {};
    g_d3dCtx->PSSetShaderResources(0, 12, nullSrvs);

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
    if (!pSourceSurface || !pDestinationSurface || !pSourceSurface->Data || !pDestinationSurface->Data) return;

    uint32_t sw = 0, sh = 0, slev = 1, sfmt = 0, spitch = 0;
    bool sswiz = false;
    DecodeXboxTextureFormat(pSourceSurface, &sw, &sh, &slev, &sfmt, &sswiz, &spitch);

    uint32_t dw = 0, dh = 0, dlev = 1, dfmt = 0, dpitch = 0;
    bool dswiz = false;
    DecodeXboxTextureFormat(pDestinationSurface, &dw, &dh, &dlev, &dfmt, &dswiz, &dpitch);

    if (sw == 0 || sh == 0 || dw == 0 || dh == 0) return;

    // Matching _D3DDevice_CopyRects@20 (0x006B0F40..0x006B1155):
    // When pSourceRectsArray == NULL or cRects == 0, default to 1 full-surface rect {0, 0, sw, sh}.
    // When pDestPointsArray == NULL, each rect's destination point defaults to {srcRect.left, srcRect.top}.
    RECT defaultRect = { 0, 0, static_cast<LONG>(sw), static_cast<LONG>(sh) };
    const RECT* rects = (pSourceRectsArray && cRects > 0) ? pSourceRectsArray : &defaultRect;
    UINT rectCount    = (pSourceRectsArray && cRects > 0) ? cRects : 1u;

    // Check if source is a GPU render target or the main backbuffer
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
                if (g_texCache[i].isRenderTarget || g_texCache[i].tex2D) {
                    srcGpuTex = g_texCache[i].tex2D;
                }
                break;
            }
        }
    }

    bool gpuCopied = false;
    if (srcGpuTex && g_d3dCtx && pSourceSurface->Data != pDestinationSurface->Data) {
        CachedTexture* dstEntry = GetOrCreateRenderTargetEntry(pDestinationSurface);
        if (dstEntry && dstEntry->tex2D) {
            bool isFullCopy = (rectCount == 1u &&
                               rects[0].left <= 0 && rects[0].top <= 0 &&
                               rects[0].right >= static_cast<LONG>(sw) && rects[0].bottom >= static_cast<LONG>(sh) &&
                               (!pDestPointsArray || (pDestPointsArray[0].x == 0 && pDestPointsArray[0].y == 0)) &&
                               sw == dw && sh == dh);
            if (isFullCopy) {
                g_d3dCtx->CopyResource(dstEntry->tex2D, srcGpuTex);
            } else {
                for (UINT i = 0; i < rectCount; ++i) {
                    LONG l = rects[i].left;
                    LONG t = rects[i].top;
                    LONG r = rects[i].right;
                    LONG b = rects[i].bottom;
                    LONG dx = pDestPointsArray ? pDestPointsArray[i].x : l;
                    LONG dy = pDestPointsArray ? pDestPointsArray[i].y : t;

                    if (l < 0) { dx -= l; l = 0; }
                    if (t < 0) { dy -= t; t = 0; }
                    if (dx < 0) { l -= dx; dx = 0; }
                    if (dy < 0) { t -= dy; dy = 0; }
                    if (r > static_cast<LONG>(sw)) r = static_cast<LONG>(sw);
                    if (b > static_cast<LONG>(sh)) b = static_cast<LONG>(sh);

                    LONG wRect = r - l;
                    LONG hRect = b - t;
                    if (dx + wRect > static_cast<LONG>(dw)) wRect = static_cast<LONG>(dw) - dx;
                    if (dy + hRect > static_cast<LONG>(dh)) hRect = static_cast<LONG>(dh) - dy;
                    if (wRect <= 0 || hRect <= 0) continue;

                    D3D11_BOX srcBox = {};
                    srcBox.left   = static_cast<UINT>(l);
                    srcBox.top    = static_cast<UINT>(t);
                    srcBox.front  = 0u;
                    srcBox.right  = static_cast<UINT>(l + wRect);
                    srcBox.bottom = static_cast<UINT>(t + hRect);
                    srcBox.back   = 1u;
                    g_d3dCtx->CopySubresourceRegion(
                        dstEntry->tex2D, 0u, static_cast<UINT>(dx), static_cast<UINT>(dy), 0u,
                        srcGpuTex, 0u, &srcBox);
                }
            }
            gpuCopied = true;
        }
    }

    if (gpuCopied) {
        return;
    }

    // CPU surface copy (swizzled/unswizzled/linear sub-rectangle copy)
    uint32_t bpp = 4;
    switch (sfmt) {
        case 0x00: case 0x01: case 0x0B: case 0x13: case 0x19: case 0x2E: bpp = 1; break;
        case 0x02: case 0x03: case 0x04: case 0x05: case 0x11: case 0x12:
        case 0x1A: case 0x1B: case 0x1C: case 0x1D: case 0x20: case 0x24:
        case 0x25: case 0x27: case 0x28: case 0x2D: bpp = 2; break;
        default: bpp = 4; break;
    }

    typedef void (__stdcall *PFN_XGSwizzleRect)(
        const void* pSource, uint32_t Pitch, const RECT* pRect,
        void* pDest, uint32_t Width, uint32_t Height, const POINT* pPoint, uint32_t BytesPerPixel);
    PFN_XGSwizzleRect fnSwiz = reinterpret_cast<PFN_XGSwizzleRect>(ADDR_XG_SWIZZLE_RECT);

    uint32_t srcRowPitch = spitch ? spitch : (sw * bpp);
    uint32_t dstRowPitch = dpitch ? dpitch : (dw * bpp);
    const uint8_t* srcBytes = reinterpret_cast<const uint8_t*>(pSourceSurface->Data);
    uint8_t* dstBytes       = reinterpret_cast<uint8_t*>(pDestinationSurface->Data);

    for (UINT i = 0; i < rectCount; ++i) {
        RECT r = rects[i];
        POINT pt = pDestPointsArray ? pDestPointsArray[i] : POINT{ r.left, r.top };

        if (r.left < 0) { pt.x -= r.left; r.left = 0; }
        if (r.top  < 0) { pt.y -= r.top;  r.top  = 0; }
        if (pt.x   < 0) { r.left -= pt.x; pt.x   = 0; }
        if (pt.y   < 0) { r.top  -= pt.y; pt.y   = 0; }
        if (r.right  > static_cast<LONG>(sw)) r.right  = static_cast<LONG>(sw);
        if (r.bottom > static_cast<LONG>(sh)) r.bottom = static_cast<LONG>(sh);
        if (pt.x + (r.right - r.left) > static_cast<LONG>(dw)) r.right  = r.left + (static_cast<LONG>(dw) - pt.x);
        if (pt.y + (r.bottom - r.top) > static_cast<LONG>(dh)) r.bottom = r.top  + (static_cast<LONG>(dh) - pt.y);
        if (r.right <= r.left || r.bottom <= r.top) continue;

        if (!sswiz && dswiz) {
            fnSwiz(srcBytes, srcRowPitch, &r, dstBytes, dw, dh, &pt, bpp);
        } else if (sswiz && !dswiz) {
            g_pfnXGUnswizzleRect(srcBytes, sw, sh, &r, dstBytes, dstRowPitch, &pt, bpp);
        } else if (sswiz && dswiz) {
            if (r.left == 0 && r.top == 0 && r.right == static_cast<LONG>(sw) && r.bottom == static_cast<LONG>(sh) &&
                pt.x == 0 && pt.y == 0 && sw == dw && sh == dh && srcBytes != dstBytes)
            {
                memcpy(dstBytes, srcBytes, (size_t)sw * sh * bpp);
            } else {
                uint32_t rw = static_cast<uint32_t>(r.right - r.left);
                uint32_t rh = static_cast<uint32_t>(r.bottom - r.top);
                std::vector<uint8_t> tmp((size_t)rw * rh * bpp);
                POINT zeroPt = { 0, 0 };
                RECT  tmpRect = { 0, 0, static_cast<LONG>(rw), static_cast<LONG>(rh) };
                g_pfnXGUnswizzleRect(srcBytes, sw, sh, &r, tmp.data(), rw * bpp, &zeroPt, bpp);
                fnSwiz(tmp.data(), rw * bpp, &tmpRect, dstBytes, dw, dh, &pt, bpp);
            }
        } else {
            uint32_t rw = static_cast<uint32_t>(r.right - r.left);
            uint32_t rh = static_cast<uint32_t>(r.bottom - r.top);
            for (uint32_t y = 0; y < rh; ++y) {
                const uint8_t* sRow = srcBytes + (size_t)(r.top + y) * srcRowPitch + (size_t)r.left * bpp;
                uint8_t*       dRow = dstBytes + (size_t)(pt.y + y)  * dstRowPitch + (size_t)pt.x   * bpp;
                memmove(dRow, sRow, (size_t)rw * bpp);
            }
        }
    }
    MarkTextureDirtyByData(pDestinationSurface->Data);
}

void InitXboxD3D8TextureBridges() {
    constexpr uint32_t kAddrD3DVolumeTextureLockBox = 0x006B3470u;
    InstallBridge(0x005F41F0, &Bridge_rlMain_SetRenderTarget);                    // rlMain_SetRenderTarget
    InstallBridge(0x006B1900, &Bridge_D3DDevice_SetTexture);                      // _D3DDevice_SetTexture@8
    InstallBridge(0x006B33D0, &Bridge_D3DTexture_LockRect);                       // _D3DTexture_LockRect@20
    InstallBridge(kAddrD3DVolumeTextureLockBox, &Bridge_D3DVolumeTexture_LockBox);// _D3DVolumeTexture_LockBox@20
    InstallBridge(0x006B45A0, &Bridge_D3DSurface_LockRect);                       // _D3DSurface_LockRect@16
    InstallBridge(0x006B0F40, &Bridge_D3DDevice_CopyRects);                       // _D3DDevice_CopyRects@20
}
