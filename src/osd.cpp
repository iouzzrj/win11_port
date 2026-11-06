#include "ghost_runtime.h"
#include "xbox_d3d8_types.h"
#include <d3d11.h>
#include <vector>

// ============================================================================
// In-Game On-Screen Display (OSD) & Host Extension Overlay
// ============================================================================

volatile bool  g_osdShowFps    = false;
volatile float g_osdCurrentFps = 0.0f;

enum : UINT_PTR {
    IDM_SYS_VIEW_FPS = 0x1020,
    IDT_OSD_FPS_TICK = 1,
};

static ID3D11Texture2D*          g_osdTex           = nullptr;
static ID3D11ShaderResourceView* g_osdSRV           = nullptr;
static ID3D11Buffer*             g_osdVB            = nullptr;
static uint32_t                  g_osdTexW          = 0;
static uint32_t                  g_osdTexH          = 0;
static wchar_t                   g_lastOsdText[256] = { 0 };
static HFONT                     g_hOsdFont         = nullptr;

void InitOsdWindowHooks(HWND hWnd) {
    if (!hWnd) return;
    HMENU hSysMenu = GetSystemMenu(hWnd, FALSE);
    if (hSysMenu) {
        InsertMenuW(
            hSysMenu, SC_CLOSE,
            MF_BYCOMMAND | MF_STRING | (g_osdShowFps ? MF_CHECKED : MF_UNCHECKED),
            IDM_SYS_VIEW_FPS, L"&FPS");
        InsertMenuW(hSysMenu, SC_CLOSE, MF_BYCOMMAND | MF_SEPARATOR, 0, nullptr);
    }
    SetTimer(hWnd, IDT_OSD_FPS_TICK, 500, nullptr);
}

bool HandleOsdSysCommand(HWND hWnd, WPARAM wParam) {
    if ((wParam & 0xFFF0) == IDM_SYS_VIEW_FPS) {
        g_osdShowFps = !g_osdShowFps;
        HMENU hSysMenu = GetSystemMenu(hWnd, FALSE);
        if (hSysMenu) {
            CheckMenuItem(
                hSysMenu, IDM_SYS_VIEW_FPS,
                MF_BYCOMMAND | (g_osdShowFps ? MF_CHECKED : MF_UNCHECKED));
        }
        return true;
    }
    return false;
}

bool HandleOsdTimer(HWND hWnd, WPARAM wParam) {
    (void)hWnd;
    if (wParam == IDT_OSD_FPS_TICK) {
        static DWORD    s_lastTick  = GetTickCount();
        static uint32_t s_lastFrame = 0;
        DWORD    nowTick  = GetTickCount();
        uint32_t nowFrame = g_swapFrameCount;
        DWORD    dt       = nowTick - s_lastTick;
        if (dt >= 250) {
            g_osdCurrentFps = static_cast<float>(nowFrame - s_lastFrame) * 1000.0f / static_cast<float>(dt);
            s_lastTick      = nowTick;
            s_lastFrame     = nowFrame;
        }
        return true;
    }
    return false;
}

static void UpdateOsdTexture(const wchar_t* text) {
    if (!g_d3dDevice || !g_d3dCtx || !text || !text[0]) return;
    if (!g_hOsdFont) {
        g_hOsdFont = CreateFontW(
            -12, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
            OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY,
            DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
    }

    HDC hdcScreen = GetDC(nullptr);
    HDC hdc = CreateCompatibleDC(hdcScreen);
    ReleaseDC(nullptr, hdcScreen);
    if (!hdc) return;

    HGDIOBJ oldFont = SelectObject(hdc, g_hOsdFont);
    SIZE sz = {};
    GetTextExtentPoint32W(hdc, text, static_cast<int>(wcslen(text)), &sz);

    uint32_t w = static_cast<uint32_t>(sz.cx + 14);
    uint32_t h = static_cast<uint32_t>(sz.cy + 6);
    if (w < 16)  w = 16;
    if (w > 620) w = 620;
    if (h < 14)  h = 14;
    if (h > 64)  h = 64;

    BITMAPINFO bmi = {};
    bmi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth       = static_cast<LONG>(w);
    bmi.bmiHeader.biHeight      = -static_cast<LONG>(h); // top-down DIB
    bmi.bmiHeader.biPlanes      = 1;
    bmi.bmiHeader.biBitCount    = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    void* dibBits = nullptr;
    HBITMAP hbm = CreateDIBSection(hdc, &bmi, DIB_RGB_COLORS, &dibBits, nullptr, 0);
    if (hbm && dibBits) {
        HGDIOBJ oldBm = SelectObject(hdc, hbm);
        SetBkMode(hdc, TRANSPARENT);
        SetTextColor(hdc, RGB(255, 255, 255));

        uint32_t numPx = w * h;
        std::vector<uint8_t> shadowMask(numPx, 0);
        std::vector<uint8_t> glyphMask(numPx, 0);

        // 1. Render 1px drop-shadow mask at (+8, +4)
        memset(dibBits, 0, numPx * 4);
        RECT rcShadow = { 8, 4, static_cast<LONG>(w), static_cast<LONG>(h) };
        DrawTextW(hdc, text, -1, &rcShadow, DT_LEFT | DT_TOP | DT_SINGLELINE | DT_NOPREFIX);
        GdiFlush();
        const uint32_t* px = reinterpret_cast<const uint32_t*>(dibBits);
        for (uint32_t i = 0; i < numPx; ++i) {
            uint8_t r = static_cast<uint8_t>((px[i] >> 16) & 0xFF);
            uint8_t g = static_cast<uint8_t>((px[i] >> 8)  & 0xFF);
            uint8_t b = static_cast<uint8_t>(px[i] & 0xFF);
            uint8_t m = r > g ? r : g;
            shadowMask[i] = m > b ? m : b;
        }

        // 2. Render crisp foreground glyph mask at (+7, +3)
        memset(dibBits, 0, numPx * 4);
        RECT rcGlyph = { 7, 3, static_cast<LONG>(w) - 1, static_cast<LONG>(h) - 1 };
        DrawTextW(hdc, text, -1, &rcGlyph, DT_LEFT | DT_TOP | DT_SINGLELINE | DT_NOPREFIX);
        GdiFlush();
        for (uint32_t i = 0; i < numPx; ++i) {
            uint8_t r = static_cast<uint8_t>((px[i] >> 16) & 0xFF);
            uint8_t g = static_cast<uint8_t>((px[i] >> 8)  & 0xFF);
            uint8_t b = static_cast<uint8_t>(px[i] & 0xFF);
            uint8_t m = r > g ? r : g;
            glyphMask[i] = m > b ? m : b;
        }

        // 3. Composite semi-transparent rounded dark pill + shadow + bright glyph into BGRA8
        std::vector<uint32_t> outBgra(numPx, 0);
        for (uint32_t y = 0; y < h; ++y) {
            for (uint32_t x = 0; x < w; ++x) {
                // 3px corner bevel clip for rounded badge
                uint32_t dx = (x < 3) ? (2 - x) : ((x >= w - 3) ? (x - (w - 3)) : 0);
                uint32_t dy = (y < 3) ? (2 - y) : ((y >= h - 3) ? (y - (h - 3)) : 0);
                bool insidePill = (dx + dy <= 2);

                uint32_t idx = y * w + x;
                float bgA = insidePill ? (135.0f / 255.0f) : 0.0f;
                float shA = shadowMask[idx] * (1.0f / 255.0f);
                float fgA = glyphMask[idx]  * (1.0f / 255.0f);

                // Base dark translucent pill color: RGB(12, 16, 22)
                float r = 12.0f * (1.0f - shA);
                float g = 16.0f * (1.0f - shA);
                float b = 22.0f * (1.0f - shA);
                float a = bgA + shA * (1.0f - bgA);

                // Blend foreground glyph: RGB(240, 246, 255)
                r = r * (1.0f - fgA) + 240.0f * fgA;
                g = g * (1.0f - fgA) + 246.0f * fgA;
                b = b * (1.0f - fgA) + 255.0f * fgA;
                a = a + fgA * (1.0f - a);

                uint32_t outR = static_cast<uint32_t>(r + 0.5f); if (outR > 255) outR = 255;
                uint32_t outG = static_cast<uint32_t>(g + 0.5f); if (outG > 255) outG = 255;
                uint32_t outB = static_cast<uint32_t>(b + 0.5f); if (outB > 255) outB = 255;
                uint32_t outA = static_cast<uint32_t>(a * 255.0f + 0.5f); if (outA > 255) outA = 255;

                outBgra[idx] = (outA << 24) | (outR << 16) | (outG << 8) | outB;
            }
        }

        if (!g_osdTex || g_osdTexW != w || g_osdTexH != h) {
            if (g_osdSRV) { g_osdSRV->Release(); g_osdSRV = nullptr; }
            if (g_osdTex) { g_osdTex->Release(); g_osdTex = nullptr; }
            g_osdTexW = w;
            g_osdTexH = h;

            D3D11_TEXTURE2D_DESC td = {};
            td.Width            = w;
            td.Height           = h;
            td.MipLevels        = 1;
            td.ArraySize        = 1;
            td.Format           = DXGI_FORMAT_B8G8R8A8_UNORM;
            td.SampleDesc.Count = 1;
            td.Usage            = D3D11_USAGE_DEFAULT;
            td.BindFlags        = D3D11_BIND_SHADER_RESOURCE;

            D3D11_SUBRESOURCE_DATA initData = {};
            initData.pSysMem     = outBgra.data();
            initData.SysMemPitch = w * 4;
            if (SUCCEEDED(g_d3dDevice->CreateTexture2D(&td, &initData, &g_osdTex))) {
                g_d3dDevice->CreateShaderResourceView(g_osdTex, nullptr, &g_osdSRV);
            }
        } else {
            g_d3dCtx->UpdateSubresource(g_osdTex, 0, nullptr, outBgra.data(), w * 4, 0);
        }

        SelectObject(hdc, oldBm);
        DeleteObject(hbm);
    }

    SelectObject(hdc, oldFont);
    DeleteDC(hdc);
}

void DrawOsdOverlayOnBackbuffer() {
    ID3D11PixelShader* osdPS = GetPixelShaderPermutation(0u);
    if (!g_d3dDevice || !g_d3dCtx || !g_rtv || !g_vs || !osdPS || !g_cbPerDraw) return;
    if (!g_osdShowFps) return;

    wchar_t curText[64];
    swprintf(curText, 64, L"%.1f FPS", g_osdCurrentFps);

    if (wcscmp(curText, g_lastOsdText) != 0 || !g_osdSRV) {
        wcscpy(g_lastOsdText, curText);
        UpdateOsdTexture(curText);
    }
    if (!g_osdSRV || g_osdTexW == 0 || g_osdTexH == 0) return;

    if (!g_osdVB) {
        D3D11_BUFFER_DESC bd = {};
        bd.ByteWidth      = 4 * sizeof(UnifiedVertex);
        bd.Usage          = D3D11_USAGE_DYNAMIC;
        bd.BindFlags      = D3D11_BIND_VERTEX_BUFFER;
        bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        if (FAILED(g_d3dDevice->CreateBuffer(&bd, nullptr, &g_osdVB))) return;
    }

    float x0 = static_cast<float>(640 - static_cast<int>(g_osdTexW) - 8);
    float y0 = 8.0f;
    float x1 = x0 + static_cast<float>(g_osdTexW);
    float y1 = y0 + static_cast<float>(g_osdTexH);

    UnifiedVertex quad[4] = {};
    quad[0] = { x0, y0, 0.0f, 1.0f, 0,0,1, 1,1,1,1, 0.0f, 0.0f, 0,0, 0,0, 0,0 };
    quad[1] = { x1, y0, 0.0f, 1.0f, 0,0,1, 1,1,1,1, 1.0f, 0.0f, 0,0, 0,0, 0,0 };
    quad[2] = { x0, y1, 0.0f, 1.0f, 0,0,1, 1,1,1,1, 0.0f, 1.0f, 0,0, 0,0, 0,0 };
    quad[3] = { x1, y1, 0.0f, 1.0f, 0,0,1, 1,1,1,1, 1.0f, 1.0f, 0,0, 0,0, 0,0 };

    D3D11_MAPPED_SUBRESOURCE mappedVB = {};
    if (SUCCEEDED(g_d3dCtx->Map(g_osdVB, 0, D3D11_MAP_WRITE_DISCARD, 0, &mappedVB))) {
        memcpy(mappedVB.pData, quad, sizeof(quad));
        g_d3dCtx->Unmap(g_osdVB, 0);
    }

    PerDrawCBData cb = {};
    cb.viewport[0]    = 0.0f;
    cb.viewport[1]    = 0.0f;
    cb.viewport[2]    = 640.0f;
    cb.viewport[3]    = 480.0f;
    cb.stage0Ops[0]   = 2u; // D3DTOP_SELECTARG1
    cb.stage0Ops[1]   = 2u; // D3DTA_TEXTURE
    cb.stage0Alpha[0] = 2u; // D3DTOP_SELECTARG1
    cb.stage0Alpha[1] = 2u; // D3DTA_TEXTURE
    cb.flags[0]       = 1u; // isRhw = 1
    cb.flags[1]       = 1u; // texMask = Stage 0
    cb.alphaTest[2]   = 1.0f; // gamma = 1.0
    cb.alphaTest[3]   = 1.0f; // brightness = 1.0
    cb.uvScale01[0]   = 1.0f;
    cb.uvScale01[1]   = 1.0f;

    D3D11_MAPPED_SUBRESOURCE mappedCB = {};
    if (SUCCEEDED(g_d3dCtx->Map(g_cbPerDraw, 0, D3D11_MAP_WRITE_DISCARD, 0, &mappedCB))) {
        memcpy(mappedCB.pData, &cb, sizeof(cb));
        g_d3dCtx->Unmap(g_cbPerDraw, 0);
    }

    g_d3dCtx->OMSetRenderTargets(1, &g_rtv, nullptr);
    D3D11_VIEWPORT vp11 = { 0.0f, 0.0f, 640.0f, 480.0f, 0.0f, 1.0f };
    g_d3dCtx->RSSetViewports(1, &vp11);

    ID3D11BlendState*        bs  = GetBlendState(1u, 5u /* SRCALPHA */, 6u /* INVSRCALPHA */, 1u /* ADD */, 0x0Fu);
    ID3D11DepthStencilState* ds  = GetDepthState(0u, 0u, 8u, 0u, 1u, 1u, 1u, 8u, 0xFFu, 0xFFu);
    ID3D11RasterizerState*   rs  = GetRastState(0u, 3u, 0u, false, 0x0900u);
    ID3D11SamplerState*      smp = GetSamplerState(kXboxTexAddressClamp, kXboxTexAddressClamp, kXboxTexFilterPoint, 0u);

    float blendFactor[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
    g_d3dCtx->OMSetBlendState(bs, blendFactor, 0xFFFFFFFFu);
    g_d3dCtx->OMSetDepthStencilState(ds, 0);
    g_d3dCtx->RSSetState(rs);

    g_d3dCtx->IASetInputLayout(g_inputLayout);
    UINT stride = sizeof(UnifiedVertex);
    UINT offset = 0;
    g_d3dCtx->IASetVertexBuffers(0, 1, &g_osdVB, &stride, &offset);
    g_d3dCtx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);

    g_d3dCtx->VSSetShader(g_vs, nullptr, 0);
    g_d3dCtx->VSSetConstantBuffers(0, 1, &g_cbPerDraw);
    g_d3dCtx->PSSetShader(osdPS, nullptr, 0);
    g_d3dCtx->PSSetConstantBuffers(0, 1, &g_cbPerDraw);

    ID3D11ShaderResourceView* srvs[4] = { g_osdSRV, nullptr, nullptr, nullptr };
    ID3D11SamplerState*       smps[4] = { smp, smp, smp, smp };
    g_d3dCtx->PSSetShaderResources(0, 4, srvs);
    g_d3dCtx->PSSetSamplers(0, 4, smps);

    g_d3dCtx->Draw(4, 0);

    ID3D11ShaderResourceView* nullSrv = nullptr;
    g_d3dCtx->PSSetShaderResources(0, 1, &nullSrv);
}
