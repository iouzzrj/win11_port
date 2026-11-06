#include "ghost_runtime.h"
#include "ghost_addresses.h"
#include "xbox_d3d8_types.h"
#include <d3d11.h>
#include <math.h>
#include <vector>

// ============================================================================
// cDivxVideo (.vid Movie Player in VIDPlayerXB.obj) 733.333 MHz RDTSC Virtualization
// ============================================================================
// Original cDivxVideo::FrameMove (0x0043DE80) and cDivxVideo::Render (0x0043DEE0)
// compute elapsed milliseconds via `(rdtsc * 3) / 0x2191C0` (2,200,000), assuming the
// Original Xbox 733.333 MHz Pentium III CPU. We virtualize the `rdtsc * 3` step to
// return exact 733.333 MHz Xbox CPU cycles * 3 in edx:eax while letting Ghost.exe's
// native cDivxVideo::FrameMove and cDivxVideo::Render execute unmodified.

static LARGE_INTEGER g_qpcFreq  = {};
static LARGE_INTEGER g_qpcStart = {};
static bool          g_qpcInit  = false;

static uint64_t QueryXbox733MHzCyclesTimes3() {
    if (!g_qpcInit) {
        QueryPerformanceFrequency(&g_qpcFreq);
        QueryPerformanceCounter(&g_qpcStart);
        g_qpcInit = true;
    }
    LARGE_INTEGER now = {};
    QueryPerformanceCounter(&now);
    uint64_t elapsedTicks = static_cast<uint64_t>(now.QuadPart - g_qpcStart.QuadPart);
    uint64_t freq = static_cast<uint64_t>(g_qpcFreq.QuadPart);
    if (freq == 0) {
        return static_cast<uint64_t>(GetTickCount64()) * 0x2191C0ULL;
    }
    // 733,333,333.333 Hz * 3 = 2,200,000,000 Hz = 2,200,000 (0x2191C0) ticks per millisecond
    uint64_t secPart = elapsedTicks / freq;
    uint64_t remPart = elapsedTicks % freq;
    return secPart * 2200000000ULL + (remPart * 2200000000ULL) / freq;
}

static uint64_t __cdecl Bridge_DivxRdtscTimes3_FrameMove() {
    return QueryXbox733MHzCyclesTimes3();
}

static uint64_t __cdecl Bridge_DivxRdtscTimes3_Render() {
    UpdateXboxKernelTick();
    UpdateXboxInputTick();
    UpdateXboxAudioTick();
    SwitchToThread();
    return QueryXbox733MHzCyclesTimes3();
}

static void PatchCallAndNops(uint32_t addr, const void* targetFn, size_t totalBytes) {
    DWORD oldProt = 0;
    VirtualProtect(reinterpret_cast<void*>(addr), totalBytes, PAGE_EXECUTE_READWRITE, &oldProt);
    uint8_t* p = reinterpret_cast<uint8_t*>(addr);
    p[0] = 0xE8; // CALL rel32
    *reinterpret_cast<int32_t*>(p + 1) = static_cast<int32_t>(
        reinterpret_cast<uintptr_t>(targetFn) - (static_cast<uintptr_t>(addr) + 5u));
    for (size_t i = 5; i < totalBytes; ++i) {
        p[i] = 0x90; // NOP
    }
    VirtualProtect(reinterpret_cast<void*>(addr), totalBytes, oldProt, &oldProt);
}

// ============================================================================
// YUY2 Hardware Overlay Emulation for .vid Movies
// Faithfully reproduces _D3DDevice_UpdateOverlay@20 (0x006B30D0),
// _D3DDevice_EnableOverlay@4 (0x006B3250), and _D3DDevice_GetOverlayUpdateStatus@0 (0x006B30B0).
// ============================================================================

static std::vector<uint32_t> g_overlayFrameBuf;
static bool                  g_overlayEnabled     = false;
static bool                  g_overlayHasData     = false;
static RECT                  g_overlayDstRect     = { 0, 0, 640, 480 };
static bool                  g_overlayUseColorKey = false;
static uint32_t              g_overlayColorKey    = 0u;

static void __stdcall Bridge_D3DDevice_EnableOverlay(BOOL bEnable) {
    // In Ghost.exe (0x006B3250), EnableOverlay stops active NV_PVIDEO_BUFFER ([esi+8704h] = 1)
    // and resets the NV_PVIDEO overlay registers; the overlay only scans out after UpdateOverlay
    // sets NV_PVIDEO_BUFFER = 1 ([esi+8700h] = 1) while enabled.
    g_overlayEnabled = (bEnable != FALSE);
    if (!g_overlayEnabled) {
        g_overlayHasData = false;
        *reinterpret_cast<uint32_t*>(ADDR_D3D_DEVICE + 0x1C1C) = 0u;
    }
}

static BOOL __stdcall Bridge_D3DDevice_GetOverlayUpdateStatus() {
    // In Ghost.exe (0x006B30B0): returns (pDevice->m_OverlaySwapCount != pDevice->m_SwapCount).
    // Since FreeVideoAndResetGamma (0x0043B4B3) spins on this without calling Swap inside the loop
    // (relying on Xbox hardware VBlank interrupt to advance m_SwapCount), advance m_SwapCount here
    // so the Device state fields match and the spin loop completes cleanly.
    uint32_t* pOverlaySwap = reinterpret_cast<uint32_t*>(ADDR_D3D_DEVICE + 0x2410);
    uint32_t* pSwapCount   = reinterpret_cast<uint32_t*>(ADDR_D3D_DEVICE + 0x1DE8);
    if (*pOverlaySwap == *pSwapCount) {
        (*pSwapCount)++;
    }
    return (*pOverlaySwap != *pSwapCount) ? TRUE : FALSE;
}

void FlushXboxOverlayToBackbuffer() {
    if (!g_overlayEnabled || !g_overlayHasData) return;
    if (g_d3dDevice && g_d3dCtx && g_backBufferTex && g_overlayFrameBuf.size() == 640u * 480u) {
        int32_t l = g_overlayDstRect.left   < 0   ? 0   : (g_overlayDstRect.left   > 640 ? 640 : g_overlayDstRect.left);
        int32_t t = g_overlayDstRect.top    < 0   ? 0   : (g_overlayDstRect.top    > 480 ? 480 : g_overlayDstRect.top);
        int32_t r = g_overlayDstRect.right  < 0   ? 0   : (g_overlayDstRect.right  > 640 ? 640 : g_overlayDstRect.right);
        int32_t b = g_overlayDstRect.bottom < 0   ? 0   : (g_overlayDstRect.bottom > 480 ? 480 : g_overlayDstRect.bottom);
        if (r > l && b > t) {
            D3D11_BOX dstBox = {
                static_cast<UINT>(l), static_cast<UINT>(t), 0u,
                static_cast<UINT>(r), static_cast<UINT>(b), 1u
            };
            if (g_overlayUseColorKey) {
                static ID3D11Texture2D* s_overlayStagingTex = nullptr;
                if (!s_overlayStagingTex) {
                    D3D11_TEXTURE2D_DESC td = {};
                    td.Width            = 640u;
                    td.Height           = 480u;
                    td.MipLevels        = 1u;
                    td.ArraySize        = 1u;
                    td.Format           = DXGI_FORMAT_B8G8R8A8_UNORM;
                    td.SampleDesc.Count = 1u;
                    td.Usage            = D3D11_USAGE_STAGING;
                    td.CPUAccessFlags   = D3D11_CPU_ACCESS_READ;
                    g_d3dDevice->CreateTexture2D(&td, nullptr, &s_overlayStagingTex);
                }
                if (s_overlayStagingTex) {
                    g_d3dCtx->CopySubresourceRegion(s_overlayStagingTex, 0, static_cast<UINT>(l), static_cast<UINT>(t), 0, g_backBufferTex, 0, &dstBox);
                    D3D11_MAPPED_SUBRESOURCE mapped = {};
                    if (SUCCEEDED(g_d3dCtx->Map(s_overlayStagingTex, 0, D3D11_MAP_READ, 0, &mapped))) {
                        uint32_t keyRgb = g_overlayColorKey & 0x00FFFFFFu;
                        for (int32_t y = t; y < b; ++y) {
                            const uint32_t* bbRow = reinterpret_cast<const uint32_t*>(
                                static_cast<const uint8_t*>(mapped.pData) + static_cast<size_t>(y) * mapped.RowPitch);
                            uint32_t* ovRow = &g_overlayFrameBuf[static_cast<size_t>(y) * 640u];
                            for (int32_t x = l; x < r; ++x) {
                                if ((bbRow[x] & 0x00FFFFFFu) != keyRgb) {
                                    ovRow[x] = bbRow[x];
                                }
                            }
                        }
                        g_d3dCtx->Unmap(s_overlayStagingTex, 0);
                    }
                }
            }
            const uint32_t* srcPtr = &g_overlayFrameBuf[static_cast<size_t>(t) * 640u + static_cast<size_t>(l)];
            g_d3dCtx->UpdateSubresource(g_backBufferTex, 0, &dstBox, srcPtr, 640u * 4u, 0);
        }
    }
    g_overlayHasData = false;
}

static void __stdcall Bridge_D3DDevice_UpdateOverlay(
    XboxPixelContainer* pSurface, const RECT* pSrcRect, const RECT* pDstRect,
    BOOL bColorKey, DWORD ColorKey)
{
    if (!pSurface || !pSurface->Data) return;

    // 1. Update D3DDevice overlay tracking fields (0x006B30D3..0x006B30E9)
    uint32_t swapCount = *reinterpret_cast<uint32_t*>(ADDR_D3D_DEVICE + 0x1DE8);
    *reinterpret_cast<XboxPixelContainer**>(ADDR_D3D_DEVICE + 0x1C1C) = pSurface;
    *reinterpret_cast<uint32_t*>(ADDR_D3D_DEVICE + 0x2410)            = swapCount;

    uint32_t surfW = 0, surfH = 0, levels = 1, xboxFmt = 0, pitch = 0;
    bool isSwizzled = false;
    DecodeXboxTextureFormat(pSurface, &surfW, &surfH, &levels, &xboxFmt, &isSwizzled, &pitch);
    if (surfW == 0 || surfH == 0 || surfW > 2048 || surfH > 2048) return;
    if (pitch == 0) pitch = surfW * 2;

    // 2. Extract pSrcRect & pDstRect (0x006B30F8..0x006B3165)
    int32_t srcLeft = pSrcRect ? (pSrcRect->left & ~1) : 0;
    int32_t srcTop  = pSrcRect ? (pSrcRect->top  & ~1) : 0;
    int32_t srcW    = (pSrcRect && pSrcRect->right  > pSrcRect->left) ? (pSrcRect->right  - pSrcRect->left) : (int32_t)surfW;
    int32_t srcH    = (pSrcRect && pSrcRect->bottom > pSrcRect->top)  ? (pSrcRect->bottom - pSrcRect->top)  : (int32_t)surfH;

    int32_t dstLeft = pDstRect ? pDstRect->left : 0;
    int32_t dstTop  = pDstRect ? pDstRect->top  : 0;
    int32_t dstW    = (pDstRect && pDstRect->right  > pDstRect->left) ? (pDstRect->right  - pDstRect->left) : srcW;
    int32_t dstH    = (pDstRect && pDstRect->bottom > pDstRect->top)  ? (pDstRect->bottom - pDstRect->top)  : srcH;

    if (srcW <= 0 || srcH <= 0 || dstW <= 0 || dstH <= 0) return;
    if (srcLeft < 0) srcLeft = 0;
    if (srcTop  < 0) srcTop  = 0;
    if (srcLeft + srcW > (int32_t)surfW) srcW = (int32_t)surfW - srcLeft;
    if (srcTop  + srcH > (int32_t)surfH) srcH = (int32_t)surfH - srcTop;
    if (srcW <= 0 || srcH <= 0) return;

    g_overlayDstRect.left   = dstLeft;
    g_overlayDstRect.top    = dstTop;
    g_overlayDstRect.right  = dstLeft + dstW;
    g_overlayDstRect.bottom = dstTop  + dstH;
    g_overlayUseColorKey    = (bColorKey != FALSE);
    g_overlayColorKey       = ColorKey;

    // 3. Decode source YUY2 (0x24) / UYVY (0x25) region to BGRA8
    std::vector<uint32_t> decodedSrc((size_t)srcW * (size_t)srcH);
    const uint8_t* srcBase = reinterpret_cast<const uint8_t*>(pSurface->Data)
                           + (size_t)srcTop * pitch + (size_t)srcLeft * 2u;

    for (int32_t y = 0; y < srcH; ++y) {
        const uint8_t* sRow = srcBase + (size_t)y * pitch;
        uint32_t* dRow = &decodedSrc[(size_t)y * (size_t)srcW];
        for (int32_t x = 0; x < srcW; x += 2) {
            int y0, u, y1, v;
            if (xboxFmt == 0x25) { // D3DFMT_UYVY
                u  = (int)sRow[x * 2 + 0] - 128;
                y0 = (int)sRow[x * 2 + 1];
                v  = (int)sRow[x * 2 + 2] - 128;
                y1 = (int)sRow[x * 2 + 3];
            } else {               // D3DFMT_YUY2 (0x24)
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

            if (x + 1 < srcW) {
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

    // 4. Scale into 640x480 overlay plane using NV_PVIDEO 12.20 fixed-point step ratios
    // (0x006B3197..0x006B31F8: ds_dx = ((srcW - 1) << 20) / (dstW - 1), dt_dy = ((srcH - 1) << 20) / (dstH - 1))
    uint32_t dsDx = (dstW > 1) ? (((uint32_t)(srcW - 1) << 20) / (uint32_t)(dstW - 1)) : 0x100000u;
    uint32_t dtDy = (dstH > 1) ? (((uint32_t)(srcH - 1) << 20) / (uint32_t)(dstH - 1)) : 0x100000u;

    g_overlayFrameBuf.assign(640u * 480u, (bColorKey != FALSE) ? (ColorKey | 0xFF000000u) : 0xFF000000u);
    if (dstLeft == 0 && dstTop == 0 && dstW == 640 && dstH == 480 && srcW == 640 && srcH == 480) {
        memcpy(g_overlayFrameBuf.data(), decodedSrc.data(), 640u * 480u * sizeof(uint32_t));
    } else {
        for (int32_t dy = 0; dy < dstH; ++dy) {
            int32_t outY = dstTop + dy;
            if (outY < 0 || outY >= 480) continue;
            uint32_t sy = ((uint32_t)dy * dtDy) >> 20;
            if (sy >= (uint32_t)srcH) sy = (uint32_t)(srcH - 1);
            const uint32_t* sLine = &decodedSrc[(size_t)sy * (size_t)srcW];
            uint32_t* dLine = &g_overlayFrameBuf[(size_t)outY * 640u];
            for (int32_t dx = 0; dx < dstW; ++dx) {
                int32_t outX = dstLeft + dx;
                if (outX < 0 || outX >= 640) continue;
                uint32_t sx = ((uint32_t)dx * dsDx) >> 20;
                if (sx >= (uint32_t)srcW) sx = (uint32_t)(srcW - 1);
                dLine[outX] = sLine[sx];
            }
        }
    }

    // Setting NV_PVIDEO_BUFFER = 1 ([esi+8700h] = 1 at 0x006B3233) activates the overlay plane
    g_overlayEnabled = true;
    g_overlayHasData = true;

    UpdateXboxKernelTick();
    UpdateXboxInputTick();
    UpdateXboxAudioTick();
}

void InitXboxVideo() {
    // 1. Virtualize the 13-byte `rdtsc * 3` blocks in cDivxVideo::FrameMove (0x0043DE97..0x0043DEA3)
    //    and cDivxVideo::Render (0x0043DF56..0x0043DF62) to 733.333 MHz Xbox CPU cycles while letting
    //    both functions execute their native Ghost.exe machine code.
    PatchCallAndNops(0x0043DE97, &Bridge_DivxRdtscTimes3_FrameMove, 13u);
    PatchCallAndNops(0x0043DF56, &Bridge_DivxRdtscTimes3_Render,    13u);

    // 2. D3DDevice YUY2 Hardware Overlay bridges
    InstallBridge(0x006B3250, &Bridge_D3DDevice_EnableOverlay);              // _D3DDevice_EnableOverlay@4
    InstallBridge(0x006B30B0, &Bridge_D3DDevice_GetOverlayUpdateStatus);     // _D3DDevice_GetOverlayUpdateStatus@0
    InstallBridge(0x006B30D0, &Bridge_D3DDevice_UpdateOverlay);              // _D3DDevice_UpdateOverlay@20
}
