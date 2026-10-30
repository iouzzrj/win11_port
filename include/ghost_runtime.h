#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <malloc.h>

#include "ghost_symbols.h"

// Global handles and paths
extern HWND g_hWnd;
extern HINSTANCE g_hInstance;
extern char g_appRootDir[MAX_PATH];
extern char g_gameRootDir[MAX_PATH];
extern volatile bool g_appRunning;
extern volatile uint32_t g_keTickCount;
extern FILETIME g_keSystemTime;
extern volatile uint32_t g_rendered3DFrames;
extern volatile uint32_t g_swapFrameCount;

// Pre-launch Win32 UI configuration
extern int g_selectedAdapterIndex;
extern wchar_t g_selectedAdapterName[128];

struct GhostKeyBindings {
    int vkCrouch;      // L-Trigger (bAnalogButtons[6])
    int vkFire;        // R-Trigger (bAnalogButtons[7])
    int vkMoveUp;      // Left Stick Up (+LY)
    int vkMoveDown;    // Left Stick Down (-LY)
    int vkMoveLeft;    // Left Stick Left (-LX)
    int vkMoveRight;   // Left Stick Right (+LX)
    int vkSight;       // D-Pad Up
    int vkMenuStart;   // Start button
    int vkMenuBack;    // Back button
    int vkCloak;       // D-Pad Left
    int vkGunLight;    // D-Pad Down (Gun Light / Psi Charge)
    int vkDpadRight;   // D-Pad Right (--)
    int vkAimUp;       // Right Stick Up (+RY)
    int vkAimDown;     // Right Stick Down (-RY)
    int vkAimLeft;     // Right Stick Left (-RX)
    int vkAimRight;    // Right Stick Right (+RX)
    int vkAimClick;    // Right Stick Click (Sniper zoom)
    int vkMelee;       // X button (Melee Attack)
    int vkWeaponMode;  // Y button (Weapon Mode)
    int vkAction;      // B button (Action)
    int vkJump;        // A button (Jump)
    int vkNextFirearm; // Black button (Next Firearm)
    int vkNextGrenade; // White button (Next Grenade)
};

extern GhostKeyBindings g_keyBindings;
extern bool g_autoBotEnabled;
extern bool g_cliAutoBotLaunch;
extern bool g_cliAutoBotTrain;
void ResetKeyBindingsToDefault(GhostKeyBindings* kb);
bool ShowLauncherDialog(HINSTANCE hInst);

// Generic D3D11 backbuffer pixel readback (640x480 BGRA8 top-down)
bool ReadBackbufferBgra(void* outBgraBuf, uint32_t width, uint32_t height, uint32_t dstRowPitch);

// Optional AI Auto-Play Bot hooks (implemented in xbox_autobot.cpp)
void UpdateAutoBotTick();
bool ApplyAutoBotInput(void* pXboxInputState);
const char* GetAutoBotStatusText();

// Logging API
void LogInit(const char* logFilePath);
void LogClose();
void LogPrintf(const char* fmt, ...);

// Hooking / Bridging API
// Overwrites the first 5 bytes of an Xbox function at `xboxVA` with `JMP rel32` to `hostFn`.
void InstallBridge(uint32_t xboxVA, const void* hostFn, const char* name = nullptr);

// Writes raw bytes to an Xbox virtual address
void PatchBytes(uint32_t xboxVA, const void* data, size_t len);
void PatchU32(uint32_t xboxVA, uint32_t val);
void PatchRet(uint32_t xboxVA, uint16_t stackBytes = 0, uint32_t retVal = 0, bool setEax = false);

// Path translation helper: translates Xbox paths ("d:\...", "u:\...", "t:\...", "z:\...")
// and remaps Xbox-specific .xvu vertex shader paths to standard vs_1_1 .xvu files.
void TranslateXboxPath(const char* xboxPath, char* outWinPath, size_t outMaxLen);
void RegisterPeSection(const char* name8, uint32_t va, uint32_t vsize);

// Subsystem initialization functions
bool InitD3D11HostDevice();
void InitXboxKernelAndXapi();
void InitXboxInput();
void InitXboxD3D8();
void InitXboxAudio();

// Per-frame tick updates
void UpdateXboxKernelTick();
void UpdateXboxInputTick();
void UpdateXboxAudioTick();
void PushDebugKeystroke(uint16_t vk, wchar_t ch, bool keyUp);
