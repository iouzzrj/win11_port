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
// ============================================================================

static ID3D11Texture2D*          g_overlayTex     = nullptr;
static ID3D11ShaderResourceView* g_overlaySRV     = nullptr;
static uint32_t                  g_overlayW       = 0;
static uint32_t                  g_overlayH       = 0;
static bool                      g_overlayEnabled = false;
static std::vector<uint32_t>     g_overlayPixels;

static void __stdcall Bridge_D3DDevice_EnableOverlay(BOOL bEnable) {
    g_overlayEnabled = (bEnable != FALSE);
}

static BOOL __stdcall Bridge_D3DDevice_GetOverlayUpdateStatus() {
    return TRUE;
}

static void __stdcall Bridge_D3DDevice_UpdateOverlay(
    XboxPixelContainer* pSurface, const RECT* pSrcRect, const RECT* pDstRect,
    BOOL bColorKey, DWORD ColorKey)
{
    (void)pDstRect; (void)bColorKey; (void)ColorKey;
    if (!pSurface || !pSurface->Data) return;

    uint32_t w = 0, h = 0, levels = 1, xboxFmt = 0, pitch = 0;
    bool isSwizzled = false;
    DecodeXboxTextureFormat(pSurface, &w, &h, &levels, &xboxFmt, &isSwizzled, &pitch);
    if (pSrcRect && pSrcRect->right > pSrcRect->left && pSrcRect->bottom > pSrcRect->top) {
        w = (uint32_t)(pSrcRect->right - pSrcRect->left);
        h = (uint32_t)(pSrcRect->bottom - pSrcRect->top);
    }
    if (w == 0 || h == 0 || w > 2048 || h > 2048) return;
    if (pitch == 0) pitch = w * 2;

    g_overlayPixels.resize(w * h);
    const uint8_t* srcBase = reinterpret_cast<const uint8_t*>(pSurface->Data);
    for (uint32_t y = 0; y < h; ++y) {
        const uint8_t* sRow = srcBase + y * pitch;
        uint32_t* dRow = &g_overlayPixels[y * w];
        for (uint32_t x = 0; x < w; x += 2) {
            int y0, u, y1, v;
            if (xboxFmt == 0x25) {
                u  = (int)sRow[x * 2 + 0] - 128;
                y0 = (int)sRow[x * 2 + 1];
                v  = (int)sRow[x * 2 + 2] - 128;
                y1 = (int)sRow[x * 2 + 3];
            } else {
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

            if (x + 1 < w) {
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

    if (g_d3dDevice && g_d3dCtx && g_swapChain) {
        if (!g_overlayTex || g_overlayW != w || g_overlayH != h) {
            if (g_overlaySRV) { g_overlaySRV->Release(); g_overlaySRV = nullptr; }
            if (g_overlayTex) { g_overlayTex->Release(); g_overlayTex = nullptr; }
            g_overlayW = w;
            g_overlayH = h;
            D3D11_TEXTURE2D_DESC td = {};
            td.Width = w;
            td.Height = h;
            td.MipLevels = 1;
            td.ArraySize = 1;
            td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
            td.SampleDesc.Count = 1;
            td.Usage = D3D11_USAGE_DEFAULT;
            td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            if (SUCCEEDED(g_d3dDevice->CreateTexture2D(&td, nullptr, &g_overlayTex))) {
                g_d3dDevice->CreateShaderResourceView(g_overlayTex, nullptr, &g_overlaySRV);
            }
        }
        if (g_overlayTex && g_backBufferTex && w == 640 && h == 480) {
            g_swapFrameCount++;
            g_d3dCtx->UpdateSubresource(g_backBufferTex, 0, nullptr, g_overlayPixels.data(), w * 4, 0);
            DrawOsdOverlayOnBackbuffer();
            g_swapChain->Present(0, 0);
            if (g_rtv && g_dsv) {
                g_d3dCtx->OMSetRenderTargets(1, &g_rtv, g_dsv);
            }
        }
    }
    UpdateXboxKernelTick();
    UpdateXboxInputTick();
    UpdateXboxAudioTick();
}

void InitXboxVideo() {
    // 1. cDivxVideo (.vid Movie Player) wall-clock frame pacing bridges
    InstallBridge(kAddrDivxVideoFrameMove, &Bridge_cDivxVideo_FrameMove,          "?FrameMove@cDivxVideo@@QAEJXZ");
    InstallBridge(kAddrDivxVideoRender,    &Bridge_cDivxVideo_Render,             "?Render@cDivxVideo@@QAEJXZ");

    // 2. D3DDevice YUY2 Hardware Overlay bridges
    InstallBridge(0x006B3250, &Bridge_D3DDevice_EnableOverlay,                    "_D3DDevice_EnableOverlay@4");
    InstallBridge(0x006B30B0, &Bridge_D3DDevice_GetOverlayUpdateStatus,           "_D3DDevice_GetOverlayUpdateStatus@0");
    InstallBridge(0x006B30D0, &Bridge_D3DDevice_UpdateOverlay,                    "_D3DDevice_UpdateOverlay@20");
}
