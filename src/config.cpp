#include "ghost_runtime.h"

// ============================================================================
// Path & Config INI Helpers (<exeDir>\GhostWin11_Data\config.ini)
// ============================================================================

GhostKeyBindings g_editKeyBindings        = {};
bool             g_enableHavokDebugServer = false;

KeyBindSlotMeta g_keySlots[23] = {
    { L"Crouch",      L"",      L"Crouch (L-Trigger)",                   &g_editKeyBindings.vkCrouch,      { 272, 108, 408, 138 } },
    { L"Fire",        L"",      L"Fire (R-Trigger)",                     &g_editKeyBindings.vkFire,        { 462,  46, 598,  76 } },
    { L"MoveUp",      L"\x2191", L"Move Up (Left Stick Up)",             &g_editKeyBindings.vkMoveUp,      { 103, 150, 161, 178 } },
    { L"MoveLeft",    L"\x2190", L"Move Left (Left Stick Left)",         &g_editKeyBindings.vkMoveLeft,    {  39, 184,  97, 212 } },
    { L"MoveDown",    L"\x2193", L"Move Down (Left Stick Down)",         &g_editKeyBindings.vkMoveDown,    { 103, 184, 161, 212 } },
    { L"MoveRight",   L"\x2192", L"Move Right (Left Stick Right)",       &g_editKeyBindings.vkMoveRight,   { 167, 184, 225, 212 } },
    { L"Sight",       L"",      L"Sight (D-Pad Up)",                     &g_editKeyBindings.vkSight,       {  46, 266, 218, 294 } },
    { L"MenuStart",   L"Start", L"Menu / Pause (Start)",                 &g_editKeyBindings.vkMenuStart,   {  70, 348, 130, 378 } },
    { L"MenuBack",    L"Back",  L"Objectives / Cancel (Back)",           &g_editKeyBindings.vkMenuBack,    { 170, 348, 230, 378 } },
    { L"Cloak",       L"",      L"Cloak (D-Pad Left)",                   &g_editKeyBindings.vkCloak,       {  46, 438, 218, 466 } },
    { L"GunLight",    L"",      L"Gun Light / Psi Charge (D-Pad Down)",  &g_editKeyBindings.vkGunLight,    { 150, 520, 325, 548 } },
    { L"DpadRight",   L"",      L"D-Pad Right (--)",                     &g_editKeyBindings.vkDpadRight,   { 378, 424, 518, 452 } },
    { L"AimUp",       L"\x2191", L"Aim Up (Right Stick Up)",             &g_editKeyBindings.vkAimUp,       { 434, 496, 492, 522 } },
    { L"AimLeft",     L"\x2190", L"Aim Left (Right Stick Left)",         &g_editKeyBindings.vkAimLeft,     { 370, 526, 428, 552 } },
    { L"AimDown",     L"\x2193", L"Aim Down (Right Stick Down)",         &g_editKeyBindings.vkAimDown,     { 434, 526, 492, 552 } },
    { L"AimRight",    L"\x2192", L"Aim Right (Right Stick Right)",       &g_editKeyBindings.vkAimRight,    { 498, 526, 556, 552 } },
    { L"AimClick",    L"Click", L"Sniper Zoom (Right Stick Click)",      &g_editKeyBindings.vkAimClick,    { 564, 526, 622, 552 } },
    { L"Melee",       L"",      L"Melee Attack (X Button)",              &g_editKeyBindings.vkMelee,       { 668,  96, 856, 124 } },
    { L"WeaponMode",  L"",      L"Weapon Mode (Y Button)",               &g_editKeyBindings.vkWeaponMode,  { 668, 174, 856, 202 } },
    { L"Action",      L"",      L"Action (B Button)",                    &g_editKeyBindings.vkAction,      { 668, 252, 856, 280 } },
    { L"Jump",        L"",      L"Jump (A Button)",                      &g_editKeyBindings.vkJump,        { 668, 330, 856, 358 } },
    { L"NextFirearm", L"",      L"Next Firearm (Black Button)",          &g_editKeyBindings.vkNextFirearm, { 668, 408, 856, 436 } },
    { L"NextGrenade", L"",      L"Next Grenade (White Button)",          &g_editKeyBindings.vkNextGrenade, { 668, 486, 856, 514 } },
};

void TrimTrailingSlashesW(wchar_t* path) {
    if (!path) return;
    if (path[0] >= L'a' && path[0] <= L'z' && path[1] == L':') {
        path[0] = (wchar_t)(path[0] - L'a' + L'A');
    }
    size_t len = wcslen(path);
    while (len > 0 && (path[len - 1] == L' ' || path[len - 1] == L'\t' || path[len - 1] == L'\r' || path[len - 1] == L'\n')) {
        path[--len] = L'\0';
    }
    while (len > 0 && (path[len - 1] == L'\\' || path[len - 1] == L'/')) {
        if (len == 3 && path[1] == L':') break;
        path[--len] = L'\0';
    }
}

void JoinPathW(wchar_t* outBuf, size_t maxCount, const wchar_t* baseDir, const wchar_t* subPath) {
    if (!baseDir || !baseDir[0]) {
        wcsncpy(outBuf, subPath ? subPath : L"", maxCount - 1);
        outBuf[maxCount - 1] = L'\0';
        return;
    }
    size_t len = wcslen(baseDir);
    bool hasSlash = (baseDir[len - 1] == L'\\' || baseDir[len - 1] == L'/');
    swprintf(outBuf, maxCount, hasSlash ? L"%ls%ls" : L"%ls\\%ls", baseDir, subPath);
}

void GetExeDirW(wchar_t* outDirW, size_t maxCount) {
    if (!outDirW || maxCount == 0) return;
    outDirW[0] = L'\0';
    GetModuleFileNameW(nullptr, outDirW, (DWORD)maxCount);
    wchar_t* lastSlashW = wcsrchr(outDirW, L'\\');
    if (lastSlashW) *lastSlashW = L'\0';
    TrimTrailingSlashesW(outDirW);
}

void GetAppDataAndIniPathsW(wchar_t* outDataDirW, size_t dataDirMax, wchar_t* outIniPathW, size_t iniPathMax) {
    wchar_t exeDirW[MAX_PATH] = {};
    wchar_t dataDirW[MAX_PATH] = {};
    GetExeDirW(exeDirW, MAX_PATH);
    JoinPathW(dataDirW, MAX_PATH, exeDirW, kAppDataFolderNameW);

    if (outDataDirW && dataDirMax > 0) {
        wcsncpy(outDataDirW, dataDirW, dataDirMax - 1);
        outDataDirW[dataDirMax - 1] = L'\0';
    }
    if (outIniPathW && iniPathMax > 0) {
        JoinPathW(outIniPathW, iniPathMax, dataDirW, L"config.ini");
    }
}

bool IsValidGameDirW(const wchar_t* dirW) {
    if (!dirW || !dirW[0]) return false;
    wchar_t ghostExeW[MAX_PATH] = {};
    JoinPathW(ghostExeW, MAX_PATH, dirW, L"Ghost.exe");
    DWORD attr = GetFileAttributesW(ghostExeW);
    return (attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY));
}

void FormatVkNameW(int vk, wchar_t* outBuf, size_t maxCount) {
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

void LoadKeyBindingsFromIniW(const wchar_t* iniPathW) {
    ResetKeyBindingsToDefault(&g_keyBindings);
    if (!iniPathW || !iniPathW[0] || GetFileAttributesW(iniPathW) == INVALID_FILE_ATTRIBUTES) return;
    g_editKeyBindings = g_keyBindings;
    for (size_t i = 0; i < kNumKeyBindSlots; ++i) {
        int defVk = *g_keySlots[i].pVk;
        int val   = (int)GetPrivateProfileIntW(L"KeyBindings", g_keySlots[i].iniKey, defVk, iniPathW);
        if (val > 0 && val < 256) {
            *g_keySlots[i].pVk = val;
        }
    }
    g_keyBindings = g_editKeyBindings;
    g_osdShowFps             = GetPrivateProfileIntW(L"OSD", L"ShowFps", 0, iniPathW) != 0;
    g_enableHavokDebugServer = GetPrivateProfileIntW(L"Compatibility", L"EnableHavokDebugServer", 0, iniPathW) != 0;
}

void SaveOsdStateToIni() {
    wchar_t dataDirW[MAX_PATH] = {};
    wchar_t iniPathW[MAX_PATH] = {};
    GetAppDataAndIniPathsW(dataDirW, MAX_PATH, iniPathW, MAX_PATH);
    if (iniPathW[0]) {
        CreateDirectoryW(dataDirW, nullptr);
        WritePrivateProfileStringW(L"OSD", L"ShowFps", g_osdShowFps ? L"1" : L"0", iniPathW);
    }
}

bool IsShaderCacheCompiledInIni() {
    wchar_t dataDirW[MAX_PATH] = {};
    wchar_t iniPathW[MAX_PATH] = {};
    GetAppDataAndIniPathsW(dataDirW, MAX_PATH, iniPathW, MAX_PATH);
    if (!iniPathW[0] || GetFileAttributesW(iniPathW) == INVALID_FILE_ATTRIBUTES) return false;
    return GetPrivateProfileIntW(L"Shaders", L"Compiled", 0, iniPathW) != 0;
}

void SetShaderCacheCompiledInIni(bool compiled) {
    wchar_t dataDirW[MAX_PATH] = {};
    wchar_t iniPathW[MAX_PATH] = {};
    GetAppDataAndIniPathsW(dataDirW, MAX_PATH, iniPathW, MAX_PATH);
    if (iniPathW[0]) {
        CreateDirectoryW(dataDirW, nullptr);
        WritePrivateProfileStringW(L"Shaders", L"Compiled", compiled ? L"1" : L"0", iniPathW);
    }
}

void SaveKeyBindingsToIniW(const wchar_t* iniPathW) {
    if (!iniPathW || !iniPathW[0]) return;
    g_editKeyBindings = g_keyBindings;
    for (size_t i = 0; i < kNumKeyBindSlots; ++i) {
        wchar_t numBuf[32];
        swprintf(numBuf, 32, L"%d", *g_keySlots[i].pVk);
        WritePrivateProfileStringW(L"KeyBindings", g_keySlots[i].iniKey, numBuf, iniPathW);
    }
}

void PersistAndInitUserDataDir(const wchar_t* gameDirW) {
    if (!IsValidGameDirW(gameDirW)) return;
    WideCharToMultiByte(CP_ACP, 0, gameDirW, -1, g_gameRootDir, MAX_PATH, nullptr, nullptr);

    wchar_t userDataDirW[MAX_PATH] = {};
    wchar_t iniPathW[MAX_PATH] = {};
    GetAppDataAndIniPathsW(userDataDirW, MAX_PATH, iniPathW, MAX_PATH);
    CreateDirectoryW(userDataDirW, nullptr);
    WideCharToMultiByte(CP_ACP, 0, userDataDirW, -1, g_appRootDir, sizeof(g_appRootDir), nullptr, nullptr);

    wchar_t prevGameDirW[MAX_PATH] = {};
    GetPrivateProfileStringW(L"Paths", L"GameDir", L"", prevGameDirW, MAX_PATH, iniPathW);
    TrimTrailingSlashesW(prevGameDirW);
    int shadersCompiled = (int)GetPrivateProfileIntW(L"Shaders", L"Compiled", 0, iniPathW);
    if (prevGameDirW[0] != L'\0' && _wcsicmp(prevGameDirW, gameDirW) != 0) {
        shadersCompiled = 0;
    }

    wchar_t numBuf[32];
    swprintf(numBuf, 32, L"%d", g_selectedAdapterIndex);

    WritePrivateProfileStringW(L"Paths", L"GameDir", gameDirW, iniPathW);
    WritePrivateProfileStringW(L"Graphics", L"AdapterIndex", numBuf, iniPathW);
    WritePrivateProfileStringW(L"Graphics", L"AdapterName", g_selectedAdapterName, iniPathW);
    WritePrivateProfileStringW(L"OSD", L"ShowFps", g_osdShowFps ? L"1" : L"0", iniPathW);
    WritePrivateProfileStringW(L"Shaders", L"Compiled", shadersCompiled ? L"1" : L"0", iniPathW);
    WritePrivateProfileStringW(L"Compatibility", L"EnableHavokDebugServer", g_enableHavokDebugServer ? L"1" : L"0", iniPathW);
    SaveKeyBindingsToIniW(iniPathW);
}
