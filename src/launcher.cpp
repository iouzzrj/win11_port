#include "ghost_runtime.h"
#include <dxgi.h>
#include <shobjidl.h>
#include <vector>

// ============================================================================
// Native Win32 Pre-Launch Setup & Keyboard Configuration UI
// ============================================================================

struct GpuAdapterInfo {
    UINT    index;
    wchar_t description[128];
    SIZE_T  dedicatedVramMB;
    SIZE_T  sharedVramMB;
    UINT    vendorId;
    UINT    deviceId;
    bool    isSoftware;
};

enum LauncherCtrlIds {
    IDC_COMBO_GPU           = 101,
    IDC_STATIC_INFO         = 104,
    IDC_EDIT_GAMEDIR        = 105,
    IDC_BTN_BROWSE          = 106,
    IDC_BTN_KEYCONFIG       = 107,
    IDC_CHK_COMPILE_SHADERS = 108,
};

static std::vector<GpuAdapterInfo> s_gpuAdapters;
static HWND    s_hComboGpu    = nullptr;
static HWND    s_hStaticInfo  = nullptr;
static HWND    s_hEditGameDir = nullptr;
static HBRUSH  s_hDlgBrush    = nullptr;
static bool    s_launcherOk   = false;
static bool    s_launcherDone = false;
static wchar_t s_selectedGameDirW[MAX_PATH] = { 0 };

static bool TryPortableFallbackToExeDir() {
    wchar_t exeDirW[MAX_PATH] = {};
    GetExeDirW(exeDirW, MAX_PATH);
    if (IsValidGameDirW(exeDirW)) {
        wcsncpy(s_selectedGameDirW, exeDirW, MAX_PATH - 1);
        s_selectedGameDirW[MAX_PATH - 1] = L'\0';
        if (s_hEditGameDir) {
            SetWindowTextW(s_hEditGameDir, s_selectedGameDirW);
        }
        return true;
    }
    s_selectedGameDirW[0] = L'\0';
    return false;
}

static int ResolveGpuComboSelectionFromIniW(const wchar_t* iniPathW) {
    if (s_gpuAdapters.empty()) return 0;

    int bestSel = 0;

    if (iniPathW && iniPathW[0] && GetFileAttributesW(iniPathW) != INVALID_FILE_ATTRIBUTES) {
        int savedAdapterIdx = (int)GetPrivateProfileIntW(L"Graphics", L"AdapterIndex", -1, iniPathW);
        wchar_t savedAdapterName[128] = {};
        GetPrivateProfileStringW(L"Graphics", L"AdapterName", L"", savedAdapterName, 128, iniPathW);

        bool matchedByName = false;
        if (savedAdapterName[0] != L'\0') {
            for (size_t i = 0; i < s_gpuAdapters.size(); ++i) {
                if (_wcsicmp(s_gpuAdapters[i].description, savedAdapterName) == 0) {
                    bestSel = (int)i;
                    matchedByName = true;
                    break;
                }
            }
        }
        if (!matchedByName && savedAdapterIdx >= 0 && savedAdapterIdx < (int)s_gpuAdapters.size()) {
            bestSel = savedAdapterIdx;
        }
    }
    return bestSel;
}

// ============================================================================
// Interactive Keyboard Controls Configuration Dialog
// ============================================================================

struct KeyCalloutBoxMeta {
    const wchar_t* title;
    RECT           rcBox;
    COLORREF       borderColor;
    POINT          lineStart;
    POINT          lineEnd;
};

static int  s_activeBindSlot = -1;
static int  s_hoverBindSlot  = -1;
static int  s_hoverBottomBtn = -1; // 0=Reset, 1=Save, 2=Cancel
static bool s_keyDlgSaved    = false;
static bool s_keyDlgDone     = false;

static const KeyCalloutBoxMeta s_calloutBoxes[15] = {
    { L"Crouch",                 { 255,  78, 425, 146 }, RGB(235,  35,  35), { 315, 146 }, { 365, 185 } },
    { L"Fire",                   { 445,  16, 615,  84 }, RGB(235,  35,  35), { 515,  84 }, { 528, 185 } },
    { L"Move",                   {  24, 122, 240, 222 }, RGB(235,  35,  35), { 240, 172 }, { 358, 250 } },
    { L"Sight",                  {  24, 236, 240, 304 }, RGB( 40,  90, 255), { 240, 268 }, { 402, 297 } },
    { L"Menu",                   {  24, 316, 240, 390 }, RGB(235,  35,  35), { 240, 348 }, { 345, 315 } },
    { L"Cloak",                  {  24, 406, 240, 474 }, RGB( 40,  90, 255), { 240, 440 }, { 384, 315 } },
    { L"Gun Light / Psi Charge", { 130, 488, 345, 556 }, RGB( 40,  90, 255), { 245, 488 }, { 402, 333 } },
    { L"- -",                    { 360, 394, 535, 460 }, RGB( 40,  90, 255), { 410, 394 }, { 418, 315 } },
    { L"Aim / Sniper (click)",   { 360, 472, 630, 558 }, RGB( 40, 175,  60), { 488, 472 }, { 488, 322 } },
    { L"Melee Attack",           { 648,  66, 876, 134 }, RGB( 40, 195, 215), { 648,  98 }, { 518, 265 } },
    { L"Weapon Mode",            { 648, 144, 876, 212 }, RGB(215, 195,  35), { 648, 176 }, { 542, 244 } },
    { L"Action",                 { 648, 222, 876, 290 }, RGB(215, 115,  30), { 648, 254 }, { 564, 268 } },
    { L"Jump",                   { 648, 300, 876, 368 }, RGB( 45, 215,  60), { 648, 332 }, { 538, 290 } },
    { L"Next Firearm",           { 648, 378, 876, 446 }, RGB(235,  35,  35), { 648, 412 }, { 564, 310 } },
    { L"Next Grenade",           { 648, 456, 876, 524 }, RGB(235,  35,  35), { 648, 490 }, { 542, 328 } },
};

static const RECT s_rcBottomBtns[3] = {
    { 500, 568, 626, 598 }, // 0: Reset Defaults
    { 638, 568, 764, 598 }, // 1: Save & Close
    { 776, 568, 876, 598 }, // 2: Cancel
};

static void PaintKeyboardConfigDiagram(HDC hdc, int clientW, int clientH) {
    // 1. Pure black background matching the in-game controller layout screen
    RECT rcFull = { 0, 0, clientW, clientH };
    HBRUSH hBgBrush = CreateSolidBrush(RGB(8, 10, 14));
    FillRect(hdc, &rcFull, hBgBrush);
    DeleteObject(hBgBrush);

    SetBkMode(hdc, TRANSPARENT);

    HFONT hFontBoxTitle = CreateFontW(-15, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
        DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
    HFONT hFontKey = CreateFontW(-13, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
        DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
    HFONT hFontSub = CreateFontW(-12, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
        DEFAULT_PITCH | FF_SWISS, L"Segoe UI");

    // 2. Draw Xbox Controller schematic in the center (x=305..585, y=180..395)
    HBRUSH hPadBody  = CreateSolidBrush(RGB(34, 38, 45));
    HBRUSH hPadDark  = CreateSolidBrush(RGB(22, 25, 30));
    HPEN   hPadPen   = CreatePen(PS_SOLID, 2, RGB(82, 88, 100));
    HGDIOBJ oldBrush = SelectObject(hdc, hPadBody);
    HGDIOBJ oldPen   = SelectObject(hdc, hPadPen);

    // Triggers (L-Trigger & R-Trigger at top of controller)
    RoundRect(hdc, 345, 176, 392, 206, 10, 10);
    RoundRect(hdc, 502, 176, 549, 206, 10, 10);

    // Left & Right ergonomic grips + central body
    Ellipse(hdc, 304, 210, 392, 392);
    Ellipse(hdc, 502, 210, 590, 392);
    RoundRect(hdc, 326, 194, 568, 344, 54, 54);

    // Center Xbox Jewel
    HBRUSH hJewelBrush = CreateSolidBrush(RGB(22, 115, 32));
    HPEN   hJewelPen   = CreatePen(PS_SOLID, 2, RGB(70, 200, 75));
    SelectObject(hdc, hJewelBrush);
    SelectObject(hdc, hJewelPen);
    Ellipse(hdc, 424, 224, 470, 270);
    SetTextColor(hdc, RGB(150, 255, 150));
    SelectObject(hdc, hFontSub);
    RECT rcJewel = { 420, 238, 474, 260 };
    DrawTextW(hdc, L"XBOX", -1, &rcJewel, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    DeleteObject(hJewelBrush);
    DeleteObject(hJewelPen);

    // Left Stick (358, 250) & Right Stick (488, 320)
    SelectObject(hdc, hPadDark);
    SelectObject(hdc, hPadPen);
    Ellipse(hdc, 336, 228, 380, 272);
    Ellipse(hdc, 344, 236, 372, 264);
    Ellipse(hdc, 466, 298, 510, 342);
    Ellipse(hdc, 474, 306, 502, 334);

    // D-Pad (402, 315)
    Ellipse(hdc, 378, 291, 426, 339);
    RoundRect(hdc, 395, 294, 409, 336, 3, 3);
    RoundRect(hdc, 381, 308, 423, 322, 3, 3);

    // Back & Start small buttons (lower-left)
    Ellipse(hdc, 328, 286, 344, 300);
    Ellipse(hdc, 338, 308, 354, 322);

    // Colored Face Buttons: X (Blue), Y (Yellow), B (Red), A (Green), Black & White
    auto drawPadBtn = [&](int cx, int cy, int r, COLORREF fillCol, const wchar_t* label) {
        HBRUSH hb = CreateSolidBrush(fillCol);
        HPEN   hp = CreatePen(PS_SOLID, 1, RGB(210, 220, 230));
        SelectObject(hdc, hb);
        SelectObject(hdc, hp);
        Ellipse(hdc, cx - r, cy - r, cx + r, cy + r);
        SetTextColor(hdc, RGB(255, 255, 255));
        SelectObject(hdc, hFontSub);
        RECT rcB = { cx - r, cy - r, cx + r, cy + r };
        DrawTextW(hdc, label, -1, &rcB, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        DeleteObject(hb);
        DeleteObject(hp);
    };
    drawPadBtn(518, 265, 10, RGB( 30, 125, 220), L"X");
    drawPadBtn(542, 244, 10, RGB(210, 165,  15), L"Y");
    drawPadBtn(564, 268, 10, RGB(210,  45,  35), L"B");
    drawPadBtn(538, 290, 10, RGB( 45, 170,  45), L"A");
    drawPadBtn(564, 310,  7, RGB( 25,  28,  34), L"");
    drawPadBtn(542, 328,  7, RGB(185, 190, 200), L"");

    // 3. Draw the 15 Callout Lines & Callout Boxes (matching screenshot layout)
    HBRUSH hBoxFill = CreateSolidBrush(RGB(12, 14, 20));
    for (size_t i = 0; i < sizeof(s_calloutBoxes) / sizeof(s_calloutBoxes[0]); ++i) {
        const KeyCalloutBoxMeta& b = s_calloutBoxes[i];
        HPEN hLinePen = CreatePen(PS_SOLID, 2, b.borderColor);
        SelectObject(hdc, hLinePen);
        MoveToEx(hdc, b.lineStart.x, b.lineStart.y, nullptr);
        LineTo(hdc, b.lineEnd.x, b.lineEnd.y);

        SelectObject(hdc, hBoxFill);
        Rectangle(hdc, b.rcBox.left, b.rcBox.top, b.rcBox.right, b.rcBox.bottom);

        SelectObject(hdc, hFontBoxTitle);
        SetTextColor(hdc, RGB(245, 247, 252));
        RECT rcTitle = { b.rcBox.left + 8, b.rcBox.top + 4, b.rcBox.right - 6, b.rcBox.top + 26 };
        DrawTextW(hdc, b.title, -1, &rcTitle, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        DeleteObject(hLinePen);
    }
    DeleteObject(hBoxFill);

    // 4. Draw all 23 Interactive Key Slots inside the Callout Boxes
    for (int i = 0; i < (int)kNumKeyBindSlots; ++i) {
        const KeyBindSlotMeta& s = g_keySlots[i];
        bool isActive = (s_activeBindSlot == i);
        bool isHover  = (s_hoverBindSlot == i);
        bool isArrow  = (s.shortLabel && s.shortLabel[0] >= 0x2190 && s.shortLabel[0] <= 0x2193);

        // Draw optional sub-label ("Start", "Back" to the left, or "Click" above)
        if (s.shortLabel && s.shortLabel[0] && !isArrow) {
            SelectObject(hdc, hFontSub);
            SetTextColor(hdc, RGB(175, 185, 205));
            if (_wcsicmp(s.shortLabel, L"Click") == 0) {
                RECT rcSub = { s.rcSlot.left, s.rcSlot.top - 18, s.rcSlot.right, s.rcSlot.top - 2 };
                DrawTextW(hdc, L"Click", -1, &rcSub, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            } else {
                RECT rcSub = { s.rcSlot.left - 38, s.rcSlot.top, s.rcSlot.left - 4, s.rcSlot.bottom };
                DrawTextW(hdc, s.shortLabel, -1, &rcSub, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
            }
        }

        COLORREF fillCol   = isActive ? RGB(58, 46, 12) : (isHover ? RGB(34, 42, 58) : RGB(22, 26, 36));
        COLORREF borderCol = isActive ? RGB(255, 210, 55) : (isHover ? RGB(110, 175, 255) : RGB(75, 86, 110));
        COLORREF textCol   = isActive ? RGB(255, 225, 85) : RGB(235, 242, 255);

        HBRUSH hSlotFill = CreateSolidBrush(fillCol);
        HPEN   hSlotPen  = CreatePen(PS_SOLID, isActive ? 2 : 1, borderCol);
        SelectObject(hdc, hSlotFill);
        SelectObject(hdc, hSlotPen);
        RoundRect(hdc, s.rcSlot.left, s.rcSlot.top, s.rcSlot.right, s.rcSlot.bottom, 6, 6);

        wchar_t keyName[64] = {};
        if (isActive) {
            wcscpy(keyName, (s.rcSlot.right - s.rcSlot.left < 80) ? L"..." : L"Press Key...");
        } else {
            FormatVkNameW(*s.pVk, keyName, 64);
        }

        if (isArrow && !isActive) {
            SelectObject(hdc, hFontSub);
            SetTextColor(hdc, RGB(130, 145, 170));
            RECT rcArr = { s.rcSlot.left + 5, s.rcSlot.top, s.rcSlot.left + 18, s.rcSlot.bottom };
            DrawTextW(hdc, s.shortLabel, -1, &rcArr, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

            SelectObject(hdc, hFontKey);
            SetTextColor(hdc, textCol);
            RECT rcText = { s.rcSlot.left + 14, s.rcSlot.top, s.rcSlot.right, s.rcSlot.bottom };
            DrawTextW(hdc, keyName, -1, &rcText, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        } else {
            SelectObject(hdc, hFontKey);
            SetTextColor(hdc, textCol);
            RECT rcText = s.rcSlot;
            DrawTextW(hdc, keyName, -1, &rcText, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        }

        DeleteObject(hSlotFill);
        DeleteObject(hSlotPen);
    }

    // 5. Draw Bottom Status Bar & Action Buttons (Reset Defaults, Save & Close, Cancel)
    SelectObject(hdc, hFontSub);
    RECT rcHint = { 24, 568, 490, 598 };
    if (s_activeBindSlot >= 0 && s_activeBindSlot < (int)kNumKeyBindSlots) {
        SetTextColor(hdc, RGB(255, 215, 70));
        wchar_t promptBuf[256];
        swprintf(promptBuf, 256, L"Rebinding [%ls]: Press any keyboard key (or click mouse button)...",
            g_keySlots[s_activeBindSlot].actionDesc);
        DrawTextW(hdc, promptBuf, -1, &rcHint, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    } else {
        SetTextColor(hdc, RGB(165, 175, 195));
        DrawTextW(hdc, L"Tip: Click any key box above, then press a keyboard key (or mouse button) to rebind.",
            -1, &rcHint, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    }

    const wchar_t* btnLabels[3] = { L"Reset Defaults", L"Save && Close", L"Cancel" };
    for (int b = 0; b < 3; ++b) {
        bool isHov = (s_hoverBottomBtn == b);
        COLORREF bFill   = (b == 1)
            ? (isHov ? RGB(45, 125, 225) : RGB(32, 102, 195))
            : (isHov ? RGB(52, 58, 74)   : RGB(34, 38, 50));
        COLORREF bBorder = (b == 1) ? RGB(95, 170, 255) : RGB(95, 105, 128);
        HBRUSH hb = CreateSolidBrush(bFill);
        HPEN   hp = CreatePen(PS_SOLID, 1, bBorder);
        SelectObject(hdc, hb);
        SelectObject(hdc, hp);
        RoundRect(hdc, s_rcBottomBtns[b].left, s_rcBottomBtns[b].top, s_rcBottomBtns[b].right, s_rcBottomBtns[b].bottom, 6, 6);
        SelectObject(hdc, hFontKey);
        SetTextColor(hdc, RGB(245, 248, 255));
        RECT rcB = s_rcBottomBtns[b];
        DrawTextW(hdc, btnLabels[b], -1, &rcB, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        DeleteObject(hb);
        DeleteObject(hp);
    }

    SelectObject(hdc, oldBrush);
    SelectObject(hdc, oldPen);
    DeleteObject(hPadBody);
    DeleteObject(hPadDark);
    DeleteObject(hPadPen);
    DeleteObject(hFontBoxTitle);
    DeleteObject(hFontKey);
    DeleteObject(hFontSub);
}

static LRESULT CALLBACK KeyConfigWndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_ERASEBKGND:
            return 1; // Double-buffered in WM_PAINT
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC hdc = BeginPaint(hWnd, &ps);
            RECT rc;
            GetClientRect(hWnd, &rc);
            int w = rc.right - rc.left;
            int h = rc.bottom - rc.top;
            HDC hdcMem = CreateCompatibleDC(hdc);
            HBITMAP hBmp = CreateCompatibleBitmap(hdc, w, h);
            HGDIOBJ hOld = SelectObject(hdcMem, hBmp);
            PaintKeyboardConfigDiagram(hdcMem, w, h);
            BitBlt(hdc, 0, 0, w, h, hdcMem, 0, 0, SRCCOPY);
            SelectObject(hdcMem, hOld);
            DeleteObject(hBmp);
            DeleteDC(hdcMem);
            EndPaint(hWnd, &ps);
            return 0;
        }
        case WM_MOUSEMOVE: {
            POINT pt = { (short)LOWORD(lParam), (short)HIWORD(lParam) };
            int newHoverSlot = -1;
            for (int i = 0; i < (int)kNumKeyBindSlots; ++i) {
                if (PtInRect(&g_keySlots[i].rcSlot, pt)) {
                    newHoverSlot = i;
                    break;
                }
            }
            int newHoverBtn = -1;
            for (int b = 0; b < 3; ++b) {
                if (PtInRect(&s_rcBottomBtns[b], pt)) {
                    newHoverBtn = b;
                    break;
                }
            }
            if (newHoverSlot != s_hoverBindSlot || newHoverBtn != s_hoverBottomBtn) {
                s_hoverBindSlot  = newHoverSlot;
                s_hoverBottomBtn = newHoverBtn;
                InvalidateRect(hWnd, nullptr, FALSE);
            }
            SetCursor(LoadCursor(nullptr, (newHoverSlot >= 0 || newHoverBtn >= 0) ? IDC_HAND : IDC_ARROW));
            return 0;
        }
        case WM_LBUTTONDOWN: {
            POINT pt = { (short)LOWORD(lParam), (short)HIWORD(lParam) };
            // Check bottom buttons first
            for (int b = 0; b < 3; ++b) {
                if (PtInRect(&s_rcBottomBtns[b], pt)) {
                    s_activeBindSlot = -1;
                    if (b == 0) {
                        ResetKeyBindingsToDefault(&g_editKeyBindings);
                        InvalidateRect(hWnd, nullptr, FALSE);
                    } else if (b == 1) {
                        g_keyBindings = g_editKeyBindings;
                        s_keyDlgSaved = true;
                        s_keyDlgDone  = true;
                        DestroyWindow(hWnd);
                    } else if (b == 2) {
                        s_keyDlgSaved = false;
                        s_keyDlgDone  = true;
                        DestroyWindow(hWnd);
                    }
                    return 0;
                }
            }
            // Check key binding slots
            int clickedSlot = -1;
            for (int i = 0; i < (int)kNumKeyBindSlots; ++i) {
                if (PtInRect(&g_keySlots[i].rcSlot, pt)) {
                    clickedSlot = i;
                    break;
                }
            }
            if (clickedSlot >= 0) {
                if (s_activeBindSlot == clickedSlot) {
                    // Clicking the already-listening slot a second time binds Left Mouse Button
                    *g_keySlots[clickedSlot].pVk = VK_LBUTTON;
                    s_activeBindSlot = -1;
                } else {
                    s_activeBindSlot = clickedSlot;
                }
            } else {
                s_activeBindSlot = -1;
            }
            InvalidateRect(hWnd, nullptr, FALSE);
            return 0;
        }
        case WM_RBUTTONDOWN: {
            if (s_activeBindSlot >= 0 && s_activeBindSlot < (int)kNumKeyBindSlots) {
                *g_keySlots[s_activeBindSlot].pVk = VK_RBUTTON;
                s_activeBindSlot = -1;
                InvalidateRect(hWnd, nullptr, FALSE);
            }
            return 0;
        }
        case WM_MBUTTONDOWN: {
            if (s_activeBindSlot >= 0 && s_activeBindSlot < (int)kNumKeyBindSlots) {
                *g_keySlots[s_activeBindSlot].pVk = VK_MBUTTON;
                s_activeBindSlot = -1;
                InvalidateRect(hWnd, nullptr, FALSE);
            }
            return 0;
        }
        case WM_KEYDOWN:
        case WM_SYSKEYDOWN: {
            int vk = (int)wParam;
            if (s_activeBindSlot >= 0 && s_activeBindSlot < (int)kNumKeyBindSlots) {
                if (vk > 0 && vk < 256) {
                    *g_keySlots[s_activeBindSlot].pVk = vk;
                }
                s_activeBindSlot = -1;
                InvalidateRect(hWnd, nullptr, FALSE);
                return 0;
            }
            if (vk == VK_ESCAPE) {
                s_keyDlgSaved = false;
                s_keyDlgDone  = true;
                DestroyWindow(hWnd);
                return 0;
            }
            return 0;
        }
        case WM_CLOSE:
            s_activeBindSlot = -1;
            s_keyDlgSaved = false;
            s_keyDlgDone  = true;
            DestroyWindow(hWnd);
            return 0;
        case WM_DESTROY:
            s_keyDlgDone = true;
            return 0;
    }
    return DefWindowProcW(hWnd, msg, wParam, lParam);
}

static void ShowKeyboardConfigDialog(HWND hParent, HINSTANCE hInst) {
    g_editKeyBindings = g_keyBindings;
    s_activeBindSlot  = -1;
    s_hoverBindSlot   = -1;
    s_hoverBottomBtn  = -1;
    s_keyDlgSaved     = false;
    s_keyDlgDone      = false;

    WNDCLASSEXW wc = {};
    wc.cbSize        = sizeof(wc);
    wc.style         = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc   = KeyConfigWndProc;
    wc.hInstance     = hInst;
    wc.hIcon         = LoadIcon(nullptr, IDI_APPLICATION);
    wc.hCursor       = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    wc.lpszClassName = L"StarCraftGhostKeyConfigWndClass";
    RegisterClassExW(&wc);

    const int clientW = 900;
    const int clientH = 608;
    RECT rc = { 0, 0, clientW, clientH };
    DWORD dlgStyle = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU;
    AdjustWindowRect(&rc, dlgStyle, FALSE);
    int winW = rc.right - rc.left;
    int winH = rc.bottom - rc.top;
    int posX = (GetSystemMetrics(SM_CXSCREEN) - winW) / 2;
    int posY = (GetSystemMetrics(SM_CYSCREEN) - winH) / 2;
    if (posX < 0) posX = 0;
    if (posY < 0) posY = 0;

    EnableWindow(hParent, FALSE);

    HWND hKeyDlg = CreateWindowExW(
        WS_EX_DLGMODALFRAME, wc.lpszClassName,
        L"StarCraft: Ghost — Keyboard Controls Configuration",
        dlgStyle, posX, posY, winW, winH,
        hParent, nullptr, hInst, nullptr);
    if (!hKeyDlg) {
        EnableWindow(hParent, TRUE);
        return;
    }

    ShowWindow(hKeyDlg, SW_SHOW);
    UpdateWindow(hKeyDlg);
    SetForegroundWindow(hKeyDlg);
    SetFocus(hKeyDlg);

    MSG msg;
    while (!s_keyDlgDone && GetMessageW(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    EnableWindow(hParent, TRUE);
    SetForegroundWindow(hParent);

    if (s_keyDlgSaved) {
        wchar_t userDataDirW[MAX_PATH] = {};
        wchar_t iniPathW[MAX_PATH] = {};
        GetAppDataAndIniPathsW(userDataDirW, MAX_PATH, iniPathW, MAX_PATH);
        CreateDirectoryW(userDataDirW, nullptr);
        SaveKeyBindingsToIniW(iniPathW);
    }
}

// ============================================================================
// Pre-Launch Game Directory & GPU Adapter Dialog
// ============================================================================

static void UpdateAdapterDetailLabel() {
    if (!s_hComboGpu || !s_hStaticInfo || s_gpuAdapters.empty()) return;
    int sel = (int)SendMessageW(s_hComboGpu, CB_GETCURSEL, 0, 0);
    if (sel < 0 || sel >= (int)s_gpuAdapters.size()) sel = 0;
    const GpuAdapterInfo& a = s_gpuAdapters[sel];
    wchar_t infoBuf[256];
    swprintf(infoBuf, 256,
        L"Dedicated VRAM: %u MB   |   Shared Memory: %u MB   |   PCI %04X:%04X",
        (unsigned)a.dedicatedVramMB, (unsigned)a.sharedVramMB, a.vendorId, a.deviceId);
    SetWindowTextW(s_hStaticInfo, infoBuf);
}

static bool BrowseForGameFolder(HWND hParent) {
    HRESULT hrCo = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    IFileOpenDialog* pFileDlg = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&pFileDlg));
    if (FAILED(hr) || !pFileDlg) {
        if (SUCCEEDED(hrCo)) CoUninitialize();
        return false;
    }

    DWORD dwOptions = 0;
    if (SUCCEEDED(pFileDlg->GetOptions(&dwOptions))) {
        pFileDlg->SetOptions(dwOptions | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);
    }
    pFileDlg->SetTitle(L"Select Game Directory (containing Ghost.exe)");

    bool selectedValid = false;
    HRESULT hrShow = pFileDlg->Show(hParent);
    if (SUCCEEDED(hrShow)) {
        IShellItem* pItem = nullptr;
        if (SUCCEEDED(pFileDlg->GetResult(&pItem)) && pItem) {
            PWSTR pszFolderPath = nullptr;
            if (SUCCEEDED(pItem->GetDisplayName(SIGDN_FILESYSPATH, &pszFolderPath)) && pszFolderPath) {
                wchar_t candidateW[MAX_PATH] = {};
                wcsncpy(candidateW, pszFolderPath, MAX_PATH - 1);
                candidateW[MAX_PATH - 1] = L'\0';
                TrimTrailingSlashesW(candidateW);
                if (IsValidGameDirW(candidateW)) {
                    wcsncpy(s_selectedGameDirW, candidateW, MAX_PATH - 1);
                    s_selectedGameDirW[MAX_PATH - 1] = L'\0';
                    if (s_hEditGameDir) {
                        SetWindowTextW(s_hEditGameDir, s_selectedGameDirW);
                    }
                    selectedValid = true;
                } else {
                    wchar_t errMsg[512] = {};
                    swprintf(errMsg, 512,
                        L"The selected directory does not contain 'Ghost.exe':\n\n%ls\n\nPlease select the root game folder where 'Ghost.exe' is located.",
                        pszFolderPath);
                    MessageBoxW(hParent, errMsg, L"Ghost.exe Not Found", MB_ICONWARNING | MB_OK);
                    if (TryPortableFallbackToExeDir()) {
                        selectedValid = true;
                    }
                }
                CoTaskMemFree(pszFolderPath);
            }
            pItem->Release();
        }
    }

    pFileDlg->Release();
    if (SUCCEEDED(hrCo)) CoUninitialize();
    return selectedValid;
}

static LRESULT CALLBACK LauncherWndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

// Page 1 & Page 2 shared Launcher UI handles & state
static HWND     s_hTitle             = nullptr;
static HWND     s_hSub               = nullptr;
static HWND     s_hGrpDir            = nullptr;
static HWND     s_hLblDir            = nullptr;
static HWND     s_hBtnBrowse         = nullptr;
static HWND     s_hGrpGpu            = nullptr;
static HWND     s_hLblGpu            = nullptr;
static HWND     s_hChkCompileShaders = nullptr;
static HWND     s_hBtnKeys           = nullptr;
static HWND     s_hBtnOk             = nullptr;
static HWND     s_hBtnCancel         = nullptr;
static HFONT    s_hFontTitle         = nullptr;
static HFONT    s_hFontUI            = nullptr;
static HFONT    s_hFontBold          = nullptr;
static HFONT    s_hFontSmall         = nullptr;

static bool     s_inShaderCompilePage    = false;
static bool     s_shaderCompileCancelled = false;
static uint32_t s_shaderDoneCount        = 0;
static uint32_t s_shaderTotalCount       = 308;
static char     s_currentShaderName[128] = "fixed_ps";

static void UpdateShaderCheckboxState() {
    if (!s_hChkCompileShaders) return;

    wchar_t exeDataDirW[MAX_PATH] = {};
    wchar_t exeIniPathW[MAX_PATH] = {};
    GetAppDataAndIniPathsW(exeDataDirW, MAX_PATH, exeIniPathW, MAX_PATH);

    UINT compiledVal = GetPrivateProfileIntW(L"Shaders", L"Compiled", 0, exeIniPathW);

    wchar_t savedGameDirW[MAX_PATH] = {};
    GetPrivateProfileStringW(L"Paths", L"GameDir", L"", savedGameDirW, MAX_PATH, exeIniPathW);
    TrimTrailingSlashesW(savedGameDirW);

    bool gameDirMatches = (savedGameDirW[0] != L'\0' && _wcsicmp(savedGameDirW, s_selectedGameDirW) == 0);

    wchar_t fixedVsCsoW[MAX_PATH] = {};
    wchar_t fixedPsCsoW[MAX_PATH] = {};
    JoinPathW(fixedVsCsoW, MAX_PATH, exeDataDirW, L"shaders\\fixed_vs.cso");
    JoinPathW(fixedPsCsoW, MAX_PATH, exeDataDirW, L"shaders\\fixed_ps.cso");
    bool csoExists = (GetFileAttributesW(fixedVsCsoW) != INVALID_FILE_ATTRIBUTES &&
                      GetFileAttributesW(fixedPsCsoW) != INVALID_FILE_ATTRIBUTES);

    if (compiledVal != 0 && gameDirMatches && csoExists) {
        // Compiled=1: Unchecked by default, enabled so the user can check it to force recompilation
        SetWindowTextW(s_hChkCompileShaders, L"Recompile Shaders");
        SendMessageW(s_hChkCompileShaders, BM_SETCHECK, BST_UNCHECKED, 0);
        EnableWindow(s_hChkCompileShaders, TRUE);
    } else {
        // Compiled=0 (or cache missing / game folder changed): Automatically checked and grayed out
        SetWindowTextW(s_hChkCompileShaders, L"Compile Shaders");
        SendMessageW(s_hChkCompileShaders, BM_SETCHECK, BST_CHECKED, 0);
        EnableWindow(s_hChkCompileShaders, FALSE);
    }
}

static bool OnLauncherShaderProgressTick(uint32_t doneCount, uint32_t totalCount, const char* currentShaderName, void* userData) {
    HWND hWnd = reinterpret_cast<HWND>(userData);
    s_shaderDoneCount  = doneCount;
    s_shaderTotalCount = (totalCount > 0) ? totalCount : 308u;
    if (currentShaderName) {
        strncpy(s_currentShaderName, currentShaderName, sizeof(s_currentShaderName) - 1);
        s_currentShaderName[sizeof(s_currentShaderName) - 1] = '\0';
    }

    MSG msg;
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
        if (msg.message == WM_QUIT) {
            s_shaderCompileCancelled = true;
        }
        if (msg.message == WM_KEYDOWN && msg.wParam == VK_ESCAPE) {
            s_shaderCompileCancelled = true;
            continue;
        }
        if (!IsDialogMessageW(hWnd, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }

    if (hWnd && IsWindow(hWnd)) {
        RECT rcProgressZone = { 0, 0, 580, 276 };
        InvalidateRect(hWnd, &rcProgressZone, FALSE);
        UpdateWindow(hWnd);
    }

    return !s_shaderCompileCancelled;
}

static LRESULT CALLBACK LauncherWndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_CTLCOLORSTATIC: {
            HDC hdc = (HDC)wParam;
            SetBkMode(hdc, TRANSPARENT);
            SetTextColor(hdc, RGB(30, 34, 42));
            return (LRESULT)(s_hDlgBrush ? s_hDlgBrush : GetSysColorBrush(COLOR_BTNFACE));
        }
        case WM_PAINT: {
            if (!s_inShaderCompilePage) {
                break;
            }
            PAINTSTRUCT ps;
            HDC hdc = BeginPaint(hWnd, &ps);

            // Double-buffer the upper wizard page area (y = 0..276) so the bottom buttons never flicker
            const int w = 580;
            const int h = 276;
            HDC memDC = CreateCompatibleDC(hdc);
            HBITMAP memBmp = CreateCompatibleBitmap(hdc, w, h);
            HGDIOBJ oldBmp = SelectObject(memDC, memBmp);

            RECT rcUpper = { 0, 0, w, h };
            HBRUSH hBgBrush = CreateSolidBrush(RGB(243, 244, 248));
            FillRect(memDC, &rcUpper, hBgBrush);

            SetBkMode(memDC, TRANSPARENT);
            HGDIOBJ oldFont = SelectObject(memDC, s_hFontTitle);

            // 1. Top Title & Subtitle (matching exact Page 1 coordinates)
            SetTextColor(memDC, RGB(30, 34, 42));
            RECT rcTitle = { 22, 16, 558, 42 };
            DrawTextW(memDC, L"StarCraft: Ghost (PC Native Port)", -1, &rcTitle, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

            SelectObject(memDC, s_hFontUI);
            SetTextColor(memDC, RGB(60, 66, 78));
            RECT rcSub = { 22, 44, 558, 64 };
            DrawTextW(memDC, L"Precompiling Xbox NV2A shaders (.xpu / .xvu) to Direct3D 11...",
                      -1, &rcSub, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

            // 2. GroupBox Frame (18, 80 .. 562, 268) matching Page 1's GroupBoxes
            RECT rcGroup = { 18, 80, 562, 268 };
            HBRUSH hGroupBorder = CreateSolidBrush(RGB(208, 213, 222));
            FrameRect(memDC, &rcGroup, hGroupBorder);
            DeleteObject(hGroupBorder);

            SelectObject(memDC, s_hFontBold);
            SetBkMode(memDC, OPAQUE);
            SetBkColor(memDC, RGB(243, 244, 248));
            SetTextColor(memDC, RGB(30, 34, 42));
            RECT rcGrpTitle = { 28, 71, 310, 91 };
            DrawTextW(memDC, L" Direct3D 11 Shader Precompilation ", -1, &rcGrpTitle, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
            SetBkMode(memDC, TRANSPARENT);

            // 3. Shader Compilation Description, Current File & Percentage
            uint32_t done  = s_shaderDoneCount;
            uint32_t total = (s_shaderTotalCount > 0) ? s_shaderTotalCount : 308u;
            if (done > total) done = total;
            int pct = (int)((done * 100u) / total);

            SelectObject(memDC, s_hFontUI);
            SetTextColor(memDC, RGB(68, 74, 88));
            RECT rcDesc = { 34, 106, 546, 126 };
            DrawTextW(memDC, L"Decompiling Xbox (.xpu / .xvu) shaders to HLSL and compiling DXBC (.cso):",
                      -1, &rcDesc, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

            SelectObject(memDC, s_hFontBold);
            SetTextColor(memDC, RGB(24, 28, 36));
            char statusBuf[256];
            if (done >= total) {
                snprintf(statusBuf, sizeof(statusBuf), "Completed (%u / %u shaders)", done, total);
            } else {
                snprintf(statusBuf, sizeof(statusBuf), "Compiling (%u / %u): %s",
                         done, total, s_currentShaderName[0] ? s_currentShaderName : "fixed_ps");
            }
            RECT rcStatus = { 34, 138, 486, 158 };
            DrawTextA(memDC, statusBuf, -1, &rcStatus, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

            char pctBuf[32];
            snprintf(pctBuf, sizeof(pctBuf), "%d%%", pct);
            RECT rcPct = { 488, 138, 546, 158 };
            DrawTextA(memDC, pctBuf, -1, &rcPct, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);

            // 4. Segmented Block-by-Block Progress Bar (32 discrete blocks)
            RECT rcOuter = { 31, 168, 549, 202 };
            HBRUSH hFrameBrush = CreateSolidBrush(RGB(158, 165, 178));
            HBRUSH hTrackBrush = CreateSolidBrush(RGB(255, 255, 255));
            FillRect(memDC, &rcOuter, hTrackBrush);
            FrameRect(memDC, &rcOuter, hFrameBrush);
            DeleteObject(hFrameBrush);
            DeleteObject(hTrackBrush);

            constexpr int kNumBlocks = 32;
            int filledBlocks = (int)((done * (uint32_t)kNumBlocks) / total);
            if (done > 0 && filledBlocks == 0) filledBlocks = 1;
            if (filledBlocks > kNumBlocks) filledBlocks = kNumBlocks;

            HBRUSH hFillTopBrush  = CreateSolidBrush(RGB(44, 198, 78));
            HBRUSH hFillBotBrush  = CreateSolidBrush(RGB(16, 166, 50));
            HBRUSH hFillEdgeBrush = CreateSolidBrush(RGB(10, 134, 38));
            HBRUSH hEmptyBgBrush  = CreateSolidBrush(RGB(233, 236, 242));
            HBRUSH hEmptyEdgeBrush = CreateSolidBrush(RGB(214, 219, 228));

            for (int b = 0; b < kNumBlocks; ++b) {
                int bx = 35 + b * 16;
                RECT rcCell = { bx, 173, bx + 14, 197 };
                if (b < filledBlocks) {
                    RECT rcTop = { bx + 1, 174, bx + 13, 185 };
                    RECT rcBot = { bx + 1, 185, bx + 13, 196 };
                    FillRect(memDC, &rcTop, hFillTopBrush);
                    FillRect(memDC, &rcBot, hFillBotBrush);
                    FrameRect(memDC, &rcCell, hFillEdgeBrush);
                } else {
                    FillRect(memDC, &rcCell, hEmptyBgBrush);
                    FrameRect(memDC, &rcCell, hEmptyEdgeBrush);
                }
            }

            DeleteObject(hFillTopBrush);
            DeleteObject(hFillBotBrush);
            DeleteObject(hFillEdgeBrush);
            DeleteObject(hEmptyBgBrush);
            DeleteObject(hEmptyEdgeBrush);

            // 5. Footer Note inside GroupBox
            SelectObject(memDC, s_hFontSmall);
            SetTextColor(memDC, RGB(106, 114, 128));
            RECT rcFooter = { 34, 220, 546, 240 };
            wchar_t footerText[256] = {};
            swprintf(footerText, 256,
                     L"Shader cache is saved to %ls\\shaders\\ — future launches start immediately.",
                     kAppDataFolderNameW);
            DrawTextW(memDC, footerText, -1, &rcFooter, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

            SelectObject(memDC, oldFont);
            DeleteObject(hBgBrush);

            BitBlt(hdc, 0, 0, w, h, memDC, 0, 0, SRCCOPY);
            SelectObject(memDC, oldBmp);
            DeleteObject(memBmp);
            DeleteDC(memDC);
            EndPaint(hWnd, &ps);
            return 0;
        }
        case WM_COMMAND: {
            WORD id   = LOWORD(wParam);
            WORD code = HIWORD(wParam);
            if (id == IDC_COMBO_GPU && code == CBN_SELCHANGE) {
                UpdateAdapterDetailLabel();
                return 0;
            }
            if (id == IDC_BTN_BROWSE && code == BN_CLICKED) {
                BrowseForGameFolder(hWnd);
                UpdateShaderCheckboxState();
                SetForegroundWindow(hWnd);
                return 0;
            }
            if (id == IDC_BTN_KEYCONFIG && code == BN_CLICKED) {
                ShowKeyboardConfigDialog(hWnd, g_hInstance);
                return 0;
            }
            if (id == IDOK && code == BN_CLICKED) {
                if (s_inShaderCompilePage) {
                    return 0;
                }
                if (!IsValidGameDirW(s_selectedGameDirW)) {
                    TryPortableFallbackToExeDir();
                }
                if (!IsValidGameDirW(s_selectedGameDirW)) {
                    MessageBoxW(hWnd,
                        L"Please select a valid Game Directory containing 'Ghost.exe' first.",
                        L"Game Directory Required", MB_ICONINFORMATION | MB_OK);
                    if (!BrowseForGameFolder(hWnd) || !IsValidGameDirW(s_selectedGameDirW)) {
                        UpdateShaderCheckboxState();
                        SetForegroundWindow(hWnd);
                        return 0;
                    }
                    UpdateShaderCheckboxState();
                }
                int gpuSel = (int)SendMessageW(s_hComboGpu, CB_GETCURSEL, 0, 0);
                if (gpuSel >= 0 && gpuSel < (int)s_gpuAdapters.size()) {
                    g_selectedAdapterIndex = (int)s_gpuAdapters[gpuSel].index;
                    wcsncpy(g_selectedAdapterName, s_gpuAdapters[gpuSel].description, 127);
                    g_selectedAdapterName[127] = L'\0';
                }

                bool shouldCompileShaders = (s_hChkCompileShaders &&
                    SendMessageW(s_hChkCompileShaders, BM_GETCHECK, 0, 0) == BST_CHECKED);

                // Persist selected GameDir & GPU to config.ini and load Ghost.exe image
                PersistAndInitUserDataDir(s_selectedGameDirW);
                if (shouldCompileShaders) {
                    ClearShaderCacheDirectory();
                    SetShaderCacheCompiledInIni(false);
                }
                SetCurrentDirectoryA(g_gameRootDir);

                char ghostExePath[MAX_PATH] = {};
                snprintf(ghostExePath, sizeof(ghostExePath), "%s\\Ghost.exe", g_gameRootDir);
                if (!LoadGhostXboxPeImage(ghostExePath)) {
                    MessageBoxW(hWnd, L"Failed to load Ghost.exe from the selected Game Directory!", L"Error", MB_ICONERROR);
                    return 0;
                }

                // If checkbox is checked or shaders are not yet compiled, transition this window to Page 2
                if (shouldCompileShaders || !IsShaderCacheReadyOnDisk()) {
                    s_inShaderCompilePage    = true;
                    s_shaderCompileCancelled = false;
                    s_shaderDoneCount        = 0;
                    s_shaderTotalCount       = 308;
                    strncpy(s_currentShaderName, "fixed_ps", sizeof(s_currentShaderName) - 1);

                    ShowWindow(s_hTitle,             SW_HIDE);
                    ShowWindow(s_hSub,               SW_HIDE);
                    ShowWindow(s_hGrpDir,            SW_HIDE);
                    ShowWindow(s_hLblDir,            SW_HIDE);
                    ShowWindow(s_hEditGameDir,       SW_HIDE);
                    ShowWindow(s_hBtnBrowse,         SW_HIDE);
                    ShowWindow(s_hGrpGpu,            SW_HIDE);
                    ShowWindow(s_hLblGpu,            SW_HIDE);
                    ShowWindow(s_hComboGpu,          SW_HIDE);
                    ShowWindow(s_hStaticInfo,        SW_HIDE);
                    ShowWindow(s_hChkCompileShaders, SW_HIDE);

                    EnableWindow(s_hBtnKeys, FALSE);
                    SetWindowTextW(s_hBtnOk, L"Compiling...");
                    EnableWindow(s_hBtnOk, FALSE);
                    SetWindowTextW(s_hBtnCancel, L"Cancel");
                    SetWindowTextW(hWnd, L"StarCraft: Ghost — Compiling Shaders");

                    InvalidateRect(hWnd, nullptr, FALSE);
                    UpdateWindow(hWnd);

                    bool ok = CompileAllGameShadersToCache(OnLauncherShaderProgressTick, hWnd);
                    if (ok && IsWindow(hWnd)) {
                        RECT rcProgressZone = { 0, 0, 580, 276 };
                        InvalidateRect(hWnd, &rcProgressZone, FALSE);
                        UpdateWindow(hWnd);
                        Sleep(160);
                    }
                    s_launcherOk   = ok;
                    s_launcherDone = true;
                    if (IsWindow(hWnd)) {
                        DestroyWindow(hWnd);
                    }
                    return 0;
                }

                s_launcherOk   = true;
                s_launcherDone = true;
                DestroyWindow(hWnd);
                return 0;
            }
            if (id == IDCANCEL && code == BN_CLICKED) {
                if (s_inShaderCompilePage) {
                    s_shaderCompileCancelled = true;
                    return 0;
                }
                s_launcherOk   = false;
                s_launcherDone = true;
                DestroyWindow(hWnd);
                return 0;
            }
            break;
        }
        case WM_CLOSE:
            if (s_inShaderCompilePage) {
                s_shaderCompileCancelled = true;
                return 0;
            }
            s_launcherOk   = false;
            s_launcherDone = true;
            DestroyWindow(hWnd);
            return 0;
        case WM_DESTROY:
            s_launcherDone = true;
            PostMessageW(nullptr, WM_NULL, 0, 0);
            return 0;
    }
    return DefWindowProcW(hWnd, msg, wParam, lParam);
}

bool ShowLauncherDialog(HINSTANCE hInst) {
    // Enumerate DXGI Hardware Adapters
    s_gpuAdapters.clear();
    IDXGIFactory* pFactory = nullptr;
    if (SUCCEEDED(CreateDXGIFactory(__uuidof(IDXGIFactory), reinterpret_cast<void**>(&pFactory))) && pFactory) {
        IDXGIAdapter* pAdapter = nullptr;
        for (UINT i = 0; pFactory->EnumAdapters(i, &pAdapter) != DXGI_ERROR_NOT_FOUND; ++i) {
            DXGI_ADAPTER_DESC desc = {};
            if (SUCCEEDED(pAdapter->GetDesc(&desc))) {
                bool isSw = (desc.VendorId == 0x1414 && desc.DeviceId == 0x8C);
                // Filter out Windows built-in virtual WARP software adapter so only real hardware GPUs are listed
                if (!isSw) {
                    GpuAdapterInfo info = {};
                    info.index           = i;
                    wcsncpy(info.description, desc.Description, 127);
                    info.description[127] = L'\0';
                    info.dedicatedVramMB = desc.DedicatedVideoMemory / (1024 * 1024);
                    info.sharedVramMB    = desc.SharedSystemMemory / (1024 * 1024);
                    info.vendorId        = desc.VendorId;
                    info.deviceId        = desc.DeviceId;
                    info.isSoftware      = false;
                    s_gpuAdapters.push_back(info);
                }
            }
            pAdapter->Release();
        }
        pFactory->Release();
    }

    // 1. Locate config.ini in <exeDir>\<exeBaseName>\config.ini
    wchar_t exeDataDirW[MAX_PATH] = {};
    wchar_t exeIniPathW[MAX_PATH] = {};
    GetAppDataAndIniPathsW(exeDataDirW, MAX_PATH, exeIniPathW, MAX_PATH);

    s_selectedGameDirW[0] = L'\0';
    if (GetFileAttributesW(exeIniPathW) != INVALID_FILE_ATTRIBUTES) {
        GetPrivateProfileStringW(L"Paths", L"GameDir", L"", s_selectedGameDirW, MAX_PATH, exeIniPathW);
        TrimTrailingSlashesW(s_selectedGameDirW);
    }

    // 2. If config.ini had no GameDir or Ghost.exe was not found there, portable fallback checks ONLY exeDirW
    if (!IsValidGameDirW(s_selectedGameDirW)) {
        TryPortableFallbackToExeDir();
    }

    LoadKeyBindingsFromIniW(exeIniPathW);

    int initialGpuComboSel = ResolveGpuComboSelectionFromIniW(exeIniPathW);
    if (!s_gpuAdapters.empty()) {
        g_selectedAdapterIndex = (int)s_gpuAdapters[initialGpuComboSel].index;
        wcsncpy(g_selectedAdapterName, s_gpuAdapters[initialGpuComboSel].description, 127);
        g_selectedAdapterName[127] = L'\0';
    }

    WNDCLASSEXW wc = {};
    wc.cbSize        = sizeof(wc);
    wc.style         = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc   = LauncherWndProc;
    wc.hInstance     = hInst;
    wc.hIcon         = LoadIcon(nullptr, IDI_APPLICATION);
    wc.hCursor       = LoadCursor(nullptr, IDC_ARROW);
    s_hDlgBrush      = CreateSolidBrush(RGB(243, 244, 248));
    wc.hbrBackground = s_hDlgBrush;
    wc.lpszClassName = L"StarCraftGhostLauncherWndClass";
    RegisterClassExW(&wc);

    const int clientW = 580;
    const int clientH = 334;
    RECT rc = { 0, 0, clientW, clientH };
    DWORD dlgStyle = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
    AdjustWindowRect(&rc, dlgStyle, FALSE);
    int winW = rc.right - rc.left;
    int winH = rc.bottom - rc.top;
    int posX = (GetSystemMetrics(SM_CXSCREEN) - winW) / 2;
    int posY = (GetSystemMetrics(SM_CYSCREEN) - winH) / 2;
    if (posX < 0) posX = 0;
    if (posY < 0) posY = 0;

    HWND hDlg = CreateWindowExW(
        WS_EX_DLGMODALFRAME, wc.lpszClassName,
        L"StarCraft: Ghost — Launch Setup",
        dlgStyle, posX, posY, winW, winH,
        nullptr, nullptr, hInst, nullptr);
    if (!hDlg) {
        if (!IsValidGameDirW(s_selectedGameDirW)) return false;
        PersistAndInitUserDataDir(s_selectedGameDirW);
        return true;
    }

    s_hFontTitle = CreateFontW(-19, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
        DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
    s_hFontUI = CreateFontW(-14, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
        DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
    s_hFontBold = CreateFontW(-14, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
        DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
    s_hFontSmall = CreateFontW(-12, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
        DEFAULT_PITCH | FF_SWISS, L"Segoe UI");

    // Title & Subtitle
    s_hTitle = CreateWindowExW(0, L"STATIC", L"StarCraft: Ghost (PC Native Port)",
        WS_CHILD | WS_VISIBLE, 22, 16, 536, 26, hDlg, nullptr, hInst, nullptr);
    SendMessageW(s_hTitle, WM_SETFONT, (WPARAM)s_hFontTitle, TRUE);

    s_hSub = CreateWindowExW(0, L"STATIC", L"Configure game directory and Direct3D 11 graphics adapter:",
        WS_CHILD | WS_VISIBLE, 22, 44, 536, 20, hDlg, nullptr, hInst, nullptr);
    SendMessageW(s_hSub, WM_SETFONT, (WPARAM)s_hFontUI, TRUE);

    // 1. Game Directory GroupBox
    s_hGrpDir = CreateWindowExW(0, L"BUTTON", L"Game Directory (Ghost.exe)",
        WS_CHILD | WS_VISIBLE | BS_GROUPBOX, 18, 72, 544, 72, hDlg, nullptr, hInst, nullptr);
    SendMessageW(s_hGrpDir, WM_SETFONT, (WPARAM)s_hFontBold, TRUE);

    s_hLblDir = CreateWindowExW(0, L"STATIC", L"Game Folder:",
        WS_CHILD | WS_VISIBLE, 34, 104, 100, 22, hDlg, nullptr, hInst, nullptr);
    SendMessageW(s_hLblDir, WM_SETFONT, (WPARAM)s_hFontUI, TRUE);

    s_hEditGameDir = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT",
        s_selectedGameDirW[0] ? s_selectedGameDirW : L"<Click 'Browse...' to select folder containing Ghost.exe>",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL | ES_READONLY,
        136, 100, 312, 24, hDlg, (HMENU)(uintptr_t)IDC_EDIT_GAMEDIR, hInst, nullptr);
    SendMessageW(s_hEditGameDir, WM_SETFONT, (WPARAM)s_hFontUI, TRUE);

    s_hBtnBrowse = CreateWindowExW(0, L"BUTTON", L"Browse...",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
        456, 99, 88, 26, hDlg, (HMENU)(uintptr_t)IDC_BTN_BROWSE, hInst, nullptr);
    SendMessageW(s_hBtnBrowse, WM_SETFONT, (WPARAM)s_hFontUI, TRUE);

    // 2. Graphics Adapter & Shaders GroupBox
    s_hGrpGpu = CreateWindowExW(0, L"BUTTON", L"Graphics Adapter",
        WS_CHILD | WS_VISIBLE | BS_GROUPBOX, 18, 154, 544, 116, hDlg, nullptr, hInst, nullptr);
    SendMessageW(s_hGrpGpu, WM_SETFONT, (WPARAM)s_hFontBold, TRUE);

    // GPU Adapter Label & ComboBox
    s_hLblGpu = CreateWindowExW(0, L"STATIC", L"GPU Device:",
        WS_CHILD | WS_VISIBLE, 34, 186, 100, 22, hDlg, nullptr, hInst, nullptr);
    SendMessageW(s_hLblGpu, WM_SETFONT, (WPARAM)s_hFontUI, TRUE);

    s_hComboGpu = CreateWindowExW(0, L"COMBOBOX", L"",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_VSCROLL | CBS_DROPDOWNLIST,
        136, 182, 408, 240, hDlg, (HMENU)(uintptr_t)IDC_COMBO_GPU, hInst, nullptr);
    SendMessageW(s_hComboGpu, WM_SETFONT, (WPARAM)s_hFontUI, TRUE);

    for (size_t i = 0; i < s_gpuAdapters.size(); ++i) {
        const GpuAdapterInfo& a = s_gpuAdapters[i];
        wchar_t itemText[256];
        swprintf(itemText, 256, L"[#%u] %ls (%u MB)", a.index, a.description, (unsigned)a.dedicatedVramMB);
        SendMessageW(s_hComboGpu, CB_ADDSTRING, 0, (LPARAM)itemText);
    }
    if (s_gpuAdapters.empty()) {
        SendMessageW(s_hComboGpu, CB_ADDSTRING, 0, (LPARAM)L"[Default] Primary D3D11 Hardware Adapter");
    }
    SendMessageW(s_hComboGpu, CB_SETCURSEL, (WPARAM)initialGpuComboSel, 0);

    // Adapter Detail Info Label
    s_hStaticInfo = CreateWindowExW(0, L"STATIC", L"",
        WS_CHILD | WS_VISIBLE | SS_LEFTNOWORDWRAP, 138, 212, 406, 18, hDlg, (HMENU)(uintptr_t)IDC_STATIC_INFO, hInst, nullptr);
    SendMessageW(s_hStaticInfo, WM_SETFONT, (WPARAM)s_hFontSmall, TRUE);
    UpdateAdapterDetailLabel();

    // Compile Shaders Checkbox (auto-checked & disabled when Compiled=0; unchecked & enabled when Compiled=1)
    s_hChkCompileShaders = CreateWindowExW(0, L"BUTTON", L"Compile Shaders",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX,
        136, 238, 408, 22, hDlg, (HMENU)(uintptr_t)IDC_CHK_COMPILE_SHADERS, hInst, nullptr);
    SendMessageW(s_hChkCompileShaders, WM_SETFONT, (WPARAM)s_hFontUI, TRUE);
    UpdateShaderCheckboxState();

    // Keyboard Config, Launch & Exit Buttons
    s_hBtnKeys = CreateWindowExW(0, L"BUTTON", L"Keyboard Config...",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
        18, 284, 155, 34, hDlg, (HMENU)(uintptr_t)IDC_BTN_KEYCONFIG, hInst, nullptr);
    SendMessageW(s_hBtnKeys, WM_SETFONT, (WPARAM)s_hFontUI, TRUE);

    s_hBtnOk = CreateWindowExW(0, L"BUTTON", L"Launch Game",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON,
        322, 284, 125, 34, hDlg, (HMENU)(uintptr_t)IDOK, hInst, nullptr);
    SendMessageW(s_hBtnOk, WM_SETFONT, (WPARAM)s_hFontBold, TRUE);

    s_hBtnCancel = CreateWindowExW(0, L"BUTTON", L"Exit",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
        457, 284, 105, 34, hDlg, (HMENU)(uintptr_t)IDCANCEL, hInst, nullptr);
    SendMessageW(s_hBtnCancel, WM_SETFONT, (WPARAM)s_hFontUI, TRUE);

    s_launcherOk          = false;
    s_launcherDone        = false;
    s_inShaderCompilePage = false;

    ShowWindow(hDlg, SW_SHOW);
    UpdateWindow(hDlg);
    SetForegroundWindow(hDlg);
    SetFocus(s_hBtnOk);

    MSG msg;
    while (!s_launcherDone && GetMessageW(&msg, nullptr, 0, 0)) {
        if (msg.message == WM_KEYDOWN && msg.wParam == VK_ESCAPE) {
            SendMessageW(hDlg, WM_COMMAND, IDCANCEL, 0);
            continue;
        }
        if (!IsDialogMessageW(hDlg, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }

    DeleteObject(s_hFontTitle);
    DeleteObject(s_hFontUI);
    DeleteObject(s_hFontBold);
    DeleteObject(s_hFontSmall);
    s_hFontTitle = s_hFontUI = s_hFontBold = s_hFontSmall = nullptr;
    if (s_hDlgBrush) {
        DeleteObject(s_hDlgBrush);
        s_hDlgBrush = nullptr;
    }

    return s_launcherOk;
}
