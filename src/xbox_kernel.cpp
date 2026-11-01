#include "ghost_runtime.h"
#include "ghost_addresses.h"

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
    uint32_t Unknown[4];     // +0x00..+0x0F (Dispatcher header / Event)
    LONG     LockCount;      // +0x10 (-1 = unlocked, >= 0 = locked)
    LONG     RecursionCount; // +0x14
    uint32_t OwningThread;   // +0x18
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

int32_t __stdcall Krnl_ExQueryNonVolatileSetting(
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

void __stdcall Krnl_DbgBreakPoint() {
}

static int __cdecl Krnl_DbgPrint(const char* fmt, ...) {
    (void)fmt;
    return 0;
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
    uint8_t    Header[16];
    uint64_t   DueTime;
    uint32_t   TimerListEntry[2];
    XBOX_KDPC* Dpc;
    int32_t    Period;
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

void* AllocAlignedTracked(uint32_t size, uint32_t alignment) {
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

void FreeAlignedTracked(void* ptr) {
    if (!ptr || (uintptr_t)ptr < 0x00960000) return;
    PhysHeader* hdr = reinterpret_cast<PhysHeader*>((uintptr_t)ptr - sizeof(PhysHeader));
    if (hdr->magic == 0x58504859) {
        hdr->magic = 0;
        free(hdr->rawPtr);
    }
}

uint32_t SizeAlignedTracked(void* ptr) {
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

void __stdcall Krnl_XcSHAUpdate(uint8_t* pbSHAContext, const uint8_t* pbInput, uint32_t dwInputLength) {
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

// XACT Engine DoWork hook: invokes the registered XACT DPC if active and runs CEngine::DoWork
static void __stdcall Bridge_XACTEngineDoWork() {
    typedef int (__stdcall *PFN_XACTEnterCS)();
    typedef void (__stdcall *PFN_DSDoWork)();
    typedef void (__thiscall *PFN_CEngineDoWork)(void* thisPtr);

    int entered = reinterpret_cast<PFN_XACTEnterCS>(0x006FE3E0)();
    void* pEngine = *reinterpret_cast<void**>(ADDR_XACT_ENGINE_PTR); // _g_pXACTEngine
    if (pEngine) {
        reinterpret_cast<PFN_DSDoWork>(0x006E1A58)();             // _DirectSoundDoWork@0
        reinterpret_cast<PFN_CEngineDoWork>(0x00700056)(pEngine); // ?DoWork@CEngine@XACT@@QAEXXZ
    }
    if (entered) {
        Bridge_RtlLeaveCriticalSection(reinterpret_cast<XBOX_CRITICAL_SECTION*>(ADDR_XACT_CRITICAL_SECTION));
    }
    if (g_activeXactTimer && g_activeXactTimer->Dpc && g_activeXactTimer->Dpc->DeferredRoutine) {
        XBOX_KDPC* dpc = g_activeXactTimer->Dpc;
        dpc->DeferredRoutine(dpc, dpc->DeferredContext, dpc->SystemArgument1, dpc->SystemArgument2);
    }
}

void InitXboxKernel() {
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
    InstallBridge(0x00695311, &Bridge_crt_getptd);        // __getptd
    InstallBridge(0x0069545C, &Bridge_crt_freeptd);       // __freeptd
    InstallBridge(0x006954AE, &Bridge_crt_mtinit);        // __mtinit

    // 5. XACT DoWork hook to drive XACT's DPC sequencer
    InstallBridge(0x0070010F, &Bridge_XACTEngineDoWork);  // _XACTEngineDoWork@0
}
