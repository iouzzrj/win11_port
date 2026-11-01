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

// Global handles and paths
extern HWND g_hWnd;
extern HINSTANCE g_hInstance;
extern char g_appRootDir[MAX_PATH];
extern char g_gameRootDir[MAX_PATH];
extern volatile bool g_appRunning;
extern volatile uint32_t g_keTickCount;
extern volatile uint32_t g_swapFrameCount;

// Pre-launch Win32 UI configuration & In-Game OSD state
extern int g_selectedAdapterIndex;
extern wchar_t g_selectedAdapterName[128];
extern volatile bool g_osdShowFps;
extern volatile float g_osdCurrentFps;

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

struct KeyBindSlotMeta {
    const wchar_t* iniKey;
    const wchar_t* shortLabel; // Sub-label inside box (or empty if single-slot box)
    const wchar_t* actionDesc; // Full description for status bar
    int*           pVk;        // Pointer into g_editKeyBindings
    RECT           rcSlot;     // Clickable key box rect on the 900x608 dialog canvas
};

extern GhostKeyBindings g_keyBindings;
extern GhostKeyBindings g_editKeyBindings;
extern KeyBindSlotMeta  g_keySlots[23];
static constexpr size_t kNumKeyBindSlots = 23;

void ResetKeyBindingsToDefault(GhostKeyBindings* kb);
bool ShowLauncherDialog(HINSTANCE hInst);

// Config & Path Helpers (config.cpp)
void TrimTrailingSlashesW(wchar_t* path);
void JoinPathW(wchar_t* outBuf, size_t maxCount, const wchar_t* baseDir, const wchar_t* subPath);
void GetExeDirW(wchar_t* outDirW, size_t maxCount);
void GetAppDataAndIniPathsW(wchar_t* outDataDirW, size_t dataDirMax, wchar_t* outIniPathW, size_t iniPathMax);
bool IsValidGameDirW(const wchar_t* dirW);
void FormatVkNameW(int vk, wchar_t* outBuf, size_t maxCount);
void LoadKeyBindingsFromIniW(const wchar_t* iniPathW);
void SaveKeyBindingsToIniW(const wchar_t* iniPathW);
void PersistAndInitUserDataDir(const wchar_t* gameDirW);

// Hooking / Bridging API (patch.cpp)
// Overwrites the first 5 bytes of an Xbox function at `xboxVA` with `JMP rel32` to `hostFn`.
void InstallBridge(uint32_t xboxVA, const void* hostFn);

// Writes raw bytes to an Xbox virtual address
void PatchBytes(uint32_t xboxVA, const void* data, size_t len);
void PatchU32(uint32_t xboxVA, uint32_t val);
void PatchRet(uint32_t xboxVA, uint16_t stackBytes = 0, uint32_t retVal = 0, bool setEax = false);

// Path translation helper: translates Xbox paths ("d:\...", "u:\...", "t:\...", "z:\...")
// and remaps Xbox-specific .xvu vertex shader paths to standard vs_1_1 .xvu files.
void EnsureDirExists(const char* dirPath);
void TranslateXboxPath(const char* xboxPath, char* outWinPath, size_t outMaxLen);
void RegisterPeSection(const char* name8, uint32_t va, uint32_t vsize);

// Shared Xbox Kernel / XAPI Internal Helpers (xbox_kernel.cpp / xbox_xapi.cpp)
void* AllocAlignedTracked(uint32_t size, uint32_t alignment);
void FreeAlignedTracked(void* ptr);
uint32_t SizeAlignedTracked(void* ptr);
void __stdcall Krnl_XcSHAUpdate(uint8_t* pbSHAContext, const uint8_t* pbInput, uint32_t dwInputLength);
void __stdcall Krnl_DbgBreakPoint();
int32_t __stdcall Krnl_ExQueryNonVolatileSetting(
    uint32_t ValueIndex, uint32_t* Type, void* Value, uint32_t ValueLength, uint32_t* ResultLength);
BOOL __stdcall Bridge_CloseHandle(HANDLE hObject);
void InitXboxKernel();
void InitXboxXapi();

// Shared Xbox Audio Structures & Helpers (audio_adpcm.cpp / audio_buffer.cpp / audio_stream.cpp)
#define XBOX_WAVE_FORMAT_XBOX_ADPCM 0x0069

static constexpr LONG  kMinDirectSoundVolumeMb = -10000;
static constexpr LONG  kMaxDirectSoundVolumeMb = 0;
static constexpr LONG  kMinDirectSoundPanMb    = -10000;
static constexpr LONG  kMaxDirectSoundPanMb    = 10000;
static constexpr DWORD kMinDirectSoundFreqHz   = 100u;
static constexpr DWORD kMaxDirectSoundFreqHz   = 100000u;
static constexpr DWORD kMaxXboxPitchFreqHz     = 192000u;
static constexpr DWORD kDefaultSampleRateHz    = 48000u;

#pragma pack(push, 1)
struct XBOX_DSMIXBINVOLUMEPAIR {
    uint32_t dwMixBin;
    int32_t  lVolume;
};

struct XBOX_DSMIXBINS {
    uint32_t                       dwMixBinCount;
    const XBOX_DSMIXBINVOLUMEPAIR* lpMixBinVolumePairs;
};

struct XBOX_DSFILTERDESC {
    uint32_t dwMode;              // +0x00: 0 = BYPASS, 1 = DLS2, 2 = PARAMEQ, 3 = MULTI
    uint32_t dwQCoefficient;      // +0x04
    uint32_t adwCoefficients[4];  // +0x08..+0x17
};
static_assert(sizeof(XBOX_DSFILTERDESC) == 0x18, "XBOX_DSFILTERDESC must be 0x18 bytes");

struct XBOX_DSVOICEPROPS {
    uint32_t                dwMixBinCount;        // +0x00
    XBOX_DSMIXBINVOLUMEPAIR MixBinVolumePairs[8]; // +0x04 (64 bytes)
    int32_t                 lPitch;               // +0x44
    int32_t                 l3DDistanceVolume;    // +0x48
    int32_t                 l3DConeVolume;        // +0x4C
    int32_t                 l3DDopplerPitch;      // +0x50
    int32_t                 lI3DL2DirectVolume;   // +0x54
    int32_t                 lI3DL2RoomVolume;     // +0x58
};
static_assert(sizeof(XBOX_DSVOICEPROPS) == 0x5C, "XBOX_DSVOICEPROPS must be 0x5C bytes");
#pragma pack(pop)

struct Xbox3DVoiceMetrics {
    LONG    pan;
    LONG    totalAttenMb;
    int32_t distanceVol;
    int32_t coneVol;
    int32_t dopplerPitch;
    int32_t i3dl2DirectVol;
    int32_t i3dl2RoomVol;
    bool    active3D;
};

struct XboxDSObject {
    void** vtable;
    LONG   refCount;
};

extern CRITICAL_SECTION g_audioCs;
extern bool             g_audioCsInit;

DWORD XboxPitchToFrequencyHz(LONG lPitch);
LONG  FrequencyHzToXboxPitch(DWORD freqHz);
void  Compute3DAudioPanAndAtten(const void* pVoiceData, LONG* outPan, LONG* outAttenDb, Xbox3DVoiceMetrics* outMetrics = nullptr);
void  ComputeMixBinsPan(const XBOX_DSMIXBINS* pMixBins, LONG* outPan);
void  InitVoiceDefaultMixBins(uint32_t dwFlags, XBOX_DSMIXBINVOLUMEPAIR outPairs[8], uint32_t* outCount);
void  AssignVoiceMixBins(const XBOX_DSMIXBINS* pMixBins, XBOX_DSMIXBINVOLUMEPAIR outPairs[8], uint32_t* outCount);
void  UpdateVoiceMixBinVolumes(const XBOX_DSMIXBINS* pMixBins, XBOX_DSMIXBINVOLUMEPAIR ioPairs[8], uint32_t* ioCount);
void  ComputeMixBinPairsPan(const XBOX_DSMIXBINVOLUMEPAIR pairs[8], uint32_t count, LONG* outPan);
void  FillVoicePropsMixBins(const XBOX_DSMIXBINVOLUMEPAIR pairs[8], uint32_t count, XBOX_DSVOICEPROPS* outProps);
bool  ComputeDls2FilterCoeffs(const XBOX_DSFILTERDESC* pFilter, uint32_t sampleRateHz, float* outF, float* outD);
void  ApplyDls2FilterToPcm16(int16_t* pcm, size_t numFrames, uint32_t channels, float fCoeff, float dCoeff, float ioLow[2], float ioBand[2]);
void  AdvanceActiveAudioBuffersSubLoop();
void  InitXboxAudioBuffers();

// Subsystem initialization functions
bool InitD3D11HostDevice();
void InitXboxKernelAndXapi();
void InitXboxInput();
void InitXboxD3D8();
void InitXboxAudio();
void InitXboxVideo();

// Per-frame tick updates
void UpdateXboxKernelTick();
void UpdateXboxInputTick();
void UpdateXboxAudioTick();
void PushDebugKeystroke(uint16_t vk, wchar_t ch, bool keyUp);
