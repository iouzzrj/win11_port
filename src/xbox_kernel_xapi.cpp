#include "ghost_runtime.h"
#include <stdarg.h>
#include <direct.h>
#include <io.h>
#include <sys/stat.h>

// ============================================================================
// Patching Utilities
// ============================================================================

void InstallBridge(uint32_t xboxVA, const void* hostFn, const char* name) {
    uint8_t* p = reinterpret_cast<uint8_t*>(xboxVA);
    uint32_t rel = reinterpret_cast<uintptr_t>(hostFn) - (xboxVA + 5);
    p[0] = 0xE9; // JMP rel32
    *reinterpret_cast<uint32_t*>(p + 1) = rel;
    (void)name;
}

void PatchBytes(uint32_t xboxVA, const void* data, size_t len) {
    memcpy(reinterpret_cast<void*>(xboxVA), data, len);
}

void PatchU32(uint32_t xboxVA, uint32_t val) {
    *reinterpret_cast<uint32_t*>(xboxVA) = val;
}

void PatchRet(uint32_t xboxVA, uint16_t stackBytes, uint32_t retVal, bool setEax) {
    uint8_t* p = reinterpret_cast<uint8_t*>(xboxVA);
    size_t idx = 0;
    if (setEax) {
        if (retVal == 0) {
            p[idx++] = 0x33; p[idx++] = 0xC0; // xor eax, eax
        } else {
            p[idx++] = 0xB8; // mov eax, imm32
            *reinterpret_cast<uint32_t*>(p + idx) = retVal;
            idx += 4;
        }
    }
    if (stackBytes == 0) {
        p[idx++] = 0xC3; // ret
    } else {
        p[idx++] = 0xC2; // ret imm16
        *reinterpret_cast<uint16_t*>(p + idx) = stackBytes;
        idx += 2;
    }
}

// ============================================================================
// Path Translation & Xbox Vertex Shader Remapping
// ============================================================================

struct ShaderRemapEntry {
    const char* xboxSubstr;
    const char* pcFile;
};

static const ShaderRemapEntry kShaderRemaps[] = {
    { "SpecularRigid.xvu",       "DefaultRigid.xvu" },
    { "SpecularSkinned.xvu",     "DefaultSkinned.xvu" },
    { "SpecularSkinnedSoft.xvu", "DefaultSkinnedSoft.xvu" },
    { "shield0.xvu",             "shieldRigid0.xvu" },
    { "shield1.xvu",             "shieldRigid1.xvu" },
    { "shield2.xvu",             "shieldRigid2.xvu" },
    { "shield3.xvu",             "shieldRigid3.xvu" },
    { "ScreenUVScale.xvu",       "screenHaze.xvu" },
    { "heatshimmer.xvu",         "heatShimmer0.xvu" },
};

static void EnsureDirExists(const char* dirPath) {
    char tmp[MAX_PATH];
    strncpy(tmp, dirPath, sizeof(tmp) - 1);
    tmp[sizeof(tmp) - 1] = '\0';
    for (char* p = tmp + 3; *p; ++p) {
        if (*p == '\\' || *p == '/') {
            char ch = *p;
            *p = '\0';
            _mkdir(tmp);
            *p = ch;
        }
    }
    _mkdir(tmp);
}

void TranslateXboxPath(const char* xboxPath, char* outWinPath, size_t outMaxLen) {
    if (!xboxPath || !xboxPath[0]) {
        outWinPath[0] = '\0';
        return;
    }

    // Check if this is an Xbox vertex/pixel shader path: "...\VertexShaders\xbox\<name>"
    const char* vsXbox = strstr(xboxPath, "VertexShaders\\xbox\\");
    if (!vsXbox) vsXbox = strstr(xboxPath, "VertexShaders/xbox/");
    if (!vsXbox) vsXbox = strstr(xboxPath, "vertexshaders\\xbox\\");
    if (vsXbox) {
        const char* fileName = vsXbox + strlen("VertexShaders\\xbox\\");
        snprintf(outWinPath, outMaxLen, "%s\\VertexShaders\\XBox\\%s", g_gameRootDir, fileName);
        if (GetFileAttributesA(outWinPath) != INVALID_FILE_ATTRIBUTES) {
            return;
        }
        const char* mappedName = fileName;
        for (size_t i = 0; i < sizeof(kShaderRemaps) / sizeof(kShaderRemaps[0]); ++i) {
            if (_stricmp(fileName, kShaderRemaps[i].xboxSubstr) == 0) {
                mappedName = kShaderRemaps[i].pcFile;
                break;
            }
        }
        snprintf(outWinPath, outMaxLen, "%s\\VertexShaders\\%s", g_gameRootDir, mappedName);
        if (GetFileAttributesA(outWinPath) != INVALID_FILE_ATTRIBUTES) {
            return;
        }
        snprintf(outWinPath, outMaxLen, "%s\\VertexShaders\\XBox\\%s", g_gameRootDir, mappedName);
        return;
    }

    const char* rel = xboxPath;
    const char* subFolder = nullptr;

    // Handle NT object paths like "\Device\Harddisk0\Partition1\..." or "\??\D:\..."
    if (strncmp(rel, "\\??\\", 4) == 0) {
        rel += 4;
    }
    if ((rel[0] && rel[1] == ':')) {
        char drive = (char)tolower((unsigned char)rel[0]);
        rel += 2;
        while (*rel == '\\' || *rel == '/') ++rel;
        if (drive == 'd') {
            subFolder = nullptr;
        } else if (drive == 'u') {
            subFolder = "saves";
        } else if (drive == 't') {
            subFolder = "tdata";
        } else if (drive == 'z') {
            subFolder = "cache";
        } else if (drive >= 'f' && drive <= 'm') {
            subFolder = "mu";
        } else {
            // Already a Windows drive letter (e.g., C:\...)
            strncpy(outWinPath, xboxPath, outMaxLen - 1);
            outWinPath[outMaxLen - 1] = '\0';
            return;
        }
    } else {
        while (*rel == '\\' || *rel == '/') ++rel;
    }

    // Redirect engine .reslog files into cache\ so the original game directory remains 100% untouched
    if (!subFolder && rel) {
        const char* dot = strrchr(rel, '.');
        if (dot && _stricmp(dot, ".reslog") == 0) {
            subFolder = "cache";
        }
    }

    if (subFolder) {
        char baseDir[MAX_PATH];
        const char* saveRoot = g_appRootDir[0] ? g_appRootDir : g_gameRootDir;
        snprintf(baseDir, sizeof(baseDir), "%s\\%s", saveRoot, subFolder);
        EnsureDirExists(baseDir);
        if (*rel) {
            snprintf(outWinPath, outMaxLen, "%s\\%s", baseDir, rel);
        } else {
            snprintf(outWinPath, outMaxLen, "%s", baseDir);
        }
    } else {
        if (*rel) {
            snprintf(outWinPath, outMaxLen, "%s\\%s", g_gameRootDir, rel);
        } else {
            snprintf(outWinPath, outMaxLen, "%s", g_gameRootDir);
        }
    }

    for (char* p = outWinPath; *p; ++p) {
        if (*p == '/') *p = '\\';
    }

    // Fallback for 'Strings\game.nls' -> 'Misc\game.nls' if 'Strings\game.nls' does not exist
    if (strstr(outWinPath, "\\Strings\\game.nls") != nullptr && GetFileAttributesA(outWinPath) == INVALID_FILE_ATTRIBUTES) {
        snprintf(outWinPath, outMaxLen, "%s\\Misc\\game.nls", g_gameRootDir);
    }
}

// ============================================================================
// LIBCMT Thread-Local Storage (__getptd / __freeptd / __mtinit)
// ============================================================================

static DWORD g_crtTlsIndex = TLS_OUT_OF_INDEXES;

static void* InitThreadPtd() {
    uint8_t* ptd = (uint8_t*)calloc(1, 0x100);
    if (ptd) {
        *reinterpret_cast<uint32_t*>(ptd + 0x00) = GetCurrentThreadId();
        *reinterpret_cast<uint32_t*>(ptd + 0x04) = 0xFFFFFFFF;
        *reinterpret_cast<uint32_t*>(ptd + 0x14) = 1;          // _holdrand = 1
        *reinterpret_cast<uint32_t*>(ptd + 0x54) = 0x007D37E0; // __initmbctable
        TlsSetValue(g_crtTlsIndex, ptd);
    }
    return ptd;
}

static void* __cdecl Bridge_crt_getptd() {
    void* ptd = TlsGetValue(g_crtTlsIndex);
    if (!ptd) {
        ptd = InitThreadPtd();
    }
    return ptd;
}

static void __cdecl Bridge_crt_freeptd(void* ptd) {
    if (!ptd) {
        ptd = TlsGetValue(g_crtTlsIndex);
        if (ptd) {
            TlsSetValue(g_crtTlsIndex, nullptr);
        }
    }
    if (ptd) {
        free(ptd);
    }
}

static int __cdecl Bridge_crt_mtinit() {
    if (g_crtTlsIndex == TLS_OUT_OF_INDEXES) {
        g_crtTlsIndex = TlsAlloc();
    }
    // Initialize LIBCMT's internal _locktable via __mtinitlocks (0x00697500)
    typedef int (__cdecl *PFN_mtinitlocks)();
    reinterpret_cast<PFN_mtinitlocks>(0x00697500)();
    InitThreadPtd();
    return 1;
}

// ============================================================================
// Xbox 28-byte RTL_CRITICAL_SECTION Support
// ============================================================================

#pragma pack(push, 1)
struct XBOX_CRITICAL_SECTION {
    uint32_t Unknown[4];   // +0x00..+0x0F (Dispatcher header / Event)
    LONG     LockCount;    // +0x10 (-1 = unlocked, >= 0 = locked)
    LONG     RecursionCount; // +0x14
    uint32_t OwningThread; // +0x18
};
#pragma pack(pop)

// Note: On Xbox, statically-allocated critical sections in .data (like _XapiProcessLock)
// start with LockCount = -1, RecursionCount = 0, OwningThread = 0, whereas some zero-initialized
// structs may have LockCount == 0 && OwningThread == 0 before RtlInitializeCriticalSection.
static void __stdcall Bridge_RtlInitializeCriticalSection(XBOX_CRITICAL_SECTION* cs) {
    if (!cs) return;
    memset(cs, 0, sizeof(*cs));
    cs->LockCount = -1;
    cs->RecursionCount = 0;
    cs->OwningThread = 0;
}

static void __stdcall Bridge_RtlEnterCriticalSection(XBOX_CRITICAL_SECTION* cs) {
    if (!cs) return;
    uint32_t tid = GetCurrentThreadId();
    if (cs->OwningThread == tid && cs->RecursionCount > 0) {
        InterlockedIncrement(&cs->LockCount);
        cs->RecursionCount++;
        return;
    }
    // Handle lazily zero-initialized critical sections where LockCount == 0 && OwningThread == 0
    for (;;) {
        LONG prevOwner = InterlockedCompareExchange((volatile LONG*)&cs->OwningThread, (LONG)tid, 0);
        if (prevOwner == 0) {
            cs->RecursionCount = 1;
            cs->LockCount = 0;
            return;
        }
        if ((uint32_t)prevOwner == tid) {
            cs->RecursionCount++;
            InterlockedIncrement(&cs->LockCount);
            return;
        }
        SwitchToThread();
    }
}

static void __stdcall Bridge_RtlLeaveCriticalSection(XBOX_CRITICAL_SECTION* cs) {
    if (!cs) return;
    uint32_t tid = GetCurrentThreadId();
    if (cs->OwningThread != tid) return;
    if (--cs->RecursionCount <= 0) {
        cs->RecursionCount = 0;
        cs->LockCount = -1;
        InterlockedExchange((volatile LONG*)&cs->OwningThread, 0);
    } else {
        InterlockedDecrement(&cs->LockCount);
    }
}

// ============================================================================
// Xbox Kernel Data Exports & Functions (0x0074FE80..0x007500A0)
// ============================================================================

volatile uint32_t g_keTickCount = 0;

static uint8_t  g_launchDataPage[0x1000] = { 0 };
static uint8_t  g_xboxEEPROMKey[16]      = { 1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16 };
static uint8_t  g_xboxHDKey[16]          = { 16,15,14,13,12,11,10,9,8,7,6,5,4,3,2,1 };
static uint8_t  g_xboxSignatureKey[16]   = { 0xAA,0xBB,0xCC,0xDD,1,2,3,4,5,6,7,8,9,10,11,12 };
static uint8_t  g_xboxLANKey[16]         = { 0x11,0x22,0x33,0x44,5,6,7,8,9,10,11,12,13,14,15,16 };
static uint32_t g_idexChannelObj[32]     = { 0 };
static uint32_t g_halCachePartCount      = 3;
static uint32_t g_avPackStandard         = 6; // XC_AV_PACK_STANDARD
static uint16_t g_xboxKrnlVersion[4]     = { 1, 0, 5849, 1 };
static uint32_t g_dummyObjectType[16]    = { 0 };

struct XBOX_HARDWARE_INFO {
    uint32_t Flags;
    uint8_t  GpuRevision;
    uint8_t  McpRevision;
    uint8_t  Reserved[2];
};
static XBOX_HARDWARE_INFO g_xboxHwInfo = { 0, 0xA3, 0xD4, { 0, 0 } };

struct XBOX_OBJECT_STRING {
    uint16_t Length;
    uint16_t MaximumLength;
    char*    Buffer;
};
static char g_xeImageNameBuf[] = "d:\\Ghost.xbe";
static XBOX_OBJECT_STRING g_xeImageFileName = {
    (uint16_t)(sizeof(g_xeImageNameBuf) - 1),
    (uint16_t)sizeof(g_xeImageNameBuf),
    g_xeImageNameBuf
};
static char g_diskModelBuf[] = "ST310014ACE";
static XBOX_OBJECT_STRING g_halDiskModelNumber = {
    (uint16_t)(sizeof(g_diskModelBuf) - 1),
    (uint16_t)sizeof(g_diskModelBuf),
    g_diskModelBuf
};
static char g_diskSerialBuf[] = "3HW0ABCD";
static XBOX_OBJECT_STRING g_halDiskSerialNumber = {
    (uint16_t)(sizeof(g_diskSerialBuf) - 1),
    (uint16_t)sizeof(g_diskSerialBuf),
    g_diskSerialBuf
};
static uint8_t g_xePublicKeyData[284] = { 0 };

void UpdateXboxKernelTick() {
    g_keTickCount = GetTickCount();
}

static int32_t __stdcall Krnl_ExQueryNonVolatileSetting(
    uint32_t ValueIndex, uint32_t* Type, void* Value, uint32_t ValueLength, uint32_t* ResultLength)
{
    if (Type) *Type = 4; // REG_DWORD
    if (ResultLength) *ResultLength = 4;
    if (Value && ValueLength >= 4) {
        uint32_t v = 0;
        switch (ValueIndex) {
            case 0x0007: v = 1; break; // XC_LANGUAGE -> 1 (English)
            case 0x0008: v = 1; break; // XC_VIDEO_STANDARD -> NTSC_M
            case 0x0009: v = 0; break; // XC_AUDIO_FLAGS -> Stereo
            case 0x0104: v = 1; break; // XC_GAME_REGION -> NA
            default:     v = 0; break;
        }
        *reinterpret_cast<uint32_t*>(Value) = v;
    }
    return 0; // STATUS_SUCCESS
}

static void __stdcall Krnl_DbgBreakPoint() {
}

static int __cdecl Krnl_DbgPrint(const char* fmt, ...) {
    if (!fmt) return 0;
    char buf[1024];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf) - 1, fmt, args);
    va_end(args);
    buf[sizeof(buf) - 1] = '\0';
    size_t len = strlen(buf);
    while (len > 0 && (buf[len - 1] == '\n' || buf[len - 1] == '\r')) {
        buf[--len] = '\0';
    }
    return (int)len;
}

static uint64_t __stdcall Krnl_KeQueryPerformanceCounter() {
    LARGE_INTEGER li;
    QueryPerformanceCounter(&li);
    return (uint64_t)li.QuadPart;
}

static uint64_t __stdcall Krnl_KeQueryPerformanceFrequency() {
    LARGE_INTEGER li;
    QueryPerformanceFrequency(&li);
    return (uint64_t)li.QuadPart;
}

static void __stdcall Krnl_KeQuerySystemTime(FILETIME* outTime) {
    if (outTime) GetSystemTimeAsFileTime(outTime);
}

static uint8_t __fastcall Krnl_KfRaiseIrql(uint8_t newIrql) {
    (void)newIrql;
    return 0;
}

static void __fastcall Krnl_KfLowerIrql(uint8_t oldIrql) {
    (void)oldIrql;
}

static uint8_t __stdcall Krnl_KeRaiseIrqlToDpcLevel() {
    return 0;
}

static int32_t __stdcall Krnl_KeSaveFloatingPointState(void* pFloatSave) {
    (void)pFloatSave;
    return 0;
}

static int32_t __stdcall Krnl_KeRestoreFloatingPointState(void* pFloatSave) {
    (void)pFloatSave;
    return 0;
}

// Kernel DPC & Timer support (used by XACT::CEngine)
struct XBOX_KDPC {
    uint16_t Type;
    uint8_t  Inserted;
    uint8_t  Padding;
    uint32_t DpcListEntry[2];
    void (__stdcall *DeferredRoutine)(XBOX_KDPC*, void*, void*, void*);
    void*    DeferredContext;
    void*    SystemArgument1;
    void*    SystemArgument2;
};

struct XBOX_KTIMER {
    uint8_t  Header[16];
    uint64_t DueTime;
    uint32_t TimerListEntry[2];
    XBOX_KDPC* Dpc;
    int32_t  Period;
};

static XBOX_KTIMER* g_activeXactTimer = nullptr;

static void __stdcall Krnl_KeInitializeDpc(XBOX_KDPC* Dpc, void* DeferredRoutine, void* DeferredContext) {
    if (!Dpc) return;
    memset(Dpc, 0, sizeof(*Dpc));
    Dpc->Type = 19; // DpcObject
    Dpc->DeferredRoutine = reinterpret_cast<void(__stdcall*)(XBOX_KDPC*, void*, void*, void*)>(DeferredRoutine);
    Dpc->DeferredContext = DeferredContext;
}

static void __stdcall Krnl_KeInitializeTimerEx(XBOX_KTIMER* Timer, uint32_t Type) {
    if (!Timer) return;
    memset(Timer, 0, sizeof(*Timer));
    Timer->Header[0] = (uint8_t)(8 + Type);
}

static uint8_t __stdcall Krnl_KeSetTimerEx(XBOX_KTIMER* Timer, uint32_t DueLow, int32_t DueHigh, int32_t Period, XBOX_KDPC* Dpc) {
    (void)DueLow; (void)DueHigh;
    if (Timer) {
        Timer->Dpc = Dpc;
        Timer->Period = Period;
        if (Dpc && Dpc->DeferredRoutine) {
            g_activeXactTimer = Timer;
        }
    }
    return 0;
}

static uint8_t __stdcall Krnl_KeSetTimer(XBOX_KTIMER* Timer, uint32_t DueLow, int32_t DueHigh, XBOX_KDPC* Dpc) {
    return Krnl_KeSetTimerEx(Timer, DueLow, DueHigh, 0, Dpc);
}

static uint8_t __stdcall Krnl_KeCancelTimer(XBOX_KTIMER* Timer) {
    if (Timer == g_activeXactTimer) {
        g_activeXactTimer = nullptr;
    }
    return 1;
}

// Kernel Memory Allocators
static void* __stdcall Krnl_ExAllocatePool(uint32_t NumberOfBytes) {
    uint32_t sz = NumberOfBytes ? NumberOfBytes : 4;
    void* p = malloc(sz);
    if (p) memset(p, 0, sz);
    return p;
}

static void* __stdcall Krnl_ExAllocatePoolWithTag(uint32_t NumberOfBytes, uint32_t Tag) {
    (void)Tag;
    return Krnl_ExAllocatePool(NumberOfBytes);
}

static uint32_t __stdcall Krnl_ExQueryPoolBlockSize(void* P) {
    if (!P || (uintptr_t)P < 0x00960000) return 0;
    return (uint32_t)_msize(P);
}

static void __stdcall Krnl_ExFreePool(void* P) {
    if (P && (uintptr_t)P >= 0x00960000) {
        free(P);
    }
}

struct PhysHeader {
    uint32_t magic;
    uint32_t size;
    void*    rawPtr;
};

static void* AllocAlignedTracked(uint32_t size, uint32_t alignment) {
    if (alignment < 4096) alignment = 4096;
    if (size == 0) size = 4096;
    size_t total = size + alignment + sizeof(PhysHeader);
    uint8_t* raw = (uint8_t*)calloc(1, total);
    if (!raw) return nullptr;
    uintptr_t aligned = ((uintptr_t)raw + sizeof(PhysHeader) + alignment - 1) & ~((uintptr_t)alignment - 1);
    PhysHeader* hdr = reinterpret_cast<PhysHeader*>(aligned - sizeof(PhysHeader));
    hdr->magic = 0x58504859; // 'XPHY'
    hdr->size = size;
    hdr->rawPtr = raw;
    return reinterpret_cast<void*>(aligned);
}

static void FreeAlignedTracked(void* ptr) {
    if (!ptr || (uintptr_t)ptr < 0x00960000) return;
    PhysHeader* hdr = reinterpret_cast<PhysHeader*>((uintptr_t)ptr - sizeof(PhysHeader));
    if (hdr->magic == 0x58504859) {
        hdr->magic = 0;
        free(hdr->rawPtr);
    }
}

static uint32_t SizeAlignedTracked(void* ptr) {
    if (!ptr || (uintptr_t)ptr < 0x00960000) return 0;
    PhysHeader* hdr = reinterpret_cast<PhysHeader*>((uintptr_t)ptr - sizeof(PhysHeader));
    if (hdr->magic == 0x58504859) {
        return hdr->size;
    }
    return 0;
}

static void* __stdcall Krnl_MmAllocateContiguousMemory(uint32_t NumberOfBytes) {
    return AllocAlignedTracked(NumberOfBytes, 4096);
}

static void* __stdcall Krnl_MmAllocateContiguousMemoryEx(
    uint32_t NumberOfBytes, uint32_t LowestAcceptableAddress, uint32_t HighestAcceptableAddress,
    uint32_t Alignment, uint32_t ProtectionType)
{
    (void)LowestAcceptableAddress; (void)HighestAcceptableAddress; (void)ProtectionType;
    return AllocAlignedTracked(NumberOfBytes, Alignment ? Alignment : 4096);
}

static void __stdcall Krnl_MmFreeContiguousMemory(void* BaseAddress) {
    FreeAlignedTracked(BaseAddress);
}

static uint32_t __stdcall Krnl_MmQueryAllocationSize(void* BaseAddress) {
    return SizeAlignedTracked(BaseAddress);
}

static void __stdcall Krnl_MmPersistContiguousMemory(void* BaseAddress, uint32_t NumberOfBytes, uint32_t Persist) {
    (void)BaseAddress; (void)NumberOfBytes; (void)Persist;
}

static void* __stdcall Krnl_MmGetPhysicalAddress(void* BaseAddress) {
    return BaseAddress;
}

static uint32_t __stdcall Krnl_MmQueryAddressProtect(void* VirtualAddress) {
    return VirtualAddress != nullptr ? PAGE_READWRITE : 0;
}

static void __stdcall Krnl_MmSetAddressProtect(void* BaseAddress, uint32_t NumberOfBytes, uint32_t NewProtect) {
    (void)BaseAddress; (void)NumberOfBytes; (void)NewProtect;
}

struct XBOX_MM_STATISTICS {
    uint32_t Length;
    uint32_t TotalPhysicalPages;
    uint32_t AvailablePages;
    uint32_t VirtualMemoryBytesCommitted;
    uint32_t VirtualMemoryBytesReserved;
    uint32_t CachePagesCommitted;
    uint32_t PoolPagesCommitted;
    uint32_t StackPagesCommitted;
    uint32_t ImagePagesCommitted;
};

static int32_t __stdcall Krnl_MmQueryStatistics(XBOX_MM_STATISTICS* Stats) {
    if (Stats) {
        Stats->TotalPhysicalPages = 16384; // 64 MB
        Stats->AvailablePages = 12288;     // 48 MB free
        Stats->VirtualMemoryBytesCommitted = 16 * 1024 * 1024;
        Stats->VirtualMemoryBytesReserved = 16 * 1024 * 1024;
        Stats->CachePagesCommitted = 128;
        Stats->PoolPagesCommitted = 128;
        Stats->StackPagesCommitted = 64;
        Stats->ImagePagesCommitted = 1300;
    }
    return 0;
}

static int32_t __stdcall Krnl_NtAllocateVirtualMemory(
    void** BaseAddress, uint32_t ZeroBits, uint32_t* AllocationSize, uint32_t AllocationType, uint32_t Protect)
{
    (void)ZeroBits;
    if (!BaseAddress || !AllocationSize) return -1;
    void* p = VirtualAlloc(*BaseAddress, *AllocationSize, AllocationType & (MEM_RESERVE | MEM_COMMIT | MEM_TOP_DOWN), Protect ? Protect : PAGE_READWRITE);
    if (!p) return -1;
    *BaseAddress = p;
    return 0;
}

static int32_t __stdcall Krnl_NtFreeVirtualMemory(void** BaseAddress, uint32_t* FreeSize, uint32_t FreeType) {
    if (!BaseAddress || !*BaseAddress) return -1;
    VirtualFree(*BaseAddress, FreeSize ? *FreeSize : 0, FreeType);
    return 0;
}

// Crypto functions (XcSHA*, XcHMAC, etc.)
static void __stdcall Krnl_XcSHAInit(uint8_t* pbSHAContext) {
    if (pbSHAContext) memset(pbSHAContext, 0, 116);
}

static void __stdcall Krnl_XcSHAUpdate(uint8_t* pbSHAContext, const uint8_t* pbInput, uint32_t dwInputLength) {
    if (!pbSHAContext || !pbInput) return;
    uint32_t* h = reinterpret_cast<uint32_t*>(pbSHAContext);
    for (uint32_t i = 0; i < dwInputLength; ++i) {
        h[i & 7] = (h[i & 7] * 33u) ^ pbInput[i];
    }
}

static void __stdcall Krnl_XcSHAFinal(uint8_t* pbSHAContext, uint8_t* pbDigest) {
    if (pbDigest && pbSHAContext) {
        memcpy(pbDigest, pbSHAContext, 20);
    }
}

static void __stdcall Krnl_XcHMAC(
    const uint8_t* pbKey, uint32_t dwKeyLength,
    const uint8_t* pbInput, uint32_t dwInputLength,
    const uint8_t* pbInput2, uint32_t dwInputLength2,
    uint8_t* pbDigest)
{
    uint8_t ctx[128] = { 0 };
    Krnl_XcSHAUpdate(ctx, pbKey, dwKeyLength);
    Krnl_XcSHAUpdate(ctx, pbInput, dwInputLength);
    Krnl_XcSHAUpdate(ctx, pbInput2, dwInputLength2);
    if (pbDigest) memcpy(pbDigest, ctx, 20);
}

static uint32_t __stdcall Krnl_XcVerifyPKCS1Signature(void* a, void* b, void* c) {
    (void)a; (void)b; (void)c;
    return 1;
}

// AV functions
static void* g_avSavedDataAddress = nullptr;
static void* __stdcall Krnl_AvGetSavedDataAddress() { return g_avSavedDataAddress; }
static void __stdcall Krnl_AvSetSavedDataAddress(void* addr) { g_avSavedDataAddress = addr; }
static void __stdcall Krnl_AvSendTVEncoderOption(void* a, uint32_t opt, uint32_t param, uint32_t* res) {
    (void)a; (void)opt; (void)param;
    if (res) *res = 0;
}
static uint32_t __stdcall Krnl_AvSetDisplayMode(void* a, uint32_t b, uint32_t c, uint32_t d, uint32_t e, void* f) {
    (void)a; (void)b; (void)c; (void)d; (void)e; (void)f;
    return 0;
}

// Generic kernel stubs by arg count
static uint32_t __stdcall Krnl_Stub0() { return 0; }
static uint32_t __stdcall Krnl_Stub1(uint32_t) { return 0; }
static uint32_t __stdcall Krnl_Stub2(uint32_t, uint32_t) { return 0; }
static uint32_t __stdcall Krnl_Stub3(uint32_t, uint32_t, uint32_t) { return 0; }
static uint32_t __stdcall Krnl_Stub4(uint32_t, uint32_t, uint32_t, uint32_t) { return 0; }
static uint32_t __stdcall Krnl_Stub5(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t) { return 0; }
static uint32_t __stdcall Krnl_Stub6(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t) { return 0; }
static uint32_t __stdcall Krnl_Stub7(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t) { return 0; }
static uint32_t __stdcall Krnl_Stub8(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t) { return 0; }
static uint32_t __stdcall Krnl_Stub9(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t) { return 0; }
static uint32_t __stdcall Krnl_Stub10(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t) { return 0; }
static void     __fastcall Krnl_FastStub1(uint32_t) {}
static void     __fastcall Krnl_FastStub2(uint32_t, uint32_t) {}
static int32_t  __stdcall Krnl_XeLoadUnloadSection(uint32_t* pSec) {
    if (pSec && (uintptr_t)pSec >= 0x00400000 && (uintptr_t)pSec < 0x00950000) {
        pSec[5] = 1; // SectionReferenceCount > 0
    }
    return 0;
}

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

static BOOL __stdcall Bridge_CloseHandle(HANDLE hObject) {
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
    char     szSaveGameDirectory[MAX_PATH];
    wchar_t  szSaveGameName[kMaxXboxSaveGameNameChars];
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

// XACT Engine DoWork hook: invokes the registered XACT DPC if active and runs CEngine::DoWork
static void __stdcall Bridge_XACTEngineDoWork() {
    typedef int (__stdcall *PFN_XACTEnterCS)();
    typedef void (__stdcall *PFN_DSDoWork)();
    typedef void (__thiscall *PFN_CEngineDoWork)(void* thisPtr);

    int entered = reinterpret_cast<PFN_XACTEnterCS>(0x006FE3E0)();
    void* pEngine = *reinterpret_cast<void**>(0x00708F74); // _g_pXACTEngine
    if (pEngine) {
        reinterpret_cast<PFN_DSDoWork>(0x006E1A58)();          // _DirectSoundDoWork@0
        reinterpret_cast<PFN_CEngineDoWork>(0x00700056)(pEngine); // ?DoWork@CEngine@XACT@@QAEXXZ
    }
    if (entered) {
        Bridge_RtlLeaveCriticalSection(reinterpret_cast<XBOX_CRITICAL_SECTION*>(0x00708F58));
    }
    if (g_activeXactTimer && g_activeXactTimer->Dpc && g_activeXactTimer->Dpc->DeferredRoutine) {
        XBOX_KDPC* dpc = g_activeXactTimer->Dpc;
        dpc->DeferredRoutine(dpc, dpc->DeferredContext, dpc->SystemArgument1, dpc->SystemArgument2);
    }
}

// ============================================================================
// Initialize Kernel IAT & XAPILIB Bridges
// ============================================================================

void InitXboxKernelAndXapi() {
    InitializeCriticalSection(&g_findCs);
    UpdateXboxKernelTick();

    // Initialize self-referential empty LIST_ENTRY inside g_idexChannelObj (+0x28) for 0x0074FF20
    g_idexChannelObj[0x28 / 4] = reinterpret_cast<uint32_t>(&g_idexChannelObj[0x28 / 4]);
    g_idexChannelObj[0x2C / 4] = reinterpret_cast<uint32_t>(&g_idexChannelObj[0x28 / 4]);

    // 1. Initialize all 137 xboxkrnl.exe IAT slots (0x0074FE80..0x007500A0) with safe defaults first
    for (uint32_t addr = 0x0074FE80; addr <= 0x007500A0; addr += 4) {
        PatchU32(addr, reinterpret_cast<uint32_t>(&Krnl_Stub0));
    }

    // 2. Populate exact xboxkrnl.exe IAT slots (100% verified against Ghost.exe __imp__ symbols)
    PatchU32(0x0074FE80, reinterpret_cast<uint32_t>(&Krnl_ExQueryNonVolatileSetting)); // ord  24: __imp__ExQueryNonVolatileSetting@20
    PatchU32(0x0074FE84, reinterpret_cast<uint32_t>(&Krnl_DbgBreakPoint));             // ord   5: __imp__DbgBreakPoint@0
    PatchU32(0x0074FE88, reinterpret_cast<uint32_t>(&Krnl_KeQuerySystemTime));         // ord 128: __imp__KeQuerySystemTime@4
    PatchU32(0x0074FE8C, reinterpret_cast<uint32_t>(&Krnl_MmPersistContiguousMemory)); // ord 178: __imp__MmPersistContiguousMemory@12
    PatchU32(0x0074FE90, reinterpret_cast<uint32_t>(&Krnl_MmAllocateContiguousMemory));// ord 165: __imp__MmAllocateContiguousMemory@4
    PatchU32(0x0074FE94, reinterpret_cast<uint32_t>(g_launchDataPage));                // ord 164: _LaunchDataPage (DATA)
    PatchU32(0x0074FE98, reinterpret_cast<uint32_t>(&Krnl_MmFreeContiguousMemory));    // ord 171: __imp__MmFreeContiguousMemory@4
    PatchU32(0x0074FE9C, reinterpret_cast<uint32_t>(&Krnl_Stub1));                     // ord 301: __imp__RtlNtStatusToDosError@4
    PatchU32(0x0074FEA0, reinterpret_cast<uint32_t>(&Krnl_Stub1));                     // ord  49: __imp__HalReturnToFirmware@4
    PatchU32(0x0074FEA4, reinterpret_cast<uint32_t>(&Krnl_Stub5));                     // ord 226: __imp__NtSetInformationFile@20
    PatchU32(0x0074FEA8, reinterpret_cast<uint32_t>(&Krnl_Stub9));                     // ord 190: __imp__NtCreateFile@36
    PatchU32(0x0074FEAC, reinterpret_cast<uint32_t>(&Krnl_Stub2));                     // ord 289: __imp__RtlInitAnsiString@8
    PatchU32(0x0074FEB0, reinterpret_cast<uint32_t>(&Krnl_XcSHAUpdate));               // ord 336: __imp__XcSHAUpdate@12
    PatchU32(0x0074FEB4, reinterpret_cast<uint32_t>(&Krnl_XcSHAInit));                 // ord 335: __imp__XcSHAInit@4
    PatchU32(0x0074FEB8, reinterpret_cast<uint32_t>(&Krnl_XcSHAFinal));                // ord 337: __imp__XcSHAFinal@8
    PatchU32(0x0074FEBC, reinterpret_cast<uint32_t>(g_xboxHDKey));                     // ord 323: _XboxHDKey (DATA)
    PatchU32(0x0074FEC0, reinterpret_cast<uint32_t>(g_xboxSignatureKey));              // ord 325: __imp__XboxSignatureKey (DATA)
    PatchU32(0x0074FEC4, reinterpret_cast<uint32_t>(g_xboxEEPROMKey));                 // ord 354: __imp__XboxAlternateSignatureKeys (DATA)
    PatchU32(0x0074FEC8, reinterpret_cast<uint32_t>(&Bridge_CloseHandle));             // ord 187: __imp__NtClose@4
    PatchU32(0x0074FECC, reinterpret_cast<uint32_t>(&Krnl_Stub3));                     // ord 233: __imp__NtWaitForSingleObject@12
    PatchU32(0x0074FED0, reinterpret_cast<uint32_t>(&Krnl_Stub8));                     // ord 219: __imp__NtReadFile@32
    PatchU32(0x0074FED4, reinterpret_cast<uint32_t>(&Krnl_Stub8));                     // ord 236: __imp__NtWriteFile@32
    PatchU32(0x0074FED8, reinterpret_cast<uint32_t>(&Krnl_Stub5));                     // ord 211: __imp__NtQueryInformationFile@20
    PatchU32(0x0074FEDC, reinterpret_cast<uint32_t>(&Krnl_Stub5));                     // ord 218: __imp__NtQueryVolumeInformationFile@20
    PatchU32(0x0074FEE0, reinterpret_cast<uint32_t>(&Krnl_Stub2));                     // ord 198: __imp__NtFlushBuffersFile@8
    PatchU32(0x0074FEE4, reinterpret_cast<uint32_t>(&Krnl_Stub10));                    // ord 200: __imp__NtFsControlFile@40
    PatchU32(0x0074FEE8, reinterpret_cast<uint32_t>(&Krnl_Stub10));                    // ord 196: __imp__NtDeviceIoControlFile@40
    PatchU32(0x0074FEEC, reinterpret_cast<uint32_t>(&Krnl_FastStub1));                 // ord 250: __imp_@ObfDereferenceObject@4
    PatchU32(0x0074FEF0, reinterpret_cast<uint32_t>(&Krnl_Stub3));                     // ord 246: __imp__ObReferenceObjectByHandle@12
    PatchU32(0x0074FEF4, reinterpret_cast<uint32_t>(g_dummyObjectType));               // ord  71: __imp__IoFileObjectType (DATA)
    PatchU32(0x0074FEF8, reinterpret_cast<uint32_t>(&Krnl_Stub6));                     // ord 202: __imp__NtOpenFile@24
    PatchU32(0x0074FEFC, reinterpret_cast<uint32_t>(&Krnl_Stub2));                     // ord 305: __imp__RtlTimeToTimeFields@8
    PatchU32(0x0074FF00, reinterpret_cast<uint32_t>(&g_keTickCount));                  // ord 156: __imp__KeTickCount (DATA)
    PatchU32(0x0074FF04, reinterpret_cast<uint32_t>(&Krnl_Stub2));                     // ord 304: __imp__RtlTimeFieldsToTime@8
    PatchU32(0x0074FF08, reinterpret_cast<uint32_t>(&Krnl_Stub1));                     // ord  37: __imp__FscSetCacheSize@4
    PatchU32(0x0074FF0C, reinterpret_cast<uint32_t>(&Krnl_Stub10));                    // ord 207: __imp__NtQueryDirectoryFile@40
    PatchU32(0x0074FF10, reinterpret_cast<uint32_t>(&Krnl_FastStub2));                 // ord  87: __imp_@IofCompleteRequest@8
    PatchU32(0x0074FF14, reinterpret_cast<uint32_t>(&Krnl_Stub3));                     // ord 119: __imp__KeInsertQueueDpc@12
    PatchU32(0x0074FF18, reinterpret_cast<uint32_t>(&Krnl_KeInitializeDpc));           // ord 107: __imp__KeInitializeDpc@12
    PatchU32(0x0074FF1C, reinterpret_cast<uint32_t>(&Krnl_KfLowerIrql));               // ord 161: __imp_@KfLowerIrql@4
    PatchU32(0x0074FF20, reinterpret_cast<uint32_t>(g_idexChannelObj));                // ord 357: _IdexChannelObject (DATA)
    PatchU32(0x0074FF24, reinterpret_cast<uint32_t>(&Krnl_KeRaiseIrqlToDpcLevel));     // ord 129: __imp__KeRaiseIrqlToDpcLevel@0
    PatchU32(0x0074FF28, reinterpret_cast<uint32_t>(&g_halCachePartCount));            // ord  40: __imp__HalDiskCachePartitionCount (DATA)
    PatchU32(0x0074FF2C, reinterpret_cast<uint32_t>(&Krnl_Stub2));                     // ord  67: __imp__IoCreateSymbolicLink@8
    PatchU32(0x0074FF30, reinterpret_cast<uint32_t>(&Krnl_Stub3));                     // ord 215: __imp__NtQuerySymbolicLinkObject@12
    PatchU32(0x0074FF34, reinterpret_cast<uint32_t>(&Krnl_Stub2));                     // ord 203: __imp__NtOpenSymbolicLinkObject@8
    PatchU32(0x0074FF38, reinterpret_cast<uint32_t>(&Krnl_Stub1));                     // ord  69: __imp__IoDeleteSymbolicLink@4
    PatchU32(0x0074FF3C, reinterpret_cast<uint32_t>(&Krnl_MmAllocateContiguousMemoryEx)); // ord 166: __imp__MmAllocateContiguousMemoryEx@20
    PatchU32(0x0074FF40, reinterpret_cast<uint32_t>(&Krnl_MmQueryAllocationSize));     // ord 180: __imp__MmQueryAllocationSize@4
    PatchU32(0x0074FF44, reinterpret_cast<uint32_t>(&Krnl_MmSetAddressProtect));       // ord 182: __imp__MmSetAddressProtect@12
    PatchU32(0x0074FF48, reinterpret_cast<uint32_t>(&Krnl_MmQueryAddressProtect));     // ord 179: __imp__MmQueryAddressProtect@4
    PatchU32(0x0074FF4C, reinterpret_cast<uint32_t>(&Krnl_Stub4));                     // ord 189: __imp__NtCreateEvent@16
    PatchU32(0x0074FF50, reinterpret_cast<uint32_t>(g_dummyObjectType));               // ord  16: _ExEventObjectType (DATA)
    PatchU32(0x0074FF54, reinterpret_cast<uint32_t>(&Krnl_Stub2));                     // ord 225: __imp__NtSetEvent@8
    PatchU32(0x0074FF58, reinterpret_cast<uint32_t>(&Krnl_Stub3));                     // ord 192: __imp__NtCreateMutant@12
    PatchU32(0x0074FF5C, reinterpret_cast<uint32_t>(&Krnl_Stub2));                     // ord 221: __imp__NtReleaseMutant@8
    PatchU32(0x0074FF60, reinterpret_cast<uint32_t>(&Krnl_Stub4));                     // ord 234: __imp__NtWaitForSingleObjectEx@16
    PatchU32(0x0074FF64, reinterpret_cast<uint32_t>(&Krnl_Stub6));                     // ord 235: __imp__NtWaitForMultipleObjectsEx@24
    PatchU32(0x0074FF68, reinterpret_cast<uint32_t>(&Krnl_Stub3));                     // ord  99: __imp__KeDelayExecutionThread@12
    PatchU32(0x0074FF6C, reinterpret_cast<uint32_t>(&Bridge_RtlLeaveCriticalSection)); // ord 294: __imp__RtlLeaveCriticalSection@4
    PatchU32(0x0074FF70, reinterpret_cast<uint32_t>(&Bridge_RtlEnterCriticalSection)); // ord 277: __imp__RtlEnterCriticalSection@4
    PatchU32(0x0074FF74, reinterpret_cast<uint32_t>(&Krnl_Stub2));                     // ord 224: __imp__NtResumeThread@8
    PatchU32(0x0074FF78, reinterpret_cast<uint32_t>(&Krnl_Stub1));                     // ord 302: __imp__RtlRaiseException@4
    PatchU32(0x0074FF7C, reinterpret_cast<uint32_t>(&Krnl_Stub1));                     // ord 258: __imp__PsTerminateSystemThread@4
    PatchU32(0x0074FF80, reinterpret_cast<uint32_t>(&Krnl_Stub10));                    // ord 255: __imp__PsCreateSystemThreadEx@40
    PatchU32(0x0074FF84, reinterpret_cast<uint32_t>(&g_xeImageFileName));              // ord 326: __imp__XeImageFileName (DATA)
    PatchU32(0x0074FF88, reinterpret_cast<uint32_t>(&Krnl_XcVerifyPKCS1Signature));    // ord 344: __imp__XcVerifyPKCS1Signature@12
    PatchU32(0x0074FF8C, reinterpret_cast<uint32_t>(g_xePublicKeyData));               // ord 355: _XePublicKeyData (DATA)
    PatchU32(0x0074FF90, reinterpret_cast<uint32_t>(&g_xboxHwInfo));                   // ord 322: _XboxHardwareInfo (DATA)
    PatchU32(0x0074FF94, reinterpret_cast<uint32_t>(&Krnl_Stub2));                     // ord  47: __imp__HalRegisterShutdownNotification@8
    PatchU32(0x0074FF98, reinterpret_cast<uint32_t>(g_xboxKrnlVersion));               // ord 324: __imp__XboxKrnlVersion (DATA)
    PatchU32(0x0074FF9C, reinterpret_cast<uint32_t>(&Krnl_XcHMAC));                    // ord 340: __imp__XcHMAC@28
    PatchU32(0x0074FFA0, reinterpret_cast<uint32_t>(&Krnl_Stub3));                     // ord 279: __imp__RtlEqualString@12
    PatchU32(0x0074FFA4, reinterpret_cast<uint32_t>(&Krnl_XeLoadUnloadSection));       // ord 327: __imp__XeLoadSection@4
    PatchU32(0x0074FFA8, reinterpret_cast<uint32_t>(&Krnl_XeLoadUnloadSection));       // ord 328: __imp__XeUnloadSection@4
    PatchU32(0x0074FFAC, reinterpret_cast<uint32_t>(&Krnl_NtFreeVirtualMemory));       // ord 199: __imp__NtFreeVirtualMemory@12
    PatchU32(0x0074FFB0, reinterpret_cast<uint32_t>(&Krnl_NtAllocateVirtualMemory));   // ord 184: __imp__NtAllocateVirtualMemory@20
    PatchU32(0x0074FFB4, reinterpret_cast<uint32_t>(&Krnl_Stub3));                     // ord 269: __imp__RtlCompareMemoryUlong@12
    PatchU32(0x0074FFB8, reinterpret_cast<uint32_t>(&Bridge_RtlInitializeCriticalSection)); // ord 291: __imp__RtlInitializeCriticalSection@4
    PatchU32(0x0074FFBC, reinterpret_cast<uint32_t>(&Krnl_Stub2));                     // ord 217: __imp__NtQueryVirtualMemory@8
    PatchU32(0x0074FFC0, reinterpret_cast<uint32_t>(&Krnl_KeSetTimer));                // ord 149: __imp__KeSetTimer@16
    PatchU32(0x0074FFC4, reinterpret_cast<uint32_t>(&Krnl_Stub0));                     // ord 360: __imp__HalInitiateShutdown@0
    PatchU32(0x0074FFC8, reinterpret_cast<uint32_t>(&Krnl_KeInitializeTimerEx));       // ord 113: __imp__KeInitializeTimerEx@8
    PatchU32(0x0074FFCC, reinterpret_cast<uint32_t>(&Krnl_Stub3));                     // ord 145: __imp__KeSetEvent@12
    PatchU32(0x0074FFD0, reinterpret_cast<uint32_t>(&Krnl_Stub5));                     // ord 159: __imp__KeWaitForSingleObject@20
    PatchU32(0x0074FFD4, reinterpret_cast<uint32_t>(&Krnl_AvGetSavedDataAddress));     // ord   1: __imp__AvGetSavedDataAddress@0
    PatchU32(0x0074FFD8, reinterpret_cast<uint32_t>(&Krnl_AvSendTVEncoderOption));     // ord   2: __imp__AvSendTVEncoderOption@16
    PatchU32(0x0074FFDC, reinterpret_cast<uint32_t>(&Krnl_AvSetDisplayMode));          // ord   3: __imp__AvSetDisplayMode@24
    PatchU32(0x0074FFE0, reinterpret_cast<uint32_t>(&Krnl_AvSetSavedDataAddress));     // ord   4: __imp__AvSetSavedDataAddress@4
    PatchU32(0x0074FFE4, reinterpret_cast<uint32_t>(&Krnl_Stub6));                     // ord  46: __imp__HalReadWritePCISpace@24
    PatchU32(0x0074FFE8, reinterpret_cast<uint32_t>(&Krnl_Stub1));                     // ord  98: __imp__KeConnectInterrupt@4
    PatchU32(0x0074FFEC, reinterpret_cast<uint32_t>(&Krnl_Stub7));                     // ord 109: __imp__KeInitializeInterrupt@28
    PatchU32(0x0074FFF0, reinterpret_cast<uint32_t>(&Krnl_Stub2));                     // ord  44: __imp__HalGetInterruptVector@8
    PatchU32(0x0074FFF4, reinterpret_cast<uint32_t>(&Krnl_Stub1));                     // ord 100: __imp__KeDisconnectInterrupt@4
    PatchU32(0x0074FFF8, reinterpret_cast<uint32_t>(&Krnl_DbgPrint));                  // ord   8: __imp__DbgPrint
    PatchU32(0x0074FFFC, reinterpret_cast<uint32_t>(&Krnl_Stub2));                     // ord 168: __imp__MmClaimGpuInstanceMemory@8
    PatchU32(0x00750000, reinterpret_cast<uint32_t>(&Krnl_Stub1));                     // ord 151: __imp__KeStallExecutionProcessor@4
    PatchU32(0x00750004, reinterpret_cast<uint32_t>(&Krnl_KfRaiseIrql));               // ord 160: __imp_@KfRaiseIrql@4
    PatchU32(0x00750008, reinterpret_cast<uint32_t>(&Krnl_KeSaveFloatingPointState));  // ord 142: __imp__KeSaveFloatingPointState@4
    PatchU32(0x0075000C, reinterpret_cast<uint32_t>(&Krnl_KeRestoreFloatingPointState));// ord 139: __imp__KeRestoreFloatingPointState@4
    PatchU32(0x00750010, reinterpret_cast<uint32_t>(&Krnl_ExQueryPoolBlockSize));      // ord  23: __imp__ExQueryPoolBlockSize@4
    PatchU32(0x00750014, reinterpret_cast<uint32_t>(&Krnl_ExAllocatePoolWithTag));     // ord  15: __imp__ExAllocatePoolWithTag@8
    PatchU32(0x00750018, reinterpret_cast<uint32_t>(&Krnl_ExFreePool));                // ord  17: __imp__ExFreePool@4
    PatchU32(0x0075001C, reinterpret_cast<uint32_t>(&Krnl_Stub1));                     // ord 137: __imp__KeRemoveQueueDpc@4
    PatchU32(0x00750020, reinterpret_cast<uint32_t>(&Krnl_KeCancelTimer));             // ord  97: __imp__KeCancelTimer@4
    PatchU32(0x00750024, reinterpret_cast<uint32_t>(&Krnl_Stub3));                     // ord 153: __imp__KeSynchronizeExecution@12
    PatchU32(0x00750028, reinterpret_cast<uint32_t>(&Krnl_Stub3));                     // ord 175: __imp__MmLockUnlockBufferPages@12
    PatchU32(0x0075002C, reinterpret_cast<uint32_t>(&Krnl_MmGetPhysicalAddress));      // ord 173: __imp__MmGetPhysicalAddress@4
    PatchU32(0x00750030, reinterpret_cast<uint32_t>(&Krnl_Stub1));                     // ord  95: __imp__KeBugCheck@4
    PatchU32(0x00750034, reinterpret_cast<uint32_t>(&Krnl_Stub4));                     // ord 312: __imp__RtlUnwind@16
    PatchU32(0x00750038, reinterpret_cast<uint32_t>(&Krnl_MmQueryStatistics));         // ord 181: __imp__MmQueryStatistics@4
    PatchU32(0x0075003C, reinterpret_cast<uint32_t>(&Krnl_Stub6));                     // ord  65: __imp__IoCreateDevice@24
    PatchU32(0x00750040, reinterpret_cast<uint32_t>(&Krnl_Stub2));                     // ord  74: __imp__IoInvalidDeviceRequest@8
    PatchU32(0x00750044, reinterpret_cast<uint32_t>(&Krnl_Stub1));                     // ord  91: __imp__IoDismountVolumeByName@4
    PatchU32(0x00750048, reinterpret_cast<uint32_t>(&g_avPackStandard));               // ord 356: _HalBootSMCVideoMode (DATA)
    PatchU32(0x0075004C, reinterpret_cast<uint32_t>(&Krnl_Stub3));                     // ord  83: __imp__IoStartPacket@12
    PatchU32(0x00750050, reinterpret_cast<uint32_t>(&Krnl_Stub1));                     // ord  81: __imp__IoStartNextPacket@4
    PatchU32(0x00750054, reinterpret_cast<uint32_t>(&Krnl_Stub1));                     // ord 359: __imp__IoMarkIrpMustComplete@4
    PatchU32(0x00750058, reinterpret_cast<uint32_t>(&Krnl_Stub0));                     // ord 358: __imp__HalIsResetOrShutdownPending@0
    PatchU32(0x0075005C, reinterpret_cast<uint32_t>(&Krnl_Stub2));                     // ord 176: __imp__MmLockUnlockPhysicalPage@8
    PatchU32(0x00750060, reinterpret_cast<uint32_t>(&Krnl_Stub3));                     // ord 260: __imp__RtlAnsiStringToUnicodeString@12
    PatchU32(0x00750064, reinterpret_cast<uint32_t>(&Krnl_Stub3));                     // ord 308: __imp__RtlUnicodeStringToAnsiString@12
    PatchU32(0x00750068, reinterpret_cast<uint32_t>(&Krnl_Stub1));                     // ord 252: __imp__PhyGetLinkState@4
    PatchU32(0x0075006C, reinterpret_cast<uint32_t>(&Krnl_Stub2));                     // ord 253: __imp__PhyInitialize@8
    PatchU32(0x00750070, reinterpret_cast<uint32_t>(&Krnl_Stub8));                     // ord 158: __imp__KeWaitForMultipleObjects@32
    PatchU32(0x00750074, reinterpret_cast<uint32_t>(&Krnl_Stub2));                     // ord 346: __imp__XcDESKeyParity@8
    PatchU32(0x00750078, reinterpret_cast<uint32_t>(&Krnl_Stub7));                     // ord 349: __imp__XcBlockCryptCBC@28
    PatchU32(0x0075007C, reinterpret_cast<uint32_t>(&Krnl_Stub3));                     // ord 347: __imp__XcKeyTable@12
    PatchU32(0x00750080, reinterpret_cast<uint32_t>(&Krnl_Stub5));                     // ord 345: __imp__XcModExp@20
    PatchU32(0x00750084, reinterpret_cast<uint32_t>(g_xboxLANKey));                    // ord 353: __imp__XboxLANKey (DATA)
    PatchU32(0x00750088, reinterpret_cast<uint32_t>(&Krnl_Stub3));                     // ord 339: __imp__XcRC4Crypt@12
    PatchU32(0x0075008C, reinterpret_cast<uint32_t>(&Krnl_Stub3));                     // ord 338: __imp__XcRC4Key@12
    PatchU32(0x00750090, reinterpret_cast<uint32_t>(&Krnl_KeSetTimerEx));              // ord 150: __imp__KeSetTimerEx@20
    PatchU32(0x00750094, reinterpret_cast<uint32_t>(&Krnl_KeQueryPerformanceCounter)); // ord 126: __imp__KeQueryPerformanceCounter@0
    PatchU32(0x00750098, reinterpret_cast<uint32_t>(&Krnl_KeQueryPerformanceFrequency));// ord 127: __imp__KeQueryPerformanceFrequency@0
    PatchU32(0x0075009C, reinterpret_cast<uint32_t>(&g_halDiskSerialNumber));          // ord  42: _HalDiskSerialNumber (DATA)
    PatchU32(0x007500A0, reinterpret_cast<uint32_t>(&g_halDiskModelNumber));           // ord  41: _HalDiskModelNumber (DATA)

    // 3. Patch all 7 `movzx eax, byte ptr fs:[24h]` (64 0F B6 05 24 00 00 00) in DSOUND & XACTENG
    // so KeGetCurrentIrql() returns 0 (PASSIVE_LEVEL) without touching Win32 TEB+0x24
    static const uint8_t kXorEaxNop6[8] = { 0x33, 0xC0, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90 };
    PatchBytes(0x006DFE13, kXorEaxNop6, 8);
    PatchBytes(0x006E001E, kXorEaxNop6, 8);
    PatchBytes(0x006E54DB, kXorEaxNop6, 8);
    PatchBytes(0x006FE3E0, kXorEaxNop6, 8);
    PatchBytes(0x006FE90D, kXorEaxNop6, 8);
    PatchBytes(0x007016FC, kXorEaxNop6, 8);
    PatchBytes(0x00704639, kXorEaxNop6, 8);

    // 4. Bridge LIBCMT Thread-Local Storage (__getptd, __freeptd, __mtinit)
    Bridge_crt_mtinit();
    InstallBridge(0x00695311, &Bridge_crt_getptd,   "__getptd");
    InstallBridge(0x0069545C, &Bridge_crt_freeptd,  "__freeptd");
    InstallBridge(0x006954AE, &Bridge_crt_mtinit,   "__mtinit");

    // 5. Bridge XAPILIB Functions
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

    // XACT DoWork hook to drive XACT's DPC sequencer
    InstallBridge(0x0070010F, &Bridge_XACTEngineDoWork,          "_XACTEngineDoWork@0");

    // 6. XNet / Winsock Offline Stubs (prevents null CXnSock* dereferences when hkBsdSockServer calls XNET)
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
