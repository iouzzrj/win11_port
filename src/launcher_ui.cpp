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
    IDC_COMBO_GPU     = 101,
    IDC_STATIC_INFO   = 104,
    IDC_EDIT_GAMEDIR  = 105,
    IDC_BTN_BROWSE    = 106,
    IDC_BTN_KEYCONFIG = 107,
    IDC_CHK_AUTOBOT   = 108,
};

static std::vector<GpuAdapterInfo> s_gpuAdapters;
static HWND    s_hComboGpu    = nullptr;
static HWND    s_hStaticInfo  = nullptr;
static HWND    s_hEditGameDir = nullptr;
static HWND    s_hChkAutoBot  = nullptr;
static HBRUSH  s_hDlgBrush    = nullptr;
static bool    s_launcherOk   = false;
static bool    s_launcherDone = false;
static wchar_t s_selectedGameDirW[MAX_PATH] = { 0 };

// ============================================================================
// Path & Config INI Helpers (<exeDir>\GhostWin11\config.ini)
// ============================================================================

static void TrimTrailingSlashesW(wchar_t* path) {
    if (!path) return;
    size_t len = wcslen(path);
    while (len > 0 && (path[len - 1] == L' ' || path[len - 1] == L'\t' || path[len - 1] == L'\r' || path[len - 1] == L'\n')) {
        path[--len] = L'\0';
    }
    while (len > 0 && (path[len - 1] == L'\\' || path[len - 1] == L'/')) {
        if (len == 3 && path[1] == L':') break;
        path[--len] = L'\0';
    }
}

static void JoinPathW(wchar_t* outBuf, size_t maxCount, const wchar_t* baseDir, const wchar_t* subPath) {
    if (!baseDir || !baseDir[0]) {
        wcsncpy(outBuf, subPath ? subPath : L"", maxCount - 1);
        outBuf[maxCount - 1] = L'\0';
        return;
    }
    size_t len = wcslen(baseDir);
    bool hasSlash = (baseDir[len - 1] == L'\\' || baseDir[len - 1] == L'/');
    swprintf(outBuf, maxCount, hasSlash ? L"%ls%ls" : L"%ls\\%ls", baseDir, subPath);
}

static void GetExeDirW(wchar_t* outDirW, size_t maxCount) {
    if (!outDirW || maxCount == 0) return;
    outDirW[0] = L'\0';
    GetModuleFileNameW(nullptr, outDirW, (DWORD)maxCount);
    wchar_t* lastSlashW = wcsrchr(outDirW, L'\\');
    if (lastSlashW) *lastSlashW = L'\0';
    TrimTrailingSlashesW(outDirW);
}

static bool IsValidGameDirW(const wchar_t* dirW) {
    if (!dirW || !dirW[0]) return false;
    wchar_t ghostExeW[MAX_PATH] = {};
    JoinPathW(ghostExeW, MAX_PATH, dirW, L"Ghost.exe");
    DWORD attr = GetFileAttributesW(ghostExeW);
    return (attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY));
}

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

    // Default to the hardware GPU with the highest dedicated VRAM (discrete GPU on dual-GPU laptops)
    int bestSel = 0;
    SIZE_T bestVram = 0;
    for (size_t i = 0; i < s_gpuAdapters.size(); ++i) {
        if (!s_gpuAdapters[i].isSoftware && s_gpuAdapters[i].dedicatedVramMB > bestVram) {
            bestVram = s_gpuAdapters[i].dedicatedVramMB;
            bestSel = (int)i;
        }
    }

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

static void FormatVkNameW(int vk, wchar_t* outBuf, size_t maxCount) {
    if (!outBuf || maxCount == 0) return;
    if (vk <= 0) {
        wcsncpy(outBuf, L"None", maxCount - 1);
        outBuf[maxCount - 1] = L'\0';
        return;
    }
    switch (vk) {
        case VK_LBUTTON:  wcsncpy(outBuf, L"Mouse L", maxCount - 1); break;
        case VK_RBUTTON:  wcsncpy(outBuf, L"Mouse R", maxCount - 1); break;
        case VK_MBUTTON:  wcsncpy(outBuf, L"Mouse M", maxCount - 1); break;
        case VK_XBUTTON1: wcsncpy(outBuf, L"Mouse X1", maxCount - 1); break;
        case VK_XBUTTON2: wcsncpy(outBuf, L"Mouse X2", maxCount - 1); break;
        case VK_BACK:     wcsncpy(outBuf, L"Backspace", maxCount - 1); break;
        case VK_TAB:      wcsncpy(outBuf, L"Tab", maxCount - 1); break;
        case VK_RETURN:   wcsncpy(outBuf, L"Enter", maxCount - 1); break;
        case VK_SHIFT:    wcsncpy(outBuf, L"Shift", maxCount - 1); break;
        case VK_LSHIFT:   wcsncpy(outBuf, L"L-Shift", maxCount - 1); break;
        case VK_RSHIFT:   wcsncpy(outBuf, L"R-Shift", maxCount - 1); break;
        case VK_CONTROL:  wcsncpy(outBuf, L"Ctrl", maxCount - 1); break;
        case VK_LCONTROL: wcsncpy(outBuf, L"L-Ctrl", maxCount - 1); break;
        case VK_RCONTROL: wcsncpy(outBuf, L"R-Ctrl", maxCount - 1); break;
        case VK_MENU:     wcsncpy(outBuf, L"Alt", maxCount - 1); break;
        case VK_LMENU:    wcsncpy(outBuf, L"L-Alt", maxCount - 1); break;
        case VK_RMENU:    wcsncpy(outBuf, L"R-Alt", maxCount - 1); break;
        case VK_ESCAPE:   wcsncpy(outBuf, L"Esc", maxCount - 1); break;
        case VK_SPACE:    wcsncpy(outBuf, L"Space", maxCount - 1); break;
        case VK_PRIOR:    wcsncpy(outBuf, L"PgUp", maxCount - 1); break;
        case VK_NEXT:     wcsncpy(outBuf, L"PgDn", maxCount - 1); break;
        case VK_END:      wcsncpy(outBuf, L"End", maxCount - 1); break;
        case VK_HOME:     wcsncpy(outBuf, L"Home", maxCount - 1); break;
        case VK_LEFT:     wcsncpy(outBuf, L"Left", maxCount - 1); break;
        case VK_UP:       wcsncpy(outBuf, L"Up", maxCount - 1); break;
        case VK_RIGHT:    wcsncpy(outBuf, L"Right", maxCount - 1); break;
        case VK_DOWN:     wcsncpy(outBuf, L"Down", maxCount - 1); break;
        case VK_INSERT:   wcsncpy(outBuf, L"Insert", maxCount - 1); break;
        case VK_DELETE:   wcsncpy(outBuf, L"Delete", maxCount - 1); break;
        case VK_CAPITAL:  wcsncpy(outBuf, L"CapsLock", maxCount - 1); break;
        default:
            if ((vk >= 'A' && vk <= 'Z') || (vk >= '0' && vk <= '9')) {
                swprintf(outBuf, maxCount, L"%c", (wchar_t)vk);
            } else if (vk >= VK_NUMPAD0 && vk <= VK_NUMPAD9) {
                swprintf(outBuf, maxCount, L"Num %d", vk - VK_NUMPAD0);
            } else if (vk >= VK_F1 && vk <= VK_F12) {
                swprintf(outBuf, maxCount, L"F%d", vk - VK_F1 + 1);
            } else {
                UINT scan = MapVirtualKeyW((UINT)vk, MAPVK_VK_TO_VSC);
                if (scan == 0 || GetKeyNameTextW((LONG)(scan << 16), outBuf, (int)maxCount) == 0) {
                    swprintf(outBuf, maxCount, L"VK 0x%02X", vk);
                }
            }
            break;
    }
    outBuf[maxCount - 1] = L'\0';
}

struct KeyBindSlotMeta {
    const wchar_t* iniKey;
    const wchar_t* shortLabel; // Sub-label inside box (or empty if single-slot box)
    const wchar_t* actionDesc; // Full description for status bar
    int*           pVk;        // Pointer into s_editKeyBindings
    RECT           rcSlot;     // Clickable key box rect on the 900x608 dialog canvas
};

struct KeyCalloutBoxMeta {
    const wchar_t* title;
    RECT           rcBox;
    COLORREF       borderColor;
    POINT          lineStart;
    POINT          lineEnd;
};

static GhostKeyBindings s_editKeyBindings;
static int              s_activeBindSlot = -1;
static int              s_hoverBindSlot  = -1;
static int              s_hoverBottomBtn = -1; // 0=Reset, 1=Save, 2=Cancel
static bool             s_keyDlgSaved    = false;
static bool             s_keyDlgDone     = false;

static KeyBindSlotMeta s_keySlots[] = {
    { L"Crouch",      L"",      L"Crouch (L-Trigger)",                   &s_editKeyBindings.vkCrouch,      { 272, 108, 408, 138 } },
    { L"Fire",        L"",      L"Fire (R-Trigger)",                     &s_editKeyBindings.vkFire,        { 462,  46, 598,  76 } },
    { L"MoveUp",      L"\x2191", L"Move Up (Left Stick Up)",             &s_editKeyBindings.vkMoveUp,      { 103, 150, 161, 178 } },
    { L"MoveLeft",    L"\x2190", L"Move Left (Left Stick Left)",         &s_editKeyBindings.vkMoveLeft,    {  39, 184,  97, 212 } },
    { L"MoveDown",    L"\x2193", L"Move Down (Left Stick Down)",         &s_editKeyBindings.vkMoveDown,    { 103, 184, 161, 212 } },
    { L"MoveRight",   L"\x2192", L"Move Right (Left Stick Right)",       &s_editKeyBindings.vkMoveRight,   { 167, 184, 225, 212 } },
    { L"Sight",       L"",      L"Sight (D-Pad Up)",                     &s_editKeyBindings.vkSight,       {  46, 266, 218, 294 } },
    { L"MenuStart",   L"Start", L"Menu / Pause (Start)",                 &s_editKeyBindings.vkMenuStart,   {  70, 348, 130, 378 } },
    { L"MenuBack",    L"Back",  L"Objectives / Cancel (Back)",           &s_editKeyBindings.vkMenuBack,    { 170, 348, 230, 378 } },
    { L"Cloak",       L"",      L"Cloak (D-Pad Left)",                   &s_editKeyBindings.vkCloak,       {  46, 438, 218, 466 } },
    { L"GunLight",    L"",      L"Gun Light / Psi Charge (D-Pad Down)",  &s_editKeyBindings.vkGunLight,    { 150, 520, 325, 548 } },
    { L"DpadRight",   L"",      L"D-Pad Right (--)",                     &s_editKeyBindings.vkDpadRight,   { 378, 424, 518, 452 } },
    { L"AimUp",       L"\x2191", L"Aim Up (Right Stick Up)",             &s_editKeyBindings.vkAimUp,       { 434, 496, 492, 522 } },
    { L"AimLeft",     L"\x2190", L"Aim Left (Right Stick Left)",         &s_editKeyBindings.vkAimLeft,     { 370, 526, 428, 552 } },
    { L"AimDown",     L"\x2193", L"Aim Down (Right Stick Down)",         &s_editKeyBindings.vkAimDown,     { 434, 526, 492, 552 } },
    { L"AimRight",    L"\x2192", L"Aim Right (Right Stick Right)",       &s_editKeyBindings.vkAimRight,    { 498, 526, 556, 552 } },
    { L"AimClick",    L"Click", L"Sniper Zoom (Right Stick Click)",      &s_editKeyBindings.vkAimClick,    { 564, 526, 622, 552 } },
    { L"Melee",       L"",      L"Melee Attack (X Button)",              &s_editKeyBindings.vkMelee,       { 668,  96, 856, 124 } },
    { L"WeaponMode",  L"",      L"Weapon Mode (Y Button)",               &s_editKeyBindings.vkWeaponMode,  { 668, 174, 856, 202 } },
    { L"Action",      L"",      L"Action (B Button)",                    &s_editKeyBindings.vkAction,      { 668, 252, 856, 280 } },
    { L"Jump",        L"",      L"Jump (A Button)",                      &s_editKeyBindings.vkJump,        { 668, 330, 856, 358 } },
    { L"NextFirearm", L"",      L"Next Firearm (Black Button)",          &s_editKeyBindings.vkNextFirearm, { 668, 408, 856, 436 } },
    { L"NextGrenade", L"",      L"Next Grenade (White Button)",          &s_editKeyBindings.vkNextGrenade, { 668, 486, 856, 514 } },
};

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

static void LoadKeyBindingsFromIniW(const wchar_t* iniPathW) {
    ResetKeyBindingsToDefault(&g_keyBindings);
    if (!iniPathW || !iniPathW[0] || GetFileAttributesW(iniPathW) == INVALID_FILE_ATTRIBUTES) return;
    s_editKeyBindings = g_keyBindings;
    for (size_t i = 0; i < sizeof(s_keySlots) / sizeof(s_keySlots[0]); ++i) {
        int defVk = *s_keySlots[i].pVk;
        int val   = (int)GetPrivateProfileIntW(L"KeyBindings", s_keySlots[i].iniKey, defVk, iniPathW);
        if (val > 0 && val < 256) {
            *s_keySlots[i].pVk = val;
        }
    }
    g_keyBindings = s_editKeyBindings;
}

static void SaveKeyBindingsToIniW(const wchar_t* iniPathW) {
    if (!iniPathW || !iniPathW[0]) return;
    s_editKeyBindings = g_keyBindings;
    for (size_t i = 0; i < sizeof(s_keySlots) / sizeof(s_keySlots[0]); ++i) {
        wchar_t numBuf[32];
        swprintf(numBuf, 32, L"%d", *s_keySlots[i].pVk);
        WritePrivateProfileStringW(L"KeyBindings", s_keySlots[i].iniKey, numBuf, iniPathW);
    }
}

static void PersistAndInitUserDataDir(const wchar_t* gameDirW) {
    if (!IsValidGameDirW(gameDirW)) return;
    WideCharToMultiByte(CP_ACP, 0, gameDirW, -1, g_gameRootDir, MAX_PATH, nullptr, nullptr);

    wchar_t exeDirW[MAX_PATH] = {};
    wchar_t userDataDirW[MAX_PATH] = {};
    wchar_t iniPathW[MAX_PATH] = {};
    GetExeDirW(exeDirW, MAX_PATH);
    JoinPathW(userDataDirW, MAX_PATH, exeDirW, L"GhostWin11");
    CreateDirectoryW(userDataDirW, nullptr);
    WideCharToMultiByte(CP_ACP, 0, userDataDirW, -1, g_appRootDir, sizeof(g_appRootDir), nullptr, nullptr);

    if (g_appRootDir[0]) {
        char logPath[MAX_PATH] = {};
        snprintf(logPath, sizeof(logPath), "%s\\runtime.log", g_appRootDir);
        LogInit(logPath);
        LogPrintf("[Main] User Data Directory: '%s'", g_appRootDir);
        for (const GpuAdapterInfo& info : s_gpuAdapters) {
            LogPrintf("[Launcher] Detected GPU #%u: %ls (Dedicated VRAM=%u MB, Shared=%u MB, Vendor=0x%04X, Device=0x%04X)",
                info.index, info.description, (unsigned)info.dedicatedVramMB, (unsigned)info.sharedVramMB,
                info.vendorId, info.deviceId);
        }
    }

    JoinPathW(iniPathW, MAX_PATH, userDataDirW, L"config.ini");
    wchar_t numBuf[32];
    swprintf(numBuf, 32, L"%d", g_selectedAdapterIndex);

    WritePrivateProfileStringW(L"Paths", L"GameDir", gameDirW, iniPathW);
    WritePrivateProfileStringW(L"Graphics", L"AdapterIndex", numBuf, iniPathW);
    WritePrivateProfileStringW(L"Graphics", L"AdapterName", g_selectedAdapterName, iniPathW);
    if (!g_cliAutoBotLaunch) {
        WritePrivateProfileStringW(L"AutoBot", L"Enabled", g_autoBotEnabled ? L"1" : L"0", iniPathW);
    }
    SaveKeyBindingsToIniW(iniPathW);
}

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
    for (int i = 0; i < (int)(sizeof(s_keySlots) / sizeof(s_keySlots[0])); ++i) {
        const KeyBindSlotMeta& s = s_keySlots[i];
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
    if (s_activeBindSlot >= 0 && s_activeBindSlot < (int)(sizeof(s_keySlots) / sizeof(s_keySlots[0]))) {
        SetTextColor(hdc, RGB(255, 215, 70));
        wchar_t promptBuf[256];
        swprintf(promptBuf, 256, L"Rebinding [%ls]: Press any keyboard key (or click mouse button)...",
            s_keySlots[s_activeBindSlot].actionDesc);
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
            for (int i = 0; i < (int)(sizeof(s_keySlots) / sizeof(s_keySlots[0])); ++i) {
                if (PtInRect(&s_keySlots[i].rcSlot, pt)) {
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
                        ResetKeyBindingsToDefault(&s_editKeyBindings);
                        InvalidateRect(hWnd, nullptr, FALSE);
                    } else if (b == 1) {
                        g_keyBindings = s_editKeyBindings;
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
            for (int i = 0; i < (int)(sizeof(s_keySlots) / sizeof(s_keySlots[0])); ++i) {
                if (PtInRect(&s_keySlots[i].rcSlot, pt)) {
                    clickedSlot = i;
                    break;
                }
            }
            if (clickedSlot >= 0) {
                if (s_activeBindSlot == clickedSlot) {
                    // Clicking the already-listening slot a second time binds Left Mouse Button
                    *s_keySlots[clickedSlot].pVk = VK_LBUTTON;
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
            if (s_activeBindSlot >= 0 && s_activeBindSlot < (int)(sizeof(s_keySlots) / sizeof(s_keySlots[0]))) {
                *s_keySlots[s_activeBindSlot].pVk = VK_RBUTTON;
                s_activeBindSlot = -1;
                InvalidateRect(hWnd, nullptr, FALSE);
            }
            return 0;
        }
        case WM_MBUTTONDOWN: {
            if (s_activeBindSlot >= 0 && s_activeBindSlot < (int)(sizeof(s_keySlots) / sizeof(s_keySlots[0]))) {
                *s_keySlots[s_activeBindSlot].pVk = VK_MBUTTON;
                s_activeBindSlot = -1;
                InvalidateRect(hWnd, nullptr, FALSE);
            }
            return 0;
        }
        case WM_KEYDOWN:
        case WM_SYSKEYDOWN: {
            int vk = (int)wParam;
            if (s_activeBindSlot >= 0 && s_activeBindSlot < (int)(sizeof(s_keySlots) / sizeof(s_keySlots[0]))) {
                if (vk > 0 && vk < 256) {
                    *s_keySlots[s_activeBindSlot].pVk = vk;
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
    s_editKeyBindings = g_keyBindings;
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
        wchar_t exeDirW[MAX_PATH] = {};
        wchar_t userDataDirW[MAX_PATH] = {};
        wchar_t iniPathW[MAX_PATH] = {};
        GetExeDirW(exeDirW, MAX_PATH);
        JoinPathW(userDataDirW, MAX_PATH, exeDirW, L"GhostWin11");
        CreateDirectoryW(userDataDirW, nullptr);
        JoinPathW(iniPathW, MAX_PATH, userDataDirW, L"config.ini");
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

static LRESULT CALLBACK LauncherWndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_CTLCOLORSTATIC: {
            HDC hdc = (HDC)wParam;
            SetBkMode(hdc, TRANSPARENT);
            SetTextColor(hdc, RGB(30, 34, 42));
            return (LRESULT)(s_hDlgBrush ? s_hDlgBrush : GetSysColorBrush(COLOR_BTNFACE));
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
                SetForegroundWindow(hWnd);
                return 0;
            }
            if (id == IDC_BTN_KEYCONFIG && code == BN_CLICKED) {
                ShowKeyboardConfigDialog(hWnd, g_hInstance);
                return 0;
            }
            if (id == IDOK && code == BN_CLICKED) {
                if (!IsValidGameDirW(s_selectedGameDirW)) {
                    TryPortableFallbackToExeDir();
                }
                if (!IsValidGameDirW(s_selectedGameDirW)) {
                    MessageBoxW(hWnd,
                        L"Please select a valid Game Directory containing 'Ghost.exe' first.",
                        L"Game Directory Required", MB_ICONINFORMATION | MB_OK);
                    if (!BrowseForGameFolder(hWnd) || !IsValidGameDirW(s_selectedGameDirW)) {
                        SetForegroundWindow(hWnd);
                        return 0;
                    }
                }
                int gpuSel = (int)SendMessageW(s_hComboGpu, CB_GETCURSEL, 0, 0);
                if (gpuSel >= 0 && gpuSel < (int)s_gpuAdapters.size()) {
                    g_selectedAdapterIndex = (int)s_gpuAdapters[gpuSel].index;
                    wcsncpy(g_selectedAdapterName, s_gpuAdapters[gpuSel].description, 127);
                    g_selectedAdapterName[127] = L'\0';
                }
                if (s_hChkAutoBot) {
                    g_autoBotEnabled = (SendMessageW(s_hChkAutoBot, BM_GETCHECK, 0, 0) == BST_CHECKED);
                }
                s_launcherOk   = true;
                s_launcherDone = true;
                DestroyWindow(hWnd);
                return 0;
            }
            if (id == IDCANCEL && code == BN_CLICKED) {
                s_launcherOk   = false;
                s_launcherDone = true;
                DestroyWindow(hWnd);
                return 0;
            }
            break;
        }
        case WM_CLOSE:
            s_launcherOk   = false;
            s_launcherDone = true;
            DestroyWindow(hWnd);
            return 0;
        case WM_DESTROY:
            s_launcherDone = true;
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

    // Determine GhostWin11.exe directory
    wchar_t exeDirW[MAX_PATH] = {};
    GetExeDirW(exeDirW, MAX_PATH);

    // 1. Locate config.ini in <exeDir>\GhostWin11\config.ini
    wchar_t exeDataDirW[MAX_PATH] = {};
    wchar_t exeIniPathW[MAX_PATH] = {};
    JoinPathW(exeDataDirW, MAX_PATH, exeDirW, L"GhostWin11");
    JoinPathW(exeIniPathW, MAX_PATH, exeDataDirW, L"config.ini");

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

    if (GetFileAttributesW(exeIniPathW) != INVALID_FILE_ATTRIBUTES) {
        g_autoBotEnabled = (GetPrivateProfileIntW(L"AutoBot", L"Enabled", 0, exeIniPathW) != 0);
    }

    int initialGpuComboSel = ResolveGpuComboSelectionFromIniW(exeIniPathW);
    if (!s_gpuAdapters.empty()) {
        g_selectedAdapterIndex = (int)s_gpuAdapters[initialGpuComboSel].index;
        wcsncpy(g_selectedAdapterName, s_gpuAdapters[initialGpuComboSel].description, 127);
        g_selectedAdapterName[127] = L'\0';
    }

    const char* cmdLineA = GetCommandLineA();
    if (cmdLineA && strstr(cmdLineA, "--autobot") != nullptr) {
        g_cliAutoBotLaunch = true;
        g_cliAutoBotTrain  = (strstr(cmdLineA, "--autobot-train") != nullptr);
        g_autoBotEnabled   = true;
        if (IsValidGameDirW(s_selectedGameDirW)) {
            PersistAndInitUserDataDir(s_selectedGameDirW);
            LogPrintf("[Launcher] Headless --autobot launch: GameDir='%s', GPU=#%d (%ls)",
                g_gameRootDir, g_selectedAdapterIndex, g_selectedAdapterName);
            return true;
        }
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
    const int clientH = 335;
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

    HFONT hFontTitle = CreateFontW(-19, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
        DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
    HFONT hFontUI = CreateFontW(-14, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
        DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
    HFONT hFontBold = CreateFontW(-14, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
        DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
    HFONT hFontSmall = CreateFontW(-12, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
        DEFAULT_PITCH | FF_SWISS, L"Segoe UI");

    // Title & Subtitle
    HWND hTitle = CreateWindowExW(0, L"STATIC", L"StarCraft: Ghost (PC Native Port)",
        WS_CHILD | WS_VISIBLE, 22, 16, 536, 26, hDlg, nullptr, hInst, nullptr);
    SendMessageW(hTitle, WM_SETFONT, (WPARAM)hFontTitle, TRUE);

    HWND hSub = CreateWindowExW(0, L"STATIC", L"Configure game directory and Direct3D 11 graphics adapter:",
        WS_CHILD | WS_VISIBLE, 22, 44, 536, 20, hDlg, nullptr, hInst, nullptr);
    SendMessageW(hSub, WM_SETFONT, (WPARAM)hFontUI, TRUE);

    // 1. Game Directory GroupBox
    HWND hGrpDir = CreateWindowExW(0, L"BUTTON", L"Game Directory (Ghost.exe)",
        WS_CHILD | WS_VISIBLE | BS_GROUPBOX, 18, 72, 544, 72, hDlg, nullptr, hInst, nullptr);
    SendMessageW(hGrpDir, WM_SETFONT, (WPARAM)hFontBold, TRUE);

    HWND hLblDir = CreateWindowExW(0, L"STATIC", L"Game Folder:",
        WS_CHILD | WS_VISIBLE, 34, 104, 100, 22, hDlg, nullptr, hInst, nullptr);
    SendMessageW(hLblDir, WM_SETFONT, (WPARAM)hFontUI, TRUE);

    s_hEditGameDir = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT",
        s_selectedGameDirW[0] ? s_selectedGameDirW : L"<Click 'Browse...' to select folder containing Ghost.exe>",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL | ES_READONLY,
        136, 100, 312, 24, hDlg, (HMENU)(uintptr_t)IDC_EDIT_GAMEDIR, hInst, nullptr);
    SendMessageW(s_hEditGameDir, WM_SETFONT, (WPARAM)hFontUI, TRUE);

    HWND hBtnBrowse = CreateWindowExW(0, L"BUTTON", L"Browse...",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
        456, 99, 88, 26, hDlg, (HMENU)(uintptr_t)IDC_BTN_BROWSE, hInst, nullptr);
    SendMessageW(hBtnBrowse, WM_SETFONT, (WPARAM)hFontUI, TRUE);

    // 2. Graphics Adapter GroupBox
    HWND hGrp = CreateWindowExW(0, L"BUTTON", L"Graphics Adapter",
        WS_CHILD | WS_VISIBLE | BS_GROUPBOX, 18, 154, 544, 92, hDlg, nullptr, hInst, nullptr);
    SendMessageW(hGrp, WM_SETFONT, (WPARAM)hFontBold, TRUE);

    // GPU Adapter Label & ComboBox
    HWND hLblGpu = CreateWindowExW(0, L"STATIC", L"GPU Device:",
        WS_CHILD | WS_VISIBLE, 34, 186, 100, 22, hDlg, nullptr, hInst, nullptr);
    SendMessageW(hLblGpu, WM_SETFONT, (WPARAM)hFontUI, TRUE);

    s_hComboGpu = CreateWindowExW(0, L"COMBOBOX", L"",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_VSCROLL | CBS_DROPDOWNLIST,
        136, 182, 408, 240, hDlg, (HMENU)(uintptr_t)IDC_COMBO_GPU, hInst, nullptr);
    SendMessageW(s_hComboGpu, WM_SETFONT, (WPARAM)hFontUI, TRUE);

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
        WS_CHILD | WS_VISIBLE | SS_LEFTNOWORDWRAP, 138, 214, 406, 18, hDlg, (HMENU)(uintptr_t)IDC_STATIC_INFO, hInst, nullptr);
    SendMessageW(s_hStaticInfo, WM_SETFONT, (WPARAM)hFontSmall, TRUE);
    UpdateAdapterDetailLabel();

    // AI Auto-Play Bot Checkbox
    s_hChkAutoBot = CreateWindowExW(0, L"BUTTON",
        L"Enable AI Auto-Play Bot (F5 in-game: State Machine + Waypoint Nav + Auto-Aim)",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX,
        22, 254, 536, 22, hDlg, (HMENU)(uintptr_t)IDC_CHK_AUTOBOT, hInst, nullptr);
    SendMessageW(s_hChkAutoBot, WM_SETFONT, (WPARAM)hFontUI, TRUE);
    SendMessageW(s_hChkAutoBot, BM_SETCHECK, g_autoBotEnabled ? BST_CHECKED : BST_UNCHECKED, 0);

    // Keyboard Config, Launch & Exit Buttons
    HWND hBtnKeys = CreateWindowExW(0, L"BUTTON", L"Keyboard Config...",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
        18, 286, 155, 34, hDlg, (HMENU)(uintptr_t)IDC_BTN_KEYCONFIG, hInst, nullptr);
    SendMessageW(hBtnKeys, WM_SETFONT, (WPARAM)hFontUI, TRUE);

    HWND hBtnOk = CreateWindowExW(0, L"BUTTON", L"Launch Game",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON,
        322, 286, 125, 34, hDlg, (HMENU)(uintptr_t)IDOK, hInst, nullptr);
    SendMessageW(hBtnOk, WM_SETFONT, (WPARAM)hFontBold, TRUE);

    HWND hBtnCancel = CreateWindowExW(0, L"BUTTON", L"Exit",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
        457, 286, 105, 34, hDlg, (HMENU)(uintptr_t)IDCANCEL, hInst, nullptr);
    SendMessageW(hBtnCancel, WM_SETFONT, (WPARAM)hFontUI, TRUE);

    s_launcherOk   = false;
    s_launcherDone = false;

    ShowWindow(hDlg, SW_SHOW);
    UpdateWindow(hDlg);
    SetForegroundWindow(hDlg);
    SetFocus(hBtnOk);

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

    DeleteObject(hFontTitle);
    DeleteObject(hFontUI);
    DeleteObject(hFontBold);
    DeleteObject(hFontSmall);
    if (s_hDlgBrush) {
        DeleteObject(s_hDlgBrush);
        s_hDlgBrush = nullptr;
    }

    if (!s_launcherOk) {
        return false;
    }

    // Persist selected GameDir & GPU to <exeDir>\GhostWin11\config.ini and initialize runtime.log
    PersistAndInitUserDataDir(s_selectedGameDirW);
    LogPrintf("[Launcher] Confirmed GameDir='%s', GPU=#%d (%ls)", g_gameRootDir, g_selectedAdapterIndex, g_selectedAdapterName);
    return true;
}
