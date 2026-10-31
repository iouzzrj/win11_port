#include "ghost_runtime.h"
#include "ghost_addresses.h"

// ============================================================================
// XAPILIB Win32 / File / Directory / SaveGame / Section / Heap / Thread Bridges
// ============================================================================

// Track FindFirstFileA handles so Xbox CloseHandle (which is aliased to FindClose) closes them properly
static HANDLE g_findHandles[256] = { 0 };
static CRITICAL_SECTION g_findCs;

static void RegisterFindHandle(HANDLE h) {
    if (h == INVALID_HANDLE_VALUE || !h) return;
    EnterCriticalSection(&g_findCs);
    for (int i = 0; i < 256; ++i) {
        if (!g_findHandles[i]) {
            g_findHandles[i] = h;
            break;
        }
    }
    LeaveCriticalSection(&g_findCs);
}

static bool TryCloseFindHandle(HANDLE h) {
    if (h == INVALID_HANDLE_VALUE || !h) return false;
    bool found = false;
    EnterCriticalSection(&g_findCs);
    for (int i = 0; i < 256; ++i) {
        if (g_findHandles[i] == h) {
            g_findHandles[i] = nullptr;
            found = true;
            break;
        }
    }
    LeaveCriticalSection(&g_findCs);
    if (found) {
        FindClose(h);
        return true;
    }
    return false;
}

static HANDLE __stdcall Bridge_CreateFileA(
    const char* lpFileName, DWORD dwDesiredAccess, DWORD dwShareMode,
    LPSECURITY_ATTRIBUTES lpSecurityAttributes, DWORD dwCreationDisposition,
    DWORD dwFlagsAndAttributes, HANDLE hTemplateFile)
{
    char winPath[MAX_PATH];
    TranslateXboxPath(lpFileName, winPath, sizeof(winPath));

    // Strip FILE_FLAG_NO_BUFFERING and FILE_FLAG_OVERLAPPED so both synchronous and overlapped Xbox reads succeed deterministically
    dwFlagsAndAttributes &= ~(FILE_FLAG_NO_BUFFERING | FILE_FLAG_OVERLAPPED);
    if (dwShareMode == 0) {
        dwShareMode = FILE_SHARE_READ | FILE_SHARE_WRITE;
    }

    HANDLE h = CreateFileA(
        winPath, dwDesiredAccess, dwShareMode, lpSecurityAttributes,
        dwCreationDisposition, dwFlagsAndAttributes, hTemplateFile);

    if (h == INVALID_HANDLE_VALUE) {
        // Also check parent directory creation if writing a file in Saves\...
        if (dwCreationDisposition == CREATE_ALWAYS || dwCreationDisposition == OPEN_ALWAYS || dwCreationDisposition == CREATE_NEW) {
            char dirBuf[MAX_PATH];
            strncpy(dirBuf, winPath, sizeof(dirBuf) - 1);
            dirBuf[sizeof(dirBuf) - 1] = '\0';
            char* lastSlash = strrchr(dirBuf, '\\');
            if (lastSlash) {
                *lastSlash = '\0';
                EnsureDirExists(dirBuf);
                h = CreateFileA(
                    winPath, dwDesiredAccess, dwShareMode, lpSecurityAttributes,
                    dwCreationDisposition, dwFlagsAndAttributes, hTemplateFile);
            }
        }
    }
    return h;
}

BOOL __stdcall Bridge_CloseHandle(HANDLE hObject) {
    if (!hObject || hObject == INVALID_HANDLE_VALUE) return TRUE;
    if (TryCloseFindHandle(hObject)) return TRUE;
    return CloseHandle(hObject);
}

static BOOL __stdcall Bridge_ReadFile(
    HANDLE hFile, LPVOID lpBuffer, DWORD nNumberOfBytesToRead,
    LPDWORD lpNumberOfBytesRead, LPOVERLAPPED lpOverlapped)
{
    if (lpOverlapped) {
        LARGE_INTEGER pos;
        pos.LowPart = lpOverlapped->Offset;
        pos.HighPart = (LONG)lpOverlapped->OffsetHigh;
        SetFilePointerEx(hFile, pos, nullptr, FILE_BEGIN);
        DWORD bytesDone = 0;
        BOOL ok = ReadFile(hFile, lpBuffer, nNumberOfBytesToRead, &bytesDone, nullptr);
        lpOverlapped->Internal = 0; // STATUS_SUCCESS
        lpOverlapped->InternalHigh = bytesDone;
        if (lpNumberOfBytesRead) *lpNumberOfBytesRead = bytesDone;
        if (lpOverlapped->hEvent && ((uintptr_t)lpOverlapped->hEvent & 1u) == 0) {
            SetEvent(lpOverlapped->hEvent);
        }
        return ok;
    }
    return ReadFile(hFile, lpBuffer, nNumberOfBytesToRead, lpNumberOfBytesRead, nullptr);
}

static BOOL __stdcall Bridge_WriteFile(
    HANDLE hFile, LPCVOID lpBuffer, DWORD nNumberOfBytesToWrite,
    LPDWORD lpNumberOfBytesWritten, LPOVERLAPPED lpOverlapped)
{
    if (lpOverlapped) {
        LARGE_INTEGER pos;
        pos.LowPart = lpOverlapped->Offset;
        pos.HighPart = (LONG)lpOverlapped->OffsetHigh;
        SetFilePointerEx(hFile, pos, nullptr, FILE_BEGIN);
        DWORD bytesDone = 0;
        BOOL ok = WriteFile(hFile, lpBuffer, nNumberOfBytesToWrite, &bytesDone, nullptr);
        lpOverlapped->Internal = 0;
        lpOverlapped->InternalHigh = bytesDone;
        if (lpNumberOfBytesWritten) *lpNumberOfBytesWritten = bytesDone;
        if (lpOverlapped->hEvent && ((uintptr_t)lpOverlapped->hEvent & 1u) == 0) {
            SetEvent(lpOverlapped->hEvent);
        }
        return ok;
    }
    return WriteFile(hFile, lpBuffer, nNumberOfBytesToWrite, lpNumberOfBytesWritten, nullptr);
}

static BOOL __stdcall Bridge_SetEndOfFile(HANDLE hFile) {
    return SetEndOfFile(hFile);
}

static DWORD __stdcall Bridge_SetFilePointer(
    HANDLE hFile, LONG lDistanceToMove, PLONG lpDistanceToMoveHigh, DWORD dwMoveMethod)
{
    return SetFilePointer(hFile, lDistanceToMove, lpDistanceToMoveHigh, dwMoveMethod);
}

static BOOL __stdcall Bridge_SetFilePointerEx(
    HANDLE hFile, LARGE_INTEGER liDistanceToMove, PLARGE_INTEGER lpNewFilePointer, DWORD dwMoveMethod)
{
    return SetFilePointerEx(hFile, liDistanceToMove, lpNewFilePointer, dwMoveMethod);
}

static BOOL __stdcall Bridge_FlushFileBuffers(HANDLE hFile) {
    return FlushFileBuffers(hFile);
}

static BOOL __stdcall Bridge_GetFileSizeEx(HANDLE hFile, PLARGE_INTEGER lpFileSize) {
    return GetFileSizeEx(hFile, lpFileSize);
}

static DWORD __stdcall Bridge_GetFileSize(HANDLE hFile, LPDWORD lpFileSizeHigh) {
    return GetFileSize(hFile, lpFileSizeHigh);
}

static BOOL __stdcall Bridge_GetOverlappedResult(
    HANDLE hFile, LPOVERLAPPED lpOverlapped, LPDWORD lpNumberOfBytesTransferred, BOOL bWait)
{
    (void)hFile; (void)bWait;
    if (lpOverlapped) {
        if (lpNumberOfBytesTransferred) {
            *lpNumberOfBytesTransferred = (DWORD)lpOverlapped->InternalHigh;
        }
        return TRUE;
    }
    return FALSE;
}

static BOOL __stdcall Bridge_CancelIo(HANDLE hFile) {
    return CancelIo(hFile);
}

static BOOL __stdcall Bridge_CreateDirectoryA(const char* lpPathName, LPSECURITY_ATTRIBUTES lpSecurityAttributes) {
    (void)lpSecurityAttributes;
    char winPath[MAX_PATH];
    TranslateXboxPath(lpPathName, winPath, sizeof(winPath));
    EnsureDirExists(winPath);
    return TRUE;
}

static BOOL __stdcall Bridge_RemoveDirectoryA(const char* lpPathName) {
    char winPath[MAX_PATH];
    TranslateXboxPath(lpPathName, winPath, sizeof(winPath));
    return RemoveDirectoryA(winPath);
}

static HANDLE __stdcall Bridge_FindFirstFileA(const char* lpFileName, LPWIN32_FIND_DATAA lpFindFileData) {
    char winPath[MAX_PATH];
    TranslateXboxPath(lpFileName, winPath, sizeof(winPath));
    HANDLE h = FindFirstFileA(winPath, lpFindFileData);
    if (h != INVALID_HANDLE_VALUE) {
        RegisterFindHandle(h);
    }
    return h;
}

static BOOL __stdcall Bridge_FindNextFileA(HANDLE hFindFile, LPWIN32_FIND_DATAA lpFindFileData) {
    return FindNextFileA(hFindFile, lpFindFileData);
}

static DWORD __stdcall Bridge_XGetDiskSectorSizeA(const char* lpRootPathName) {
    (void)lpRootPathName;
    return 512;
}

static BOOL __stdcall Bridge_GetDiskFreeSpaceExA(
    const char* lpDirectoryName, PULARGE_INTEGER lpFreeBytesAvailableToCaller,
    PULARGE_INTEGER lpTotalNumberOfBytes, PULARGE_INTEGER lpTotalNumberOfFreeBytes)
{
    (void)lpDirectoryName;
    if (lpFreeBytesAvailableToCaller) lpFreeBytesAvailableToCaller->QuadPart = 1024ULL * 1024ULL * 1024ULL;
    if (lpTotalNumberOfBytes)         lpTotalNumberOfBytes->QuadPart         = 4096ULL * 1024ULL * 1024ULL;
    if (lpTotalNumberOfFreeBytes)     lpTotalNumberOfFreeBytes->QuadPart     = 1024ULL * 1024ULL * 1024ULL;
    return TRUE;
}

static BOOL __stdcall Bridge_XMountUtilityDrive(BOOL fFormatClean) {
    char zPath[MAX_PATH];
    TranslateXboxPath("Z:\\", zPath, sizeof(zPath));
    EnsureDirExists(zPath);
    (void)fFormatClean;
    return TRUE;
}

static BOOL __stdcall Bridge_XSetFileCacheSize(SIZE_T dwCacheSize) {
    (void)dwCacheSize;
    return TRUE;
}

// Xbox SaveGame APIs
static constexpr size_t   kMaxXboxSaveGameNameChars = 128u;      // 0x80 WCHARs (matches 0x005AAFF9 & 0x00436487)
static constexpr size_t   kXboxSaveFindDataSize     = 0x0344u;   // 0x140 (WIN32_FIND_DATAA) + 0x104 (dir) + 0x100 (name)
static constexpr uint32_t kSaveFindContextMagic     = 0x53415646u; // 'SAVF'
static constexpr size_t   kXboxSignatureByteSize    = 20u;       // 160-bit SHA-1 / HMAC digest (0x14 bytes)
static constexpr size_t   kXboxShaContextAllocSize  = 128u;

#pragma pack(push, 1)
struct XBOX_SAVEGAME_FIND_DATA {
    WIN32_FIND_DATAA wfd;
    char             szSaveGameDirectory[MAX_PATH];
    wchar_t          szSaveGameName[kMaxXboxSaveGameNameChars];
};
#pragma pack(pop)
static_assert(sizeof(XBOX_SAVEGAME_FIND_DATA) == kXboxSaveFindDataSize, "XBOX_SAVEGAME_FIND_DATA must be 0x344 bytes");

struct SaveFindContext {
    uint32_t magic;
    HANDLE   hFind;
    char     rootDrive[8];
    char     baseWinDir[MAX_PATH];
};

static void NormalizeXboxRootDrive(const char* lpRootPathName, char* outRoot, size_t outSize) {
    const char* src = (lpRootPathName && lpRootPathName[0]) ? lpRootPathName : "U:\\";
    strncpy(outRoot, src, outSize - 1);
    outRoot[outSize - 1] = '\0';
    size_t len = strlen(outRoot);
    if (len > 0 && outRoot[len - 1] != '\\' && outRoot[len - 1] != '/' && (len + 1) < outSize) {
        outRoot[len]     = '\\';
        outRoot[len + 1] = '\0';
    }
}

static void SanitizeSaveGameAsciiName(const wchar_t* lpSaveGameName, char* outAscii, size_t outSize) {
    strncpy(outAscii, "DefaultSave", outSize - 1);
    outAscii[outSize - 1] = '\0';
    if (lpSaveGameName && lpSaveGameName[0]) {
        WideCharToMultiByte(CP_ACP, 0, lpSaveGameName, -1, outAscii, (int)(outSize - 1), nullptr, nullptr);
        outAscii[outSize - 1] = '\0';
        for (char* p = outAscii; *p; ++p) {
            if (*p == '\\' || *p == '/' || *p == ':' || *p == '*' || *p == '?' || *p == '"' || *p == '<' || *p == '>' || *p == '|') {
                *p = '_';
            }
        }
    }
}

static DWORD __stdcall Bridge_XCreateSaveGame(
    const char* lpRootPathName, const wchar_t* lpSaveGameName, DWORD dwCreationDisposition,
    DWORD dwCreateFlags, char* lpPathBuffer, UINT uSize)
{
    (void)dwCreateFlags;
    char root[8];
    NormalizeXboxRootDrive(lpRootPathName, root, sizeof(root));
    char asciiName[kMaxXboxSaveGameNameChars];
    SanitizeSaveGameAsciiName(lpSaveGameName, asciiName, sizeof(asciiName));

    char xboxSaveDir[MAX_PATH];
    snprintf(xboxSaveDir, sizeof(xboxSaveDir), "%s%s\\", root, asciiName);
    char winSaveDir[MAX_PATH];
    TranslateXboxPath(xboxSaveDir, winSaveDir, sizeof(winSaveDir));

    DWORD existingAttrs = GetFileAttributesA(winSaveDir);
    bool dirExists = (existingAttrs != INVALID_FILE_ATTRIBUTES && (existingAttrs & FILE_ATTRIBUTE_DIRECTORY) != 0);

    if (dwCreationDisposition == OPEN_EXISTING) {
        if (!dirExists) {
            SetLastError(ERROR_PATH_NOT_FOUND);
            return ERROR_PATH_NOT_FOUND;
        }
    } else {
        EnsureDirExists(winSaveDir);
        // Save the Unicode name in SaveMeta.xbx for XFindFirstSaveGame
        char metaPath[MAX_PATH];
        snprintf(metaPath, sizeof(metaPath), "%s\\SaveMeta.xbx", winSaveDir);
        FILE* f = fopen(metaPath, "wb");
        if (f && lpSaveGameName) {
            fwrite(lpSaveGameName, sizeof(wchar_t), wcslen(lpSaveGameName) + 1, f);
            fclose(f);
        } else if (f) {
            fclose(f);
        }
    }

    if (lpPathBuffer && uSize > 0) {
        strncpy(lpPathBuffer, xboxSaveDir, uSize - 1);
        lpPathBuffer[uSize - 1] = '\0';
    }
    return ERROR_SUCCESS;
}

static DWORD __stdcall Bridge_XDeleteSaveGame(const char* lpRootPathName, const wchar_t* lpSaveGameName) {
    char root[8];
    NormalizeXboxRootDrive(lpRootPathName, root, sizeof(root));
    char asciiName[kMaxXboxSaveGameNameChars];
    SanitizeSaveGameAsciiName(lpSaveGameName, asciiName, sizeof(asciiName));

    char xboxSaveDir[MAX_PATH];
    snprintf(xboxSaveDir, sizeof(xboxSaveDir), "%s%s\\", root, asciiName);
    char winSaveDir[MAX_PATH];
    TranslateXboxPath(xboxSaveDir, winSaveDir, sizeof(winSaveDir));

    char searchPattern[MAX_PATH];
    snprintf(searchPattern, sizeof(searchPattern), "%s\\*", winSaveDir);
    WIN32_FIND_DATAA wfd = {};
    HANDLE hFind = FindFirstFileA(searchPattern, &wfd);
    if (hFind != INVALID_HANDLE_VALUE) {
        do {
            if (strcmp(wfd.cFileName, ".") != 0 && strcmp(wfd.cFileName, "..") != 0) {
                char filePath[MAX_PATH];
                snprintf(filePath, sizeof(filePath), "%s\\%s", winSaveDir, wfd.cFileName);
                DeleteFileA(filePath);
            }
        } while (FindNextFileA(hFind, &wfd));
        FindClose(hFind);
    }
    RemoveDirectoryA(winSaveDir);
    return ERROR_SUCCESS;
}

static bool FillSaveGameFindData(SaveFindContext* ctx, WIN32_FIND_DATAA* wfd, XBOX_SAVEGAME_FIND_DATA* outData) {
    while (true) {
        if ((wfd->dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) &&
            strcmp(wfd->cFileName, ".") != 0 && strcmp(wfd->cFileName, "..") != 0)
        {
            memset(outData, 0, sizeof(*outData));
            outData->wfd = *wfd;
            snprintf(outData->szSaveGameDirectory, sizeof(outData->szSaveGameDirectory), "%s%s\\", ctx->rootDrive, wfd->cFileName);
            char metaPath[MAX_PATH];
            snprintf(metaPath, sizeof(metaPath), "%s\\%s\\SaveMeta.xbx", ctx->baseWinDir, wfd->cFileName);
            FILE* f = fopen(metaPath, "rb");
            if (f) {
                fread(outData->szSaveGameName, sizeof(wchar_t), kMaxXboxSaveGameNameChars - 1, f);
                fclose(f);
            } else {
                MultiByteToWideChar(CP_ACP, 0, wfd->cFileName, -1, outData->szSaveGameName, (int)(kMaxXboxSaveGameNameChars - 1));
            }
            return true;
        }
        if (!FindNextFileA(ctx->hFind, wfd)) {
            return false;
        }
    }
}

static HANDLE __stdcall Bridge_XFindFirstSaveGame(const char* lpRootPathName, XBOX_SAVEGAME_FIND_DATA* lpFindGameData) {
    if (!lpFindGameData) return INVALID_HANDLE_VALUE;
    char root[8];
    NormalizeXboxRootDrive(lpRootPathName, root, sizeof(root));
    char winDir[MAX_PATH];
    TranslateXboxPath(root, winDir, sizeof(winDir));
    EnsureDirExists(winDir);

    char searchPattern[MAX_PATH];
    snprintf(searchPattern, sizeof(searchPattern), "%s\\*", winDir);
    WIN32_FIND_DATAA wfd;
    HANDLE hFind = FindFirstFileA(searchPattern, &wfd);
    if (hFind == INVALID_HANDLE_VALUE) return INVALID_HANDLE_VALUE;

    SaveFindContext* ctx = (SaveFindContext*)calloc(1, sizeof(SaveFindContext));
    ctx->magic = kSaveFindContextMagic;
    ctx->hFind = hFind;
    strncpy(ctx->rootDrive, root, sizeof(ctx->rootDrive) - 1);
    strncpy(ctx->baseWinDir, winDir, sizeof(ctx->baseWinDir) - 1);

    if (!FillSaveGameFindData(ctx, &wfd, lpFindGameData)) {
        FindClose(hFind);
        free(ctx);
        return INVALID_HANDLE_VALUE;
    }
    return (HANDLE)ctx;
}

static BOOL __stdcall Bridge_XFindNextSaveGame(HANDLE hFindGame, XBOX_SAVEGAME_FIND_DATA* lpFindGameData) {
    if (!hFindGame || hFindGame == INVALID_HANDLE_VALUE || !lpFindGameData) return FALSE;
    SaveFindContext* ctx = reinterpret_cast<SaveFindContext*>(hFindGame);
    if (ctx->magic != kSaveFindContextMagic) return FALSE;
    WIN32_FIND_DATAA wfd;
    if (!FindNextFileA(ctx->hFind, &wfd)) return FALSE;
    return FillSaveGameFindData(ctx, &wfd, lpFindGameData) ? TRUE : FALSE;
}

static BOOL __stdcall Bridge_XFindClose(HANDLE hFindGame) {
    if (!hFindGame || hFindGame == INVALID_HANDLE_VALUE) return FALSE;
    SaveFindContext* ctx = reinterpret_cast<SaveFindContext*>(hFindGame);
    if (ctx->magic == kSaveFindContextMagic) {
        ctx->magic = 0;
        FindClose(ctx->hFind);
        free(ctx);
        return TRUE;
    }
    return Bridge_CloseHandle(hFindGame);
}

// Xbox Signature Calculation
static HANDLE __stdcall Bridge_XCalculateSignatureBegin(DWORD dwFlags) {
    (void)dwFlags;
    uint8_t* ctx = (uint8_t*)calloc(1, kXboxShaContextAllocSize);
    return (HANDLE)ctx;
}

static HANDLE __stdcall Bridge_XCalculateSignatureBeginEx(DWORD dwFlags, DWORD dwAltTitleId) {
    (void)dwAltTitleId;
    return Bridge_XCalculateSignatureBegin(dwFlags);
}

static DWORD __stdcall Bridge_XCalculateSignatureUpdate(HANDLE hCalcSig, const BYTE* pbData, ULONG cbData) {
    if (hCalcSig && pbData) {
        Krnl_XcSHAUpdate((uint8_t*)hCalcSig, pbData, cbData);
    }
    return ERROR_SUCCESS;
}

static DWORD __stdcall Bridge_XCalculateSignatureEnd(HANDLE hCalcSig, void* pSignature) {
    if (pSignature) {
        if (hCalcSig) {
            memcpy(pSignature, hCalcSig, kXboxSignatureByteSize);
        } else {
            memset(pSignature, 0, kXboxSignatureByteSize);
        }
    }
    if (hCalcSig) free(hCalcSig);
    return ERROR_SUCCESS;
}

// Xbox Section Loading (_XGetSectionHandleA, _XLoadSectionByHandle, _XFreeSectionByHandle, _XGetSectionSize)
static constexpr int      kMaxXboxPeSections     = 40;
static constexpr size_t   kPeShortNameLen        = 8u;
static constexpr uint32_t kXboxPeBaseAddr        = 0x00400000u;
static constexpr uint32_t kRvaSectionBinkYuy2Mx2 = 0x0031D3A0u;
static constexpr uint32_t kRvaSectionBinkYuy2X2  = 0x0031D440u;
static constexpr uint32_t kRvaSectionBinkYuy2M   = 0x0031D500u;
static constexpr uint32_t kRvaSectionBink16Mx2   = 0x0031D5A0u;
static constexpr uint32_t kRvaSectionBink32Mx2   = 0x0031DE40u;

struct XboxSectionInfo {
    char     name[9];
    char     fullName[16];
    uint32_t virtualAddress;
    uint32_t virtualSize;
};

static XboxSectionInfo g_peSections[kMaxXboxPeSections];
static int g_numPeSections = 0;

void RegisterPeSection(const char* name8, uint32_t va, uint32_t vsize) {
    if (g_numPeSections >= kMaxXboxPeSections) return;
    XboxSectionInfo& sec = g_peSections[g_numPeSections];
    memset(&sec, 0, sizeof(XboxSectionInfo));
    memcpy(sec.name, name8, kPeShortNameLen);
    memcpy(sec.fullName, sec.name, kPeShortNameLen);

    // Map 8-byte truncated PE section headers to their canonical full XBE section names from Ghost.map
    if (va == kRvaSectionBinkYuy2Mx2) {
        strcpy(sec.fullName, "BINKYUY2MX2");
    } else if (va == kRvaSectionBinkYuy2X2) {
        strcpy(sec.fullName, "BINKYUY2X2");
    } else if (va == kRvaSectionBinkYuy2M) {
        strcpy(sec.fullName, "BINKYUY2M");
    } else if (va == kRvaSectionBink16Mx2) {
        strcpy(sec.fullName, "BINK16MX2");
    } else if (va == kRvaSectionBink32Mx2) {
        strcpy(sec.fullName, "BINK32MX2");
    }

    sec.virtualAddress = va;
    sec.virtualSize = vsize;
    g_numPeSections++;
}

static HANDLE __stdcall Bridge_XGetSectionHandleA(const char* pSectionName) {
    if (!pSectionName) return INVALID_HANDLE_VALUE;
    for (int i = 0; i < g_numPeSections; ++i) {
        if (strcmp(g_peSections[i].fullName, pSectionName) == 0) {
            return (HANDLE)(uintptr_t)(i + 1);
        }
    }
    for (int i = 0; i < g_numPeSections; ++i) {
        if (strncmp(g_peSections[i].name, pSectionName, kPeShortNameLen) == 0) {
            return (HANDLE)(uintptr_t)(i + 1);
        }
    }
    return INVALID_HANDLE_VALUE;
}

static void* __stdcall Bridge_XLoadSectionByHandle(HANDLE hSection) {
    uintptr_t idx = (uintptr_t)hSection;
    if (idx >= 1 && idx <= (uintptr_t)g_numPeSections) {
        return reinterpret_cast<void*>(kXboxPeBaseAddr + g_peSections[idx - 1].virtualAddress);
    }
    return nullptr;
}

static BOOL __stdcall Bridge_XFreeSectionByHandle(HANDLE hSection) {
    (void)hSection;
    return TRUE;
}

static DWORD __stdcall Bridge_XGetSectionSize(HANDLE hSection) {
    uintptr_t idx = (uintptr_t)hSection;
    if (idx >= 1 && idx <= (uintptr_t)g_numPeSections) {
        return g_peSections[idx - 1].virtualSize;
    }
    return 0;
}

// Xbox Physical & XMem Allocators
static void* __stdcall Bridge_XPhysicalAlloc(SIZE_T dwSize, ULONG_PTR ulPhysicalAddress, ULONG_PTR ulAlignment, DWORD flProtect) {
    (void)ulPhysicalAddress; (void)flProtect;
    return AllocAlignedTracked((uint32_t)dwSize, (uint32_t)(ulAlignment ? ulAlignment : 4096));
}

static void* __stdcall Bridge_XPhysicalAllocEx(SIZE_T dwSize, ULONG_PTR ulLowestAcceptableAddress, ULONG_PTR ulHighestAcceptableAddress, ULONG_PTR ulAlignment, DWORD flProtect) {
    (void)ulLowestAcceptableAddress; (void)ulHighestAcceptableAddress; (void)flProtect;
    return AllocAlignedTracked((uint32_t)dwSize, (uint32_t)(ulAlignment ? ulAlignment : 4096));
}

static void __stdcall Bridge_XPhysicalFree(void* lpAddress) {
    FreeAlignedTracked(lpAddress);
}

static void* __stdcall Bridge_XMemAlloc(SIZE_T dwSize, DWORD dwAllocAttributes) {
    (void)dwAllocAttributes;
    // Check if physical memory alignment is requested in attributes
    return AllocAlignedTracked((uint32_t)dwSize, 4096);
}

static void __stdcall Bridge_XMemFree(void* pAddress, DWORD dwAllocAttributes) {
    (void)dwAllocAttributes;
    FreeAlignedTracked(pAddress);
}

// Xbox Heap APIs (redirect directly to Win32 Process Heap!)
static HANDLE __stdcall Bridge_GetProcessHeap() {
    return GetProcessHeap();
}

static HANDLE __stdcall Bridge_RtlCreateHeap(ULONG Flags, PVOID HeapBase, SIZE_T ReserveSize, SIZE_T CommitSize, PVOID Lock, PVOID Parameters) {
    (void)Flags; (void)HeapBase; (void)ReserveSize; (void)CommitSize; (void)Lock; (void)Parameters;
    HANDLE h = HeapCreate(0, 0, 0);
    return h ? h : GetProcessHeap();
}

static const uint32_t XBOX_HEAP_MAGIC = 0x58485031u; // 'XHP1'
struct XboxHeapHdr {
    uint32_t magic;
    void*    rawPtr;
    SIZE_T   reqSize;
    HANDLE   hHeap;
};

static PVOID __stdcall Bridge_RtlAllocateHeap(HANDLE HeapHandle, ULONG Flags, SIZE_T Size) {
    if (!HeapHandle || (uintptr_t)HeapHandle < 0x00960000) {
        HeapHandle = GetProcessHeap();
    }
    SIZE_T req = Size ? Size : 4;
    void* raw = HeapAlloc(HeapHandle, Flags & HEAP_ZERO_MEMORY, req + sizeof(XboxHeapHdr) + 16);
    if (!raw) return nullptr;
    uintptr_t aligned = ((uintptr_t)raw + sizeof(XboxHeapHdr) + 15u) & ~(uintptr_t)15u;
    XboxHeapHdr* hdr = reinterpret_cast<XboxHeapHdr*>(aligned) - 1;
    hdr->magic   = XBOX_HEAP_MAGIC;
    hdr->rawPtr  = raw;
    hdr->reqSize = req;
    hdr->hHeap   = HeapHandle;
    return reinterpret_cast<PVOID>(aligned);
}

static BOOL __stdcall Bridge_RtlFreeHeap(HANDLE HeapHandle, ULONG Flags, PVOID BaseAddress) {
    if (!BaseAddress || (uintptr_t)BaseAddress < 0x00960000) return TRUE;
    if (((uintptr_t)BaseAddress & 15u) == 0) {
        XboxHeapHdr* hdr = reinterpret_cast<XboxHeapHdr*>(BaseAddress) - 1;
        if (hdr->magic == XBOX_HEAP_MAGIC) {
            HANDLE h = hdr->hHeap;
            void* raw = hdr->rawPtr;
            hdr->magic = 0;
            return HeapFree(h, Flags, raw);
        }
    }
    if (!HeapHandle || (uintptr_t)HeapHandle < 0x00960000) {
        HeapHandle = GetProcessHeap();
    }
    return HeapFree(HeapHandle, Flags, BaseAddress);
}

static SIZE_T __stdcall Bridge_RtlSizeHeap(HANDLE HeapHandle, ULONG Flags, PVOID BaseAddress) {
    if (!BaseAddress || (uintptr_t)BaseAddress < 0x00960000) return 0;
    if (((uintptr_t)BaseAddress & 15u) == 0) {
        XboxHeapHdr* hdr = reinterpret_cast<XboxHeapHdr*>(BaseAddress) - 1;
        if (hdr->magic == XBOX_HEAP_MAGIC) {
            return hdr->reqSize;
        }
    }
    if (!HeapHandle || (uintptr_t)HeapHandle < 0x00960000) {
        HeapHandle = GetProcessHeap();
    }
    return HeapSize(HeapHandle, Flags, BaseAddress);
}

static PVOID __stdcall Bridge_RtlReAllocateHeap(HANDLE HeapHandle, ULONG Flags, PVOID BaseAddress, SIZE_T Size) {
    if (!BaseAddress) {
        return Bridge_RtlAllocateHeap(HeapHandle, Flags, Size);
    }
    SIZE_T oldSize = Bridge_RtlSizeHeap(HeapHandle, 0, BaseAddress);
    PVOID newPtr = Bridge_RtlAllocateHeap(HeapHandle, Flags, Size);
    if (newPtr && oldSize > 0) {
        SIZE_T copyBytes = (oldSize < Size) ? oldSize : Size;
        memcpy(newPtr, BaseAddress, copyBytes);
    }
    if (newPtr) {
        Bridge_RtlFreeHeap(HeapHandle, 0, BaseAddress);
    }
    return newPtr;
}

static HLOCAL __stdcall Bridge_LocalAlloc(UINT uFlags, SIZE_T uBytes) {
    return LocalAlloc(uFlags, uBytes);
}

static HLOCAL __stdcall Bridge_LocalFree(HLOCAL hMem) {
    return LocalFree(hMem);
}

// Synchronization, Threads, Time, Performance Counter, Memory Status
static BOOL __stdcall Bridge_QueryPerformanceCounter(LARGE_INTEGER* lpPerformanceCount) {
    return QueryPerformanceCounter(lpPerformanceCount);
}

static BOOL __stdcall Bridge_QueryPerformanceFrequency(LARGE_INTEGER* lpFrequency) {
    return QueryPerformanceFrequency(lpFrequency);
}

static DWORD __stdcall Bridge_GetTickCount() {
    g_keTickCount = GetTickCount();
    return g_keTickCount;
}

static void __stdcall Bridge_GetSystemTimeAsFileTime(LPFILETIME lpSystemTimeAsFileTime) {
    GetSystemTimeAsFileTime(lpSystemTimeAsFileTime);
}

static void __stdcall Bridge_GetLocalTime(LPSYSTEMTIME lpSystemTime) {
    GetLocalTime(lpSystemTime);
}

static BOOL __stdcall Bridge_FileTimeToSystemTime(const FILETIME* lpFileTime, LPSYSTEMTIME lpSystemTime) {
    return FileTimeToSystemTime(lpFileTime, lpSystemTime);
}

static BOOL __stdcall Bridge_SystemTimeToFileTime(const SYSTEMTIME* lpSystemTime, LPFILETIME lpFileTime) {
    return SystemTimeToFileTime(lpSystemTime, lpFileTime);
}

static DWORD __stdcall Bridge_GetTimeZoneInformation(LPTIME_ZONE_INFORMATION lpTimeZoneInformation) {
    return GetTimeZoneInformation(lpTimeZoneInformation);
}

static HANDLE __stdcall Bridge_CreateEventA(LPSECURITY_ATTRIBUTES lpEventAttributes, BOOL bManualReset, BOOL bInitialState, LPCSTR lpName) {
    return CreateEventA(lpEventAttributes, bManualReset, bInitialState, lpName);
}

static BOOL __stdcall Bridge_SetEvent(HANDLE hEvent) {
    return SetEvent(hEvent);
}

static HANDLE __stdcall Bridge_CreateMutexA(LPSECURITY_ATTRIBUTES lpMutexAttributes, BOOL bInitialOwner, LPCSTR lpName) {
    return CreateMutexA(lpMutexAttributes, bInitialOwner, lpName);
}

static BOOL __stdcall Bridge_ReleaseMutex(HANDLE hMutex) {
    return ReleaseMutex(hMutex);
}

static DWORD __stdcall Bridge_WaitForSingleObjectEx(HANDLE hHandle, DWORD dwMilliseconds, BOOL bAlertable) {
    return WaitForSingleObjectEx(hHandle, dwMilliseconds, bAlertable);
}

static DWORD __stdcall Bridge_WaitForSingleObject(HANDLE hHandle, DWORD dwMilliseconds) {
    return WaitForSingleObject(hHandle, dwMilliseconds);
}

static DWORD __stdcall Bridge_WaitForMultipleObjectsEx(DWORD nCount, const HANDLE* lpHandles, BOOL bWaitAll, DWORD dwMilliseconds, BOOL bAlertable) {
    return WaitForMultipleObjectsEx(nCount, lpHandles, bWaitAll, dwMilliseconds, bAlertable);
}

static DWORD __stdcall Bridge_WaitForMultipleObjects(DWORD nCount, const HANDLE* lpHandles, BOOL bWaitAll, DWORD dwMilliseconds) {
    return WaitForMultipleObjects(nCount, lpHandles, bWaitAll, dwMilliseconds);
}

static DWORD __stdcall Bridge_SleepEx(DWORD dwMilliseconds, BOOL bAlertable) {
    return SleepEx(dwMilliseconds > 50 ? 10 : dwMilliseconds, bAlertable);
}

static void __stdcall Bridge_Sleep(DWORD dwMilliseconds) {
    Sleep(dwMilliseconds > 50 ? 10 : dwMilliseconds);
}

static HANDLE __stdcall Bridge_CreateThread(
    LPSECURITY_ATTRIBUTES lpThreadAttributes, SIZE_T dwStackSize,
    LPTHREAD_START_ROUTINE lpStartAddress, LPVOID lpParameter,
    DWORD dwCreationFlags, LPDWORD lpThreadId)
{
    DWORD tid = 0;
    HANDLE h = CreateThread(lpThreadAttributes, dwStackSize, lpStartAddress, lpParameter, dwCreationFlags, &tid);
    if (lpThreadId) *lpThreadId = tid;
    return h;
}

static DWORD __stdcall Bridge_ResumeThread(HANDLE hThread) {
    return ResumeThread(hThread);
}

static DWORD __stdcall Bridge_GetCurrentThreadId() {
    return GetCurrentThreadId();
}

static DWORD __stdcall Bridge_GetLastError() {
    return GetLastError();
}

static void __stdcall Bridge_SetLastError(DWORD dwErrCode) {
    SetLastError(dwErrCode);
}

static void __stdcall Bridge_OutputDebugStringA(LPCSTR lpOutputString) {
    (void)lpOutputString;
}

static void __stdcall Bridge_GlobalMemoryStatus(LPMEMORYSTATUS lpBuffer) {
    if (lpBuffer) {
        memset(lpBuffer, 0, sizeof(*lpBuffer));
        lpBuffer->dwLength = sizeof(*lpBuffer);
        lpBuffer->dwMemoryLoad = 25;
        lpBuffer->dwTotalPhys = 64 * 1024 * 1024;
        lpBuffer->dwAvailPhys = 48 * 1024 * 1024;
        lpBuffer->dwTotalPageFile = 64 * 1024 * 1024;
        lpBuffer->dwAvailPageFile = 48 * 1024 * 1024;
        lpBuffer->dwTotalVirtual = 64 * 1024 * 1024;
        lpBuffer->dwAvailVirtual = 48 * 1024 * 1024;
    }
}

static DWORD __stdcall Bridge_XGetLanguage() { return 1; } // XC_LANGUAGE_ENGLISH
static DWORD __stdcall Bridge_XGetGameRegion() { return 1; } // XC_GAME_REGION_NA
static DWORD __stdcall Bridge_XGetAVPack() { return 6; } // XC_AV_PACK_STANDARD
static DWORD __stdcall Bridge_XGetVideoFlags() { return 0; }
static DWORD __stdcall Bridge_XGetVideoStandard() { return 1; } // XC_VIDEO_STANDARD_NTSC_M
static DWORD __stdcall Bridge_XGetAudioFlags() { return 0; }
static DWORD __stdcall Bridge_XGetParentalControlSetting() { return 0; }

static DWORD __stdcall Bridge_XGetLaunchInfo(PDWORD pdwLaunchDataType, void* pLaunchData) {
    if (pdwLaunchDataType) *pdwLaunchDataType = 2; // LDT_NONE
    if (pLaunchData) memset(pLaunchData, 0, 64);
    return ERROR_NOT_FOUND;
}

static DWORD __stdcall Bridge_XLaunchNewImageA(LPCSTR lpTitlePath, void* pLaunchData) {
    (void)lpTitlePath; (void)pLaunchData;
    return ERROR_SUCCESS;
}

static int __stdcall Bridge_MultiByteToWideChar(UINT CodePage, DWORD dwFlags, LPCCH lpMultiByteStr, int cbMultiByte, LPWSTR lpWideCharStr, int cchWideChar) {
    return MultiByteToWideChar(CodePage, dwFlags, lpMultiByteStr, cbMultiByte, lpWideCharStr, cchWideChar);
}

static int __stdcall Bridge_WideCharToMultiByte(UINT CodePage, DWORD dwFlags, LPCWCH lpWideCharStr, int cchWideChar, LPSTR lpMultiByteStr, int cbMultiByte, LPCCH lpDefaultChar, LPBOOL lpUsedDefaultChar) {
    return WideCharToMultiByte(CodePage, dwFlags, lpWideCharStr, cchWideChar, lpMultiByteStr, cbMultiByte, lpDefaultChar, lpUsedDefaultChar);
}

void InitXboxXapi() {
    InitializeCriticalSection(&g_findCs);

    // 1. Bridge XAPILIB Functions
    InstallBridge(0x0059FB75, &Bridge_QueryPerformanceCounter,   "_QueryPerformanceCounter@4");
    InstallBridge(0x0059FB86, &Bridge_QueryPerformanceFrequency, "_QueryPerformanceFrequency@4");
    InstallBridge(0x0059FB9A, &Bridge_XGetLanguage,              "_XGetLanguage@0");
    InstallBridge(0x0059FBD4, &Krnl_DbgBreakPoint,               "_DebugBreak@0");
    InstallBridge(0x0059FBD9, &Bridge_GetSystemTimeAsFileTime,   "_GetSystemTimeAsFileTime@4");
    InstallBridge(0x0059FC09, &Bridge_XLaunchNewImageA,          "_XLaunchNewImageA@8");
    PatchRet(0x0059FC4B, 24, 0, true);                           // _XWriteTitleInfoNoReboot@24
    InstallBridge(0x0059FD2C, &Bridge_XGetLaunchInfo,            "_XGetLaunchInfo@8");
    PatchRet(0x0059FD90, 20, 0, true);                           // _XWriteTitleInfoAndRebootA@20

    // SaveGame & Signature APIs
    InstallBridge(0x005A0225, &Bridge_XCreateSaveGame,           "_XCreateSaveGame@24");
    InstallBridge(0x005A0488, &Bridge_XDeleteSaveGame,           "_XDeleteSaveGame@8");
    InstallBridge(0x005A0558, &Bridge_XFindFirstSaveGame,        "_XFindFirstSaveGame@8");
    InstallBridge(0x005A0662, &Bridge_XFindNextSaveGame,         "_XFindNextSaveGame@8");
    InstallBridge(0x005A06A9, &Bridge_XFindClose,                "_XFindClose@4");
    InstallBridge(0x005A09A9, &Bridge_XCalculateSignatureBeginEx,"_XCalculateSignatureBeginEx@8");
    InstallBridge(0x005A09EA, &Bridge_XCalculateSignatureBegin,  "_XCalculateSignatureBegin@4");
    InstallBridge(0x005A09FE, &Bridge_XCalculateSignatureUpdate, "_XCalculateSignatureUpdate@12");
    InstallBridge(0x005A0A4C, &Bridge_XCalculateSignatureEnd,    "_XCalculateSignatureEnd@8");

    // File & Directory APIs
    InstallBridge(0x005A0A91, &Bridge_CloseHandle,               "_CloseHandle@4");
    InstallBridge(0x005A0AAF, &Bridge_ReadFile,                  "_ReadFile@20");
    InstallBridge(0x005A0B9C, &Bridge_WriteFile,                 "_WriteFile@20");
    InstallBridge(0x005A0C72, &Bridge_SetEndOfFile,              "_SetEndOfFile@4");
    InstallBridge(0x005A0CF6, &Bridge_SetFilePointer,            "_SetFilePointer@16");
    InstallBridge(0x005A0DF0, &Bridge_SetFilePointerEx,          "_SetFilePointerEx@20");
    InstallBridge(0x005A0EB8, &Bridge_FlushFileBuffers,          "_FlushFileBuffers@4");
    InstallBridge(0x005A0EDF, &Bridge_GetFileSizeEx,             "_GetFileSizeEx@8");
    InstallBridge(0x005A0F1D, &Bridge_GetFileSize,               "_GetFileSize@8");
    InstallBridge(0x005A1079, &Bridge_CreateFileA,               "_CreateFileA@28");
    InstallBridge(0x005A1204, &Bridge_GetLastError,              "_GetLastError@0");
    InstallBridge(0x005A122C, &Bridge_SetLastError,              "_SetLastError@4");
    InstallBridge(0x005A1271, &Bridge_GetOverlappedResult,       "_GetOverlappedResult@16");
    InstallBridge(0x005A1514, &Bridge_GetTickCount,              "_GetTickCount@0");
    InstallBridge(0x005A151C, &Bridge_FileTimeToSystemTime,      "_FileTimeToSystemTime@8");
    InstallBridge(0x005A1587, &Bridge_SystemTimeToFileTime,      "_SystemTimeToFileTime@8");
    InstallBridge(0x005A1806, &Bridge_GetTimeZoneInformation,    "_GetTimeZoneInformation@4");
    InstallBridge(0x005A19B8, &Bridge_GetLocalTime,              "_GetLocalTime@4");
    InstallBridge(0x005A1A37, &Bridge_XSetFileCacheSize,         "_XSetFileCacheSize@4");
    InstallBridge(0x005A1A8A, &Bridge_CancelIo,                  "_CancelIo@4");
    InstallBridge(0x005A1B53, &Bridge_GetDiskFreeSpaceExA,       "_GetDiskFreeSpaceExA@16");
    PatchRet(0x005A1C47, 12, 1, true);                           // _XapiSelectCachePartition@12
    InstallBridge(0x005A1E92, &Bridge_XMountUtilityDrive,        "_XMountUtilityDrive@4");
    PatchRet(0x005A1F97, 4, 0, true);                            // _XUnmountAlternateTitleA@4
    InstallBridge(0x005A1FF4, &Bridge_XGetDiskSectorSizeA,       "_XGetDiskSectorSizeA@4");
    PatchRet(0x005A208F, 12, 0, true);                           // _XMUNameFromDriveLetter@12

    // Physical Memory & Synchronization & Thread APIs
    InstallBridge(0x005A216A, &Bridge_XPhysicalAlloc,            "_XPhysicalAlloc@16");
    InstallBridge(0x005A21AD, &Bridge_XPhysicalAllocEx,          "_XPhysicalAllocEx@20");
    InstallBridge(0x005A21DA, &Bridge_XPhysicalFree,             "_XPhysicalFree@4");
    InstallBridge(0x005A21E0, &Bridge_CreateEventA,              "_CreateEventA@16");
    InstallBridge(0x005A2241, &Bridge_SetEvent,                  "_SetEvent@4");
    InstallBridge(0x005A2261, &Bridge_CreateMutexA,              "_CreateMutexA@12");
    InstallBridge(0x005A22B9, &Bridge_ReleaseMutex,              "_ReleaseMutex@4");
    InstallBridge(0x005A22D9, &Bridge_WaitForSingleObjectEx,     "_WaitForSingleObjectEx@12");
    InstallBridge(0x005A231D, &Bridge_WaitForMultipleObjectsEx,  "_WaitForMultipleObjectsEx@20");
    InstallBridge(0x005A239D, &Bridge_SleepEx,                   "_SleepEx@8");
    InstallBridge(0x005A23EF, &Bridge_WaitForSingleObject,       "_WaitForSingleObject@8");
    InstallBridge(0x005A2401, &Bridge_WaitForMultipleObjects,    "_WaitForMultipleObjects@16");
    InstallBridge(0x005A241B, &Bridge_Sleep,                     "_Sleep@4");
    PatchRet(0x005A2429, 4, 0, false);                           // _XapiCallThreadNotifyRoutines@4
    InstallBridge(0x005A2461, &Bridge_ResumeThread,              "_ResumeThread@4");
    PatchRet(0x005A24EF, 8, 0, false);                           // _XRegisterThreadNotifyRoutine@8
    InstallBridge(0x005A260C, &Bridge_CreateThread,              "_CreateThread@24");
    InstallBridge(0x005A267A, &Bridge_GetCurrentThreadId,        "_GetCurrentThreadId@0");
    InstallBridge(0x005A276D, &Bridge_XGetGameRegion,            "_XGetGameRegion@0");
    InstallBridge(0x005A2795, &Bridge_OutputDebugStringA,        "_OutputDebugStringA@4");
    InstallBridge(0x005A2EBC, &Bridge_CreateDirectoryA,          "_CreateDirectoryA@8");
    InstallBridge(0x005A2F28, &Bridge_RemoveDirectoryA,          "_RemoveDirectoryA@4");
    PatchRet(0x005A2FB6, 8, 1, true);                            // _XapiNukeDirectoryFromHandle@8
    PatchRet(0x005A3133, 8, 1, true);                            // _XapiNukeDirectory@8
    InstallBridge(0x005A31A2, &Bridge_XMemAlloc,                 "_XMemAlloc@8");
    InstallBridge(0x005A3242, &Bridge_XMemFree,                  "_XMemFree@8");
    InstallBridge(0x005A32C8, &Bridge_FindFirstFileA,            "_FindFirstFileA@8");
    InstallBridge(0x005A33D5, &Bridge_FindNextFileA,             "_FindNextFileA@8");
    InstallBridge(0x005A3426, &Krnl_ExQueryNonVolatileSetting,   "_XQueryValue@20");
    PatchRet(0x005A3B20, 12, 0, false);                          // _XapiBootToDash@12
    PatchRet(0x005A3E16, 0, 0, false);                           // _XapiInitProcess@0
    PatchRet(0x005A4338, 0, 0, false);                           // _XapiRestrictCodeSelectorLimit@0
    PatchRet(0x005A439C, 0, 0, false);                           // _XapiApplyKernelPatches@0

    // Heap & Section & Misc XAPILIB
    InstallBridge(0x005A4646, &Bridge_RtlFreeHeap,               "_HeapFree@12");
    InstallBridge(0x005A465D, &Bridge_GetProcessHeap,            "_GetProcessHeap@0");
    InstallBridge(0x005A4663, &Bridge_LocalAlloc,                "_LocalAlloc@8");
    InstallBridge(0x005A4680, &Bridge_LocalFree,                 "_LocalFree@4");
    InstallBridge(0x005A469E, &Bridge_XGetSectionHandleA,        "_XGetSectionHandleA@4");
    InstallBridge(0x005A4705, &Bridge_XLoadSectionByHandle,      "_XLoadSectionByHandle@4");
    InstallBridge(0x005A4726, &Bridge_XFreeSectionByHandle,      "_XFreeSectionByHandle@4");
    InstallBridge(0x005A4744, &Bridge_XGetSectionSize,           "_XGetSectionSize@4");
    InstallBridge(0x005A474E, &Bridge_XGetParentalControlSetting,"_XGetParentalControlSetting@0");
    InstallBridge(0x005A528B, &Bridge_RtlSizeHeap,               "_RtlSizeHeap@12");
    InstallBridge(0x005A5809, &Bridge_RtlCreateHeap,             "_RtlCreateHeap@24");
    InstallBridge(0x005A5C2D, &Bridge_RtlAllocateHeap,           "_RtlAllocateHeap@12");
    InstallBridge(0x005A63A8, &Bridge_RtlFreeHeap,               "_RtlFreeHeap@12");
    InstallBridge(0x005A659C, &Bridge_RtlReAllocateHeap,         "_RtlReAllocateHeap@16");
    PatchRet(0x005A6CCA, 0, 0, false);                           // _XAutoPowerDownResetTimer@0
    PatchRet(0x005A6CF7, 0, 0, false);                           // _XapiInitAutoPowerDown@0
    PatchRet(0x005A6DE4, 8, 1, true);                            // _XapiChangeCodeSelectorLimit@8

    InstallBridge(0x0069C68B, &Bridge_XGetAudioFlags,            "_XGetAudioFlags@0");
    InstallBridge(0x0069C714, &Bridge_GlobalMemoryStatus,        "_GlobalMemoryStatus@4");
    InstallBridge(0x0069C763, &Bridge_XGetAVPack,                "_XGetAVPack@0");
    InstallBridge(0x0069C76B, &Bridge_XGetVideoFlags,            "_XGetVideoFlags@0");
    PatchRet(0x0069C7B4, 8, 0, true);                            // _IsBadReadPtr@8 -> FALSE
    PatchRet(0x0069C819, 8, 0, true);                            // _IsBadWritePtr@8 -> FALSE
    PatchRet(0x0069C882, 4, 0, true);                            // _IsBadCodePtr@4 -> FALSE
    InstallBridge(0x0069C890, &Bridge_XGetVideoStandard,         "_XGetVideoStandard@0");
    InstallBridge(0x006AA8E7, &Bridge_MultiByteToWideChar,       "_MultiByteToWideChar@24");
    InstallBridge(0x006AA9A2, &Bridge_WideCharToMultiByte,       "_WideCharToMultiByte@32");

    // 2. XNet / Winsock Offline Stubs (prevents null CXnSock* dereferences when hkBsdSockServer calls XNET)
    constexpr uint32_t kAddrXNetSocket         = 0x00739AA0u;
    constexpr uint32_t kAddrXNetCloseSocket    = 0x00739AABu;
    constexpr uint32_t kAddrXNetSetsockopt     = 0x00739AB6u;
    constexpr uint32_t kAddrXNetBind           = 0x00739AC5u;
    constexpr uint32_t kAddrXNetListen         = 0x00739AD0u;
    constexpr uint32_t kAddrXNetAccept         = 0x00739ADBu;
    constexpr uint32_t kAddrXNetSelect         = 0x00739AE6u;
    constexpr uint32_t kAddrXNetRecv           = 0x00739AF5u;
    constexpr uint32_t kAddrXNetSend           = 0x00739B00u;
    constexpr uint32_t kAddrXNetStartup        = 0x00739D14u;
    constexpr uint32_t kAddrWSAStartup         = 0x00739D2Bu;
    constexpr uint32_t kAddrWSACleanup         = 0x00739E19u;
    constexpr uint32_t kAddrWSAGetLastError    = 0x00739E90u;
    constexpr uint32_t kAddrWSAFDIsSet         = 0x00739E95u;
    constexpr uint32_t kXboxInvalidSocket      = 0xFFFFFFFFu;
    constexpr uint32_t kXboxSocketError        = 0xFFFFFFFFu;
    constexpr uint32_t kXboxWsaErrorNetDown    = 10050u;

    PatchRet(kAddrXNetStartup,     4,  0,                   true); // _XNetStartup@4 -> 0
    PatchRet(kAddrWSAStartup,      8,  0,                   true); // _WSAStartup@8 -> 0
    PatchRet(kAddrWSACleanup,      0,  0,                   true); // _WSACleanup@0 -> 0
    PatchRet(kAddrXNetSocket,      12, kXboxInvalidSocket,  true); // _socket@12 -> INVALID_SOCKET
    PatchRet(kAddrXNetCloseSocket, 4,  0,                   true); // _closesocket@4 -> 0
    PatchRet(kAddrXNetSetsockopt,  20, kXboxSocketError,    true); // _setsockopt@20 -> SOCKET_ERROR
    PatchRet(kAddrXNetBind,        12, kXboxSocketError,    true); // _bind@12 -> SOCKET_ERROR
    PatchRet(kAddrXNetListen,      8,  kXboxSocketError,    true); // _listen@8 -> SOCKET_ERROR
    PatchRet(kAddrXNetAccept,      12, kXboxInvalidSocket,  true); // _accept@12 -> INVALID_SOCKET
    PatchRet(kAddrXNetSelect,      20, kXboxSocketError,    true); // _select@20 -> SOCKET_ERROR
    PatchRet(kAddrXNetRecv,        16, kXboxSocketError,    true); // _recv@16 -> SOCKET_ERROR
    PatchRet(kAddrXNetSend,        16, kXboxSocketError,    true); // _send@16 -> SOCKET_ERROR
    PatchRet(kAddrWSAGetLastError, 0,  kXboxWsaErrorNetDown,true); // _WSAGetLastError@0 -> WSAENETDOWN
    PatchRet(kAddrWSAFDIsSet,      8,  0,                   true); // ___WSAFDIsSet@8 -> 0
}

void InitXboxKernelAndXapi() {
    InitXboxKernel();
    InitXboxXapi();
}
