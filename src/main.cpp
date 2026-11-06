#include "ghost_runtime.h"
#include "ghost_addresses.h"
#include <vector>

#pragma comment(linker, "\"/manifestdependency:type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='x86' publicKeyToken='6595b64144ccf1df' language='*'\"")

HWND          g_hWnd        = nullptr;
HINSTANCE     g_hInstance   = nullptr;
char          g_appRootDir[MAX_PATH]  = { 0 };
char          g_gameRootDir[MAX_PATH] = { 0 };
volatile bool g_appRunning  = true;

int           g_selectedAdapterIndex = 0;
wchar_t       g_selectedAdapterName[128] = { 0 };

// ============================================================================
// Vectored Exception Handler (Ring-0 / SSE Emulation & Crash Diagnostics)
// ============================================================================

static LONG WINAPI XboxCrashHandler(EXCEPTION_POINTERS* ep) {
    if (!ep || !ep->ExceptionRecord || !ep->ContextRecord) return EXCEPTION_CONTINUE_SEARCH;
    DWORD code = ep->ExceptionRecord->ExceptionCode;

    // Only intercept genuine CPU/memory faults; let Windows system DLLs (COM, WinRT, RPC, Shell,
    // OutputDebugString, C++ EH) handle their own internal first-chance software exceptions.
    if (code != EXCEPTION_ACCESS_VIOLATION &&
        code != EXCEPTION_PRIV_INSTRUCTION &&
        code != EXCEPTION_ILLEGAL_INSTRUCTION &&
        code != EXCEPTION_INT_DIVIDE_BY_ZERO &&
        code != EXCEPTION_STACK_OVERFLOW &&
        code != EXCEPTION_IN_PAGE_ERROR)
    {
        return EXCEPTION_CONTINUE_SEARCH;
    }

    CONTEXT* ctx = ep->ContextRecord;
    // Handle ring-0 Xbox privileged instructions (wbinvd = 0F 09, invd = 0F 08, cli = FA, sti = FB)
    if (code == 0xC0000096 && ctx->Eip >= 0x00400000 && ctx->Eip < 0x00950000) {
        uint8_t* ins = reinterpret_cast<uint8_t*>(ctx->Eip);
        if (ins[0] == 0x0F && (ins[1] == 0x09 || ins[1] == 0x08)) {
            ins[0] = 0x90;
            ins[1] = 0x90;
            ctx->Eip += 2;
            return EXCEPTION_CONTINUE_EXECUTION;
        }
        if (ins[0] == 0xFA || ins[0] == 0xFB) {
            ins[0] = 0x90;
            ctx->Eip += 1;
            return EXCEPTION_CONTINUE_EXECUTION;
        }
    }

    // Handle misaligned SSE movaps (0F 28 / 0F 29) by converting in-place to movups (0F 10 / 0F 11)
    if (code == 0xC0000005 && ep->ExceptionRecord->NumberParameters >= 2 &&
        ep->ExceptionRecord->ExceptionInformation[1] == 0xFFFFFFFF &&
        ctx->Eip >= 0x00400000 && ctx->Eip < 0x00950000)
    {
        uint8_t* ins = reinterpret_cast<uint8_t*>(ctx->Eip);
        if (ins[0] == 0x0F && ins[1] == 0x28) {
            ins[1] = 0x10; // movups xmm, m128
            return EXCEPTION_CONTINUE_EXECUTION;
        }
        if (ins[0] == 0x0F && ins[1] == 0x29) {
            ins[1] = 0x11; // movups m128, xmm
            return EXCEPTION_CONTINUE_EXECUTION;
        }
    }

    ExitProcess(1);
    return EXCEPTION_EXECUTE_HANDLER;
}

// ============================================================================
// Win32 Game Window & Message Procedure
// ============================================================================

static LRESULT CALLBACK GhostWndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_SYSCOMMAND:
            if (HandleOsdSysCommand(hWnd, wParam)) {
                return 0;
            }
            break;
        case WM_CLOSE:
            g_appRunning = false;
            SaveOsdStateToIni();
            ExitProcess(0);
            return 0;
        case WM_DESTROY:
            g_appRunning = false;
            SaveOsdStateToIni();
            PostQuitMessage(0);
            return 0;
        case WM_TIMER:
            if (HandleOsdTimer(hWnd, wParam)) {
                return 0;
            }
            break;
        case WM_KEYDOWN:
            PushDebugKeystroke((uint16_t)wParam, 0, false);
            return 0;
        case WM_KEYUP:
            PushDebugKeystroke((uint16_t)wParam, 0, true);
            return 0;
        case WM_CHAR:
            PushDebugKeystroke(0, (wchar_t)wParam, false);
            return 0;
    }
    return DefWindowProcA(hWnd, msg, wParam, lParam);
}

static bool InitWin32Window(HINSTANCE hInst) {
    WNDCLASSEXA wc = {};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW | CS_OWNDC;
    wc.lpfnWndProc = GhostWndProc;
    wc.hInstance = hInst;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    wc.lpszClassName = "StarCraftGhostPCWndClass";
    RegisterClassExA(&wc);

    // Native Xbox 640x480 resolution (fixed size, non-resizable, maximize disabled, freely movable)
    RECT rc = { 0, 0, 640, 480 };
    DWORD style   = (WS_OVERLAPPEDWINDOW & ~(WS_THICKFRAME | WS_MAXIMIZEBOX)) | WS_VISIBLE;
    DWORD exStyle = 0;
    AdjustWindowRectEx(&rc, style, FALSE, exStyle);
    int winW = rc.right - rc.left;
    int winH = rc.bottom - rc.top;
    int winX = (GetSystemMetrics(SM_CXSCREEN) - winW) / 2;
    int winY = (GetSystemMetrics(SM_CYSCREEN) - winH) / 2;
    if (winX < 0) winX = 0;
    if (winY < 0) winY = 0;

    g_hWnd = CreateWindowExA(
        exStyle, wc.lpszClassName,
        "StarCraft: Ghost",
        style, winX, winY, winW, winH,
        nullptr, nullptr, hInst, nullptr);

    if (!g_hWnd) return false;

    InitOsdWindowHooks(g_hWnd);
    SetWindowTextW(g_hWnd, L"StarCraft: Ghost");
    ShowWindow(g_hWnd, SW_SHOWNORMAL);
    UpdateWindow(g_hWnd);
    return true;
}

// ============================================================================
// Ghost.exe PE Section Loader (into reserved 0x00401000..0x00923000)
// ============================================================================

bool LoadGhostXboxPeImage(const char* ghostExePath) {
    static bool s_ghostPeLoaded = false;
    if (s_ghostPeLoaded) {
        return true;
    }
    FILE* f = fopen(ghostExePath, "rb");
    if (!f) {
        return false;
    }
    fseek(f, 0, SEEK_END);
    size_t fileSize = (size_t)ftell(f);
    fseek(f, 0, SEEK_SET);

    std::vector<uint8_t> pe(fileSize);
    if (fread(pe.data(), 1, fileSize, f) != fileSize) {
        fclose(f);
        return false;
    }
    fclose(f);

    // Make the entire reserved Xbox region (0x00400000..0x00950000) RWX
    // Note: Ghost.exe's .text starts at RVA 0x00000740 (0x00400740), which is above GhostWin11.exe's PE headers (0x00400000..0x00400300)
    constexpr uint8_t  kFsPrefixOpcode       = 0x64u;
    constexpr uint8_t  kMovMoffs32EaxOpcode  = 0xA3u;
    constexpr uint8_t  kMovRm32RegOpcode     = 0x89u;
    constexpr uint8_t  kModRmDisp32Ecx       = 0x0Du;
    constexpr uint8_t  kModRmDisp32Ebx       = 0x1Du;
    constexpr uint8_t  kModRmDisp32Esp       = 0x25u;
    constexpr uint8_t  kNopOpcode            = 0x90u;
    constexpr size_t   kMovFs0EaxByteLen     = 6u;
    constexpr size_t   kMovFs0RegByteLen     = 7u;

    DWORD oldProt = 0;
    if (!VirtualProtect(reinterpret_cast<void*>(kXboxPeImageBase), kXboxReservedSize, PAGE_EXECUTE_READWRITE, &oldProt)) {
        return false;
    }

    // Zero the Xbox image space (from 0x00400740 to 0x00923000) before copying sections
    memset(reinterpret_cast<void*>(kXboxCodeScanStartVa), 0, kXboxZeroImageSize);

    const IMAGE_DOS_HEADER* dosHdr = reinterpret_cast<const IMAGE_DOS_HEADER*>(pe.data());
    const IMAGE_NT_HEADERS32* ntHdr = reinterpret_cast<const IMAGE_NT_HEADERS32*>(pe.data() + dosHdr->e_lfanew);
    const IMAGE_SECTION_HEADER* secHdr = IMAGE_FIRST_SECTION(ntHdr);
    uint16_t numSecs = ntHdr->FileHeader.NumberOfSections;

    for (uint16_t i = 0; i < numSecs; ++i) {
        const IMAGE_SECTION_HEADER& s = secHdr[i];
        char nameBuf[9] = { 0 };
        memcpy(nameBuf, s.Name, 8);

        RegisterPeSection(reinterpret_cast<const char*>(s.Name), s.VirtualAddress, s.Misc.VirtualSize);

        // Skip .reloc at 0x00922280 (no relocation needed since we load at exact 0x00400000!)
        if (strcmp(nameBuf, ".reloc") == 0) continue;

        uint32_t dstVA = kXboxPeImageBase + s.VirtualAddress;
        uint32_t copyBytes = s.SizeOfRawData;
        if (s.Misc.VirtualSize > 0 && s.Misc.VirtualSize < copyBytes) {
            // Note: For initialized data sections where SizeOfRawData is padded, copy SizeOfRawData up to next section
            copyBytes = s.SizeOfRawData;
        }
        if (s.PointerToRawData > 0 && copyBytes > 0 && (s.PointerToRawData + copyBytes) <= fileSize) {
            memcpy(reinterpret_cast<void*>(dstVA), pe.data() + s.PointerToRawData, copyBytes);
        }
    }

    // Load Xbox XBE Header + Certificate page into reserved .xbox space at 0x0094F000
    // (0x00010000 is already occupied by the Windows process environment block on Win11 WOW64)
    constexpr uint32_t kXbeHeaderBaseVa = 0x0094F000u;
    constexpr uint32_t kOrigXbeBaseVa   = 0x00010000u;
    void* xbePage = reinterpret_cast<void*>(kXbeHeaderBaseVa);
    memset(xbePage, 0, 0x1000);
    bool loadedXbeHeader = false;
    char ghostXbePath[MAX_PATH];
    snprintf(ghostXbePath, sizeof(ghostXbePath), "%s\\Ghost.xbe", g_gameRootDir);
    FILE* fxbe = fopen(ghostXbePath, "rb");
    if (fxbe) {
        if (fread(xbePage, 1, 0x1000, fxbe) == 0x1000 &&
            *reinterpret_cast<const uint32_t*>(kXbeHeaderBaseVa) == 0x48454258u)
        {
            loadedXbeHeader = true;
            uint32_t origCertVa = *reinterpret_cast<const uint32_t*>(kXbeHeaderBaseVa + 0x118);
            if (origCertVa >= kOrigXbeBaseVa && origCertVa < kOrigXbeBaseVa + 0x1000u) {
                *reinterpret_cast<uint32_t*>(kXbeHeaderBaseVa + 0x118) = kXbeHeaderBaseVa + (origCertVa - kOrigXbeBaseVa);
            }
            *reinterpret_cast<uint32_t*>(kXbeHeaderBaseVa + 0x104) = kXbeHeaderBaseVa;
        }
        fclose(fxbe);
    }
    if (!loadedXbeHeader) {
        memset(xbePage, 0, 0x1000);
        *reinterpret_cast<uint32_t*>(kXbeHeaderBaseVa + 0x000) = 0x48454258u; // 'XBEH'
        *reinterpret_cast<uint32_t*>(kXbeHeaderBaseVa + 0x104) = kXbeHeaderBaseVa; // dwBaseAddr
        *reinterpret_cast<uint32_t*>(kXbeHeaderBaseVa + 0x118) = kXbeHeaderBaseVa + 0x184u; // dwCertificateAddr
        *reinterpret_cast<uint32_t*>(kXbeHeaderBaseVa + 0x184 + 0x00) = 0x000001ECu; // cbSize
        *reinterpret_cast<uint32_t*>(kXbeHeaderBaseVa + 0x184 + 0x08) = 0x00000002u; // dwTitleId (Ghost.xbe)
        wcsncpy(reinterpret_cast<wchar_t*>(kXbeHeaderBaseVa + 0x184 + 0x0C), L"Starcraft: Ghost (Release)", 40);
        *reinterpret_cast<uint32_t*>(kXbeHeaderBaseVa + 0x184 + 0xAC) = 0x00000100u; // dwVersion
        memcpy(reinterpret_cast<void*>(kXbeHeaderBaseVa + 0x184 + 0xB0), "TESTTESTTESTTEST", 16); // bzLANKey
        memcpy(reinterpret_cast<void*>(kXbeHeaderBaseVa + 0x184 + 0xC0), "TESTTESTTESTTEST", 16); // bzSignatureKey
    }

    // Relocate all 27 XeImageHeader() references (ds:[0x00010108..0x00010138]) in XAPILIB/XNET/XPP to kXbeHeaderBaseVa
    static const uint32_t kXbeHeaderRefVas[] = {
        0x0059FD45u, 0x005A0851u, 0x005A09EBu, 0x005A1C51u, 0x005A1ED1u,
        0x005A2617u, 0x005A26FDu, 0x005A2702u, 0x005A2BE1u, 0x005A2D65u,
        0x005A2E40u, 0x005A3B2Au, 0x005A3B91u, 0x005A3C46u, 0x005A3C60u,
        0x005A3E52u, 0x005A3E58u, 0x005A3EDBu, 0x005A3F06u, 0x005A3F29u,
        0x005A46B8u, 0x005A46BEu, 0x0073BA42u, 0x0073D700u, 0x007464DAu,
        0x007492E3u, 0x0074930Bu
    };
    for (uint32_t refVa : kXbeHeaderRefVas) {
        uint32_t origTarget = *reinterpret_cast<const uint32_t*>(refVa);
        if (origTarget >= kOrigXbeBaseVa && origTarget < kOrigXbeBaseVa + 0x1000u) {
            *reinterpret_cast<uint32_t*>(refVa) = kXbeHeaderBaseVa + (origTarget - kOrigXbeBaseVa);
        }
    }

    // Neutralize all writes to fs:[0] (NT_TIB.ExceptionList) across all code sections (0x00400740..0x0074FE80,
    // including .text LIBCMT:trnsctrl.obj mov fs:[0], ebx and BINK yuv.obj ?checkrdtsc@@YAXXZ)
    // so Windows 11 SEHOP/SafeSEH never aborts on Xbox SEH frames and XboxCrashHandler catches everything!
    uint8_t* codeStart = reinterpret_cast<uint8_t*>(kXboxCodeScanStartVa);
    uint8_t* codeEnd   = reinterpret_cast<uint8_t*>(kXboxCodeScanEndVa - kMovFs0RegByteLen);
    for (uint8_t* p = codeStart; p < codeEnd; ++p) {
        if (p[0] == kFsPrefixOpcode && p[1] == kMovMoffs32EaxOpcode &&
            p[2] == 0x00 && p[3] == 0x00 && p[4] == 0x00 && p[5] == 0x00)
        {
            memset(p, kNopOpcode, kMovFs0EaxByteLen); // mov fs:[0], eax -> nop x6
        } else if (p[0] == kFsPrefixOpcode && p[1] == kMovRm32RegOpcode &&
                   (p[2] == kModRmDisp32Ecx || p[2] == kModRmDisp32Ebx || p[2] == kModRmDisp32Esp) &&
                   p[3] == 0x00 && p[4] == 0x00 && p[5] == 0x00 && p[6] == 0x00)
        {
            memset(p, kNopOpcode, kMovFs0RegByteLen); // mov fs:[0], ecx/ebx/esp -> nop x7
        }
    }

    s_ghostPeLoaded = true;
    return true;
}

// ============================================================================
// Dedicated Xbox Game Thread & Win32 UI Entry Point (WinMain)
// ============================================================================

// Per-frame tick wrapper around cSequence::MainLoop (0x0043C250) called from _main at 0x004354B7
static void __fastcall Bridge_MainLoopStep(void* thisPtr, void* edxUnused, uint32_t flags) {
    (void)edxUnused;
    if (!g_appRunning) {
        ExitThread(0);
    }
    UpdateXboxKernelTick();
    UpdateXboxInputTick();
    UpdateXboxAudioTick();
    typedef void (__thiscall *PFN_MainLoop)(void* thisPtr, uint32_t flags);
    reinterpret_cast<PFN_MainLoop>(0x0043C250)(thisPtr, flags);
}

static DWORD WINAPI XboxGameThreadProc(LPVOID) {
    InitD3D11HostDevice();

    // Install all compatibility runtime bridges (and initialize Xbox TLS on this thread)
    InitXboxKernelAndXapi();
    InitXboxInput();
    InitXboxD3D8();
    InitXboxAudio();

    // Hook the per-frame call to cSequence::MainLoop (0x0043C250) inside native _main at 0x004354B7
    {
        uint8_t* callSite = reinterpret_cast<uint8_t*>(0x004354B7);
        uint32_t rel = reinterpret_cast<uintptr_t>(&Bridge_MainLoopStep) - (0x004354B7u + 5u);
        callSite[0] = 0xE8; // CALL rel32
        *reinterpret_cast<uint32_t*>(callSite + 1) = rel;
    }

    // Run native Xbox CRT initializers (__rtinit, _cinit) and native _main (0x00435460)
    __try {
        typedef int (__stdcall *PFN_rtinit)();
        typedef void (__cdecl *PFN_cinit)();
        typedef int (__cdecl *PFN_main)();

        reinterpret_cast<PFN_rtinit>(0x005A430F)(); // __rtinit (.CRT$XRI..XRZ)
        reinterpret_cast<PFN_cinit>(0x005A42B7)();  // _cinit (__FPinit, .CRT$XIA..XIZ, .CRT$XCA..XCZ including mu.obj)
        reinterpret_cast<PFN_main>(0x00435460)();   // _main (0x00434780, XGetLaunchInfo, StartApplication, MainLoop, Shutdown)
    } __except (XboxCrashHandler(GetExceptionInformation())) {
    }

    g_appRunning = false;
    if (g_hWnd) {
        PostMessageA(g_hWnd, WM_CLOSE, 0, 0);
    }
    return 0;
}

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow) {
    (void)hPrevInstance; (void)lpCmdLine; (void)nCmdShow;
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
    g_hInstance = hInstance;

    // Show Native Win32 Pre-Launch Setup UI (and integrated Page 2 Shader Compilation if needed)
    if (!ShowLauncherDialog(hInstance)) {
        return 0;
    }

    SetCurrentDirectoryA(g_gameRootDir);

    char ghostExePath[MAX_PATH];
    snprintf(ghostExePath, sizeof(ghostExePath), "%s\\Ghost.exe", g_gameRootDir);
    if (!LoadGhostXboxPeImage(ghostExePath)) {
        MessageBoxA(nullptr, "Failed to load Ghost.exe from the selected Game Directory!", "Error", MB_ICONERROR);
        return 1;
    }

    // Install Xbox exception handlers only after the Win32 shell/COM launcher UI has closed
    AddVectoredExceptionHandler(1, XboxCrashHandler);
    SetUnhandledExceptionFilter(XboxCrashHandler);

    if (!InitWin32Window(hInstance)) {
        return 1;
    }

    // Spawn dedicated Xbox Game Thread (2 MB stack)
    DWORD gameTid = 0;
    HANDLE hGameThread = CreateThread(nullptr, 2 * 1024 * 1024, XboxGameThreadProc, nullptr, 0, &gameTid);
    (void)hGameThread;

    // Dedicated Win32 UI Message Loop (never blocked by game rendering or file I/O)
    MSG msg;
    while (g_appRunning) {
        while (PeekMessageA(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) {
                g_appRunning = false;
                break;
            }
            TranslateMessage(&msg);
            DispatchMessageA(&msg);
        }
        if (!g_appRunning) break;
        MsgWaitForMultipleObjects(0, nullptr, FALSE, 25, QS_ALLINPUT);
    }

    g_appRunning = false;
    ExitProcess(0);
    return 0;
}
