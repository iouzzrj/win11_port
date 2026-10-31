#include "ghost_runtime.h"

// ============================================================================
// Xbox Input Structures (Exact 22-byte XINPUT_STATE layout)
// ============================================================================

#pragma pack(push, 1)
struct XBOX_XINPUT_GAMEPAD {
    uint16_t wButtons;          // +0x00
    uint8_t  bAnalogButtons[8]; // +0x02 (0=A, 1=B, 2=X, 3=Y, 4=Black, 5=White, 6=LTrig, 7=RTrig)
    int16_t  sThumbLX;          // +0x0A
    int16_t  sThumbLY;          // +0x0C
    int16_t  sThumbRX;          // +0x0E
    int16_t  sThumbRY;          // +0x10
};

struct XBOX_XINPUT_STATE {
    uint32_t            dwPacketNumber; // +0x00
    XBOX_XINPUT_GAMEPAD Gamepad;        // +0x04 (18 bytes)
};

struct XBOX_XINPUT_KEYSTROKE {
    uint8_t VirtualKey; // +0x00
    char    Ascii;      // +0x01
    uint8_t Flags;      // +0x02 (0x01=CTRL, 0x02=SHIFT, 0x04=ALT, 0x40=KEYUP, 0x80=REPEAT)
};
static_assert(sizeof(XBOX_XINPUT_KEYSTROKE) == 3, "XBOX_XINPUT_KEYSTROKE must be 3 bytes");
#pragma pack(pop)

// Xbox wButtons bitmasks (identical to Windows XInput for bits 0..7)
#define XBOX_GAMEPAD_DPAD_UP        0x0001
#define XBOX_GAMEPAD_DPAD_DOWN      0x0002
#define XBOX_GAMEPAD_DPAD_LEFT      0x0004
#define XBOX_GAMEPAD_DPAD_RIGHT     0x0008
#define XBOX_GAMEPAD_START          0x0010
#define XBOX_GAMEPAD_BACK           0x0020
#define XBOX_GAMEPAD_LEFT_THUMB     0x0040
#define XBOX_GAMEPAD_RIGHT_THUMB    0x0080

// Windows XInput dynamic loader
#pragma pack(push, 1)
struct WIN_XINPUT_GAMEPAD {
    WORD  wButtons;
    BYTE  bLeftTrigger;
    BYTE  bRightTrigger;
    SHORT sThumbLX;
    SHORT sThumbLY;
    SHORT sThumbRX;
    SHORT sThumbRY;
};
struct WIN_XINPUT_STATE {
    DWORD              dwPacketNumber;
    WIN_XINPUT_GAMEPAD Gamepad;
};
#pragma pack(pop)

typedef DWORD (WINAPI *PFN_XInputGetState)(DWORD dwUserIndex, WIN_XINPUT_STATE* pState);
static PFN_XInputGetState g_pfnXInputGetState = nullptr;

static uint32_t g_packetCounter = 1;
static bool     g_padReportedInsert[4] = { false, false, false, false };

// Debug Keyboard Queue
static XBOX_XINPUT_KEYSTROKE g_keyQueue[64];
static volatile int g_keyHead = 0;
static volatile int g_keyTail = 0;

void PushDebugKeystroke(uint16_t vk, wchar_t ch, bool keyUp) {
    int next = (g_keyHead + 1) & 63;
    if (next == g_keyTail) return;
    if (vk == VK_SHIFT)   vk = VK_LSHIFT;
    if (vk == VK_CONTROL) vk = VK_LCONTROL;
    if (vk == VK_MENU)    vk = VK_LMENU;

    uint8_t flags = keyUp ? 0x40u : 0x00u; // 0x40 = XINPUT_DEBUG_KEYSTROKE_FLAG_KEYUP
    if (GetAsyncKeyState(VK_CONTROL) & 0x8000) flags |= 0x01u;
    if (GetAsyncKeyState(VK_SHIFT)   & 0x8000) flags |= 0x02u;
    if (GetAsyncKeyState(VK_MENU)    & 0x8000) flags |= 0x04u;

    g_keyQueue[g_keyHead].VirtualKey = static_cast<uint8_t>(vk & 0xFFu);
    g_keyQueue[g_keyHead].Ascii      = (ch >= 1 && ch <= 127) ? static_cast<char>(ch) : 0;
    g_keyQueue[g_keyHead].Flags      = flags;
    g_keyHead = next;
}

void UpdateXboxInputTick() {
    g_packetCounter++;
}

static void __stdcall Bridge_XInitDevices(DWORD dwPreallocTypeCount, void* PreallocTypes) {
    (void)dwPreallocTypeCount; (void)PreallocTypes;
    *reinterpret_cast<uint32_t*>(0x00747C38) = 1; // _XPP_XInitDevicesHasBeenCalled
}

static DWORD __stdcall Bridge_XGetDevices(void* pDeviceType) {
    // Compare against _XDEVICE_TYPE_GAMEPAD_TABLE (0x00747B8C)
    if (reinterpret_cast<uintptr_t>(pDeviceType) == 0x00747B8C) {
        return 0x00000001; // Port 0 connected
    }
    return 0; // No Memory Units or IR remotes needed (HDD U:\ used for saves)
}

static BOOL __stdcall Bridge_XGetDeviceChanges(void* pDeviceType, DWORD* pdwInsertions, DWORD* pdwRemovals) {
    if (pdwInsertions) *pdwInsertions = 0;
    if (pdwRemovals)   *pdwRemovals   = 0;

    if (reinterpret_cast<uintptr_t>(pDeviceType) == 0x00747B8C) {
        if (!g_padReportedInsert[0]) {
            g_padReportedInsert[0] = true;
            if (pdwInsertions) *pdwInsertions = 0x00000001;
            return TRUE;
        }
    }
    return FALSE;
}

static HANDLE __stdcall Bridge_XInputOpen(
    void* pDeviceType, DWORD dwPort, DWORD dwSlot, void* pPollingParameters)
{
    (void)pDeviceType; (void)dwSlot; (void)pPollingParameters;
    if (dwPort == 0) {
        return reinterpret_cast<HANDLE>(0x1001 + dwPort);
    }
    return nullptr;
}

static void __stdcall Bridge_XInputClose(HANDLE hDevice) {
    (void)hDevice;
}

GhostKeyBindings g_keyBindings = {
    'C',        // vkCrouch      (L-Trigger)
    VK_CONTROL, // vkFire        (R-Trigger)
    'W',        // vkMoveUp      (Left Stick Up)
    'S',        // vkMoveDown    (Left Stick Down)
    'A',        // vkMoveLeft    (Left Stick Left)
    'D',        // vkMoveRight   (Left Stick Right)
    VK_UP,      // vkSight       (D-Pad Up)
    VK_RETURN,  // vkMenuStart   (Start)
    VK_TAB,     // vkMenuBack    (Back)
    VK_LEFT,    // vkCloak       (D-Pad Left)
    VK_DOWN,    // vkGunLight    (D-Pad Down: Gun Light / Psi Charge)
    VK_RIGHT,   // vkDpadRight   (D-Pad Right: --)
    'I',        // vkAimUp       (Right Stick Up)
    'K',        // vkAimDown     (Right Stick Down)
    'J',        // vkAimLeft     (Right Stick Left)
    'L',        // vkAimRight    (Right Stick Right)
    'Z',        // vkAimClick    (Right Stick Click: Sniper)
    'R',        // vkMelee       (X: Melee Attack)
    'F',        // vkWeaponMode  (Y: Weapon Mode)
    'E',        // vkAction      (B: Action)
    VK_SPACE,   // vkJump        (A: Jump)
    'Q',        // vkNextFirearm (Black: Next Firearm)
    'G',        // vkNextGrenade (White: Next Grenade)
};

void ResetKeyBindingsToDefault(GhostKeyBindings* kb) {
    if (!kb) return;
    kb->vkCrouch      = 'C';
    kb->vkFire        = VK_CONTROL;
    kb->vkMoveUp      = 'W';
    kb->vkMoveDown    = 'S';
    kb->vkMoveLeft    = 'A';
    kb->vkMoveRight   = 'D';
    kb->vkSight       = VK_UP;
    kb->vkMenuStart   = VK_RETURN;
    kb->vkMenuBack    = VK_TAB;
    kb->vkCloak       = VK_LEFT;
    kb->vkGunLight    = VK_DOWN;
    kb->vkDpadRight   = VK_RIGHT;
    kb->vkAimUp       = 'I';
    kb->vkAimDown     = 'K';
    kb->vkAimLeft     = 'J';
    kb->vkAimRight    = 'L';
    kb->vkAimClick    = 'Z';
    kb->vkMelee       = 'R';
    kb->vkWeaponMode  = 'F';
    kb->vkAction      = 'E';
    kb->vkJump        = VK_SPACE;
    kb->vkNextFirearm = 'Q';
    kb->vkNextGrenade = 'G';
}

static bool IsKeyDown(int vk) {
    if (vk <= 0 || GetForegroundWindow() != g_hWnd) return false;
    return (GetAsyncKeyState(vk) & 0x8000) != 0;
}

static DWORD __stdcall Bridge_XInputGetState(HANDLE hDevice, XBOX_XINPUT_STATE* pState) {
    if (!pState) return ERROR_INVALID_PARAMETER;
    uintptr_t h = reinterpret_cast<uintptr_t>(hDevice);
    DWORD port = (h >= 0x1001 && h <= 0x1004) ? (DWORD)(h - 0x1001) : 0;
    if (port != 0) return ERROR_DEVICE_NOT_CONNECTED;

    memset(pState, 0, sizeof(*pState));
    pState->dwPacketNumber = g_packetCounter;

    // 1. Poll physical XInput controller if present
    if (g_pfnXInputGetState) {
        WIN_XINPUT_STATE xst;
        if (g_pfnXInputGetState(0, &xst) == ERROR_SUCCESS) {
            pState->Gamepad.wButtons = xst.Gamepad.wButtons & 0x00FF;
            if (xst.Gamepad.wButtons & 0x1000) pState->Gamepad.bAnalogButtons[0] = 255; // A
            if (xst.Gamepad.wButtons & 0x2000) pState->Gamepad.bAnalogButtons[1] = 255; // B
            if (xst.Gamepad.wButtons & 0x4000) pState->Gamepad.bAnalogButtons[2] = 255; // X
            if (xst.Gamepad.wButtons & 0x8000) pState->Gamepad.bAnalogButtons[3] = 255; // Y
            if (xst.Gamepad.wButtons & 0x0200) pState->Gamepad.bAnalogButtons[4] = 255; // RB -> Black
            if (xst.Gamepad.wButtons & 0x0100) pState->Gamepad.bAnalogButtons[5] = 255; // LB -> White
            pState->Gamepad.bAnalogButtons[6] = xst.Gamepad.bLeftTrigger;
            pState->Gamepad.bAnalogButtons[7] = xst.Gamepad.bRightTrigger;
            pState->Gamepad.sThumbLX = xst.Gamepad.sThumbLX;
            pState->Gamepad.sThumbLY = xst.Gamepad.sThumbLY;
            pState->Gamepad.sThumbRX = xst.Gamepad.sThumbRX;
            pState->Gamepad.sThumbRY = xst.Gamepad.sThumbRY;
        }
    }

    // 2. Overlay user-configured Keyboard & Mouse controls when window is focused
    if (GetForegroundWindow() == g_hWnd) {
        // D-Pad: Sight (Up), Gun Light / Psi Charge (Down), Cloak (Left), -- (Right)
        if (IsKeyDown(g_keyBindings.vkSight))     pState->Gamepad.wButtons |= XBOX_GAMEPAD_DPAD_UP;
        if (IsKeyDown(g_keyBindings.vkGunLight))  pState->Gamepad.wButtons |= XBOX_GAMEPAD_DPAD_DOWN;
        if (IsKeyDown(g_keyBindings.vkCloak))     pState->Gamepad.wButtons |= XBOX_GAMEPAD_DPAD_LEFT;
        if (IsKeyDown(g_keyBindings.vkDpadRight)) pState->Gamepad.wButtons |= XBOX_GAMEPAD_DPAD_RIGHT;

        // Menu (Start & Back)
        if (IsKeyDown(g_keyBindings.vkMenuStart)) pState->Gamepad.wButtons |= XBOX_GAMEPAD_START;
        if (IsKeyDown(g_keyBindings.vkMenuBack) || IsKeyDown(VK_ESCAPE)) pState->Gamepad.wButtons |= XBOX_GAMEPAD_BACK;

        // Right Stick Click: Sniper (click)
        if (IsKeyDown(g_keyBindings.vkAimClick))  pState->Gamepad.wButtons |= XBOX_GAMEPAD_RIGHT_THUMB;

        // Analog buttons (0=A Jump, 1=B Action, 2=X Melee, 3=Y Weapon Mode, 4=Black Next Firearm, 5=White Next Grenade, 6=LTrig Crouch, 7=RTrig Fire)
        if (IsKeyDown(g_keyBindings.vkJump))        pState->Gamepad.bAnalogButtons[0] = 255;
        if (IsKeyDown(g_keyBindings.vkAction))      pState->Gamepad.bAnalogButtons[1] = 255;
        if (IsKeyDown(g_keyBindings.vkMelee))       pState->Gamepad.bAnalogButtons[2] = 255;
        if (IsKeyDown(g_keyBindings.vkWeaponMode))  pState->Gamepad.bAnalogButtons[3] = 255;
        if (IsKeyDown(g_keyBindings.vkNextFirearm)) pState->Gamepad.bAnalogButtons[4] = 255;
        if (IsKeyDown(g_keyBindings.vkNextGrenade)) pState->Gamepad.bAnalogButtons[5] = 255;
        if (IsKeyDown(g_keyBindings.vkCrouch))      pState->Gamepad.bAnalogButtons[6] = 255;
        if (IsKeyDown(g_keyBindings.vkFire))        pState->Gamepad.bAnalogButtons[7] = 255;

        // Left Stick: Move
        int lx = 0, ly = 0;
        if (IsKeyDown(g_keyBindings.vkMoveLeft))  lx -= 32767;
        if (IsKeyDown(g_keyBindings.vkMoveRight)) lx += 32767;
        if (IsKeyDown(g_keyBindings.vkMoveDown))  ly -= 32767;
        if (IsKeyDown(g_keyBindings.vkMoveUp))    ly += 32767;
        if (lx != 0) pState->Gamepad.sThumbLX = (int16_t)lx;
        if (ly != 0) pState->Gamepad.sThumbLY = (int16_t)ly;

        // Right Stick: Aim
        int rx = 0, ry = 0;
        if (IsKeyDown(g_keyBindings.vkAimLeft))  rx -= 28000;
        if (IsKeyDown(g_keyBindings.vkAimRight)) rx += 28000;
        if (IsKeyDown(g_keyBindings.vkAimDown))  ry -= 28000;
        if (IsKeyDown(g_keyBindings.vkAimUp))    ry += 28000;
        if (rx != 0) pState->Gamepad.sThumbRX = (int16_t)rx;
        if (ry != 0) pState->Gamepad.sThumbRY = (int16_t)ry;
    }

    return ERROR_SUCCESS;
}

static DWORD __stdcall Bridge_XInputSetState(HANDLE hDevice, void* pFeedback) {
    (void)hDevice; (void)pFeedback;
    return ERROR_SUCCESS;
}

static DWORD __stdcall Bridge_XInputDebugInitKeyboardQueue(void* pParameters) {
    (void)pParameters;
    return ERROR_SUCCESS;
}

static DWORD __stdcall Bridge_XInputDebugGetKeystroke(XBOX_XINPUT_KEYSTROKE* pKeystroke) {
    if (!pKeystroke) return ERROR_INVALID_PARAMETER;
    if (g_keyTail == g_keyHead) {
        return ERROR_EMPTY;
    }
    *pKeystroke = g_keyQueue[g_keyTail];
    g_keyTail = (g_keyTail + 1) & 63;
    return ERROR_SUCCESS;
}

static DWORD __stdcall Bridge_XMountMUA(DWORD dwPort, DWORD dwSlot, char* pchDrive) {
    (void)dwPort; (void)dwSlot;
    if (pchDrive) *pchDrive = 'F';
    return ERROR_DEVICE_NOT_CONNECTED;
}

static DWORD __stdcall Bridge_XUnmountMU(DWORD dwPort, DWORD dwSlot) {
    (void)dwPort; (void)dwSlot;
    return ERROR_SUCCESS;
}

void InitXboxInput() {
    HMODULE hXInput = LoadLibraryA("xinput1_4.dll");
    if (!hXInput) hXInput = LoadLibraryA("xinput9_1_0.dll");
    if (!hXInput) hXInput = LoadLibraryA("xinput1_3.dll");
    if (hXInput) {
        g_pfnXInputGetState = reinterpret_cast<PFN_XInputGetState>(GetProcAddress(hXInput, "XInputGetState"));
    }

    // Bridge XInput & XPP functions
    InstallBridge(0x007491A9, &Bridge_XInitDevices,                "_XInitDevices@8");
    InstallBridge(0x007491AE, &Bridge_XGetDevices,                 "_XGetDevices@4");
    InstallBridge(0x007491D0, &Bridge_XGetDeviceChanges,           "_XGetDeviceChanges@12");
    InstallBridge(0x0074907F, &Bridge_XInputOpen,                  "_XInputOpen@16");
    InstallBridge(0x007490D5, &Bridge_XInputClose,                 "_XInputClose@4");
    InstallBridge(0x007490E1, &Bridge_XInputGetState,              "_XInputGetState@8");
    InstallBridge(0x00749154, &Bridge_XInputSetState,              "_XInputSetState@8");
    InstallBridge(0x0074923D, &Bridge_XMountMUA,                   "_XMountMUA@12");
    InstallBridge(0x0074936F, &Bridge_XUnmountMU,                  "_XUnmountMU@8");
    InstallBridge(0x0074FB64, &Bridge_XInputDebugInitKeyboardQueue,"_XInputDebugInitKeyboardQueue@4");
    InstallBridge(0x0074F8A3, &Bridge_XInputDebugGetKeystroke,     "_XInputDebugGetKeystroke@4");

    // Stub cXBoxMemoryUnits::Startup (0x005AC2F0) to skip the 1.5s MU enumeration sleep
    PatchRet(0x005AC2F0, 0, 0, false);
}
