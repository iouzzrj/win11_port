#include "ghost_runtime.h"
#include "ghost_addresses.h"
#include "xbox_d3d8_types.h"
#include <d3d11.h>
#include <math.h>
#include <vector>

// ============================================================================
// cDivxVideo (.vid Movie Player in VIDPlayerXB.obj) Wall-Clock Frame Pacing Bridges
// ============================================================================
// Original cDivxVideo::FrameMove (0x0043DE80) and cDivxVideo::Render (0x0043DEE0)
// computed elapsed milliseconds via `(rdtsc * 3) / 2200000`, assuming the Original Xbox
// 733.333 MHz Pentium III CPU. On modern GHz CPUs, RDTSC ticks 5x-6x faster, causing
// blizzard_logo.vid, Nihilistic_Logo.vid, and Attract_Mode.vid to play in fast-forward
// and overflow VIDAudioHelper's packet ring buffer.

static constexpr uint32_t kDivxRenderModeOffset           = 0x000u;
static constexpr uint32_t kDivxPlayerOffset               = 0x004u;
static constexpr uint32_t kDivxRenderOffset               = 0x294u;
static constexpr uint32_t kDivxDecodeNextFrameOffset      = 0x2BCu;
static constexpr uint32_t kDivxFirstFrameOffset           = 0x2C0u;
static constexpr uint32_t kDivxLastTimeMsOffset           = 0x2C4u;
static constexpr uint32_t kDivxCurrentTimeMsOffset        = 0x2C8u;
static constexpr uint32_t kDivxTargetTimeMsOffset         = 0x2D0u;
static constexpr uint32_t kDivxFrameDurationMsOffset      = 0x2D8u;
static constexpr uint32_t kDivxPausedOffset               = 0x2DCu;

static constexpr uint32_t kDivxRenderModeTextureSwizzled  = 0u;
static constexpr uint32_t kDivxRenderModeTextureLinear    = 1u;
static constexpr uint32_t kDivxRenderModeSurface          = 2u;
static constexpr uint32_t kDivxRenderModeOverlay          = 3u;
static constexpr double   kDivxDefaultFrameDurationMs     = 1000.0 / 30.0;
static constexpr double   kDivxSleepThresholdMs           = 2.0;
static constexpr double   kDivxMaxLagCatchupMs            = 100.0;
static constexpr DWORD    kDivxWaitSleepMs                = 1u;

static HRESULT __fastcall Bridge_cDivxVideo_FrameMove(uint8_t* thisPtr, int dummyEdx) {
    (void)dummyEdx;
    if (!thisPtr) return 1;

    uint32_t bFirstFrame = *reinterpret_cast<uint32_t*>(thisPtr + kDivxFirstFrameOffset);
    if (bFirstFrame == 0) {
        *reinterpret_cast<uint32_t*>(thisPtr + kDivxDecodeNextFrameOffset) = 1u;
    }

    if (*reinterpret_cast<uint32_t*>(thisPtr + kDivxDecodeNextFrameOffset) != 0) {
        typedef uint8_t (__fastcall *PFN_DecodeNextFrame)(void* playerThis, int edx);
        PFN_DecodeNextFrame fnDecode = reinterpret_cast<PFN_DecodeNextFrame>(kAddrVidSimplePlayerDecodeNextFrame);
        uint8_t decodedOk = fnDecode(thisPtr + kDivxPlayerOffset, 0);
        if (!decodedOk) {
            return 1; // End of .vid stream
        }
    }
    return 0;
}

static HRESULT __fastcall Bridge_cDivxVideo_Render(uint8_t* thisPtr, int dummyEdx) {
    (void)dummyEdx;
    if (!thisPtr) return 0;

    typedef uint8_t (__fastcall *PFN_RenderToTexture)(void* renderThis, int edx, int mode);
    typedef uint8_t (__fastcall *PFN_RenderNoArg)(void* renderThis, int edx);
    void* pRender = thisPtr + kDivxRenderOffset;
    uint32_t renderMode = *reinterpret_cast<uint32_t*>(thisPtr + kDivxRenderModeOffset);

    switch (renderMode) {
        case kDivxRenderModeTextureSwizzled:
            reinterpret_cast<PFN_RenderToTexture>(kAddrVidRenderToTexture)(pRender, 0, 0);
            break;
        case kDivxRenderModeTextureLinear:
            reinterpret_cast<PFN_RenderToTexture>(kAddrVidRenderToTexture)(pRender, 0, 1);
            break;
        case kDivxRenderModeSurface:
            reinterpret_cast<PFN_RenderNoArg>(kAddrVidRenderToSurface)(pRender, 0);
            break;
        case kDivxRenderModeOverlay:
            reinterpret_cast<PFN_RenderNoArg>(kAddrVidRenderToOverlay)(pRender, 0);
            break;
        default:
            break;
    }

    typedef void (__fastcall *PFN_PlayerNoArg)(void* playerThis, int edx);
    void* pPlayer = thisPtr + kDivxPlayerOffset;

    if (*reinterpret_cast<uint32_t*>(thisPtr + kDivxPausedOffset) == 0) {
        uint32_t* pFirstFrame    = reinterpret_cast<uint32_t*>(thisPtr + kDivxFirstFrameOffset);
        uint32_t* pLastTimeMs    = reinterpret_cast<uint32_t*>(thisPtr + kDivxLastTimeMsOffset);
        double*   pCurrentTimeMs = reinterpret_cast<double*>(thisPtr + kDivxCurrentTimeMsOffset);
        double*   pTargetTimeMs  = reinterpret_cast<double*>(thisPtr + kDivxTargetTimeMsOffset);
        float     frameDurMs     = *reinterpret_cast<float*>(thisPtr + kDivxFrameDurationMsOffset);
        double    frameStepMs    = (_finite(frameDurMs) && frameDurMs > 1.0f && frameDurMs < 200.0f)
                                   ? static_cast<double>(frameDurMs)
                                   : kDivxDefaultFrameDurationMs;

        uint32_t nowMs = GetTickCount();
        if (*pFirstFrame != 0) {
            *pFirstFrame    = 0;
            *pLastTimeMs    = nowMs;
            *pCurrentTimeMs = 0.0;
            *pTargetTimeMs  = 0.0;
        } else {
            uint32_t lastMs    = *pLastTimeMs;
            double   currentMs = *pCurrentTimeMs + static_cast<double>(nowMs - lastMs);
            double   targetMs  = *pTargetTimeMs + frameStepMs;
            lastMs = nowMs;

            while (currentMs < targetMs) {
                UpdateXboxKernelTick();
                UpdateXboxInputTick();
                UpdateXboxAudioTick();
                if (targetMs - currentMs > kDivxSleepThresholdMs) {
                    Sleep(kDivxWaitSleepMs);
                } else {
                    SwitchToThread();
                }
                nowMs = GetTickCount();
                currentMs += static_cast<double>(nowMs - lastMs);
                lastMs = nowMs;
            }

            if (currentMs > targetMs + kDivxMaxLagCatchupMs) {
                currentMs = targetMs;
            }
            *pLastTimeMs    = lastMs;
            *pCurrentTimeMs = currentMs;
            *pTargetTimeMs  = targetMs;
        }

        reinterpret_cast<PFN_PlayerNoArg>(kAddrVidSimplePlayerHandleAudio)(pPlayer, 0);
    }

    reinterpret_cast<PFN_PlayerNoArg>(kAddrVidSimplePlayerStartPlayback)(pPlayer, 0);
    return 0;
}

// ============================================================================
// YUY2 Hardware Overlay Emulation for .vid Movies
// Faithfully reproduces _D3DDevice_UpdateOverlay@20 (0x006B30D0),
// _D3DDevice_EnableOverlay@4 (0x006B3250), and _D3DDevice_GetOverlayUpdateStatus@0 (0x006B30B0).
// ============================================================================

static std::vector<uint32_t> g_overlayFrameBuf;
static bool                  g_overlayEnabled = false;
static bool                  g_overlayHasData = false;

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
        g_d3dCtx->UpdateSubresource(g_backBufferTex, 0, nullptr, g_overlayFrameBuf.data(), 640u * 4u, 0);
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
    // 1. cDivxVideo (.vid Movie Player) wall-clock frame pacing bridges
    InstallBridge(kAddrDivxVideoFrameMove, &Bridge_cDivxVideo_FrameMove);    // ?FrameMove@cDivxVideo@@QAEJXZ
    InstallBridge(kAddrDivxVideoRender,    &Bridge_cDivxVideo_Render);       // ?Render@cDivxVideo@@QAEJXZ

    // 2. D3DDevice YUY2 Hardware Overlay bridges
    InstallBridge(0x006B3250, &Bridge_D3DDevice_EnableOverlay);              // _D3DDevice_EnableOverlay@4
    InstallBridge(0x006B30B0, &Bridge_D3DDevice_GetOverlayUpdateStatus);     // _D3DDevice_GetOverlayUpdateStatus@0
    InstallBridge(0x006B30D0, &Bridge_D3DDevice_UpdateOverlay);              // _D3DDevice_UpdateOverlay@20
}
