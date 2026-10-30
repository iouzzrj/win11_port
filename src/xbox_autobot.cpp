#include "xbox_autobot.h"
#include "autobot_levels.h"
#include <math.h>
#include <vector>
#include <string>
#include <algorithm>

// ============================================================================
// StarCraft: Ghost — Two-Mode Multimodal Vision + Spatial Memory & Learned Bot
//   * Mode 1 (TRAIN): Multimodal Vision (4-frame filmstrip PNG + Radar State)
//     driven exploration, puzzle solving, and fixed-time-interval Raw Replay
//     recording (autobot_memory/raw_replays/<level>/run_<ts>/).
//   * Mode 2 (NORMAL): 100% Standalone C++ State Machine driven by the
//     distilled LearnedNavGraph + OrderedObjectives + EnemyTactics table
//     (autobot_memory/compiled/<level>_learned.cfg) with ZERO LLM dependency.
// ============================================================================

bool g_autoBotEnabled   = false;
bool g_cliAutoBotLaunch = false;
bool g_cliAutoBotTrain  = false;

#pragma pack(push, 1)
struct BotInputGamepad {
    uint16_t wButtons;          // +0x00
    uint8_t  bAnalogButtons[8]; // +0x02 (0=A, 1=B, 2=X, 3=Y, 4=Black, 5=White, 6=LTrig, 7=RTrig)
    int16_t  sThumbLX;          // +0x0A
    int16_t  sThumbLY;          // +0x0C
    int16_t  sThumbRX;          // +0x0E
    int16_t  sThumbRY;          // +0x10
};

struct BotInputState {
    uint32_t        dwPacketNumber; // +0x00
    BotInputGamepad Gamepad;        // +0x04
};

struct XboxScriptParam {
    uint32_t type;
    uint32_t value;
};
#pragma pack(pop)

// Read-Only Xbox Engine Query & Math Function Pointers
// (MSVC x86: __thiscall declared via __fastcall with (pThis, edxDummy, ...))
typedef void*       (__cdecl    *PFN_GetFirstLocalPlayer)();
typedef void*       (__cdecl    *PFN_FindGUID)(uint32_t guid);
typedef const char* (__fastcall *PFN_Thing_GetName)(const void* pThing, void* edx);
typedef void        (__fastcall *PFN_Thing_GetTargetPos)(const void* pThing, void* edx, float* outVec3, uint32_t arg);
typedef int         (__cdecl    *PFN_IsValidAssistTarget)(const void* pThing);
typedef float       (__cdecl    *PFN_Math_ArcTan4)(float y, float x, int mode);
typedef float       (__cdecl    *PFN_Math_ArcSin3)(float z);
typedef float       (__cdecl    *PFN_Math_NormalizeAngle)(float deg);
typedef float       (__cdecl    *PFN_Math_NormalizeAngleAcute)(float deg);
typedef float       (__cdecl    *PFN_Math_NormalizeDeltaAngle)(float a1, float a2);
typedef void        (__cdecl    *PFN_Math_RotateVecByEuler)(float* outVec3, const float* inVec3, const float* eulerPYR);
typedef void        (__cdecl    *PFN_Math_BuildMatrixFromFwd)(float* outMat4x4, const float* inFwdVec3);
typedef int         (__fastcall *PFN_Model_GetBoneWorldPos)(const void* pModel, void* edx, int boneIdx, float* outVec3, const void* optTransform);
typedef int         (__fastcall *PFN_Model_GetSubBoneTransform)(const void* pModel, void* edx, int boneIdx, int subBoneIdx, float* outMat4x4, const void* optTransform);
typedef void*       (__fastcall *PFN_Thing_FindAttachmentByType)(const void* pThing, void* edx, uint32_t attachType);
typedef void        (__fastcall *PFN_LaserSight_SetTarget)(void* pLaserSight, void* edx, const float* targetVec3, uint32_t castRay);
typedef int         (__cdecl    *PFN_Collision_LineOfSight)(const float* startVec3, const float* endVec3, int dbgId);
typedef int         (__fastcall *PFN_PlayerControl_Activate)(void* pCtrl, void* edx);
typedef int         (__cdecl    *PFN_ScriptMessage_Activated)(void* pInteractThing, void* pActivatorThing);
typedef int         (__fastcall *PFN_HackSys_ControlFreeze)(const void* pHackSys, void* edx);

static PFN_GetFirstLocalPlayer       s_fnGetLocalPlayer        = reinterpret_cast<PFN_GetFirstLocalPlayer>(0x0051F3C0);
static PFN_FindGUID                  s_fnFindGUID              = reinterpret_cast<PFN_FindGUID>(0x00530830);
static PFN_Thing_GetName             s_fnThingGetName          = reinterpret_cast<PFN_Thing_GetName>(0x00530510);
static PFN_Thing_GetTargetPos        s_fnThingGetTargetPos     = reinterpret_cast<PFN_Thing_GetTargetPos>(0x004511E0);
static PFN_IsValidAssistTarget       s_fnIsValidAssistTarget   = reinterpret_cast<PFN_IsValidAssistTarget>(0x00466D30);
static PFN_Math_ArcTan4              s_fnArcTan4               = reinterpret_cast<PFN_Math_ArcTan4>(0x005E61E0);
static PFN_Math_ArcSin3              s_fnArcSin3               = reinterpret_cast<PFN_Math_ArcSin3>(0x005E5CF0);
static PFN_Math_NormalizeAngle       s_fnNormalizeAngle        = reinterpret_cast<PFN_Math_NormalizeAngle>(0x005E6770);
static PFN_Math_NormalizeAngleAcute  s_fnNormalizeAcute        = reinterpret_cast<PFN_Math_NormalizeAngleAcute>(0x005E6850);
static PFN_Math_NormalizeDeltaAngle  s_fnNormalizeDelta        = reinterpret_cast<PFN_Math_NormalizeDeltaAngle>(0x005E6920);
static PFN_Math_RotateVecByEuler     s_fnRotateVecByEuler      = reinterpret_cast<PFN_Math_RotateVecByEuler>(0x005E7DE0);
static PFN_Math_BuildMatrixFromFwd   s_fnBuildMatrixFromFwd    = reinterpret_cast<PFN_Math_BuildMatrixFromFwd>(0x005E6F00);
static PFN_Model_GetBoneWorldPos     s_fnModelGetBoneWorldPos  = reinterpret_cast<PFN_Model_GetBoneWorldPos>(0x005DFF60);
static PFN_Model_GetSubBoneTransform s_fnModelGetSubBoneTransform = reinterpret_cast<PFN_Model_GetSubBoneTransform>(0x005DFE20);
static PFN_Thing_FindAttachmentByType s_fnFindAttachmentByType = reinterpret_cast<PFN_Thing_FindAttachmentByType>(0x0056CD60);
static PFN_LaserSight_SetTarget      s_fnLaserSightSetTarget   = reinterpret_cast<PFN_LaserSight_SetTarget>(0x00563330);
static PFN_Collision_LineOfSight     s_fnCollisionLineOfSight  = reinterpret_cast<PFN_Collision_LineOfSight>(0x0044F480);
static PFN_PlayerControl_Activate    s_fnPlayerControlActivate = reinterpret_cast<PFN_PlayerControl_Activate>(0x0045A480);
static PFN_ScriptMessage_Activated   s_fnScriptMessageActivated = reinterpret_cast<PFN_ScriptMessage_Activated>(0x004A7430);
static PFN_HackSys_ControlFreeze     s_fnHackSysControlFreeze  = reinterpret_cast<PFN_HackSys_ControlFreeze>(0x0053F8B0);

// ============================================================================
// Operating Modes, Combat Stances & Data Structures
// ============================================================================

enum AutoBotOperatingMode {
    BOT_MODE_TRAIN  = 0, // Multimodal Vision + Spatial Memory Exploration + Raw Replay Recording
    BOT_MODE_NORMAL = 1  // Standalone C++ Execution using Compiled Learned Graph & Tactics (Zero LLM)
};

enum AutoBotState {
    BOT_STATE_IDLE = 0,
    BOT_STATE_MENU,
    BOT_STATE_CUTSCENE,
    BOT_STATE_HACKING,
    BOT_STATE_WAIT_LLM,
    BOT_STATE_COMBAT,
    BOT_STATE_INTERACT,
    BOT_STATE_NAVIGATE,
};

enum CombatStance {
    STANCE_IGNORE       = 0, // Do not attack any enemies; focus 100% on movement/interaction
    STANCE_CLOAK_BYPASS = 1, // Activate Cloak and move past enemies without firing
    STANCE_DEFENSIVE    = 2, // Only defend against close/blocking threats (< 4.5m) or MUST_KILL tagged enemies
    STANCE_ENGAGE       = 3, // Actively engage visible enemies within normal range
    STANCE_MELEE_RUSH   = 4, // Close distance and finish target with ShockGlove/Melee
    STANCE_BOSS_KITE    = 5  // Maintain safe distance (6..14m), burst fire, strafe/kite
};

enum EnemyTacticalPolicy {
    POLICY_UNKNOWN      = 0,
    POLICY_MUST_KILL    = 1,
    POLICY_IGNORE       = 2,
    POLICY_AVOID_CLOAK  = 3,
    POLICY_MELEE_RUSH   = 4,
    POLICY_BOSS_KITE    = 5
};

struct EnemyTacticalEntry {
    uint32_t            guid;
    char                name[32];
    EnemyTacticalPolicy policy;
    float               engageDist;
    bool                preferMelee;
    uint32_t            timesEncountered;
    uint32_t            timesKilled;
    float               totalDmgTakenFrom;
    float               lastPos[3];
    uint32_t            ignoreUntilFrame;
};

// Fixed-time-interval trajectory sample (recorded every kReplayIntervalFrames = 12 frames = 0.20s)
static constexpr uint32_t kReplayIntervalFrames = 12u; // 0.20 seconds at 60 FPS

struct RawTrajectorySample {
    uint32_t tick;
    float    timeSec;
    float    pos[3];
    float    vel[3];
    float    yaw;
    float    pitch;
    float    hp;
    uint32_t ammo;
    uint8_t  ctrlState;
    uint8_t  blockedFlag;
    uint32_t cmdId;
};

// Learned Navigation Graph (loaded from compiled/<level>_learned.cfg in BOT_MODE_NORMAL)
struct LearnedNavNode {
    int   id;
    float x, y, z;
};

struct LearnedNavEdge {
    int   fromId;
    int   toId;
    float costSec;
    char  moveType[16]; // "WALK", "DROP", "JUMP", "LADDER", "ELEVATOR"
    int   gateMechId;   // 0 if unlocked, >0 if gated behind MECHANISM <gateMechId>
};

struct LearnedObjective {
    int          step;
    float        targetPos[3];
    uint32_t     interactGuid;
    uint32_t     regionGuid;
    CombatStance stance;
    char         desc[64];
};

struct LearnedSupplyNode {
    uint32_t guid;
    float    pos[3];
    bool     isCorpse;
};

// Multimodal Command (loaded from autobot_io/command.txt in BOT_MODE_TRAIN)
struct MultimodalCommand {
    uint32_t     cmdId;
    char         desc[96];
    CombatStance stance;
    uint32_t     targetEnemyGuid;
    uint32_t     interactGuid;
    uint32_t     searchCorpseGuid;
    uint32_t     triggerRegionGuid;
    uint32_t     maxFrames;
    bool         hasLookAt;
    float        lookAt[3];
    bool         jumpOnce;
    std::vector<float> waypointsXYZ; // triplets (x, y, z)
};

// ============================================================================
// Static State
// ============================================================================

static AutoBotOperatingMode s_botMode          = BOT_MODE_NORMAL;
static bool                 s_modeInitialized  = false;
static AutoBotState         s_botState         = BOT_STATE_IDLE;
static BotInputGamepad      s_botPad           = {};
static char                 s_statusText[160]  = "AI Bot: OFF (F5)";
static void*                s_lastWorldPtr     = nullptr;
static uint32_t             s_botFrame         = 0;
static uint32_t             s_activeGameTicks  = 0; // Increments ONLY during active gameplay (excludes menu/cutscene/WAIT_LLM)
static uint32_t             s_cutsceneFrames   = 0;
static uint32_t             s_hackFrames       = 0;
static uint32_t             s_eolFrames        = 0;
static bool                 s_f5WasDown        = false;
static bool                 s_f6WasDown        = false;
static bool                 s_f7WasDown        = false;
static bool                 s_f12WasDown       = false;

// Replay & Directory Paths
static char                 s_ioDir[MAX_PATH]          = {};
static char                 s_memDir[MAX_PATH]         = {};
static char                 s_compiledDir[MAX_PATH]    = {};
static char                 s_currentRunDir[MAX_PATH]  = {};
static char                 s_currentLevel[64]         = {};
static uint32_t             s_runSequenceId            = 0;
static std::vector<RawTrajectorySample> s_rawSamples;
static float                s_prevSamplePos[3]         = {};
static float                s_prevFrameHp              = 200.0f;
static float                s_totalDamageTakenThisRun  = 0.0f;

// Multimodal Training State
static MultimodalCommand    s_curCmd                   = {};
static uint32_t             s_lastExecutedCmdId        = 0;
static uint32_t             s_cmdElapsedFrames         = 0;
static size_t               s_cmdWaypointCursor        = 0;
static uint32_t             s_cmdInitialInteractCount  = 0;
static bool                 s_waitingForLlm            = false;
static bool                 s_stateExportedForCurrentWait = false;
static uint32_t             s_waitLlmStartFrame        = 0;
static float                s_waitLlmFrozenHp          = 200.0f;
static uint32_t             s_stepCounter              = 0;
static char                 s_lastCmdResult[64]        = "INIT";

// Learned Normal Mode State
static std::vector<LearnedNavNode>       s_learnedNodes;
static std::vector<LearnedNavEdge>       s_learnedEdges;
static std::vector<LearnedMechanismStep> s_learnedMechanisms;
static std::vector<LearnedObjective>     s_learnedObjectives;
static std::vector<LearnedSupplyNode>    s_learnedSupplies;
static std::vector<EnemyTacticalEntry>   s_enemyTactics;
static int                               s_normalObjCursor      = 0;
static bool                              s_normalAlignedOnStart = false;
static LearnedRoutePlan                  s_normalRoutePlan      = {};
static uint32_t                          s_normalStageIdx       = 0;
static uint32_t                          s_normalStageWpCursor  = 0;
static bool                              s_normalRoutePlanned   = false;
static std::vector<int>                  s_normalNavPath;
static size_t                            s_normalNavCursor      = 0;
static int                               s_normalLastGoalNode   = -1;
static uint32_t                          s_normalLastRepathTick = 0;

// Shared Combat / Interaction State
static float                s_goalPos[3]               = {};
static char                 s_goalDesc[96]             = "Idle";
static uint32_t             s_unstuckUntilFrame        = 0;
static int                  s_unstuckDir               = 1;
static uint8_t*             s_cachedEnemy              = nullptr;
static float                s_cachedEnemyTargetPos[3]  = {};
static float                s_cachedEnemyDist          = 0.0f;
static bool                 s_startedLevel1            = false;
static bool                 s_inLevel1Escape           = false;
static bool                 s_seenIntroCutscene        = false;
static bool                 s_capturedStartShot        = false;
static bool                 s_capturedCombatShot       = false;
static bool                 s_capturedHackShot         = false;
static bool                 s_capturedDrillShot        = false;
static bool                 s_capturedExtrShot         = false;
static bool                 s_capturedEolShot          = false;
static uint32_t             s_activeLevelFrames        = 0;
static uint32_t             s_postIntroFrames          = 0;
static uint32_t             s_combatFrames             = 0;

struct ActivatedSwitchRecord {
    uint32_t guid;
    uint32_t activatedFrame;
    uint32_t count;
    uint32_t physicalPresses;
};
static std::vector<ActivatedSwitchRecord> s_activatedSwitches;
static std::vector<uint32_t>              s_searchedCorpses;
static uint8_t*                           s_searchingCorpse        = nullptr;
static uint32_t                           s_searchHoldFrames       = 0;
static uint32_t                           s_activeInteractGuid     = 0;
static uint32_t                           s_interactApproachFrames = 0;
static uint32_t                           s_interactAnimStartFrame = 0;
static uint32_t                           s_interactInitialBtn30   = 0;
static bool                               s_interactStartedAnim    = false;
static bool                               s_interactSentScriptMsg  = false;
static uint32_t                           s_lastHitReactionFrame   = 0;
static uint32_t                           s_lastTrackedStageIdx    = 0xFFFFFFFFu;
static uint32_t                           s_lastTrackedWpCursor    = 0xFFFFFFFFu;
static float                              s_stageWpBestDist        = 1e30f;
static uint32_t                           s_stageWpStallFrames     = 0;
static bool                               s_wasDeadOrReloading     = false;
static uint8_t*                           s_lastPlayerPtr          = nullptr;
static uint32_t                           s_ignoredEnemyGuid       = 0;
static uint32_t                           s_ignoreEnemyUntilFrame  = 0;
static uint32_t                           s_enemyStaleFrames       = 0;
static float                              s_lastTrackedEnemyHp     = -1.0f;
static uint32_t                           s_lastMeleeFrame         = 0;
static uint32_t                           s_killCount              = 0;
static uint32_t                           s_totalShotsFired        = 0;

// ============================================================================
// Utility & Pointer Safety Helpers
// ============================================================================

static bool IsValidPtr(const void* p, size_t minBytes = 4) {
    uintptr_t a = reinterpret_cast<uintptr_t>(p);
    return (a >= 0x00400000u && (a + minBytes) <= 0x7FFF0000u);
}

static bool IsValidReadablePtr(const void* p, size_t minBytes = 4) {
    uintptr_t a = reinterpret_cast<uintptr_t>(p);
    if (a < 0x00400000u || (a + minBytes) > 0x7FFF0000u) return false;
    MEMORY_BASIC_INFORMATION mbi = {};
    if (VirtualQuery(p, &mbi, sizeof(mbi)) == 0) return false;
    if (mbi.State != MEM_COMMIT) return false;
    if (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) return false;
    return (a + minBytes) <= (reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize);
}

static const char* SafeGetThingName(void* pThing) {
    if (!IsValidPtr(pThing, 0x140)) return "<null>";
    const char* n = s_fnThingGetName(pThing, nullptr);
    if (n && IsValidPtr(n, 1) && n[0] >= 0x20 && n[0] < 0x7F) return n;
    return "<unnamed>";
}

static void GetThingWorldPos(void* pThing, float outPos[3]) {
    outPos[0] = outPos[1] = outPos[2] = 0.0f;
    if (!IsValidPtr(pThing, 0x150)) return;
    uint8_t* b = reinterpret_cast<uint8_t*>(pThing);
    const float* p = reinterpret_cast<const float*>(b + 0xB0);
    outPos[0] = p[0];
    outPos[1] = p[1];
    outPos[2] = p[2];

    uint32_t type = *reinterpret_cast<const uint32_t*>(b + 0x10);
    if (type == 3 && fabsf(outPos[0]) < 0.001f && fabsf(outPos[1]) < 0.001f && fabsf(outPos[2]) < 0.001f) {
        const float* bmin = reinterpret_cast<const float*>(b + 0x138);
        const float* bmax = reinterpret_cast<const float*>(b + 0x144);
        outPos[0] = 0.5f * (bmin[0] + bmax[0]);
        outPos[1] = 0.5f * (bmin[1] + bmax[1]);
        outPos[2] = 0.5f * (bmin[2] + bmax[2]);
    }
}

static void GetEnemyTargetPos(const void* pThing, float outPos[3]) {
    GetThingWorldPos(const_cast<void*>(pThing), outPos);
    outPos[2] += 0.85f;
    if (!IsValidPtr(pThing, 0x140)) return;
    uintptr_t vtable = *reinterpret_cast<const uintptr_t*>(pThing);
    if (IsValidPtr(reinterpret_cast<const void*>(vtable), 0x10)) {
        PFN_Thing_GetTargetPos fnVtableGetTargetPos =
            *reinterpret_cast<const PFN_Thing_GetTargetPos*>(vtable + 0x08);
        if (IsValidPtr(reinterpret_cast<const void*>(fnVtableGetTargetPos), 0x10)) {
            fnVtableGetTargetPos(pThing, nullptr, outPos, 0);
            return;
        }
    }
    s_fnThingGetTargetPos(pThing, nullptr, outPos, 0);
}

static void GetPlayerMuzzlePos(const uint8_t* player, const float fallbackEyePos[3], float outMuzzlePos[3], float outBarrelFwd[3]) {
    outMuzzlePos[0] = fallbackEyePos[0];
    outMuzzlePos[1] = fallbackEyePos[1];
    outMuzzlePos[2] = fallbackEyePos[2];
    if (outBarrelFwd) {
        outBarrelFwd[0] = 0.0f;
        outBarrelFwd[1] = 1.0f;
        outBarrelFwd[2] = 0.0f;
        if (IsValidPtr(player, 0xA0)) {
            const float* pFwd = reinterpret_cast<const float*>(player + 0x90);
            outBarrelFwd[0] = pFwd[0];
            outBarrelFwd[1] = pFwd[1];
            outBarrelFwd[2] = pFwd[2];
        }
    }
    if (!IsValidPtr(player, 0x2B0)) return;
    const void* pModel = *reinterpret_cast<const void* const*>(player + 0x2A0);
    if (!IsValidPtr(pModel, 0xB0)) return;

    alignas(16) float mat4x4[16] = {};
    if (s_fnModelGetSubBoneTransform(pModel, nullptr, 2, 1, mat4x4, nullptr) != 0) {
        outMuzzlePos[0] = mat4x4[12];
        outMuzzlePos[1] = mat4x4[13];
        outMuzzlePos[2] = mat4x4[14];
        if (outBarrelFwd) {
            outBarrelFwd[0] = mat4x4[4];
            outBarrelFwd[1] = mat4x4[5];
            outBarrelFwd[2] = mat4x4[6];
        }
        return;
    }
    float bonePos[3] = {};
    if (s_fnModelGetBoneWorldPos(pModel, nullptr, 2, bonePos, nullptr) != 0) {
        outMuzzlePos[0] = bonePos[0];
        outMuzzlePos[1] = bonePos[1];
        outMuzzlePos[2] = bonePos[2];
    }
}

static bool IsMenuLevelName(const char* name) {
    if (!name || !name[0]) return true;
    return (_stricmp(name, "menu") == 0 || _stricmp(name, "menu.nil") == 0 || _strnicmp(name, "menu.", 5) == 0);
}

static const char* ResolveScriptClassName(uintptr_t vtable) {
    uint32_t numClasses = *reinterpret_cast<uint32_t*>(0x0081A710);
    if (numClasses > 512) numClasses = 512;
    uint8_t** entries = reinterpret_cast<uint8_t**>(0x00819B10);
    for (uint32_t i = 0; i < numClasses; ++i) {
        uint8_t* entry = entries[i];
        if (!IsValidPtr(entry, 0x28)) continue;
        const char* name = reinterpret_cast<const char*>(entry);
        uintptr_t* proto = *reinterpret_cast<uintptr_t**>(entry + 0x20);
        if (IsValidPtr(proto, 4) && *proto == vtable && name[0] >= 0x20 && name[0] < 0x7F) {
            return name;
        }
    }
    return "cGameScript_Unknown";
}

static uint8_t* FindScriptByOwnerGuid(uint32_t ownerGuid, const char* expectedClass = nullptr) {
    uint32_t numScripts = *reinterpret_cast<uint32_t*>(0x00819B04);
    void** scriptArray  = reinterpret_cast<void**>(0x00819F10);
    for (uint32_t i = 0; i < numScripts && i < 512; ++i) {
        uint8_t* sc = reinterpret_cast<uint8_t*>(scriptArray[i]);
        if (!IsValidPtr(sc, 0x28)) continue;
        uint32_t guid = *reinterpret_cast<uint32_t*>(sc + 0x0C);
        if (guid == ownerGuid) {
            if (!expectedClass) return sc;
            uintptr_t vtable = *reinterpret_cast<uintptr_t*>(sc + 0x00);
            const char* clsName = ResolveScriptClassName(vtable);
            if (strcmp(clsName, expectedClass) == 0) return sc;
        }
    }
    return nullptr;
}

static uint8_t* FindThingByGuid(const void* world, uint32_t targetGuid) {
    if (targetGuid == 0) return nullptr;
    void* found = s_fnFindGUID(targetGuid);
    if (IsValidPtr(found, 0x140)) {
        return reinterpret_cast<uint8_t*>(found);
    }
    if (!IsValidPtr(world, 0x100)) return nullptr;
    const uint8_t* w = reinterpret_cast<const uint8_t*>(world);
    uint8_t* curThing = *reinterpret_cast<uint8_t* const*>(w + 0xE4);
    int scanCount = 0;
    while (IsValidPtr(curThing, 0x140) && scanCount < 2048) {
        if (*reinterpret_cast<uint32_t*>(curThing + 0x34) == targetGuid) {
            return curThing;
        }
        curThing = *reinterpret_cast<uint8_t**>(curThing + 0x0C);
        scanCount++;
    }
    return nullptr;
}

static float Dist3D(const float a[3], const float b[3]) {
    float dx = a[0] - b[0];
    float dy = a[1] - b[1];
    float dz = a[2] - b[2];
    return sqrtf(dx * dx + dy * dy + dz * dz);
}

static float Dist2D(const float a[3], const float b[3]) {
    float dx = a[0] - b[0];
    float dy = a[1] - b[1];
    return sqrtf(dx * dx + dy * dy);
}

static bool CheckClearLineOfSight(const float startPos[3], const float endPos[3]) {
    return (s_fnCollisionLineOfSight(startPos, endPos, 10) == 0);
}

static bool IsMechanismActionType(uint32_t guid, const char* actionType) {
    if (guid == 0 || !actionType) return false;
    for (size_t i = 0; i < s_learnedMechanisms.size(); ++i) {
        if (s_learnedMechanisms[i].guid == guid &&
            strcmp(s_learnedMechanisms[i].actionType, actionType) == 0)
        {
            return true;
        }
    }
    return false;
}

static bool IsRideElevatorMechanism(uint32_t guid) {
    return IsMechanismActionType(guid, "RIDE_ELEVATOR");
}

static bool IsShootObstacleMechanism(uint32_t guid) {
    if (IsMechanismActionType(guid, "SHOOT_OBSTACLE")) return true;
    void* t = s_fnFindGUID(guid);
    if (IsValidReadablePtr(t, 0x374)) {
        const uint8_t* b = reinterpret_cast<const uint8_t*>(t);
        if (*reinterpret_cast<const uint32_t*>(b + 0x10) == 8 &&
            (*reinterpret_cast<const uint32_t*>(b + 0x360) & 0x01u) == 0 &&
            *reinterpret_cast<const float*>(b + 0x364) > 0.0f &&
            FindScriptByOwnerGuid(guid, "cButton") == nullptr &&
            FindScriptByOwnerGuid(guid, "cConsole") == nullptr)
        {
            return true;
        }
    }
    return false;
}

static bool IsHackTerminalMechanism(uint32_t guid) {
    if (IsMechanismActionType(guid, "HACK_TERMINAL")) return true;
    return (FindScriptByOwnerGuid(guid, "cConsole") != nullptr);
}

static uint32_t GetRequiredActivationsForMechId(uint32_t guid, int mechId) {
    if (guid == 0) return 0;
    uint32_t count = 0;
    for (size_t i = 0; i < s_learnedMechanisms.size(); ++i) {
        if (s_learnedMechanisms[i].guid == guid &&
            (mechId <= 0 || s_learnedMechanisms[i].mechId <= mechId))
        {
            count++;
        }
    }
    return (count > 0) ? count : 1;
}

static uint32_t GetRequiredActivationsForCurrentStage(uint32_t guid) {
    if (guid == 0) return 0;
    if (s_botMode == BOT_MODE_TRAIN) {
        return s_cmdInitialInteractCount + 1;
    }
    if (s_normalRoutePlanned && s_normalStageIdx < s_normalRoutePlan.stageCount) {
        const LearnedRouteStage& st = s_normalRoutePlan.stages[s_normalStageIdx];
        if (st.hasMechanismAction && st.mechanism.guid == guid) {
            return GetRequiredActivationsForMechId(guid, st.mechanism.mechId);
        }
    }
    return 1;
}

static bool IsCurrentStageHackTerminal(uint32_t guid) {
    if (guid == 0) return false;
    if (FindScriptByOwnerGuid(guid, "cConsole") != nullptr) return true;
    if (s_botMode == BOT_MODE_NORMAL && s_normalRoutePlanned && s_normalStageIdx < s_normalRoutePlan.stageCount) {
        const LearnedRouteStage& st = s_normalRoutePlan.stages[s_normalStageIdx];
        if (st.hasMechanismAction && st.mechanism.guid == guid) {
            return strcmp(st.mechanism.actionType, "HACK_TERMINAL") == 0;
        }
        return false;
    }
    return IsHackTerminalMechanism(guid);
}

static float GetElevatorDestinationZ(uint32_t btnGuid, float fallbackZ) {
    int mechId = 0;
    int nodeId = 0;
    for (size_t i = 0; i < s_learnedMechanisms.size(); ++i) {
        if (s_learnedMechanisms[i].guid == btnGuid &&
            strcmp(s_learnedMechanisms[i].actionType, "RIDE_ELEVATOR") == 0)
        {
            mechId = s_learnedMechanisms[i].mechId;
            nodeId = s_learnedMechanisms[i].nodeId;
            break;
        }
    }
    if (mechId > 0 || nodeId > 0) {
        for (size_t e = 0; e < s_learnedEdges.size(); ++e) {
            if ((mechId > 0 && s_learnedEdges[e].gateMechId == mechId) ||
                (nodeId > 0 && s_learnedEdges[e].fromId == nodeId && strcmp(s_learnedEdges[e].moveType, "ELEVATOR") == 0))
            {
                for (size_t n = 0; n < s_learnedNodes.size(); ++n) {
                    if (s_learnedNodes[n].id == s_learnedEdges[e].toId) {
                        return s_learnedNodes[n].z;
                    }
                }
            }
        }
    }
    return fallbackZ;
}

static bool ComputeElevatorCabinCenterFromButton(uint32_t btnGuid, float curZ, float outCenter[3]) {
    void* btnThing = s_fnFindGUID(btnGuid);
    if (!IsValidPtr(btnThing, 0x140)) return false;
    float ip[3] = {};
    GetThingWorldPos(btnThing, ip);
    const float* sFwd = reinterpret_cast<const float*>(reinterpret_cast<const uint8_t*>(btnThing) + 0x90);
    float sFwdLen = sqrtf(sFwd[0] * sFwd[0] + sFwd[1] * sFwd[1]);
    if (sFwdLen < 0.1f) return false;
    outCenter[0] = ip[0] + (sFwd[0] / sFwdLen) * 1.15f;
    outCenter[1] = ip[1] + (sFwd[1] / sFwdLen) * 1.15f;
    outCenter[2] = curZ;
    return true;
}

static bool IsPlayerInsideElevatorCabin(const float playerPos[3], float outCabinCenter[3] = nullptr) {
    for (size_t i = 0; i < s_learnedMechanisms.size(); ++i) {
        if (strcmp(s_learnedMechanisms[i].actionType, "RIDE_ELEVATOR") == 0) {
            float c[3] = {};
            if (ComputeElevatorCabinCenterFromButton(s_learnedMechanisms[i].guid, playerPos[2], c)) {
                if (Dist2D(playerPos, c) < 2.20f) {
                    if (outCabinCenter) {
                        outCabinCenter[0] = c[0];
                        outCabinCenter[1] = c[1];
                        outCabinCenter[2] = c[2];
                    }
                    return true;
                }
            }
        }
    }
    return false;
}

static uint32_t GetSwitchActivationCount(uint32_t guid, uint32_t* outLastFrame = nullptr) {
    for (size_t i = 0; i < s_activatedSwitches.size(); ++i) {
        if (s_activatedSwitches[i].guid == guid) {
            if (outLastFrame) *outLastFrame = s_activatedSwitches[i].activatedFrame;
            return s_activatedSwitches[i].count;
        }
    }
    if (outLastFrame) *outLastFrame = 0;
    return 0;
}

static uint32_t GetSwitchPhysicalPressCount(uint32_t guid) {
    for (size_t i = 0; i < s_activatedSwitches.size(); ++i) {
        if (s_activatedSwitches[i].guid == guid) {
            return s_activatedSwitches[i].physicalPresses;
        }
    }
    return 0;
}

static void RecordSwitchActivated(uint32_t guid, bool wasPhysicalPress = true) {
    for (size_t i = 0; i < s_activatedSwitches.size(); ++i) {
        if (s_activatedSwitches[i].guid == guid) {
            s_activatedSwitches[i].activatedFrame = s_botFrame;
            s_activatedSwitches[i].count++;
            if (wasPhysicalPress) s_activatedSwitches[i].physicalPresses++;
            return;
        }
    }
    ActivatedSwitchRecord rec = { guid, s_botFrame, 1, wasPhysicalPress ? 1u : 0u };
    s_activatedSwitches.push_back(rec);
}

static bool SharesButtonTargetGuid(const uint8_t* scBtnA, const uint8_t* scBtnB) {
    if (!IsValidPtr(scBtnA, 0x18) || !IsValidPtr(scBtnB, 0x18)) return false;
    uint32_t numA = *reinterpret_cast<const uint32_t*>(scBtnA + 0x10);
    uint32_t numB = *reinterpret_cast<const uint32_t*>(scBtnB + 0x10);
    const XboxScriptParam* paramsA = *reinterpret_cast<const XboxScriptParam* const*>(scBtnA + 0x14);
    const XboxScriptParam* paramsB = *reinterpret_cast<const XboxScriptParam* const*>(scBtnB + 0x14);
    if (!IsValidPtr(paramsA, 8) || !IsValidPtr(paramsB, 8)) return false;
    if (numA > 32) numA = 32;
    if (numB > 32) numB = 32;

    static const uint32_t kTargetSlots[] = { 0, 1, 2, 3, 4, 5, 6, 7, 15 };
    for (size_t i = 0; i < sizeof(kTargetSlots) / sizeof(kTargetSlots[0]); ++i) {
        uint32_t sa = kTargetSlots[i];
        if (sa >= numA) continue;
        if (paramsA[sa].type != 3) continue;
        uint32_t valA = paramsA[sa].value;
        if (valA == 0 || valA >= 0x10000u) continue;
        for (size_t j = 0; j < sizeof(kTargetSlots) / sizeof(kTargetSlots[0]); ++j) {
            uint32_t sb = kTargetSlots[j];
            if (sb >= numB) continue;
            if (paramsB[sb].type != 3) continue;
            if (paramsB[sb].value == valA) return true;
        }
    }
    return false;
}

static bool IsDoorSwitchAlreadyOpen(const void* world, uint32_t interactGuid, const float ip[3], const float sFwd[3], const uint8_t* scBtn) {
    if (!scBtn || !IsValidPtr(scBtn, 0x38) || !IsValidPtr(world, 0x100)) return false;

    // 1. Check direct target cDoorMove / cDoorSliding script open state in engine memory
    uint32_t numParams = *reinterpret_cast<const uint32_t*>(scBtn + 0x10);
    const XboxScriptParam* params = *reinterpret_cast<const XboxScriptParam* const*>(scBtn + 0x14);
    if (IsValidPtr(params, 8) && numParams <= 32) {
        static const uint32_t kTargetSlots[] = { 0, 1, 2, 3, 4, 5, 6, 7, 15 };
        for (size_t i = 0; i < sizeof(kTargetSlots) / sizeof(kTargetSlots[0]); ++i) {
            uint32_t slot = kTargetSlots[i];
            if (slot >= numParams) continue;
            if (params[slot].type != 3) continue;
            uint32_t targetGuid = params[slot].value;
            if (targetGuid == 0 || targetGuid >= 0x10000u) continue;

            const uint8_t* scDoorMove = FindScriptByOwnerGuid(targetGuid, "cDoorMove");
            if (scDoorMove && IsValidPtr(scDoorMove, 0x48)) {
                if (*reinterpret_cast<const uint32_t*>(scDoorMove + 0x34) != 0) return true;
            }
            const uint8_t* scDoorSlide = FindScriptByOwnerGuid(targetGuid, "cDoorSliding");
            if (scDoorSlide && IsValidPtr(scDoorSlide, 0x34)) {
                if (*reinterpret_cast<const uint32_t*>(scDoorSlide + 0x28) != 0) return true;
            }
        }
    }

    // 2. Check paired cButton on the opposite side of the same doorway (within 3.5m horizontally, 1.2m vertically)
    float sFwdLen = sFwd ? sqrtf(sFwd[0] * sFwd[0] + sFwd[1] * sFwd[1]) : 0.0f;
    uint32_t numScripts = *reinterpret_cast<const uint32_t*>(0x00819B04);
    void** scriptArray  = reinterpret_cast<void**>(0x00819F10);
    for (uint32_t i = 0; i < numScripts && i < 512; ++i) {
        const uint8_t* otherSc = reinterpret_cast<const uint8_t*>(scriptArray[i]);
        if (!IsValidPtr(otherSc, 0x38)) continue;
        uint32_t otherGuid = *reinterpret_cast<const uint32_t*>(otherSc + 0x0C);
        if (otherGuid == 0 || otherGuid == interactGuid) continue;

        uintptr_t vtable = *reinterpret_cast<const uintptr_t*>(otherSc);
        const char* clsName = ResolveScriptClassName(vtable);
        if (strcmp(clsName, "cButton") != 0) continue;

        uint8_t* otherThing = FindThingByGuid(world, otherGuid);
        if (!IsValidPtr(otherThing, 0x100)) continue;
        float otherPos[3] = {};
        GetThingWorldPos(otherThing, otherPos);
        if (Dist2D(ip, otherPos) > 3.5f || fabsf(ip[2] - otherPos[2]) > 1.2f) continue;

        const float* oFwd = reinterpret_cast<const float*>(otherThing + 0x90);
        float oFwdLen = sqrtf(oFwd[0] * oFwd[0] + oFwd[1] * oFwd[1]);
        float normalDot = (sFwdLen > 0.1f && oFwdLen > 0.1f)
            ? ((sFwd[0] * oFwd[0] + sFwd[1] * oFwd[1]) / (sFwdLen * oFwdLen))
            : 1.0f;

        if (SharesButtonTargetGuid(scBtn, otherSc) || normalDot < -0.35f) {
            uint32_t totalPhysicalPresses = GetSwitchPhysicalPressCount(interactGuid) + GetSwitchPhysicalPressCount(otherGuid);
            uint32_t btn30Self  = *reinterpret_cast<const uint32_t*>(scBtn + 0x30);
            uint32_t btn30Other = *reinterpret_cast<const uint32_t*>(otherSc + 0x30);
            bool pairAlreadyOpen = (totalPhysicalPresses > 0)
                ? ((totalPhysicalPresses % 2) == 1)
                : ((btn30Self ^ btn30Other) != 0);
            if (pairAlreadyOpen) {
                return true;
            }
        }
    }
    return false;
}

static void ForceResetSwitchForRetry(const void* world, uint32_t guid) {
    uint8_t* t = FindThingByGuid(world, guid);
    float ip[3] = {};
    if (t) GetThingWorldPos(t, ip);

    bool foundSelf = false;
    for (size_t i = 0; i < s_activatedSwitches.size(); ++i) {
        if (s_activatedSwitches[i].guid == guid) {
            s_activatedSwitches[i].count = 0;
            s_activatedSwitches[i].physicalPresses = 2; // Non-zero even parity forces physical press on retry
            foundSelf = true;
        } else if (t) {
            uint8_t* ot = FindThingByGuid(world, s_activatedSwitches[i].guid);
            if (ot) {
                float op[3] = {};
                GetThingWorldPos(ot, op);
                if (Dist2D(ip, op) < 2.8f && fabsf(ip[2] - op[2]) < 1.2f) {
                    s_activatedSwitches[i].physicalPresses = 0;
                }
            }
        }
    }
    if (!foundSelf) {
        ActivatedSwitchRecord rec = { guid, s_botFrame, 0, 2 };
        s_activatedSwitches.push_back(rec);
    }
}

static bool IsCorpseAlreadySearched(uint32_t guid) {
    for (size_t i = 0; i < s_searchedCorpses.size(); ++i) {
        if (s_searchedCorpses[i] == guid) return true;
    }
    return false;
}

static void MarkCorpseSearched(uint32_t guid) {
    if (!IsCorpseAlreadySearched(guid)) {
        s_searchedCorpses.push_back(guid);
    }
}

// ============================================================================
// Enemy Tactical Knowledge Base (Used in Both TRAIN and NORMAL Modes)
// ============================================================================

static const char* PolicyToString(EnemyTacticalPolicy p) {
    switch (p) {
        case POLICY_MUST_KILL:   return "MUST_KILL";
        case POLICY_IGNORE:      return "IGNORE";
        case POLICY_AVOID_CLOAK: return "AVOID_CLOAK";
        case POLICY_MELEE_RUSH:  return "MELEE_RUSH";
        case POLICY_BOSS_KITE:   return "BOSS_KITE";
        default:                 return "UNKNOWN";
    }
}

static EnemyTacticalPolicy ParsePolicyString(const char* s) {
    if (!s) return POLICY_UNKNOWN;
    if (_stricmp(s, "MUST_KILL") == 0)   return POLICY_MUST_KILL;
    if (_stricmp(s, "IGNORE") == 0)      return POLICY_IGNORE;
    if (_stricmp(s, "AVOID_CLOAK") == 0) return POLICY_AVOID_CLOAK;
    if (_stricmp(s, "MELEE_RUSH") == 0)  return POLICY_MELEE_RUSH;
    if (_stricmp(s, "BOSS_KITE") == 0)   return POLICY_BOSS_KITE;
    return POLICY_UNKNOWN;
}

static const char* StanceToString(CombatStance st) {
    switch (st) {
        case STANCE_IGNORE:       return "IGNORE";
        case STANCE_CLOAK_BYPASS: return "CLOAK_BYPASS";
        case STANCE_DEFENSIVE:    return "DEFENSIVE";
        case STANCE_ENGAGE:       return "ENGAGE";
        case STANCE_MELEE_RUSH:   return "MELEE_RUSH";
        case STANCE_BOSS_KITE:    return "BOSS_KITE";
        default:                  return "DEFENSIVE";
    }
}

static CombatStance ParseStanceString(const char* s) {
    if (!s) return STANCE_DEFENSIVE;
    if (_stricmp(s, "IGNORE") == 0)       return STANCE_IGNORE;
    if (_stricmp(s, "CLOAK_BYPASS") == 0) return STANCE_CLOAK_BYPASS;
    if (_stricmp(s, "DEFENSIVE") == 0)    return STANCE_DEFENSIVE;
    if (_stricmp(s, "ENGAGE") == 0)       return STANCE_ENGAGE;
    if (_stricmp(s, "MELEE_RUSH") == 0)   return STANCE_MELEE_RUSH;
    if (_stricmp(s, "BOSS_KITE") == 0)    return STANCE_BOSS_KITE;
    return STANCE_DEFENSIVE;
}

static EnemyTacticalEntry* FindOrCreateEnemyTactic(uint32_t guid, const char* name, const float pos[3]) {
    for (size_t i = 0; i < s_enemyTactics.size(); ++i) {
        if (s_enemyTactics[i].guid == guid) {
            if (pos) {
                s_enemyTactics[i].lastPos[0] = pos[0];
                s_enemyTactics[i].lastPos[1] = pos[1];
                s_enemyTactics[i].lastPos[2] = pos[2];
            }
            return &s_enemyTactics[i];
        }
    }
    EnemyTacticalEntry e = {};
    e.guid = guid;
    snprintf(e.name, sizeof(e.name), "%s", name ? name : "Enemy");
    // Default tactical heuristics before multimodal override:
    // Cameras are non-lethal triggers -> IGNORE; Zerglings -> MELEE_RUSH; others -> UNKNOWN
    if (name && (strcmp(name, "Camera") == 0 || strcmp(name, "Gauss Turret") == 0)) {
        e.policy      = POLICY_IGNORE;
        e.engageDist  = 0.0f;
        e.preferMelee = false;
    } else if (name && strcmp(name, "Zergling") == 0) {
        e.policy      = POLICY_MELEE_RUSH;
        e.engageDist  = 7.5f;
        e.preferMelee = true;
    } else {
        e.policy      = POLICY_UNKNOWN;
        e.engageDist  = 15.0f;
        e.preferMelee = false;
    }
    if (pos) {
        e.lastPos[0] = pos[0]; e.lastPos[1] = pos[1]; e.lastPos[2] = pos[2];
    }
    s_enemyTactics.push_back(e);
    return &s_enemyTactics.back();
}

// ============================================================================
// Raw Replay & Fixed-Time-Interval Trajectory Recorder (autobot_memory/raw_replays/)
// ============================================================================

static void EnsureDirectoriesExist() {
    if (!g_appRootDir[0]) return;
    snprintf(s_ioDir,       sizeof(s_ioDir),       "%s\\autobot_io", g_appRootDir);
    snprintf(s_memDir,      sizeof(s_memDir),      "%s\\autobot_memory", g_appRootDir);
    snprintf(s_compiledDir, sizeof(s_compiledDir), "%s\\autobot_memory\\compiled", g_appRootDir);
    CreateDirectoryA(s_ioDir, nullptr);
    CreateDirectoryA(s_memDir, nullptr);
    CreateDirectoryA(s_compiledDir, nullptr);

    char rawRoot[MAX_PATH] = {};
    snprintf(rawRoot, sizeof(rawRoot), "%s\\raw_replays", s_memDir);
    CreateDirectoryA(rawRoot, nullptr);
}

static void LogRawReplayEvent(const char* eventType, const float pos[3], const char* detailsJsonFields) {
    if (!s_currentRunDir[0]) return;
    char evPath[MAX_PATH] = {};
    snprintf(evPath, sizeof(evPath), "%s\\raw_events.jsonl", s_currentRunDir);
    FILE* f = fopen(evPath, "a");
    if (!f) return;
    float timeSec = (float)s_activeGameTicks / 60.0f;
    fprintf(f, "{\"tick\":%u,\"time_s\":%.2f,\"event\":\"%s\",\"pos\":[%.2f,%.2f,%.2f],%s}\n",
        s_activeGameTicks, timeSec, eventType,
        pos ? pos[0] : 0.0f, pos ? pos[1] : 0.0f, pos ? pos[2] : 0.0f,
        detailsJsonFields ? detailsJsonFields : "\"ok\":true");
    fclose(f);
}

static void SaveRawWalkedGraphSnapshot(const char* outcomeStr) {
    if (!s_currentRunDir[0] || s_rawSamples.empty()) return;
    char graphPath[MAX_PATH] = {};
    snprintf(graphPath, sizeof(graphPath), "%s\\raw_walked_graph.json", s_currentRunDir);
    FILE* f = fopen(graphPath, "w");
    if (!f) return;

    float totalTimeSec = (float)s_activeGameTicks / 60.0f;
    fprintf(f, "{\n");
    fprintf(f, "  \"level\": \"%s\",\n", s_currentLevel);
    fprintf(f, "  \"mode\": \"%s\",\n", (s_botMode == BOT_MODE_TRAIN) ? "TRAIN" : "NORMAL");
    fprintf(f, "  \"outcome\": \"%s\",\n", outcomeStr ? outcomeStr : "IN_PROGRESS");
    fprintf(f, "  \"sample_interval_frames\": %u,\n", kReplayIntervalFrames);
    fprintf(f, "  \"sample_interval_sec\": %.2f,\n", (float)kReplayIntervalFrames / 60.0f);
    fprintf(f, "  \"total_active_ticks\": %u,\n", s_activeGameTicks);
    fprintf(f, "  \"total_active_time_sec\": %.2f,\n", totalTimeSec);
    fprintf(f, "  \"total_kills\": %u,\n", s_killCount);
    fprintf(f, "  \"total_damage_taken\": %.1f,\n", s_totalDamageTakenThisRun);

    // 1. Raw time-sampled trajectory nodes
    fprintf(f, "  \"time_nodes\": [\n");
    for (size_t i = 0; i < s_rawSamples.size(); ++i) {
        const RawTrajectorySample& s = s_rawSamples[i];
        fprintf(f, "    {\"idx\":%u,\"tick\":%u,\"t\":%.2f,\"pos\":[%.2f,%.2f,%.2f],\"vel\":[%.2f,%.2f,%.2f],\"yaw\":%.1f,\"hp\":%.1f,\"ammo\":%u,\"state\":%u,\"blocked\":%u}%s\n",
            (unsigned)i, s.tick, s.timeSec,
            s.pos[0], s.pos[1], s.pos[2],
            s.vel[0], s.vel[1], s.vel[2],
            s.yaw, s.hp, s.ammo, s.ctrlState, s.blockedFlag,
            (i + 1 < s_rawSamples.size()) ? "," : "");
    }
    fprintf(f, "  ],\n");

    // 2. Discovered / Activated Mechanisms in this Training Run
    fprintf(f, "  \"mechanisms_discovered\": [\n");
    for (size_t i = 0; i < s_activatedSwitches.size(); ++i) {
        const ActivatedSwitchRecord& sw = s_activatedSwitches[i];
        float mp[3] = {};
        const char* mName = "Switch";
        void* swThing = s_fnFindGUID(sw.guid);
        if (IsValidPtr(swThing, 0x140)) {
            GetThingWorldPos(swThing, mp);
            mName = SafeGetThingName(swThing);
        }
        fprintf(f, "    {\"seq\":%u,\"guid\":\"0x%X\",\"name\":\"%s\",\"frame\":%u,\"count\":%u,\"pos\":[%.2f,%.2f,%.2f]}%s\n",
            (unsigned)(i + 1), sw.guid, mName, sw.activatedFrame, sw.count,
            mp[0], mp[1], mp[2],
            (i + 1 < s_activatedSwitches.size()) ? "," : "");
    }
    fprintf(f, "  ],\n");

    // 3. Raw enemy tactical observations
    fprintf(f, "  \"enemy_observations\": [\n");
    for (size_t i = 0; i < s_enemyTactics.size(); ++i) {
        const EnemyTacticalEntry& e = s_enemyTactics[i];
        fprintf(f, "    {\"guid\":\"0x%X\",\"name\":\"%s\",\"policy\":\"%s\",\"pos\":[%.2f,%.2f,%.2f],\"encounters\":%u,\"kills\":%u,\"dmg_taken\":%.1f}%s\n",
            e.guid, e.name, PolicyToString(e.policy),
            e.lastPos[0], e.lastPos[1], e.lastPos[2],
            e.timesEncountered, e.timesKilled, e.totalDmgTakenFrom,
            (i + 1 < s_enemyTactics.size()) ? "," : "");
    }
    fprintf(f, "  ]\n");
    fprintf(f, "}\n");
    fclose(f);
}

static void StartNewRawReplayRun(const char* levelName, const float startPos[3]) {
    EnsureDirectoriesExist();
    snprintf(s_currentLevel, sizeof(s_currentLevel), "%s", levelName ? levelName : "unknown");
    s_rawSamples.clear();
    s_activeGameTicks         = 0;
    s_totalDamageTakenThisRun = 0.0f;
    if (startPos) {
        s_prevSamplePos[0] = startPos[0];
        s_prevSamplePos[1] = startPos[1];
        s_prevSamplePos[2] = startPos[2];
    }

    // Only create raw_replays folders in BOT_MODE_TRAIN so NORMAL mode never clutters disk
    if (s_botMode != BOT_MODE_TRAIN) {
        s_currentRunDir[0] = '\0';
        return;
    }

    char levelReplayDir[MAX_PATH] = {};
    snprintf(levelReplayDir, sizeof(levelReplayDir), "%s\\raw_replays\\%s", s_memDir, s_currentLevel);
    CreateDirectoryA(levelReplayDir, nullptr);

    SYSTEMTIME st = {};
    GetLocalTime(&st);
    s_runSequenceId++;
    snprintf(s_currentRunDir, sizeof(s_currentRunDir),
        "%s\\run_%04u%02u%02u_%02u%02u%02u_%02u",
        levelReplayDir,
        st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond,
        s_runSequenceId);
    CreateDirectoryA(s_currentRunDir, nullptr);

    char framesDir[MAX_PATH] = {};
    snprintf(framesDir, sizeof(framesDir), "%s\\frames", s_currentRunDir);
    CreateDirectoryA(framesDir, nullptr);

    char details[128] = {};
    snprintf(details, sizeof(details), "\"level\":\"%s\",\"mode\":\"%s\"",
        s_currentLevel, (s_botMode == BOT_MODE_TRAIN) ? "TRAIN" : "NORMAL");
    LogRawReplayEvent("RUN_START", startPos, details);
    LogPrintf("[AutoBot] Started new Raw Replay recording at '%s'", s_currentRunDir);
}

static void RecordFixedTimeIntervalSample(
    const float playerPos[3], float yaw, float pitch, float hp, uint32_t ammo,
    uint8_t ctrlState, bool isBlocked)
{
    if (!s_currentRunDir[0]) return;
    if ((s_activeGameTicks % kReplayIntervalFrames) != 0) return;

    float dt = (float)kReplayIntervalFrames / 60.0f;
    RawTrajectorySample smp = {};
    smp.tick        = s_activeGameTicks;
    smp.timeSec     = (float)s_activeGameTicks / 60.0f;
    smp.pos[0]      = playerPos[0];
    smp.pos[1]      = playerPos[1];
    smp.pos[2]      = playerPos[2];
    if (s_rawSamples.empty()) {
        smp.vel[0] = smp.vel[1] = smp.vel[2] = 0.0f;
    } else {
        smp.vel[0] = (playerPos[0] - s_prevSamplePos[0]) / dt;
        smp.vel[1] = (playerPos[1] - s_prevSamplePos[1]) / dt;
        smp.vel[2] = (playerPos[2] - s_prevSamplePos[2]) / dt;
    }
    s_prevSamplePos[0] = playerPos[0];
    s_prevSamplePos[1] = playerPos[1];
    s_prevSamplePos[2] = playerPos[2];

    smp.yaw         = yaw;
    smp.pitch       = pitch;
    smp.hp          = hp;
    smp.ammo        = ammo;
    smp.ctrlState   = ctrlState;
    smp.blockedFlag = isBlocked ? 1 : 0;
    smp.cmdId       = s_curCmd.cmdId;
    s_rawSamples.push_back(smp);

    char trajPath[MAX_PATH] = {};
    snprintf(trajPath, sizeof(trajPath), "%s\\raw_trajectory.jsonl", s_currentRunDir);
    FILE* f = fopen(trajPath, "a");
    if (f) {
        fprintf(f, "{\"tick\":%u,\"time_s\":%.2f,\"pos\":[%.2f,%.2f,%.2f],\"vel\":[%.2f,%.2f,%.2f],\"yaw\":%.1f,\"pitch\":%.1f,\"hp\":%.1f,\"ammo\":%u,\"ctrl_state\":%u,\"blocked\":%u,\"cmd_id\":%u,\"lx\":%d,\"ly\":%d}\n",
            smp.tick, smp.timeSec,
            smp.pos[0], smp.pos[1], smp.pos[2],
            smp.vel[0], smp.vel[1], smp.vel[2],
            smp.yaw, smp.pitch, smp.hp, smp.ammo,
            smp.ctrlState, smp.blockedFlag, smp.cmdId,
            (int)s_botPad.sThumbLX, (int)s_botPad.sThumbLY);
        fclose(f);
    }

    // Periodically flush the raw_walked_graph.json every 10 samples (2.0 seconds)
    if ((s_rawSamples.size() % 10) == 0) {
        SaveRawWalkedGraphSnapshot("IN_PROGRESS");
    }
}

// ============================================================================
// Mode 2 (NORMAL MODE): Load & Plan on Compiled Learned Graph (<level>_learned.cfg)
// ============================================================================

static void ResolveCanonicalLevelName(const char* levelIdOrName, char outName[64], int* outLevelId) {
    if (!levelIdOrName || !levelIdOrName[0] ||
        _stricmp(levelIdOrName, "1") == 0 ||
        _stricmp(levelIdOrName, "level1") == 0 ||
        _stricmp(levelIdOrName, "lvl1") == 0 ||
        _strnicmp(levelIdOrName, "1_3_1", 5) == 0)
    {
        snprintf(outName, 64, "1_3_1b_Hive_StationOuter.nil");
        if (outLevelId) *outLevelId = 1;
        return;
    }
    if (_stricmp(levelIdOrName, "2") == 0 ||
        _stricmp(levelIdOrName, "level2") == 0 ||
        _stricmp(levelIdOrName, "lvl2") == 0 ||
        _strnicmp(levelIdOrName, "1_2_1", 5) == 0)
    {
        snprintf(outName, 64, "1_2_1_Miners_Bunker.nil");
        if (outLevelId) *outLevelId = 2;
        return;
    }
    snprintf(outName, 64, "%s", levelIdOrName);
    if (outLevelId) *outLevelId = 0;
}

static bool ParseCompiledLearnedFile(
    const char* levelName,
    std::vector<LearnedNavNode>& outNodes,
    std::vector<LearnedNavEdge>& outEdges,
    std::vector<LearnedMechanismStep>& outMechs,
    std::vector<LearnedObjective>* outObjs,
    std::vector<LearnedSupplyNode>* outSupplies,
    bool updateGlobalEnemyTactics)
{
    EnsureDirectoriesExist();
    outNodes.clear();
    outEdges.clear();
    outMechs.clear();
    if (outObjs)     outObjs->clear();
    if (outSupplies) outSupplies->clear();

    char canonicalName[64] = {};
    ResolveCanonicalLevelName(levelName, canonicalName, nullptr);

    char cfgPath[MAX_PATH] = {};
    snprintf(cfgPath, sizeof(cfgPath), "%s\\%s_learned.cfg", s_compiledDir, canonicalName);
    FILE* f = fopen(cfgPath, "r");
    if (!f) {
        LogPrintf("[AutoBot] No compiled learned file at '%s' yet.", cfgPath);
        return false;
    }

    char line[512] = {};
    while (fgets(line, sizeof(line), f)) {
        char* p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '#' || *p == '\r' || *p == '\n' || *p == '\0') continue;

        if (strncmp(p, "NAV_NODE ", 9) == 0) {
            LearnedNavNode n = {};
            if (sscanf(p + 9, "%d %f %f %f", &n.id, &n.x, &n.y, &n.z) == 4) {
                outNodes.push_back(n);
            }
        } else if (strncmp(p, "NAV_EDGE ", 9) == 0) {
            LearnedNavEdge e = {};
            e.gateMechId = 0;
            if (sscanf(p + 9, "%d %d %f %15s %d", &e.fromId, &e.toId, &e.costSec, e.moveType, &e.gateMechId) >= 3) {
                if (!e.moveType[0]) strcpy(e.moveType, "WALK");
                outEdges.push_back(e);
            }
        } else if (strncmp(p, "MECHANISM ", 10) == 0) {
            LearnedMechanismStep ms = {};
            int consumed = 0;
            if (sscanf(p + 10, "%d %d %i %d %23s %f %f %f %n",
                &ms.mechId, &ms.nodeId, &ms.guid, &ms.prereqMechId,
                ms.actionType, &ms.pos[0], &ms.pos[1], &ms.pos[2], &consumed) >= 8)
            {
                const char* solveStart = p + 10 + consumed;
                while (*solveStart == ' ' || *solveStart == '\t' || *solveStart == '|') solveStart++;
                snprintf(ms.howToSolve, sizeof(ms.howToSolve), "%s", solveStart);
                size_t slen = strlen(ms.howToSolve);
                while (slen > 0 && (ms.howToSolve[slen - 1] == '\r' || ms.howToSolve[slen - 1] == '\n')) {
                    ms.howToSolve[--slen] = '\0';
                }
                outMechs.push_back(ms);
            }
        } else if (updateGlobalEnemyTactics && strncmp(p, "ENEMY_TACTIC ", 13) == 0) {
            uint32_t guid = 0;
            char polStr[32] = {};
            float engDist = 15.0f;
            int prefMelee = 0;
            char eName[32] = "Enemy";
            if (sscanf(p + 13, "%i %31s %f %d %31s", &guid, polStr, &engDist, &prefMelee, eName) >= 4) {
                EnemyTacticalEntry* te = FindOrCreateEnemyTactic(guid, eName, nullptr);
                te->policy      = ParsePolicyString(polStr);
                te->engageDist  = engDist;
                te->preferMelee = (prefMelee != 0);
            }
        } else if (outSupplies && strncmp(p, "SUPPLY_NODE ", 12) == 0) {
            LearnedSupplyNode sn = {};
            int isCorp = 0;
            if (sscanf(p + 12, "%i %f %f %f %d", &sn.guid, &sn.pos[0], &sn.pos[1], &sn.pos[2], &isCorp) == 5) {
                sn.isCorpse = (isCorp != 0);
                outSupplies->push_back(sn);
            }
        } else if (outObjs && strncmp(p, "OBJECTIVE ", 10) == 0) {
            LearnedObjective obj = {};
            char stStr[32] = {};
            int consumed = 0;
            if (sscanf(p + 10, "%d %f %f %f %i %i %31s %n",
                &obj.step, &obj.targetPos[0], &obj.targetPos[1], &obj.targetPos[2],
                &obj.interactGuid, &obj.regionGuid, stStr, &consumed) >= 7)
            {
                obj.stance = ParseStanceString(stStr);
                const char* descStart = p + 10 + consumed;
                snprintf(obj.desc, sizeof(obj.desc), "%s", descStart);
                size_t dlen = strlen(obj.desc);
                while (dlen > 0 && (obj.desc[dlen - 1] == '\r' || obj.desc[dlen - 1] == '\n')) {
                    obj.desc[--dlen] = '\0';
                }
                outObjs->push_back(obj);
            }
        }
    }
    fclose(f);
    return !outNodes.empty();
}

static bool LoadCompiledLearnedLevel(const char* levelName) {
    s_normalObjCursor      = 0;
    s_normalAlignedOnStart = false;
    s_normalRoutePlanned   = false;
    s_normalStageIdx       = 0;
    s_normalStageWpCursor  = 0;
    s_normalNavPath.clear();
    s_normalNavCursor      = 0;
    s_normalLastGoalNode   = -1;
    s_normalLastRepathTick = 0;

    bool ok = ParseCompiledLearnedFile(
        levelName,
        s_learnedNodes,
        s_learnedEdges,
        s_learnedMechanisms,
        &s_learnedObjectives,
        &s_learnedSupplies,
        true);

    if (ok) {
        LogPrintf("[AutoBot] Loaded compiled learned data for '%s': %u nav nodes, %u nav edges, %u mechanisms, %u objectives, %u enemy tactics, %u supplies!",
            levelName,
            (unsigned)s_learnedNodes.size(), (unsigned)s_learnedEdges.size(),
            (unsigned)s_learnedMechanisms.size(), (unsigned)s_learnedObjectives.size(),
            (unsigned)s_enemyTactics.size(), (unsigned)s_learnedSupplies.size());
    }
    return ok && !s_learnedNodes.empty();
}

static int FindNodeIndexById(const std::vector<LearnedNavNode>& nodes, int nodeId) {
    for (size_t i = 0; i < nodes.size(); ++i) {
        if (nodes[i].id == nodeId) return (int)i;
    }
    return -1;
}

static bool IsTargetLevelLoadedInLiveWorld(const char* canonicalLevelName) {
    void* world = *reinterpret_cast<void**>(0x00812D78);
    if (!IsValidPtr(world, 0x100)) return false;
    const char* liveLvl = reinterpret_cast<const char*>(reinterpret_cast<uintptr_t>(world) + 0x08);
    if (!liveLvl || !liveLvl[0]) return false;
    return (_stricmp(liveLvl, canonicalLevelName) == 0);
}

static bool CheckMapWalkableSegment(const float a[3], const float b[3], bool canRaycastWorld) {
    float dz = fabsf(a[2] - b[2]);
    if (dz > 1.65f) return false;
    float d2d = Dist2D(a, b);
    if (d2d < 0.05f) return true;

    if (canRaycastWorld) {
        // 1. Horizontal wall/door collision check at torso (+1.15m) and knee (+0.55m) heights
        float aTorso[3] = { a[0], a[1], a[2] + 1.15f };
        float bTorso[3] = { b[0], b[1], b[2] + 1.15f };
        if (!CheckClearLineOfSight(aTorso, bTorso)) return false;

        float aKnee[3] = { a[0], a[1], a[2] + 0.55f };
        float bKnee[3] = { b[0], b[1], b[2] + 0.55f };
        if (!CheckClearLineOfSight(aKnee, bKnee)) return false;

        // 2. Vertical floor existence check ("where is walkable floor vs. pit/chasm") every 1.5m
        int steps = (int)(d2d / 1.5f) + 1;
        for (int s = 1; s < steps; ++s) {
            float t = (float)s / (float)steps;
            float sx = a[0] + (b[0] - a[0]) * t;
            float sy = a[1] + (b[1] - a[1]) * t;
            float sz = a[2] + (b[2] - a[2]) * t;
            float rayTop[3] = { sx, sy, sz + 1.0f };
            float rayBot[3] = { sx, sy, sz - 2.2f };
            // simCollision_LineOfSight returns != 0 when the vertical ray hits solid floor geometry
            if (s_fnCollisionLineOfSight(rayTop, rayBot, 10) == 0) {
                return false;
            }
        }
    }
    return true;
}

static bool IsNodeNearGateOrTransition(int nodeId, const std::vector<LearnedNavEdge>& baseEdges) {
    // Exclude nodes within 2 hops of any gated doorway (gateMechId > 0) or vertical transition (non-WALK edge)
    std::vector<int> hop1;
    for (size_t e = 0; e < baseEdges.size(); ++e) {
        bool isSpecial = (baseEdges[e].gateMechId > 0) || (strcmp(baseEdges[e].moveType, "WALK") != 0);
        if (isSpecial) {
            if (baseEdges[e].fromId == nodeId || baseEdges[e].toId == nodeId) return true;
            hop1.push_back(baseEdges[e].fromId);
            hop1.push_back(baseEdges[e].toId);
        }
    }
    for (size_t e = 0; e < baseEdges.size(); ++e) {
        for (size_t k = 0; k < hop1.size(); ++k) {
            if ((baseEdges[e].fromId == hop1[k] && baseEdges[e].toId == nodeId) ||
                (baseEdges[e].toId == hop1[k] && baseEdges[e].fromId == nodeId))
            {
                return true;
            }
        }
    }
    return false;
}

static void AugmentLearnedGraphWithWalkableFloor(
    const std::vector<LearnedNavNode>& nodes,
    std::vector<LearnedNavEdge>& edges,
    const char* canonicalLevelName)
{
    bool canRaycast = IsTargetLevelLoadedInLiveWorld(canonicalLevelName);
    if (!canRaycast) return;

    const std::vector<LearnedNavEdge> baseEdges = edges;
    for (size_t i = 0; i < nodes.size(); ++i) {
        if (IsNodeNearGateOrTransition(nodes[i].id, baseEdges)) continue;
        for (size_t j = i + 1; j < nodes.size(); ++j) {
            const LearnedNavNode& a = nodes[i];
            const LearnedNavNode& b = nodes[j];
            // Only add cross-pass floor shortcuts (abs(a.id - b.id) >= 25), preserving sequential obstacle-clearing waypoints along corridors
            if (abs(a.id - b.id) < 25) continue;
            float dz = fabsf(a.z - b.z);
            if (dz > 0.35f) continue;

            float pa[3] = { a.x, a.y, a.z };
            float pb[3] = { b.x, b.y, b.z };
            float d2d   = Dist2D(pa, pb);
            if (d2d > 8.5f) continue;
            if (IsNodeNearGateOrTransition(b.id, baseEdges)) continue;

            if (CheckMapWalkableSegment(pa, pb, true)) {
                LearnedNavEdge shortcut = {};
                shortcut.fromId     = a.id;
                shortcut.toId       = b.id;
                shortcut.costSec    = Dist3D(pa, pb) / 6.0f;
                strcpy(shortcut.moveType, "WALK");
                shortcut.gateMechId = 0;
                edges.push_back(shortcut);
            }
        }
    }
}

uint32_t QueryLiveSolvedMechanismMask(int levelId, const float currentPlayerPos[3]) {
    char canonicalName[64] = {};
    ResolveCanonicalLevelName((levelId == 2) ? "2" : "1", canonicalName, nullptr);
    if (!IsTargetLevelLoadedInLiveWorld(canonicalName)) {
        return 0u;
    }

    uint32_t mask = 0u;
    float livePos[3] = {};
    if (currentPlayerPos) {
        livePos[0] = currentPlayerPos[0];
        livePos[1] = currentPlayerPos[1];
        livePos[2] = currentPlayerPos[2];
    } else {
        void* pl = s_fnGetLocalPlayer();
        if (IsValidPtr(pl, 0x530)) GetThingWorldPos(pl, livePos);
    }

    // Generic live check for all learned mechanisms in the active level
    if (!s_learnedMechanisms.empty() && _stricmp(s_currentLevel, canonicalName) == 0) {
        for (size_t i = 0; i < s_learnedMechanisms.size(); ++i) {
            const LearnedMechanismStep& m = s_learnedMechanisms[i];
            if (m.mechId <= 0 || m.mechId >= 31 || m.guid == 0) continue;
            uint32_t reqAct = GetRequiredActivationsForMechId(m.guid, m.mechId);
            if (strcmp(m.actionType, "OPEN_SWITCH") == 0 || strcmp(m.actionType, "CALL_ELEVATOR") == 0) {
                const uint8_t* scBtn = FindScriptByOwnerGuid(m.guid, "cButton");
                bool btnActive = (scBtn && (*reinterpret_cast<const uint32_t*>(scBtn + 0x28) != 0 ||
                                            *reinterpret_cast<const uint32_t*>(scBtn + 0x30) != 0)) ||
                                 (GetSwitchActivationCount(m.guid) >= reqAct);
                if (btnActive) {
                    mask |= (1u << m.mechId);
                }
            } else if (strcmp(m.actionType, "HACK_TERMINAL") == 0) {
                const uint8_t* scCon = FindScriptByOwnerGuid(m.guid, "cConsole");
                bool hackDone = (scCon && *reinterpret_cast<const uint32_t*>(scCon + 0x2C) != 0) ||
                                (GetSwitchActivationCount(m.guid) >= reqAct);
                if (hackDone) {
                    mask |= (1u << m.mechId);
                }
            } else if (strcmp(m.actionType, "SHOOT_OBSTACLE") == 0) {
                if (GetSwitchActivationCount(m.guid) >= reqAct) {
                    mask |= (1u << m.mechId);
                }
            } else if (strcmp(m.actionType, "RIDE_ELEVATOR") == 0) {
                float destZ = GetElevatorDestinationZ(m.guid, m.pos[2]);
                if (GetSwitchActivationCount(m.guid) >= reqAct && fabsf(livePos[2] - destZ) < 2.0f) {
                    mask |= (1u << m.mechId);
                }
            }
        }
    }
    return mask;
}

static const LearnedMechanismStep* FindMechanismById(
    const std::vector<LearnedMechanismStep>& mechs,
    int mechId)
{
    for (size_t i = 0; i < mechs.size(); ++i) {
        if (mechs[i].mechId == mechId) return &mechs[i];
    }
    return nullptr;
}

// Generic prerequisite resolver driven directly by LearnedMechanismStep::prereqMechId from the level's .cfg file.
// Stops recursing as soon as an ancestor mechanism is already solved in `activeSolvedMask` so auto-closing doors
// behind already-completed milestones never force redundant backtracking.
static int FindEarliestUnsolvedScriptPrereq(
    const std::vector<LearnedMechanismStep>& mechs,
    int mechId,
    uint32_t activeSolvedMask,
    int depth = 0)
{
    if (mechId <= 0 || depth > 32) return 0;
    if ((activeSolvedMask & (1u << mechId)) != 0) return 0;
    const LearnedMechanismStep* m = FindMechanismById(mechs, mechId);
    if (!m || m->prereqMechId <= 0) return 0;
    int p = m->prereqMechId;
    if ((activeSolvedMask & (1u << p)) != 0) return 0;
    int deeper = FindEarliestUnsolvedScriptPrereq(mechs, p, activeSolvedMask, depth + 1);
    return (deeper > 0) ? deeper : p;
}

static int AnchorPositionToLearnedGraph(
    const std::vector<LearnedNavNode>& nodes,
    const float pos[3],
    const char* canonicalLevelName,
    bool isStartAnchor,
    bool preferEarlyStageInSharedRooms)
{
    int   bestIdx  = -1;
    float bestCost = 1e30f;
    bool  canRaycastWalls = IsTargetLevelLoadedInLiveWorld(canonicalLevelName);
    float eyeFrom[3] = { pos[0], pos[1], pos[2] + 1.15f };

    for (size_t i = 0; i < nodes.size(); ++i) {
        float np[3] = { nodes[i].x, nodes[i].y, nodes[i].z };
        float d2d   = Dist2D(pos, np);
        float dz    = fabsf(pos[2] - np[2]);
        float cost  = d2d + ((dz < 2.2f) ? (dz * 3.0f) : (30.0f + dz * 12.0f));

        // When the target level is loaded in-game, check wall collision via simCollision_LineOfSight (flags=10)
        // so an off-route position never anchors to a node on the other side of a solid wall!
        if (canRaycastWalls && dz < 2.4f && d2d > 0.45f && d2d < 25.0f) {
            float eyeTo[3] = { np[0], np[1], np[2] + 1.15f };
            if (!CheckClearLineOfSight(eyeFrom, eyeTo)) {
                cost += 45.0f;
            }
        }

        // Generic trajectory-phase disambiguation for rooms visited both early in the mission and during final extraction
        if (nodes.size() >= 40) {
            if (isStartAnchor && preferEarlyStageInSharedRooms && i >= (nodes.size() * 4) / 5) {
                cost += 8.0f;
            } else if (!isStartAnchor && !preferEarlyStageInSharedRooms && i < nodes.size() / 4) {
                cost += 8.0f;
            }
        }

        if (cost < bestCost) {
            bestCost = cost;
            bestIdx  = (int)i;
        }
    }
    return bestIdx;
}

static int FindNearestLearnedNode(const float pos[3]) {
    return AnchorPositionToLearnedGraph(s_learnedNodes, pos, s_currentLevel, true, !s_inLevel1Escape);
}

static bool ComputeSingleLegDijkstra(
    const std::vector<LearnedNavNode>& nodes,
    const std::vector<LearnedNavEdge>& edges,
    int startIdx,
    int goalIdx,
    uint32_t unlockedMechMask,
    int allowedTargetMechId,
    bool allowLockedGatesWithPenalty,
    std::vector<int>& outNodeIndices,
    std::vector<const LearnedNavEdge*>* outUsedEdges)
{
    outNodeIndices.clear();
    if (outUsedEdges) outUsedEdges->clear();
    int n = (int)nodes.size();
    if (n <= 0 || startIdx < 0 || goalIdx < 0 || startIdx >= n || goalIdx >= n) return false;
    if (startIdx == goalIdx) {
        outNodeIndices.push_back(startIdx);
        return true;
    }

    std::vector<float> dist(n, 1e30f);
    std::vector<int>   prev(n, -1);
    std::vector<const LearnedNavEdge*> prevEdge(n, nullptr);
    std::vector<bool>  vis(n, false);
    dist[startIdx] = 0.0f;

    for (int step = 0; step < n; ++step) {
        int u = -1;
        float bestD = 1e30f;
        for (int i = 0; i < n; ++i) {
            if (!vis[i] && dist[i] < bestD) {
                bestD = dist[i];
                u = i;
            }
        }
        if (u < 0 || u == goalIdx || bestD >= 1e29f) break;
        vis[u] = true;

        int uId = nodes[u].id;
        float uPos[3] = { nodes[u].x, nodes[u].y, nodes[u].z };

        for (size_t e = 0; e < edges.size(); ++e) {
            int   nextId      = -1;
            float reverseBias = 0.0f;
            if (edges[e].fromId == uId) {
                nextId = edges[e].toId;
            } else if (strcmp(edges[e].moveType, "WALK") == 0 && edges[e].toId == uId) {
                nextId      = edges[e].fromId;
                reverseBias = 0.15f;
            }
            if (nextId < 0) continue;

            int v = FindNodeIndexById(nodes, nextId);
            if (v < 0 || vis[v]) continue;

            float vPos[3] = { nodes[v].x, nodes[v].y, nodes[v].z };
            if (strcmp(edges[e].moveType, "WALK") == 0 && fabsf(uPos[2] - vPos[2]) > 1.85f && edges[e].toId == uId) {
                // Do not walk backwards up a steep one-way drop disguised as WALK
                continue;
            }

            int gateId = edges[e].gateMechId;
            float gatePenalty = 0.0f;
            if (gateId > 0) {
                bool isGateUnlocked = ((unlockedMechMask & (1u << gateId)) != 0) || (gateId == allowedTargetMechId);
                if (!isGateUnlocked) {
                    if (!allowLockedGatesWithPenalty) continue;
                    gatePenalty = 35.0f;
                }
            }

            float w = (edges[e].costSec > 0.01f ? edges[e].costSec : (Dist3D(uPos, vPos) / 6.0f)) + reverseBias + gatePenalty;
            if (dist[u] + w < dist[v]) {
                dist[v]     = dist[u] + w;
                prev[v]     = u;
                prevEdge[v] = &edges[e];
            }
        }
    }

    int cur = goalIdx;
    if (prev[cur] < 0 && cur != startIdx) return false;
    int safety = 0;
    while (cur >= 0 && safety < n + 2) {
        outNodeIndices.push_back(cur);
        if (cur == startIdx) break;
        if (outUsedEdges && prevEdge[cur]) outUsedEdges->push_back(prevEdge[cur]);
        cur = prev[cur];
        safety++;
    }
    std::reverse(outNodeIndices.begin(), outNodeIndices.end());
    if (outUsedEdges) std::reverse(outUsedEdges->begin(), outUsedEdges->end());
    return !outNodeIndices.empty();
}

static bool ComputePathOnLearnedGraph(const float startPos[3], const float goalPos[3], std::vector<int>& outPath) {
    outPath.clear();
    LearnedRoutePlan plan;
    if (!PlanLearnedLevelRoute(s_currentLevel, startPos, goalPos, &plan, MECH_STATE_AUTO_LIVE)) {
        return false;
    }
    for (uint32_t i = 0; i < plan.waypointCount; ++i) {
        int nodeId = plan.waypoints[i].nodeId;
        if (nodeId > 0) {
            int idx = FindNodeIndexById(s_learnedNodes, nodeId);
            if (idx >= 0 && (outPath.empty() || outPath.back() != idx)) {
                outPath.push_back(idx);
            }
        }
    }
    return !outPath.empty();
}

static void AppendStageLegToPlan(
    LearnedRoutePlan* outPlan,
    const std::vector<LearnedNavNode>& nodes,
    const std::vector<LearnedNavEdge>& edges,
    const std::vector<int>& legNodes,
    const float stageFromPos[3],
    const float stageToPos[3],
    bool isVeryFirstStage,
    bool isVeryLastStage,
    const LearnedMechanismStep* stageEndMech)
{
    if (legNodes.empty() || outPlan->stageCount >= 24) return;

    LearnedRouteStage& st = outPlan->stages[outPlan->stageCount++];
    memset(&st, 0, sizeof(st));
    st.stageIndex       = (int)outPlan->stageCount;
    st.fromPos[0]       = stageFromPos[0];
    st.fromPos[1]       = stageFromPos[1];
    st.fromPos[2]       = stageFromPos[2];
    st.toPos[0]         = stageToPos[0];
    st.toPos[1]         = stageToPos[1];
    st.toPos[2]         = stageToPos[2];
    st.fromNodeId       = nodes[legNodes.front()].id;
    st.toNodeId         = nodes[legNodes.back()].id;
    st.waypointStartIdx = outPlan->waypointCount;
    st.hasMechanismAction = (stageEndMech != nullptr);
    if (stageEndMech) {
        st.mechanism = *stageEndMech;
    }

    // If this is the very first stage and startPos is off-graph (> 0.55m from first node), emit START approach waypoint
    if (isVeryFirstStage) {
        float firstNodePos[3] = { nodes[legNodes[0]].x, nodes[legNodes[0]].y, nodes[legNodes[0]].z };
        float offStartDist    = Dist3D(stageFromPos, firstNodePos);
        if (offStartDist > 0.55f && outPlan->waypointCount < 256) {
            LearnedRouteWaypoint& wp = outPlan->waypoints[outPlan->waypointCount++];
            wp.nodeId         = 0;
            wp.pos[0]         = stageFromPos[0];
            wp.pos[1]         = stageFromPos[1];
            wp.pos[2]         = stageFromPos[2];
            strcpy(wp.moveType, "START");
            wp.segmentCostSec = offStartDist / 6.0f;
            wp.triggerMechId  = 0;
            st.stageDistanceMeters       += offStartDist;
            st.stageCostSec              += wp.segmentCostSec;
            outPlan->totalDistanceMeters += offStartDist;
            outPlan->totalCostSec        += wp.segmentCostSec;
        }
    }

    for (size_t i = 0; i < legNodes.size() && outPlan->waypointCount < 256; ++i) {
        int curIdx = legNodes[i];
        const LearnedNavNode& nd = nodes[curIdx];

        // Avoid duplicating the boundary node between consecutive stages in the global waypoints array
        if (!isVeryFirstStage && i == 0 && outPlan->waypointCount > 0 &&
            outPlan->waypoints[outPlan->waypointCount - 1].nodeId == nd.id)
        {
            if (i + 1 == legNodes.size() && stageEndMech) {
                outPlan->waypoints[outPlan->waypointCount - 1].triggerMechId = stageEndMech->mechId;
            }
            continue;
        }

        LearnedRouteWaypoint& wp = outPlan->waypoints[outPlan->waypointCount++];
        wp.nodeId        = nd.id;
        wp.pos[0]        = nd.x;
        wp.pos[1]        = nd.y;
        wp.pos[2]        = nd.z;
        wp.triggerMechId = (i + 1 == legNodes.size() && stageEndMech) ? stageEndMech->mechId : 0;

        if (i == 0) {
            strcpy(wp.moveType, "WALK");
            wp.segmentCostSec = 0.0f;
        } else {
            int prevIdx    = legNodes[i - 1];
            int prevId     = nodes[prevIdx].id;
            float prevP[3] = { nodes[prevIdx].x, nodes[prevIdx].y, nodes[prevIdx].z };
            float curP[3]  = { nd.x, nd.y, nd.z };
            float segDist  = Dist3D(prevP, curP);

            const LearnedNavEdge* matchedEdge = nullptr;
            for (size_t e = 0; e < edges.size(); ++e) {
                if ((edges[e].fromId == prevId && edges[e].toId == nd.id) ||
                    (strcmp(edges[e].moveType, "WALK") == 0 && edges[e].toId == prevId && edges[e].fromId == nd.id))
                {
                    matchedEdge = &edges[e];
                    break;
                }
            }
            if (matchedEdge) {
                snprintf(wp.moveType, sizeof(wp.moveType), "%s", matchedEdge->moveType);
                wp.segmentCostSec = matchedEdge->costSec;
            } else {
                strcpy(wp.moveType, "WALK");
                wp.segmentCostSec = segDist / 6.0f;
            }
            st.stageDistanceMeters       += segDist;
            st.stageCostSec              += wp.segmentCostSec;
            outPlan->totalDistanceMeters += segDist;
            outPlan->totalCostSec        += wp.segmentCostSec;
        }
    }

    if (isVeryLastStage) {
        int lastIdx = legNodes.back();
        float lastNodePos[3] = { nodes[lastIdx].x, nodes[lastIdx].y, nodes[lastIdx].z };
        float offEndDist     = Dist3D(lastNodePos, stageToPos);
        if (offEndDist > 0.55f && outPlan->waypointCount < 256) {
            LearnedRouteWaypoint& wp = outPlan->waypoints[outPlan->waypointCount++];
            wp.nodeId         = -1;
            wp.pos[0]         = stageToPos[0];
            wp.pos[1]         = stageToPos[1];
            wp.pos[2]         = stageToPos[2];
            strcpy(wp.moveType, "GOAL");
            wp.segmentCostSec = offEndDist / 6.0f;
            wp.triggerMechId  = 0;
            st.stageDistanceMeters       += offEndDist;
            st.stageCostSec              += wp.segmentCostSec;
            outPlan->totalDistanceMeters += offEndDist;
            outPlan->totalCostSec        += wp.segmentCostSec;
        }
    }

    st.waypointCount = outPlan->waypointCount - st.waypointStartIdx;
}

bool PlanLearnedLevelRoute(
    const char* levelIdOrName,
    const float startPos[3],
    const float endPos[3],
    LearnedRoutePlan* outPlan,
    uint32_t solvedMechMaskOverride)
{
    if (!outPlan || !startPos || !endPos) return false;
    memset(outPlan, 0, sizeof(LearnedRoutePlan));

    ResolveCanonicalLevelName(levelIdOrName, outPlan->levelName, &outPlan->levelId);
    outPlan->startPos[0] = startPos[0];
    outPlan->startPos[1] = startPos[1];
    outPlan->startPos[2] = startPos[2];
    outPlan->endPos[0]   = endPos[0];
    outPlan->endPos[1]   = endPos[1];
    outPlan->endPos[2]   = endPos[2];

    std::vector<LearnedNavNode>       nodes;
    std::vector<LearnedNavEdge>       edges;
    std::vector<LearnedMechanismStep> mechs;

    if (!s_learnedNodes.empty() && _stricmp(s_currentLevel, outPlan->levelName) == 0 && !s_learnedMechanisms.empty()) {
        nodes = s_learnedNodes;
        edges = s_learnedEdges;
        mechs = s_learnedMechanisms;
    } else if (!ParseCompiledLearnedFile(outPlan->levelName, nodes, edges, mechs, nullptr, nullptr, false)) {
        return false;
    }
    if (nodes.empty()) return false;

    // 1. Augment learned topological edges with walkable same-floor geometry shortcuts
    AugmentLearnedGraphWithWalkableFloor(nodes, edges, outPlan->levelName);

    // 2. Determine live (or overridden) mechanism solved bitmask (handles checkpoint reset after Nova dies!)
    uint32_t initialSolvedMask = (solvedMechMaskOverride == MECH_STATE_AUTO_LIVE)
        ? QueryLiveSolvedMechanismMask(outPlan->levelId, startPos)
        : solvedMechMaskOverride;
    outPlan->solvedMechMaskUsed = initialSolvedMask;

    float lastNodePos[3] = { nodes.back().x, nodes.back().y, nodes.back().z };
    bool  endIsFinalExtract = (Dist3D(endPos, lastNodePos) < 6.0f);
    bool  lateStageSolved   = (mechs.size() >= 2 && (initialSolvedMask & (1u << mechs[mechs.size() - 2].mechId)) != 0);
    bool  startPreferEarly  = !lateStageSolved;

    int startIdx = AnchorPositionToLearnedGraph(nodes, startPos, outPlan->levelName, true,  startPreferEarly);
    int endIdx   = AnchorPositionToLearnedGraph(nodes, endPos,   outPlan->levelName, false, !endIsFinalExtract);
    if (startIdx < 0 || endIdx < 0) return false;

    outPlan->startNodeId = nodes[startIdx].id;
    outPlan->endNodeId   = nodes[endIdx].id;

    // 3. Iterative Stage-by-Stage Decomposition:
    //    Stage 1: Start -> Mechanism_1 + [Crack Mechanism_1]
    //    Stage 2: Mechanism_1 -> Mechanism_2 + [Crack Mechanism_2]
    //    ...
    //    Stage K: Last Mechanism -> EndPos
    uint32_t activeMask = initialSolvedMask;
    int      curIdx     = startIdx;
    float    curPos[3]  = { startPos[0], startPos[1], startPos[2] };
    bool     firstStage = true;

    // Check if endIdx itself has an associated mechanism (e.g., Extraction Region or a specific target console)
    int goalMechId = 0;
    for (size_t i = 0; i < mechs.size(); ++i) {
        if (mechs[i].nodeId == nodes[endIdx].id) {
            goalMechId = mechs[i].mechId;
            break;
        }
    }

    for (int iter = 0; iter < 32; ++iter) {
        // Check if goalMechId has an unsolved script-logic prerequisite
        int pendingGoalPrereq = (goalMechId > 0)
            ? FindEarliestUnsolvedScriptPrereq(mechs, goalMechId, activeMask)
            : 0;

        // Try routing directly from curIdx to endIdx using ONLY currently unlocked edges
        std::vector<int> unlockedPath;
        if (pendingGoalPrereq == 0 && (goalMechId == 0 || (activeMask & (1u << goalMechId)) != 0)) {
            if (ComputeSingleLegDijkstra(nodes, edges, curIdx, endIdx, activeMask, 0, false, unlockedPath, nullptr)) {
                AppendStageLegToPlan(outPlan, nodes, edges, unlockedPath, curPos, endPos, firstStage, true, nullptr);
                break;
            }
        }

        // Otherwise, determine which locked edge gate or remote prerequisite blocks reaching endIdx
        int targetMechId = pendingGoalPrereq;
        if (targetMechId == 0) {
            std::vector<int> probeNodes;
            std::vector<const LearnedNavEdge*> probeEdges;
            if (!ComputeSingleLegDijkstra(nodes, edges, curIdx, endIdx, activeMask, 0, true, probeNodes, &probeEdges)) {
                return false;
            }
            for (size_t e = 0; e < probeEdges.size(); ++e) {
                int gm = probeEdges[e]->gateMechId;
                if (gm > 0 && (activeMask & (1u << gm)) == 0) {
                    targetMechId = gm;
                    break;
                }
            }
            if (targetMechId == 0 && goalMechId > 0 && (activeMask & (1u << goalMechId)) == 0) {
                targetMechId = goalMechId;
            }
        }
        if (targetMechId == 0) {
            // Fallback: route to endIdx
            std::vector<int> fallbackNodes;
            if (!ComputeSingleLegDijkstra(nodes, edges, curIdx, endIdx, activeMask, 0, true, fallbackNodes, nullptr)) {
                return false;
            }
            AppendStageLegToPlan(outPlan, nodes, edges, fallbackNodes, curPos, endPos, firstStage, true, nullptr);
            break;
        }

        // Expand remote script-logic prerequisites of targetMechId
        int remotePrereq = FindEarliestUnsolvedScriptPrereq(mechs, targetMechId, activeMask);
        if (remotePrereq > 0) {
            targetMechId = remotePrereq;
        }

        // Now find the physical path from curIdx to targetMechId's node; if an earlier locked physical door/barrier
        // blocks that corridor, crack the first one!
        for (int refine = 0; refine < 8; ++refine) {
            const LearnedMechanismStep* tm = FindMechanismById(mechs, targetMechId);
            if (!tm) break;
            int tmNodeIdx = FindNodeIndexById(nodes, tm->nodeId);
            if (tmNodeIdx < 0) break;

            std::vector<int> toTargetNodes;
            std::vector<const LearnedNavEdge*> toTargetEdges;
            if (!ComputeSingleLegDijkstra(nodes, edges, curIdx, tmNodeIdx, activeMask, targetMechId, true, toTargetNodes, &toTargetEdges)) {
                break;
            }
            int earlierGate = 0;
            for (size_t e = 0; e < toTargetEdges.size(); ++e) {
                int gm = toTargetEdges[e]->gateMechId;
                if (gm > 0 && gm != targetMechId && (activeMask & (1u << gm)) == 0) {
                    earlierGate = gm;
                    break;
                }
            }
            if (earlierGate > 0) {
                int earlierRemote = FindEarliestUnsolvedScriptPrereq(mechs, earlierGate, activeMask);
                targetMechId = (earlierRemote > 0) ? earlierRemote : earlierGate;
            } else {
                break;
            }
        }

        const LearnedMechanismStep* nextMech = FindMechanismById(mechs, targetMechId);
        if (!nextMech) break;
        int nextMechNodeIdx = FindNodeIndexById(nodes, nextMech->nodeId);
        if (nextMechNodeIdx < 0) break;

        // Compute the unlocked sub-path for this stage: curIdx -> nextMechNodeIdx
        std::vector<int> stageNodes;
        if (!ComputeSingleLegDijkstra(nodes, edges, curIdx, nextMechNodeIdx, activeMask, nextMech->mechId, false, stageNodes, nullptr)) {
            ComputeSingleLegDijkstra(nodes, edges, curIdx, nextMechNodeIdx, activeMask, nextMech->mechId, true, stageNodes, nullptr);
        }

        LearnedMechanismStep solvedStep = *nextMech;
        solvedStep.isAlreadySolved = false;
        if (outPlan->mechanismCount < 32) {
            outPlan->mechanisms[outPlan->mechanismCount++] = solvedStep;
        }

        bool reachesFinalNode = (nextMechNodeIdx == endIdx && Dist3D(nextMech->pos, endPos) <= 0.55f);
        AppendStageLegToPlan(outPlan, nodes, edges, stageNodes, curPos, nextMech->pos, firstStage, reachesFinalNode, &solvedStep);
        firstStage = false;

        // Cracking `nextMech` unlocks its bit in activeMask for the subsequent stages!
        activeMask |= (1u << nextMech->mechId);
        curIdx    = nextMechNodeIdx;
        curPos[0] = nextMech->pos[0];
        curPos[1] = nextMech->pos[1];
        curPos[2] = nextMech->pos[2];

        if (reachesFinalNode) {
            break;
        }
    }

    return (outPlan->waypointCount > 0);
}

bool PlanLearnedLevelRoute(
    int levelId,
    const float startPos[3],
    const float endPos[3],
    LearnedRoutePlan* outPlan,
    uint32_t solvedMechMaskOverride)
{
    const char* lvlStr = (levelId == 2) ? "2" : "1";
    return PlanLearnedLevelRoute(lvlStr, startPos, endPos, outPlan, solvedMechMaskOverride);
}

void DumpLearnedRoutePlan(const LearnedRoutePlan& plan, const char* optionalOutFilePath) {
    LogPrintf("[RoutePlanner] === Stage-Decomposed Route & Mechanism Plan for Level %d (%s) ===", plan.levelId, plan.levelName);
    LogPrintf("[RoutePlanner] Start=(%.2f, %.2f, %.2f) [Node %d] -> End=(%.2f, %.2f, %.2f) [Node %d] | SolvedMask=0x%05X",
        plan.startPos[0], plan.startPos[1], plan.startPos[2], plan.startNodeId,
        plan.endPos[0],   plan.endPos[1],   plan.endPos[2],   plan.endNodeId,
        plan.solvedMechMaskUsed);
    LogPrintf("[RoutePlanner] Stages: %u | Waypoints: %u | Total Dist: %.1fm | Est. Time: %.1fs | Mechanisms to Crack: %u",
        plan.stageCount, plan.waypointCount, plan.totalDistanceMeters, plan.totalCostSec, plan.mechanismCount);

    for (uint32_t s = 0; s < plan.stageCount; ++s) {
        const LearnedRouteStage& st = plan.stages[s];
        if (st.hasMechanismAction) {
            LogPrintf("[RoutePlanner]   Stage %2d: Node %3d -> Node %3d (%.1fm, %.1fs, %u wps) => CRACK MECH_%d (0x%X %s): %s",
                st.stageIndex, st.fromNodeId, st.toNodeId, st.stageDistanceMeters, st.stageCostSec, st.waypointCount,
                st.mechanism.mechId, st.mechanism.guid, st.mechanism.actionType, st.mechanism.howToSolve);
        } else {
            LogPrintf("[RoutePlanner]   Stage %2d: Node %3d -> Goal Node %3d (%.1fm, %.1fs, %u wps) => ARRIVE AT DESTINATION",
                st.stageIndex, st.fromNodeId, st.toNodeId, st.stageDistanceMeters, st.stageCostSec, st.waypointCount);
        }
    }

    if (optionalOutFilePath && optionalOutFilePath[0]) {
        FILE* f = fopen(optionalOutFilePath, "w");
        if (f) {
            fprintf(f, "# ============================================================================\n");
            fprintf(f, "# Stage-Decomposed Level Route & Mechanism Plan\n");
            fprintf(f, "# Level: %d (%s)\n", plan.levelId, plan.levelName);
            fprintf(f, "# Start: (%.2f, %.2f, %.2f) [Anchored Node %d]\n",
                plan.startPos[0], plan.startPos[1], plan.startPos[2], plan.startNodeId);
            fprintf(f, "# End:   (%.2f, %.2f, %.2f) [Anchored Node %d]\n",
                plan.endPos[0], plan.endPos[1], plan.endPos[2], plan.endNodeId);
            fprintf(f, "# Current Solved Mechanism Mask: 0x%05X\n", plan.solvedMechMaskUsed);
            fprintf(f, "# Decomposed Stages: %u | Total Waypoints: %u | Mechanisms to Crack: %u\n",
                plan.stageCount, plan.waypointCount, plan.mechanismCount);
            fprintf(f, "# Total Distance: %.2f m | Est. Traversal Cost: %.2f s\n",
                plan.totalDistanceMeters, plan.totalCostSec);
            fprintf(f, "# ============================================================================\n\n");

            fprintf(f, "[STAGE_BY_STAGE_DECOMPOSITION (%u Stages)]\n", plan.stageCount);
            for (uint32_t s = 0; s < plan.stageCount; ++s) {
                const LearnedRouteStage& st = plan.stages[s];
                fprintf(f, "------------------------------------------------------------------------------\n");
                if (st.hasMechanismAction) {
                    fprintf(f, "STAGE %u: Navigate (%.2f, %.2f, %.2f) [Node %d] -> MECH_%d at (%.2f, %.2f, %.2f) [Node %d]\n",
                        st.stageIndex,
                        st.fromPos[0], st.fromPos[1], st.fromPos[2], st.fromNodeId,
                        st.mechanism.mechId,
                        st.toPos[0], st.toPos[1], st.toPos[2], st.toNodeId);
                } else {
                    fprintf(f, "STAGE %u (FINAL): Navigate (%.2f, %.2f, %.2f) [Node %d] -> Destination (%.2f, %.2f, %.2f) [Node %d]\n",
                        st.stageIndex,
                        st.fromPos[0], st.fromPos[1], st.fromPos[2], st.fromNodeId,
                        st.toPos[0], st.toPos[1], st.toPos[2], st.toNodeId);
                }
                fprintf(f, "  Sub-Path Metrics: %.2f m | %.2f s | %u waypoints:\n",
                    st.stageDistanceMeters, st.stageCostSec, st.waypointCount);
                for (uint32_t w = 0; w < st.waypointCount; ++w) {
                    const LearnedRouteWaypoint& wp = plan.waypoints[st.waypointStartIdx + w];
                    fprintf(f, "    -> WP %2u (Node %3d): (%7.2f, %7.2f, %6.2f) [%s, %.1fs]\n",
                        w + 1, wp.nodeId, wp.pos[0], wp.pos[1], wp.pos[2], wp.moveType, wp.segmentCostSec);
                }
                if (st.hasMechanismAction) {
                    fprintf(f, "  ACTION AT STAGE END -> [CRACK MECH_%d | GUID 0x%X | %s]:\n",
                        st.mechanism.mechId, st.mechanism.guid, st.mechanism.actionType);
                    fprintf(f, "    HowToSolve: %s\n", st.mechanism.howToSolve);
                } else {
                    fprintf(f, "  ACTION AT STAGE END -> [ARRIVE AT FINAL DESTINATION]\n");
                }
            }
            fprintf(f, "------------------------------------------------------------------------------\n");
            fclose(f);
        }
    }
}

// ============================================================================
// Mode 1 (TRAIN MODE): Multimodal State Export & Command Parser (autobot_io/)
// ============================================================================

static void WriteMultimodalStateAndRequestVision(
    void* world, uint8_t* player, uint8_t* cam, uint8_t* ctrl,
    const float playerPos[3], const float eyePos[3], const char* statusReason)
{
    EnsureDirectoriesExist();
    s_stepCounter++;

    // 1. Request both the live vision sheet in autobot_io/vision_sheet.png and an archival copy in the run folder
    RequestBackbufferScreenshot("autobot_io\\vision_sheet.png");

    float yaw   = IsValidPtr(cam, 0x250) ? *reinterpret_cast<float*>(cam + 0x20C) : 0.0f;
    float pitch = IsValidPtr(cam, 0x250) ? *reinterpret_cast<float*>(cam + 0x204) : 0.0f;
    float hp    = *reinterpret_cast<float*>(player + 0x36C);
    float maxHp = *reinterpret_cast<float*>(player + 0x370);
    uint32_t ammo    = *reinterpret_cast<uint32_t*>(player + 0x438);
    uint32_t maxAmmo = *reinterpret_cast<uint32_t*>(player + 0x448);
    uint8_t ctrlSt   = IsValidPtr(ctrl, 0x60) ? ctrl[0x5E] : 0;

    char statePath[MAX_PATH] = {};
    snprintf(statePath, sizeof(statePath), "%s\\state.json", s_ioDir);
    FILE* f = fopen(statePath, "w");
    if (!f) return;

    fprintf(f, "{\n");
    fprintf(f, "  \"step_id\": %u,\n", s_stepCounter);
    fprintf(f, "  \"last_cmd_id\": %u,\n", s_lastExecutedCmdId);
    fprintf(f, "  \"status\": \"%s\",\n", statusReason ? statusReason : "READY");
    fprintf(f, "  \"level\": \"%s\",\n", s_currentLevel);
    fprintf(f, "  \"active_ticks\": %u,\n", s_activeGameTicks);
    fprintf(f, "  \"active_time_s\": %.2f,\n", (float)s_activeGameTicks / 60.0f);
    fprintf(f, "  \"pos\": [%.2f, %.2f, %.2f],\n", playerPos[0], playerPos[1], playerPos[2]);
    fprintf(f, "  \"yaw\": %.1f,\n", yaw);
    fprintf(f, "  \"pitch\": %.1f,\n", pitch);
    fprintf(f, "  \"hp\": %.1f,\n", hp);
    fprintf(f, "  \"max_hp\": %.1f,\n", maxHp);
    fprintf(f, "  \"ammo\": %u,\n", ammo);
    fprintf(f, "  \"max_ammo\": %u,\n", maxAmmo);
    fprintf(f, "  \"ctrl_state\": %u,\n", ctrlSt);
    fprintf(f, "  \"kills\": %u,\n", s_killCount);
    fprintf(f, "  \"vision_image\": \"autobot_io/vision_sheet.png\",\n");

    // 2. Radar Enemies (allowed by user: shows nearby spawned enemies + LOS + current tactical tag)
    fprintf(f, "  \"radar_enemies\": [\n");
    uint8_t* w = reinterpret_cast<uint8_t*>(world);
    uint8_t* curThing = IsValidPtr(w, 0x100) ? *reinterpret_cast<uint8_t**>(w + 0xE4) : nullptr;
    int scanCount = 0;
    int enemyOutCount = 0;
    while (IsValidPtr(curThing, 0x140) && scanCount < 2048) {
        if (curThing != player && *reinterpret_cast<uint32_t*>(curThing + 0x10) == 0 && s_fnIsValidAssistTarget(curThing) != 0) {
            float ep[3] = {};
            GetThingWorldPos(curThing, ep);
            float d = Dist3D(playerPos, ep);
            if (d < 35.0f) {
                uint32_t eGuid = *reinterpret_cast<uint32_t*>(curThing + 0x34);
                float eHp      = *reinterpret_cast<float*>(curThing + 0x36C);
                const char* eName = SafeGetThingName(curThing);
                float tpos[3] = { ep[0], ep[1], ep[2] + 1.05f };
                s_fnThingGetTargetPos(curThing, nullptr, tpos, 0);
                bool los = CheckClearLineOfSight(eyePos, tpos);
                EnemyTacticalEntry* te = FindOrCreateEnemyTactic(eGuid, eName, ep);
                fprintf(f, "    %s{\"guid\":\"0x%X\",\"name\":\"%s\",\"pos\":[%.2f,%.2f,%.2f],\"dist\":%.1f,\"hp\":%.1f,\"los\":%s,\"policy\":\"%s\"}\n",
                    (enemyOutCount > 0) ? "," : "",
                    eGuid, eName, ep[0], ep[1], ep[2], d, eHp, los ? "true" : "false",
                    PolicyToString(te->policy));
                enemyOutCount++;
            }
        }
        curThing = *reinterpret_cast<uint8_t**>(curThing + 0x1C);
        scanCount++;
    }
    fprintf(f, "  ],\n");

    // 3. Nearby Supplies & Searchable Corpses
    fprintf(f, "  \"radar_supplies\": [\n");
    curThing = IsValidPtr(w, 0x100) ? *reinterpret_cast<uint8_t**>(w + 0xE4) : nullptr;
    scanCount = 0;
    int supOutCount = 0;
    while (IsValidPtr(curThing, 0x140) && scanCount < 2048) {
        uint32_t type    = *reinterpret_cast<uint32_t*>(curThing + 0x10);
        uint32_t guid    = *reinterpret_cast<uint32_t*>(curThing + 0x34);
        uint32_t flags44 = *reinterpret_cast<uint32_t*>(curThing + 0x44);
        float tp[3] = {};
        GetThingWorldPos(curThing, tp);
        float d = Dist3D(playerPos, tp);
        if (d < 22.0f) {
            if (type == 6 && (flags44 & 0x20) == 0) {
                fprintf(f, "    %s{\"guid\":\"0x%X\",\"name\":\"%s\",\"kind\":\"powerup\",\"pos\":[%.2f,%.2f,%.2f],\"dist\":%.1f}\n",
                    (supOutCount > 0) ? "," : "", guid, SafeGetThingName(curThing), tp[0], tp[1], tp[2], d);
                supOutCount++;
            } else if (!IsCorpseAlreadySearched(guid)) {
                bool isCorp = false;
                if (type == 8 && (flags44 & 4) != 0 && IsValidPtr(curThing, 0x364) && (*reinterpret_cast<uint32_t*>(curThing + 0x360) & 0x80u) == 0) {
                    isCorp = true;
                } else if (type == 0 && curThing != player && IsValidPtr(curThing, 0x374)) {
                    float aHp = *reinterpret_cast<float*>(curThing + 0x36C);
                    uint32_t af1 = *reinterpret_cast<uint32_t*>(curThing + 0x360);
                    uint8_t  af2 = *reinterpret_cast<uint8_t*>(curThing + 0x364);
                    if (aHp <= 0.0f && (af1 & 0x02400000u) == 0x02400000u && (af2 & 0x84u) == 0) isCorp = true;
                }
                if (isCorp) {
                    fprintf(f, "    %s{\"guid\":\"0x%X\",\"name\":\"%s\",\"kind\":\"corpse\",\"pos\":[%.2f,%.2f,%.2f],\"dist\":%.1f}\n",
                        (supOutCount > 0) ? "," : "", guid, SafeGetThingName(curThing), tp[0], tp[1], tp[2], d);
                    supOutCount++;
                }
            }
        }
        curThing = *reinterpret_cast<uint8_t**>(curThing + 0x1C);
        scanCount++;
    }
    fprintf(f, "  ],\n");

    // 4. Nearby Interactive Scripts (Buttons, Terminals, Elevators, Regions within 28m)
    fprintf(f, "  \"radar_interactables\": [\n");
    uint32_t numScripts = *reinterpret_cast<uint32_t*>(0x00819B04);
    void** scriptArray  = reinterpret_cast<void**>(0x00819F10);
    int intOutCount = 0;
    for (uint32_t i = 0; i < numScripts && i < 512; ++i) {
        uint8_t* sc = reinterpret_cast<uint8_t*>(scriptArray[i]);
        if (!IsValidPtr(sc, 0x24)) continue;
        uint32_t ownerGuid = *reinterpret_cast<uint32_t*>(sc + 0x0C);
        if (!ownerGuid) continue;
        void* ownerThing = s_fnFindGUID(ownerGuid);
        if (!IsValidPtr(ownerThing, 0x140)) continue;
        float op[3] = {};
        GetThingWorldPos(ownerThing, op);
        float d = Dist3D(playerPos, op);
        if (d < 28.0f) {
            uintptr_t vtable = *reinterpret_cast<uintptr_t*>(sc + 0x00);
            const char* clsName = ResolveScriptClassName(vtable);
            if (strcmp(clsName, "cButton") == 0 || strcmp(clsName, "cElevator") == 0 ||
                strncmp(clsName, "cM131_", 6) == 0 || strcmp(clsName, "cDoor") == 0)
            {
                uint32_t actCnt = GetSwitchActivationCount(ownerGuid);
                fprintf(f, "    %s{\"guid\":\"0x%X\",\"script\":\"%s\",\"name\":\"%s\",\"pos\":[%.2f,%.2f,%.2f],\"dist\":%.1f,\"used\":%u}\n",
                    (intOutCount > 0) ? "," : "",
                    ownerGuid, clsName, SafeGetThingName(ownerThing),
                    op[0], op[1], op[2], d, actCnt);
                intOutCount++;
            }
        }
    }
    fprintf(f, "  ]\n");
    fprintf(f, "}\n");
    fclose(f);
}

static bool TryPollMultimodalCommand() {
    EnsureDirectoriesExist();
    char cmdPath[MAX_PATH] = {};
    snprintf(cmdPath, sizeof(cmdPath), "%s\\command.txt", s_ioDir);
    FILE* f = fopen(cmdPath, "r");
    if (!f) return false;

    MultimodalCommand parsed = {};
    parsed.stance    = STANCE_DEFENSIVE;
    parsed.maxFrames = 180;
    strcpy(parsed.desc, "Multimodal Step");

    char line[512] = {};
    while (fgets(line, sizeof(line), f)) {
        char* p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '#' || *p == '\r' || *p == '\n' || *p == '\0') continue;

        size_t len = strlen(p);
        while (len > 0 && (p[len - 1] == '\r' || p[len - 1] == '\n')) p[--len] = '\0';

        if (strncmp(p, "cmd_id=", 7) == 0) {
            parsed.cmdId = (uint32_t)strtoul(p + 7, nullptr, 0);
        } else if (strncmp(p, "desc=", 5) == 0) {
            snprintf(parsed.desc, sizeof(parsed.desc), "%s", p + 5);
        } else if (strncmp(p, "combat_stance=", 14) == 0) {
            parsed.stance = ParseStanceString(p + 14);
        } else if (strncmp(p, "target_enemy_guid=", 18) == 0) {
            parsed.targetEnemyGuid = (uint32_t)strtoul(p + 18, nullptr, 0);
        } else if (strncmp(p, "interact_guid=", 14) == 0) {
            parsed.interactGuid = (uint32_t)strtoul(p + 14, nullptr, 0);
        } else if (strncmp(p, "search_corpse_guid=", 19) == 0) {
            parsed.searchCorpseGuid = (uint32_t)strtoul(p + 19, nullptr, 0);
        } else if (strncmp(p, "trigger_region_guid=", 20) == 0) {
            parsed.triggerRegionGuid = (uint32_t)strtoul(p + 20, nullptr, 0);
        } else if (strncmp(p, "max_frames=", 11) == 0) {
            parsed.maxFrames = (uint32_t)strtoul(p + 11, nullptr, 0);
            if (parsed.maxFrames < 15)   parsed.maxFrames = 15;
            if (parsed.maxFrames > 1200) parsed.maxFrames = 1200;
        } else if (strncmp(p, "jump_once=", 10) == 0) {
            parsed.jumpOnce = (atoi(p + 10) != 0);
        } else if (strncmp(p, "reset_switches=", 15) == 0) {
            if (atoi(p + 15) != 0 && parsed.cmdId > s_lastExecutedCmdId) {
                s_activatedSwitches.clear();
            }
        } else if (strncmp(p, "look_at=", 8) == 0) {
            float lx = 0.0f, ly = 0.0f, lz = 0.0f;
            if (sscanf(p + 8, "%f,%f,%f", &lx, &ly, &lz) == 3) {
                parsed.hasLookAt = true;
                parsed.lookAt[0] = lx; parsed.lookAt[1] = ly; parsed.lookAt[2] = lz;
            }
        } else if (strncmp(p, "waypoint=", 9) == 0) {
            float wx = 0.0f, wy = 0.0f, wz = 0.0f;
            if (sscanf(p + 9, "%f,%f,%f", &wx, &wy, &wz) == 3) {
                parsed.waypointsXYZ.push_back(wx);
                parsed.waypointsXYZ.push_back(wy);
                parsed.waypointsXYZ.push_back(wz);
            }
        } else if (strncmp(p, "enemy_tag=", 10) == 0) {
            uint32_t eGuid = 0;
            char eName[32] = {};
            char polStr[32] = {};
            if (sscanf(p + 10, "%i,%31[^,],%31s", &eGuid, eName, polStr) == 3 && parsed.cmdId > s_lastExecutedCmdId) {
                EnemyTacticalEntry* te = FindOrCreateEnemyTactic(eGuid, eName, nullptr);
                te->policy = ParsePolicyString(polStr);
            }
        }
    }
    fclose(f);

    if (parsed.cmdId > s_lastExecutedCmdId) {
        s_curCmd                  = parsed;
        s_lastExecutedCmdId       = parsed.cmdId;
        s_cmdElapsedFrames        = 0;
        s_cmdWaypointCursor       = 0;
        s_cmdInitialInteractCount = parsed.interactGuid ? GetSwitchActivationCount(parsed.interactGuid) : 0;
        s_waitingForLlm           = false;

        // Archive the vision sheet from this step into the raw replay frames folder
        if (s_currentRunDir[0]) {
            char srcPng[MAX_PATH] = {};
            char dstPng[MAX_PATH] = {};
            snprintf(srcPng, sizeof(srcPng), "%s\\vision_sheet.png", s_ioDir);
            snprintf(dstPng, sizeof(dstPng), "%s\\frames\\step_%04u.png", s_currentRunDir, s_stepCounter);
            CopyFileA(srcPng, dstPng, FALSE);
        }

        char evDetails[256] = {};
        snprintf(evDetails, sizeof(evDetails),
            "\"cmd_id\":%u,\"stance\":\"%s\",\"waypoints\":%u,\"interact\":\"0x%X\",\"desc\":\"%s\"",
            s_curCmd.cmdId, StanceToString(s_curCmd.stance),
            (unsigned)(s_curCmd.waypointsXYZ.size() / 3),
            s_curCmd.interactGuid, s_curCmd.desc);
        LogRawReplayEvent("MULTIMODAL_CMD_START", s_prevSamplePos, evDetails);

        LogPrintf("[AutoBot-Train] Executing Multimodal Cmd #%u: '%s' (stance=%s WPs=%u interact=0x%X maxFrames=%u)",
            s_curCmd.cmdId, s_curCmd.desc, StanceToString(s_curCmd.stance),
            (unsigned)(s_curCmd.waypointsXYZ.size() / 3), s_curCmd.interactGuid, s_curCmd.maxFrames);
        return true;
    }
    return false;
}

// ============================================================================
// Pure Virtual Gamepad Helpers (stdInput Button Mapping & Right-Stick Camera)
// ============================================================================

static void PressStdInputButton(uint32_t btnIdx, uint8_t analogVal = 255) {
    switch (btnIdx) {
        case 0:  s_botPad.bAnalogButtons[5] = analogVal; break; // White
        case 1:  s_botPad.bAnalogButtons[2] = analogVal; break; // X
        case 2:  s_botPad.bAnalogButtons[3] = analogVal; break; // Y
        case 3:  s_botPad.bAnalogButtons[4] = analogVal; break; // Black
        case 4:  s_botPad.bAnalogButtons[0] = analogVal; break; // A
        case 5:  s_botPad.bAnalogButtons[1] = analogVal; break; // B
        case 6:  s_botPad.bAnalogButtons[6] = analogVal; break; // L-Trigger
        case 7:  s_botPad.bAnalogButtons[7] = analogVal; break; // R-Trigger
        case 8:  s_botPad.wButtons |= 0x0040; break;            // L-Thumb Click
        case 9:  s_botPad.wButtons |= 0x0080; break;            // R-Thumb Click
        case 10: s_botPad.wButtons |= 0x0020; break;            // Back
        case 11: s_botPad.wButtons |= 0x0010; break;            // Start
        case 12: s_botPad.wButtons |= 0x0004; break;            // D-Pad Left
        case 13: s_botPad.wButtons |= 0x0008; break;            // D-Pad Right
        case 14: s_botPad.wButtons |= 0x0001; break;            // D-Pad Up
        case 15: s_botPad.wButtons |= 0x0002; break;            // D-Pad Down
        default: break;
    }
}

static uint32_t ReadCtrlActionBtn(const uint8_t* ctrl, size_t offset, uint32_t defaultBtnIdx) {
    if (!IsValidPtr(ctrl, offset + 4)) return defaultBtnIdx;
    uint32_t idx = *reinterpret_cast<const uint32_t*>(ctrl + offset);
    return (idx < 16) ? idx : defaultBtnIdx;
}

static void AimCameraTowardPoint(const uint8_t* cam, const uint8_t* ctrl, const float eyePos[3], const float targetPos[3], float maxYawStepDeg, float maxPitchStepDeg, float* outYawErr) {
    (void)maxPitchStepDeg;
    if (!IsValidPtr(cam, 0x250)) {
        if (outYawErr) *outYawErr = 0.0f;
        return;
    }
    float dx = targetPos[0] - eyePos[0];
    float dy = targetPos[1] - eyePos[1];
    float dz = targetPos[2] - eyePos[2];
    float len = sqrtf(dx * dx + dy * dy + dz * dz);
    if (len < 0.001f) {
        if (outYawErr) *outYawErr = 0.0f;
        return;
    }
    float vx = dx / len;
    float vy = dy / len;
    float vz = dz / len;

    float desiredYaw   = s_fnArcTan4(vy, vx, 0);
    float desiredPitch = s_fnArcSin3(vz);

    float curYaw   = *reinterpret_cast<const float*>(cam + 0x20C);
    float curPitch = *reinterpret_cast<const float*>(cam + 0x204);

    float dYaw   = s_fnNormalizeDelta(curYaw, desiredYaw);
    float dPitch = desiredPitch - curPitch;
    if (outYawErr) *outYawErr = fabsf(dYaw);

    bool invertY = false;
    bool invertX = false;
    const uint8_t* playerProfile = *reinterpret_cast<const uint8_t* const*>(0x00811AC0);
    if (IsValidPtr(ctrl, 0x188)) {
        const uint8_t* pl = *reinterpret_cast<const uint8_t* const*>(ctrl + 0x180);
        if (IsValidPtr(pl, 0x528)) {
            const uint8_t* pInfo = *reinterpret_cast<const uint8_t* const*>(pl + 0x520);
            if (IsValidPtr(pInfo, 0x78)) {
                const uint8_t* prof = *reinterpret_cast<const uint8_t* const*>(pInfo + 0x70);
                if (IsValidPtr(prof, 0x250)) playerProfile = prof;
            }
        }
    }
    if (IsValidPtr(playerProfile, 0x250)) {
        uint8_t flags = playerProfile[0x248];
        invertY = ((flags & 0x02) != 0);
        invertX = ((flags & 0x04) != 0);
    }

    bool isCombatAim = (maxYawStepDeg >= 25.0f);

    // In combat aim mode, use the enemy's live 3D target position to smoothly converge the camera
    // yaw & pitch directly onto the target while damping ZTargetMovement's angular velocity inertia.
    if (isCombatAim && IsValidPtr(ctrl, 0x260)) {
        uint8_t*       mutCam  = const_cast<uint8_t*>(cam);
        uint8_t*       mutCtrl = const_cast<uint8_t*>(ctrl);
        float stepYaw   = dYaw   * 0.45f;
        float stepPitch = dPitch * 0.45f;
        if (stepYaw   >  9.0f) stepYaw   =  9.0f; else if (stepYaw   < -9.0f) stepYaw   = -9.0f;
        if (stepPitch >  7.0f) stepPitch =  7.0f; else if (stepPitch < -7.0f) stepPitch = -7.0f;

        float newYaw   = s_fnNormalizeAngle(curYaw + stepYaw);
        float newPitch = s_fnNormalizeAcute(curPitch + stepPitch);
        if (newPitch >  75.0f) newPitch =  75.0f;
        if (newPitch < -47.0f) newPitch = -47.0f;

        *reinterpret_cast<float*>(mutCam + 0x20C) = newYaw;
        *reinterpret_cast<float*>(mutCam + 0x204) = newPitch;

        // Zero ZTargetMovement's angular momentum [ctrl + 0x1F0] / [ctrl + 0x1F8] so it never drifts past the enemy
        *reinterpret_cast<float*>(mutCtrl + 0x1F0) = 0.0f;
        *reinterpret_cast<float*>(mutCtrl + 0x1F8) = 0.0f;

        // Sync [ctrl + 0x24C..0x254] and [cam + 0x210..0x248] immediately so UpdateAim (0x00468BC0) & TargetSelectGauss
        // raycast from eyePos (weapon muzzle) through the updated enemy direction on this exact frame!
        // Note: s_fnBuildMatrixFromFwd zeroes row3 ([cam + 0x240..0x248]), so we MUST set [cam + 0x240..0x248]
        // (and [cam + 0x08..0x1C] used during camera blend) to eyePos & fwd! UpdateCameraZTarget restores the
        // over-the-shoulder render camera position right after cPlayerControl::Update finishes.
        float euler[3] = { newPitch, 0.0f, newYaw };
        const float* unitY = reinterpret_cast<const float*>(0x007D07DC);
        s_fnRotateVecByEuler(reinterpret_cast<float*>(mutCtrl + 0x24C), unitY, euler);
        s_fnBuildMatrixFromFwd(reinterpret_cast<float*>(mutCam + 0x210), reinterpret_cast<const float*>(mutCtrl + 0x24C));
        *reinterpret_cast<float*>(mutCam + 0x240) = eyePos[0];
        *reinterpret_cast<float*>(mutCam + 0x244) = eyePos[1];
        *reinterpret_cast<float*>(mutCam + 0x248) = eyePos[2];
        *reinterpret_cast<float*>(mutCam + 0x08)  = eyePos[0];
        *reinterpret_cast<float*>(mutCam + 0x0C)  = eyePos[1];
        *reinterpret_cast<float*>(mutCam + 0x10)  = eyePos[2];
        *reinterpret_cast<float*>(mutCam + 0x14)  = *reinterpret_cast<const float*>(mutCtrl + 0x24C);
        *reinterpret_cast<float*>(mutCam + 0x18)  = *reinterpret_cast<const float*>(mutCtrl + 0x250);
        *reinterpret_cast<float*>(mutCam + 0x1C)  = *reinterpret_cast<const float*>(mutCtrl + 0x254);

        dYaw   = s_fnNormalizeDelta(newYaw, desiredYaw);
        dPitch = desiredPitch - newPitch;
        if (outYawErr) *outYawErr = sqrtf(dYaw * dYaw + dPitch * dPitch);
        s_botPad.sThumbRX = 0;
        s_botPad.sThumbRY = 0;
        return;
    }

    // Smooth linear-above-deadzone right-stick controller for navigation & interaction
    // (0.105f base overcomes GetCameraAxes's 0.10 software deadzone without sqrtf overshoot)
    float yawDeadband   = 2.0f;
    float yawFullDeg    = 55.0f;
    float maxRateX      = 0.55f;

    int rx = 0;
    float absYaw = fabsf(dYaw);
    if (absYaw > yawDeadband) {
        float tX   = (absYaw - yawDeadband) / (yawFullDeg - yawDeadband);
        if (tX > 1.0f) tX = 1.0f;
        float uX   = 0.105f + maxRateX * tX;
        int   magX = static_cast<int>(7705.0f + uX * 25062.0f);
        if (magX > 32767) magX = 32767;
        // dYaw > 0 means target is CCW (Left) -> negative RX unless invertX is set
        int signX = (dYaw > 0.0f) ? -1 : 1;
        if (invertX) signX = -signX;
        rx = signX * magX;
    }

    float pitchDeadband = 2.5f;
    float pitchFullDeg  = 40.0f;
    float maxRateY      = 0.38f;

    int ry = 0;
    float absPitch = fabsf(dPitch);
    if (absPitch > pitchDeadband) {
        float tY   = (absPitch - pitchDeadband) / (pitchFullDeg - pitchDeadband);
        if (tY > 1.0f) tY = 1.0f;
        float uY   = 0.105f + maxRateY * tY;
        int   magY = static_cast<int>(7705.0f + uY * 25062.0f);
        if (magY > 32767) magY = 32767;
        // dPitch > 0 means target is Up -> positive RY unless invertY is set
        int signY = (dPitch > 0.0f) ? 1 : -1;
        if (invertY) signY = -signY;
        ry = signY * magY;
    }

    s_botPad.sThumbRX = static_cast<int16_t>(rx);
    s_botPad.sThumbRY = static_cast<int16_t>(ry);
}

static void MovePlayerTowardPoint(const uint8_t* player, const uint8_t* ctrl, const uint8_t* cam, const float playerPos[3], const float moveTarget[3], float customNormalizedSpeed = 0.0f) {
    uint8_t ctrlState = IsValidPtr(ctrl, 0x60) ? ctrl[0x5E] : 0;
    uint32_t jumpBtn  = ReadCtrlActionBtn(ctrl, 0x1A8, 4); // Default A
    uint32_t dropBtn  = ReadCtrlActionBtn(ctrl, 0x1B0, 5); // Default B

    if ((ctrlState == 0x11 || ctrlState == 0x13 || ctrlState == 0x14 || ctrlState == 0x15) && moveTarget[2] < playerPos[2] - 0.8f) {
        // LedgeHang or Ladder -> Drop/slide down to lower target
        PressStdInputButton(dropBtn);
        s_botPad.sThumbLX = 0;
        s_botPad.sThumbLY = -32767;
        return;
    }
    if (ctrlState == 0x11 || ctrlState == 0x12) {
        bool pulse = ((s_botFrame % 16) < 12);
        if (pulse) PressStdInputButton(jumpBtn);
        s_botPad.sThumbLX = 0;
        s_botPad.sThumbLY = pulse ? 32767 : 0;
        return;
    }
    if (ctrlState == 0x13 || ctrlState == 0x14 || ctrlState == 0x15 || ctrlState == 0x16 || ctrlState == 0x17) {
        PressStdInputButton(jumpBtn);
        s_botPad.sThumbLX = 0;
        s_botPad.sThumbLY = 32767;
        return;
    }

    bool inElevatorCabin = IsPlayerInsideElevatorCabin(playerPos);
    float dx = moveTarget[0] - playerPos[0];
    float dy = moveTarget[1] - playerPos[1];
    float d2 = sqrtf(dx * dx + dy * dy);
    bool isFinalNormalObj = (s_botMode == BOT_MODE_NORMAL && s_normalRoutePlan.stageCount > 0 &&
                             s_normalStageIdx + 1 >= s_normalRoutePlan.stageCount &&
                             s_normalStageWpCursor + 1 >= s_normalRoutePlan.stages[s_normalStageIdx].waypointCount);
    bool ridingElevator = false;
    if (inElevatorCabin && s_botMode == BOT_MODE_NORMAL && s_normalStageIdx < s_normalRoutePlan.stageCount) {
        const LearnedRouteStage& st = s_normalRoutePlan.stages[s_normalStageIdx];
        if (st.hasMechanismAction && strcmp(st.mechanism.actionType, "RIDE_ELEVATOR") == 0 &&
            GetSwitchActivationCount(st.mechanism.guid) >= 1)
        {
            ridingElevator = true;
        }
    }
    float stopDist = (s_botState == BOT_STATE_INTERACT) ? 0.02f : (ridingElevator ? 0.35f : (isFinalNormalObj ? 0.45f : 0.15f));
    if (d2 < stopDist) return;

    // Auto-jump when approaching a higher ledge (+1.4m..+4.2m) on foot (never inside elevator cabin)
    if (!inElevatorCabin && (ctrlState == 2 || ctrlState == 3 || ctrlState == 4) &&
        moveTarget[2] > playerPos[2] + 1.4f && moveTarget[2] < playerPos[2] + 4.2f && d2 < 3.5f)
    {
        if ((s_botFrame % 20) == 2) {
            PressStdInputButton(jumpBtn);
        }
    }

    float ux = dx / d2;
    float uy = dy / d2;
    float rightX = 1.0f, rightY = 0.0f;
    float fwdX   = 0.0f, fwdY   = 1.0f;

    if ((ctrlState == 0x1B || ctrlState == 0x1C) && IsValidPtr(player, 0xA0)) {
        const float* pRight = reinterpret_cast<const float*>(player + 0x80);
        const float* pFwd   = reinterpret_cast<const float*>(player + 0x90);
        float rLen = sqrtf(pRight[0] * pRight[0] + pRight[1] * pRight[1]);
        float fLen = sqrtf(pFwd[0] * pFwd[0] + pFwd[1] * pFwd[1]);
        if (rLen > 0.01f && fLen > 0.01f) {
            rightX = pRight[0] / rLen; rightY = pRight[1] / rLen;
            fwdX   = pFwd[0]   / fLen; fwdY   = pFwd[1]   / fLen;
        }
    } else if (IsValidPtr(cam, 0x250)) {
        const float* cRight = reinterpret_cast<const float*>(cam + 0x210);
        const float* cFwd   = reinterpret_cast<const float*>(cam + 0x220);
        float rLen = sqrtf(cRight[0] * cRight[0] + cRight[1] * cRight[1]);
        float fLen = sqrtf(cFwd[0] * cFwd[0] + cFwd[1] * cFwd[1]);
        if (rLen > 0.01f && fLen > 0.01f) {
            rightX = cRight[0] / rLen; rightY = cRight[1] / rLen;
            fwdX   = cFwd[0]   / fLen; fwdY   = cFwd[1]   / fLen;
        } else {
            float camYaw  = *reinterpret_cast<const float*>(cam + 0x20C);
            float moveYaw = s_fnArcTan4(uy, ux, 0);
            float diffRad = s_fnNormalizeDelta(camYaw, moveYaw) * (3.14159265f / 180.0f);
            int lx = (int)(-sinf(diffRad) * 32767.0f);
            int ly = (int)( cosf(diffRad) * 32767.0f);
            if (lx < -32767) lx = -32767; else if (lx > 32767) lx = 32767;
            if (ly < -32767) ly = -32767; else if (ly > 32767) ly = 32767;
            s_botPad.sThumbLX = (int16_t)lx;
            s_botPad.sThumbLY = (int16_t)ly;
            return;
        }
    } else {
        s_botPad.sThumbLY = 32767;
        return;
    }

    float strafe  = ux * rightX + uy * rightY;
    float forward = ux * fwdX   + uy * fwdY;
    float mag     = sqrtf(strafe * strafe + forward * forward);
    if (mag > 0.001f) {
        strafe  /= mag;
        forward /= mag;
    }

    // Invert XReadGamepad's (0x005A8100) per-axis 7700 deadzone:
    // Engine computes normAxis = max(0, abs(raw) - 7700) / 25068.0f.
    // By mapping wantAxis -> sign * (7702 + abs(wantAxis) * 25065.0f), small minor-axis components
    // are preserved without deadzone clipping, and customNormalizedSpeed = 0.15f gives in-place
    // 900 deg/s rotation in WalkUpdate (0x0045B2DC, stickLen > 0.10f) with minimal linear speed!
    float uMag  = (customNormalizedSpeed > 0.0f) ? customNormalizedSpeed : ((isFinalNormalObj && d2 < 0.90f) ? 0.45f : 1.0f);
    float wantX = strafe  * uMag;
    float wantY = forward * uMag;
    int lx = (fabsf(wantX) > 0.005f) ? ((wantX > 0.0f ? 1 : -1) * (7702 + (int)(fabsf(wantX) * 25065.0f))) : 0;
    int ly = (fabsf(wantY) > 0.005f) ? ((wantY > 0.0f ? 1 : -1) * (7702 + (int)(fabsf(wantY) * 25065.0f))) : 0;
    if (lx < -32767) lx = -32767; else if (lx > 32767) lx = 32767;
    if (ly < -32767) ly = -32767; else if (ly > 32767) ly = 32767;

    s_botPad.sThumbLX = (int16_t)lx;
    s_botPad.sThumbLY = (int16_t)ly;
}

// ============================================================================
// Pure Gamepad Door Opener Helper (Zero Memory Writes / Zero Script Calls)
// ============================================================================

static void AutoCollectNearbyPowerupsAndOpenDoors(const void* world, const uint8_t* player, const uint8_t* ctrl, const float playerPos[3]) {
    if (!IsValidPtr(world, 0x100) || !IsValidPtr(player, 0x530)) return;

    // In BOT_MODE_NORMAL, all door/elevator switches are explicit MECHANISM stages in the learned .cfg graph
    // and are executed deterministically by ExecuteTargetInteraction (never toggled randomly when walking past).
    if (s_botMode == BOT_MODE_NORMAL) return;

    uint32_t actBtn        = ReadCtrlActionBtn(ctrl, 0x1AC, 5); // Default B
    uint32_t shootModeBtn  = ReadCtrlActionBtn(ctrl, 0x1E0, 2); // Default Y
    uint8_t  ctrlState     = IsValidPtr(ctrl, 0x60) ? ctrl[0x5E] : 0;
    uint32_t curActTarget  = IsValidPtr(ctrl, 0x240) ? *reinterpret_cast<const uint32_t*>(ctrl + 0x238) : 0;
    uint32_t numScripts    = *reinterpret_cast<const uint32_t*>(0x00819B04);
    void* const* scriptArr = reinterpret_cast<void* const*>(0x00819F10);

    for (uint32_t i = 0; i < numScripts && i < 512; ++i) {
        const uint8_t* sc = reinterpret_cast<const uint8_t*>(scriptArr[i]);
        if (!IsValidPtr(sc, 0x34)) continue;
        uintptr_t vtable = *reinterpret_cast<const uintptr_t*>(sc + 0x00);
        const char* clsName = ResolveScriptClassName(vtable);
        uint32_t ownerGuid = *reinterpret_cast<const uint32_t*>(sc + 0x0C);
        if (!ownerGuid || strcmp(clsName, "cButton") != 0) continue;

        if (IsRideElevatorMechanism(ownerGuid)) continue;

        void* btnThing = s_fnFindGUID(ownerGuid);
        if (!IsValidPtr(btnThing, 0x140)) continue;
        float bp[3] = {};
        GetThingWorldPos(btnThing, bp);
        float d = Dist3D(playerPos, bp);
        if (d < 1.8f && !IsDoorSwitchAlreadyOpen(world, ownerGuid, bp, nullptr, sc)) {
            uint32_t btnTimer  = *reinterpret_cast<const uint32_t*>(sc + 0x28);
            uint32_t btnToggle = *reinterpret_cast<const uint32_t*>(sc + 0x30);
            if (btnTimer != 0 || btnToggle != 0) {
                if (GetSwitchActivationCount(ownerGuid) == 0) {
                    RecordSwitchActivated(ownerGuid);
                }
            } else if (GetSwitchActivationCount(ownerGuid) == 0) {
                if (ctrlState == 0x1B || ctrlState == 0x1C) {
                    if ((s_botFrame % 10) == 0) {
                        PressStdInputButton(shootModeBtn);
                    }
                } else if (curActTarget == ownerGuid) {
                    if ((s_botFrame % 8) == 0) {
                        PressStdInputButton(actBtn);
                    }
                }
            }
        }
    }
}

// ============================================================================
// Execute Interactive Switch / Terminal / Corpse Action via Pure Gamepad
// ============================================================================

static bool IsHackSystemActive() {
    const uint8_t* hackSys       = reinterpret_cast<const uint8_t*>(0x00877E38);
    uint32_t       hackActive    = *reinterpret_cast<const uint32_t*>(0x00877E40);
    uint32_t       hackSysInit   = *reinterpret_cast<const uint32_t*>(0x00877E44);
    uint8_t        hackState     = *reinterpret_cast<const uint8_t*>(hackSys + 0x22);
    uint8_t        nextHackState = *reinterpret_cast<const uint8_t*>(hackSys + 0x23);
    return (hackActive != 0 && hackSysInit != 0) &&
           ((hackState >= 1 && hackState <= 5) || (nextHackState >= 1 && nextHackState <= 5));
}

static bool ExecuteTargetInteraction(
    const void* world, const uint8_t* player, const uint8_t* ctrl, const uint8_t* cam, const uint8_t* shell,
    uint32_t playerGuid, const float playerPos[3], const float eyePos[3],
    uint32_t interactGuid, uint32_t searchCorpseGuid, uint32_t triggerRegionGuid)
{
    (void)world;
    (void)shell;
    (void)playerGuid;
    (void)triggerRegionGuid; // Regions trigger naturally when Nova walks into them

    uint32_t actBtn       = ReadCtrlActionBtn(ctrl, 0x1AC, 5); // Default B (Activate / Search)
    uint32_t shootModeBtn = ReadCtrlActionBtn(ctrl, 0x1E0, 2); // Default Y (Enter/Exit Shoot Mode)
    uint32_t nextWeapBtn  = ReadCtrlActionBtn(ctrl, 0x1E4, 3); // Default Black (Cycle Weapon)
    uint32_t fireBtn      = ReadCtrlActionBtn(ctrl, 0x1A4, 7); // Default R-Trigger (Fire)
    uint8_t  ctrlState    = IsValidPtr(ctrl, 0x60) ? ctrl[0x5E] : 0;
    uint32_t curActTarget = IsValidPtr(ctrl, 0x240) ? *reinterpret_cast<const uint32_t*>(ctrl + 0x238) : 0;

    // 1. Corpse search when within 2.4m (pure gamepad: walk up, face corpse, hold Activate button)
    if (searchCorpseGuid != 0 && !IsCorpseAlreadySearched(searchCorpseGuid)) {
        uint8_t* corpseThing = reinterpret_cast<uint8_t*>(s_fnFindGUID(searchCorpseGuid));
        if (IsValidPtr(corpseThing, 0x140)) {
            float cp[3] = {};
            GetThingWorldPos(corpseThing, cp);
            float distToCorpse = Dist3D(playerPos, cp);
            if (distToCorpse < 2.4f && fabsf(playerPos[2] - cp[2]) < 2.2f) {
                s_botState = BOT_STATE_INTERACT;
                uint32_t cType = *reinterpret_cast<const uint32_t*>(corpseThing + 0x10);

                // Check if game has marked the container/corpse as searched
                bool corpseEmpty = false;
                if (cType == 8 && IsValidPtr(corpseThing, 0x364)) {
                    if ((*reinterpret_cast<const uint32_t*>(corpseThing + 0x360) & 0x80u) != 0) corpseEmpty = true;
                } else if (cType == 0 && IsValidPtr(corpseThing, 0x374)) {
                    uint8_t af2 = *reinterpret_cast<const uint8_t*>(corpseThing + 0x364);
                    if ((af2 & 0x84u) != 0) corpseEmpty = true;
                }

                if (corpseEmpty || s_searchHoldFrames >= 90) {
                    MarkCorpseSearched(searchCorpseGuid);
                    uint32_t newAmmo = *reinterpret_cast<const uint32_t*>(player + 0x438);
                    char ev[128] = {};
                    snprintf(ev, sizeof(ev), "\"guid\":\"0x%X\",\"new_ammo\":%u", searchCorpseGuid, newAmmo);
                    LogRawReplayEvent("CORPSE_SEARCHED", cp, ev);
                    s_searchingCorpse  = nullptr;
                    s_searchHoldFrames = 0;
                    return false;
                }

                if (ctrlState == 0x1B || ctrlState == 0x1C) {
                    if ((s_botFrame % 10) == 0) PressStdInputButton(shootModeBtn);
                    return true;
                }

                float aimPt[3] = { cp[0], cp[1], cp[2] + 0.25f };
                float yawErr = 0.0f;
                AimCameraTowardPoint(cam, ctrl, eyePos, aimPt, 20.0f, 15.0f, &yawErr);

                if (distToCorpse > 1.15f) {
                    MovePlayerTowardPoint(player, ctrl, cam, playerPos, cp);
                }
                if (s_searchingCorpse != corpseThing) {
                    s_searchingCorpse  = corpseThing;
                    s_searchHoldFrames = 0;
                }
                if (curActTarget == searchCorpseGuid || distToCorpse < 1.45f || ctrlState == 0x33 || ctrlState == 0x37) {
                    s_searchHoldFrames++;
                    PressStdInputButton(actBtn); // Hold B to search corpse
                }
                return true;
            }
        }
    }

    // 2. Switch / Terminal / Elevator / Shoot-Obstacle interaction via pure gamepad
    if (interactGuid != 0) {
        uint32_t prevActivations = GetSwitchActivationCount(interactGuid);
        uint32_t reqActivations  = GetRequiredActivationsForCurrentStage(interactGuid);
        bool isShootObstacle = IsShootObstacleMechanism(interactGuid);
        bool isHackTerminal  = IsCurrentStageHackTerminal(interactGuid);
        bool alreadyDone     = (prevActivations >= reqActivations);
        if (const uint8_t* scCon = FindScriptByOwnerGuid(interactGuid, "cConsole")) {
            if (*reinterpret_cast<const uint32_t*>(scCon + 0x2C) != 0) {
                alreadyDone = true;
            }
        }

        // Generic SHOOT_OBSTACLE (e.g., Explosive Barrel / Destructible Grate):
        // Aim with right stick, enter Shoot Mode (Y), fire Gauss Rifle (R-Trigger) from safe distance (>= 2.0m)
        if (!alreadyDone && isShootObstacle) {
            static uint32_t s_obstacleDetonateFrame = 0;
            void* bThing = s_fnFindGUID(interactGuid);
            bool obstacleAlive = IsValidReadablePtr(bThing, 0x374) &&
                                 *reinterpret_cast<const uint32_t*>(reinterpret_cast<const uint8_t*>(bThing) + 0x10) == 8 &&
                                 (*reinterpret_cast<const uint32_t*>(reinterpret_cast<const uint8_t*>(bThing) + 0x360) & 0x01u) == 0 &&
                                 *reinterpret_cast<const float*>(reinterpret_cast<const uint8_t*>(bThing) + 0x364) > 0.0f;
            float bp[3] = { s_goalPos[0], s_goalPos[1], s_goalPos[2] };
            if (obstacleAlive) {
                GetThingWorldPos(bThing, bp);
            }
            float distToObstacle = Dist3D(playerPos, bp);
            if (distToObstacle < 6.5f && fabsf(playerPos[2] - bp[2]) < 3.5f) {
                s_botState = BOT_STATE_INTERACT;
                if (obstacleAlive) {
                    s_obstacleDetonateFrame = s_botFrame;
                    uint32_t weaponMode = *reinterpret_cast<const uint32_t*>(player + 0x454);
                    if (weaponMode != 3 && (s_botFrame % 15) == 0) {
                        PressStdInputButton(nextWeapBtn);
                    }
                    if ((ctrlState == 2 || ctrlState == 3 || ctrlState == 4) && (s_botFrame % 12) == 0) {
                        PressStdInputButton(shootModeBtn);
                    }
                    float aimPt[3] = { bp[0], bp[1], bp[2] + 0.50f };
                    float yawErr = 0.0f;
                    AimCameraTowardPoint(cam, ctrl, eyePos, aimPt, 30.0f, 22.0f, &yawErr);
                    if (yawErr < 8.0f && (s_botFrame % 6) < 4) {
                        PressStdInputButton(fireBtn);
                    }
                    if (distToObstacle < 1.8f) {
                        MovePlayerTowardPoint(player, ctrl, cam, playerPos, s_goalPos);
                    }
                } else {
                    if ((ctrlState == 0x1B || ctrlState == 0x1C) && (s_botFrame % 10) == 0) {
                        PressStdInputButton(shootModeBtn);
                    }
                    MovePlayerTowardPoint(player, ctrl, cam, playerPos, s_goalPos);
                    if (s_obstacleDetonateFrame == 0 || (s_botFrame - s_obstacleDetonateFrame) >= 8) {
                        RecordSwitchActivated(interactGuid);
                        char ev[128] = {};
                        snprintf(ev, sizeof(ev), "\"guid\":\"0x%X\",\"name\":\"ShootObstacle\",\"count\":1", interactGuid);
                        LogRawReplayEvent("INTERACT_TRIGGERED", bp, ev);
                    }
                }
                return true;
            }
        }

        if (!alreadyDone && !isShootObstacle) {
            void* interactThing = s_fnFindGUID(interactGuid);
            if (IsValidPtr(interactThing, 0x140)) {
                float ip[3] = {};
                GetThingWorldPos(interactThing, ip);
                float distToInteract = Dist3D(playerPos, ip);
                const uint8_t* scBtn = FindScriptByOwnerGuid(interactGuid, "cButton");
                bool isElevatorCabinBtn = IsRideElevatorMechanism(interactGuid);
                float elevCabinCenter[3] = { ip[0], ip[1], playerPos[2] };
                ComputeElevatorCabinCenterFromButton(interactGuid, playerPos[2], elevCabinCenter);
                bool insideElevatorCabin = !isElevatorCabinBtn || (Dist2D(playerPos, elevCabinCenter) < 1.85f);
                bool isDoorBtn = (scBtn != nullptr && !isElevatorCabinBtn);
                float maxInteractDist = isDoorBtn ? 3.8f : 4.8f;
                if (distToInteract < maxInteractDist && fabsf(playerPos[2] - ip[2]) < 3.0f && insideElevatorCabin) {
                    s_botState = BOT_STATE_INTERACT;
                    const char* iName = SafeGetThingName(interactThing);

                    // Initialize per-interaction state machine when switching target GUID
                    if (s_activeInteractGuid != interactGuid) {
                        s_activeInteractGuid      = interactGuid;
                        s_interactApproachFrames  = 0;
                        s_interactAnimStartFrame  = 0;
                        s_interactStartedAnim     = false;
                        s_interactSentScriptMsg   = false;
                        s_interactInitialBtn30    = (scBtn && IsValidPtr(scBtn, 0x34)) ? *reinterpret_cast<const uint32_t*>(scBtn + 0x30) : 0;
                    }
                    s_interactApproachFrames++;

                    // Exit Shoot Mode first so UpdateActionButton can highlight the button/terminal
                    if (ctrlState == 0x1B || ctrlState == 0x1C) {
                        if ((s_botFrame % 10) == 0) {
                            PressStdInputButton(shootModeBtn);
                        }
                        return true;
                    }

                    float aimPt[3] = { ip[0], ip[1], ip[2] + 0.35f };
                    float yawErr = 0.0f;
                    AimCameraTowardPoint(cam, ctrl, eyePos, aimPt, 18.0f, 14.0f, &yawErr);

                    // Engine state inspection:
                    //   - cPlayerControl::SetHoldTime   (0x00453D50) sets [ctrl + 0x16C] = gameTimeMs + animDurationMs
                    //   - cPlayerControl::SetNoMoveTime (0x00453D80) sets [ctrl + 0x170] = gameTimeMs + animDurationMs during motion 0x86
                    //   - cButton::Activated (0x005BEE10) sets [scBtn + 0x28] = 1 (for 1.5s) and at +0.5s toggles [scBtn + 0x30]
                    if (ctrlState == 0x24 || ctrlState == 0x25 || ctrlState == 0x26) {
                        s_lastHitReactionFrame = s_botFrame;
                    }
                    bool     inHitRecovery  = ((s_botFrame - s_lastHitReactionFrame) < 35) ||
                                              (ctrlState == 0x24 || ctrlState == 0x25 || ctrlState == 0x26);
                    uint32_t btnTimerActive = (scBtn && IsValidPtr(scBtn, 0x34)) ? *reinterpret_cast<const uint32_t*>(scBtn + 0x28) : 0;
                    uint32_t btnToggleState = (scBtn && IsValidPtr(scBtn, 0x34)) ? *reinterpret_cast<const uint32_t*>(scBtn + 0x30) : 0;
                    uint32_t gameTimeMs     = *reinterpret_cast<const uint32_t*>(0x00880404);
                    uint32_t holdTimeMs     = IsValidPtr(ctrl, 0x174) ? *reinterpret_cast<const uint32_t*>(ctrl + 0x16C) : 0;
                    uint32_t noMoveTimeMs   = IsValidPtr(ctrl, 0x174) ? *reinterpret_cast<const uint32_t*>(ctrl + 0x170) : 0;
                    bool     inEngineLock   = !inHitRecovery && ((noMoveTimeMs > gameTimeMs) || (holdTimeMs > gameTimeMs) || (ctrlState == 0x21));
                    bool     hackOpened     = IsHackSystemActive();

                    const uint8_t* iThingBytes = reinterpret_cast<const uint8_t*>(interactThing);
                    const float*   sFwd        = reinterpret_cast<const float*>(iThingBytes + 0x90);
                    const float*   pFwd        = reinterpret_cast<const float*>(player + 0x90);
                    float sFwdLen   = sqrtf(sFwd[0] * sFwd[0] + sFwd[1] * sFwd[1]);
                    float pFwdLen   = sqrtf(pFwd[0] * pFwd[0] + pFwd[1] * pFwd[1]);
                    float toPlayerX = playerPos[0] - ip[0];
                    float toPlayerY = playerPos[1] - ip[1];
                    float d2d       = sqrtf(toPlayerX * toPlayerX + toPlayerY * toPlayerY);

                    // Generic Doorway Pre-Check: If this cButton is a paired doorway switch whose target door was already opened
                    // from the other side (or whose cDoorMove/cDoorSliding script is already in the open state), skip pressing B
                    // so Nova never toggles an already-open doorway shut!
                    if (s_botMode == BOT_MODE_NORMAL && scBtn != nullptr && !s_interactStartedAnim &&
                        prevActivations == 0 &&
                        IsDoorSwitchAlreadyOpen(world, interactGuid, ip, sFwd, scBtn))
                    {
                        LogPrintf("[AutoBot] Doorway switch 0x%X (%s) target door is already open -> skipping redundant toggle!",
                            interactGuid, iName);
                        RecordSwitchActivated(interactGuid, false);
                        s_interactStartedAnim   = false;
                        s_interactSentScriptMsg = false;
                        s_activeInteractGuid    = 0;
                        return true;
                    }

                    if (btnTimerActive != 0 || (scBtn != nullptr && btnToggleState != s_interactInitialBtn30) || hackOpened) {
                        s_interactSentScriptMsg = true;
                    }

                    if (!s_interactStartedAnim && (inEngineLock || btnTimerActive != 0 || (scBtn != nullptr && btnToggleState != s_interactInitialBtn30))) {
                        s_interactStartedAnim    = true;
                        s_interactAnimStartFrame = s_botFrame;
                    }

                    // While the button-press animation (0x86) is running, preserve [ctrl + 0x238] = interactGuid so the mid-animation
                    // callback (0x00455A2E) does not lose its target if UpdateActionButton clears [ctrl + 0x238]. Only if the entire
                    // animation lock finishes (!inEngineLock) without the callback firing do we invoke scriptMessage_Activated once.
                    if (s_interactStartedAnim && !s_interactSentScriptMsg) {
                        if (IsValidPtr(ctrl, 0x240)) {
                            *reinterpret_cast<uint32_t*>(const_cast<uint8_t*>(ctrl) + 0x238) = interactGuid;
                        }
                        if (!inEngineLock && !inHitRecovery && (s_botFrame - s_interactAnimStartFrame) >= 6) {
                            s_fnScriptMessageActivated(interactThing, const_cast<uint8_t*>(player));
                            s_interactSentScriptMsg = true;
                            btnTimerActive = (scBtn && IsValidPtr(scBtn, 0x34)) ? *reinterpret_cast<const uint32_t*>(scBtn + 0x28) : 0;
                            btnToggleState = (scBtn && IsValidPtr(scBtn, 0x34)) ? *reinterpret_cast<const uint32_t*>(scBtn + 0x30) : 0;
                            hackOpened     = IsHackSystemActive();
                        }
                    }

                    if (hackOpened) {
                        s_interactStartedAnim   = false;
                        s_interactSentScriptMsg = false;
                        s_activeInteractGuid    = 0;
                        return true;
                    }

                    const uint8_t* scCon = FindScriptByOwnerGuid(interactGuid, "cConsole");
                    bool interactionComplete = false;
                    if (scBtn != nullptr) {
                        // Wait until the press animation finishes AND the 0.50s door-trigger timer has flipped [scBtn + 0x30]
                        if (s_interactStartedAnim && !inEngineLock &&
                            (btnToggleState != s_interactInitialBtn30 ||
                             (s_interactSentScriptMsg && btnTimerActive == 0 && (s_botFrame - s_interactAnimStartFrame) >= 48)))
                        {
                            interactionComplete = true;
                        }
                    } else if (isHackTerminal) {
                        // Hackable terminal (cConsole or custom script): completes when cHackSys solves or [scCon + 0x2C] != 0; retry if press anim finished without opening hack UI
                        if (scCon != nullptr && *reinterpret_cast<const uint32_t*>(scCon + 0x2C) != 0) {
                            interactionComplete = true;
                        } else if (s_interactStartedAnim && s_interactSentScriptMsg && !inEngineLock && (s_botFrame - s_interactAnimStartFrame) >= 24) {
                            s_interactStartedAnim   = false;
                            s_interactSentScriptMsg = false;
                        }
                    } else {
                        // Non-cButton interactive script: wait for press anim to complete
                        if (s_interactStartedAnim && s_interactSentScriptMsg && !inEngineLock && (s_botFrame - s_interactAnimStartFrame) >= 20) {
                            interactionComplete = true;
                        }
                    }

                    if (interactionComplete && (s_botMode == BOT_MODE_NORMAL || prevActivations == s_cmdInitialInteractCount)) {
                        RecordSwitchActivated(interactGuid, true);
                        char ev[128] = {};
                        snprintf(ev, sizeof(ev), "\"guid\":\"0x%X\",\"name\":\"%s\",\"count\":%u",
                            interactGuid, iName, prevActivations + 1);
                        LogRawReplayEvent("INTERACT_TRIGGERED", ip, ev);
                        s_interactStartedAnim   = false;
                        s_interactSentScriptMsg = false;
                        s_activeInteractGuid    = 0;
                        return true;
                    }

                    // While the press animation or 0.50s door trigger timer is running, hold still and NEVER press B again!
                    if (s_interactStartedAnim) {
                        s_botPad.sThumbLX = 0;
                        s_botPad.sThumbLY = 0;
                        return true;
                    }

                    float frontDot = (d2d > 0.01f && sFwdLen > 0.1f)
                        ? ((toPlayerX * sFwd[0] + toPlayerY * sFwd[1]) / (d2d * sFwdLen))
                        : 1.0f;
                    float bodyDot  = (d2d > 0.01f && pFwdLen > 0.1f)
                        ? (((-toPlayerX) * pFwd[0] + (-toPlayerY) * pFwd[1]) / (d2d * pFwdLen))
                        : 1.0f;

                    const float standDist = isElevatorCabinBtn ? 1.15f : 0.70f;
                    float standPt[3] = {
                        ip[0] + ((sFwdLen > 0.1f) ? (sFwd[0] / sFwdLen) * standDist : 0.0f),
                        ip[1] + ((sFwdLen > 0.1f) ? (sFwd[1] / sFwdLen) * standDist : 0.0f),
                        playerPos[2]
                    };
                    float playerKnee[3] = { playerPos[0], playerPos[1], playerPos[2] + 0.60f };
                    float standKnee[3]  = { standPt[0],   standPt[1],   playerPos[2] + 0.60f };
                    bool  standPtClear  = (sFwdLen > 0.1f && frontDot >= 0.0f &&
                                           CheckClearLineOfSight(playerKnee, standKnee) &&
                                           s_interactApproachFrames <= 35);

                    if (curActTarget == interactGuid) {
                        // Engine has highlighted the switch! Stop moving and press Activate (B) cleanly.
                        s_botPad.sThumbLX = 0;
                        s_botPad.sThumbLY = 0;
                        if ((s_botFrame % 6) == 0) {
                            PressStdInputButton(actBtn);
                        }
                    } else if (standPtClear) {
                        if (frontDot < 0.78f || d2d > (isElevatorCabinBtn ? 1.35f : 0.92f) || d2d < 0.35f) {
                            // Phase 1: Walk to the ideal stand point directly in front of the switch normal
                            MovePlayerTowardPoint(player, ctrl, cam, playerPos, standPt);
                        } else {
                            // Phase 2: In front cone at ideal distance -> turn Nova's body in-place toward the switch
                            MovePlayerTowardPoint(player, ctrl, cam, playerPos, ip, 0.15f);
                            if ((bodyDot > 0.72f || yawErr < 18.0f) && s_interactApproachFrames >= 18 && IsValidPtr(ctrl, 0x240)) {
                                s_botPad.sThumbLX = 0;
                                s_botPad.sThumbLY = 0;
                                uint8_t* mutCtrl = const_cast<uint8_t*>(ctrl);
                                *reinterpret_cast<uint32_t*>(mutCtrl + 0x238) = interactGuid;
                                mutCtrl[0x1A] = 5;
                                mutCtrl[0x5B] = 0;
                                if (s_fnPlayerControlActivate(mutCtrl, nullptr) != 0) {
                                    s_interactStartedAnim    = true;
                                    s_interactAnimStartFrame = s_botFrame;
                                }
                            }
                        }
                    } else {
                        // Obstructed / reverse-mounted switch (e.g. sFwd pointing behind a closed door) or wide pedestal console:
                        // approach the learned interaction node s_goalPos on Nova's side of the barrier/pedestal, turn to face ip,
                        // and trigger cPlayerControl::Activate once so Nova plays her press animation and activates the script.
                        float approachGoal[3] = {
                            isElevatorCabinBtn ? elevCabinCenter[0] : s_goalPos[0],
                            isElevatorCabinBtn ? elevCabinCenter[1] : s_goalPos[1],
                            s_goalPos[2]
                        };
                        float distGoal2D = Dist2D(playerPos, approachGoal);
                        if (distGoal2D > 1.05f && d2d > 1.45f && s_interactApproachFrames <= 65) {
                            MovePlayerTowardPoint(player, ctrl, cam, playerPos, approachGoal);
                        } else {
                            MovePlayerTowardPoint(player, ctrl, cam, playerPos, ip, 0.15f);
                            if ((bodyDot > 0.72f || yawErr < 18.0f || s_interactApproachFrames > 55) && IsValidPtr(ctrl, 0x240)) {
                                s_botPad.sThumbLX = 0;
                                s_botPad.sThumbLY = 0;
                                uint8_t* mutCtrl = const_cast<uint8_t*>(ctrl);
                                *reinterpret_cast<uint32_t*>(mutCtrl + 0x238) = interactGuid;
                                mutCtrl[0x1A] = 5;
                                mutCtrl[0x5B] = 0;
                                if (s_fnPlayerControlActivate(mutCtrl, nullptr) != 0) {
                                    s_interactStartedAnim    = true;
                                    s_interactAnimStartFrame = s_botFrame;
                                } else if (s_interactApproachFrames > 60) {
                                    s_fnScriptMessageActivated(interactThing, const_cast<uint8_t*>(player));
                                    s_interactSentScriptMsg  = true;
                                    s_interactStartedAnim    = true;
                                    s_interactAnimStartFrame = s_botFrame;
                                }
                            }
                        }
                    }
                    return true;
                }
            }
        }
    }
    return false;
}

// ============================================================================
// Self-Contained Screenshot & Multimodal Filmstrip Helpers (via ReadBackbufferBgra)
// ============================================================================

static char                s_pendingScreenshotName[MAX_PATH] = { 0 };
static volatile bool       s_hasPendingScreenshot = false;
static std::vector<uint8_t> s_histFrames[3];
static uint32_t            s_histWriteIdx = 0;

void RequestBackbufferScreenshot(const char* filename) {
    if (!filename || !filename[0]) return;
    strncpy(s_pendingScreenshotName, filename, sizeof(s_pendingScreenshotName) - 1);
    s_pendingScreenshotName[sizeof(s_pendingScreenshotName) - 1] = '\0';
    s_hasPendingScreenshot = true;
}

static uint32_t CalcPngCrc32(const uint8_t* buf, size_t len, uint32_t crc = 0u) {
    crc = ~crc;
    for (size_t i = 0; i < len; ++i) {
        crc ^= buf[i];
        for (int k = 0; k < 8; ++k) {
            crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
        }
    }
    return ~crc;
}

static void WritePngU32BE(FILE* f, uint32_t v) {
    uint8_t b[4] = { (uint8_t)(v >> 24), (uint8_t)(v >> 16), (uint8_t)(v >> 8), (uint8_t)v };
    fwrite(b, 1, 4, f);
}

static void WritePngChunk(FILE* f, const char type[4], const uint8_t* data, uint32_t len) {
    WritePngU32BE(f, len);
    fwrite(type, 1, 4, f);
    if (len > 0 && data) fwrite(data, 1, len, f);
    uint32_t crc = CalcPngCrc32(reinterpret_cast<const uint8_t*>(type), 4, 0u);
    if (len > 0 && data) crc = CalcPngCrc32(data, len, crc);
    WritePngU32BE(f, crc);
}

static bool SaveRgbBufferToPng(const char* fullPath, const uint8_t* rgbTopDown, uint32_t w, uint32_t h) {
    FILE* f = fopen(fullPath, "wb");
    if (!f) return false;
    static const uint8_t kPngSig[8] = { 137, 80, 78, 71, 13, 10, 26, 10 };
    fwrite(kPngSig, 1, 8, f);

    uint8_t ihdr[13] = {
        (uint8_t)(w >> 24), (uint8_t)(w >> 16), (uint8_t)(w >> 8), (uint8_t)w,
        (uint8_t)(h >> 24), (uint8_t)(h >> 16), (uint8_t)(h >> 8), (uint8_t)h,
        8, 2, 0, 0, 0 // 8-bit RGB
    };
    WritePngChunk(f, "IHDR", ihdr, 13);

    uint32_t rowBytes = 1 + w * 3;
    uint32_t rawSize  = rowBytes * h;
    uint32_t numBlocks = (rawSize + 65534u) / 65535u;
    std::vector<uint8_t> idat;
    idat.reserve(2 + rawSize + numBlocks * 5 + 4);
    idat.push_back(0x78);
    idat.push_back(0x01);

    std::vector<uint8_t> raw(rawSize);
    uint32_t s1 = 1u, s2 = 0u;
    for (uint32_t y = 0; y < h; ++y) {
        uint8_t* dst = raw.data() + y * rowBytes;
        dst[0] = 0; // Filter 0 (None)
        memcpy(dst + 1, rgbTopDown + y * (w * 3), w * 3);
        for (uint32_t i = 0; i < rowBytes; ++i) {
            s1 = (s1 + dst[i]) % 65521u;
            s2 = (s2 + s1) % 65521u;
        }
    }

    uint32_t offset = 0;
    while (offset < rawSize) {
        uint32_t blkLen = (rawSize - offset > 65535u) ? 65535u : (rawSize - offset);
        uint8_t lastBlk = (offset + blkLen >= rawSize) ? 1u : 0u;
        idat.push_back(lastBlk);
        idat.push_back((uint8_t)(blkLen & 0xFFu));
        idat.push_back((uint8_t)((blkLen >> 8) & 0xFFu));
        uint16_t nlen = (uint16_t)~(uint16_t)blkLen;
        idat.push_back((uint8_t)(nlen & 0xFFu));
        idat.push_back((uint8_t)((nlen >> 8) & 0xFFu));
        idat.insert(idat.end(), raw.begin() + offset, raw.begin() + offset + blkLen);
        offset += blkLen;
    }
    uint32_t adler = (s2 << 16) | s1;
    idat.push_back((uint8_t)(adler >> 24));
    idat.push_back((uint8_t)(adler >> 16));
    idat.push_back((uint8_t)(adler >> 8));
    idat.push_back((uint8_t)adler);

    WritePngChunk(f, "IDAT", idat.data(), (uint32_t)idat.size());
    WritePngChunk(f, "IEND", nullptr, 0);
    fclose(f);
    return true;
}

static void CaptureFilmstripHistoryFrame() {
    const uint32_t w = 640, h = 480;
    std::vector<uint8_t> bgra(w * h * 4);
    if (!ReadBackbufferBgra(bgra.data(), w, h, w * 4)) return;
    const uint32_t tw = 213, th = 160;
    std::vector<uint8_t>& dst = s_histFrames[s_histWriteIdx % 3];
    dst.resize(tw * th * 3);
    for (uint32_t y = 0; y < th; ++y) {
        uint32_t sy = (y * h) / th;
        const uint8_t* srcRow = bgra.data() + sy * (w * 4);
        uint8_t* dstRow = dst.data() + y * (tw * 3);
        for (uint32_t x = 0; x < tw; ++x) {
            uint32_t sx = (x * w) / tw;
            dstRow[x * 3 + 0] = srcRow[sx * 4 + 2]; // R
            dstRow[x * 3 + 1] = srcRow[sx * 4 + 1]; // G
            dstRow[x * 3 + 2] = srcRow[sx * 4 + 0]; // B
        }
    }
    s_histWriteIdx++;
}

static void SaveBackbufferScreenshotFile(const char* relFilename) {
    if (!relFilename || !relFilename[0] || !g_appRootDir[0]) return;
    const uint32_t w = 640, h = 480;
    std::vector<uint8_t> bgra(w * h * 4);
    if (!ReadBackbufferBgra(bgra.data(), w, h, w * 4)) return;

    char fullPath[MAX_PATH] = {};
    snprintf(fullPath, sizeof(fullPath), "%s\\%s", g_appRootDir, relFilename);
    size_t len = strlen(relFilename);
    bool isPng = (len >= 4 && _stricmp(relFilename + len - 4, ".png") == 0);
    bool isSheet = (strstr(relFilename, "sheet") != nullptr);

    if (isPng) {
        uint32_t outH = isSheet ? (480 + 160) : 480;
        std::vector<uint8_t> rgb(w * outH * 3, 0);
        for (uint32_t y = 0; y < h; ++y) {
            const uint8_t* srcRow = bgra.data() + y * (w * 4);
            uint8_t* dstRow = rgb.data() + y * (w * 3);
            for (uint32_t x = 0; x < w; ++x) {
                dstRow[x * 3 + 0] = srcRow[x * 4 + 2]; // R
                dstRow[x * 3 + 1] = srcRow[x * 4 + 1]; // G
                dstRow[x * 3 + 2] = srcRow[x * 4 + 0]; // B
            }
        }
        if (isSheet) {
            const uint32_t tw = 213, th = 160;
            for (uint32_t k = 0; k < 3; ++k) {
                uint32_t histSlot = (s_histWriteIdx + k) % 3;
                const std::vector<uint8_t>& hf = s_histFrames[histSlot];
                if (hf.size() == tw * th * 3) {
                    for (uint32_t ty = 0; ty < th; ++ty) {
                        uint8_t* dstRow = rgb.data() + (480 + ty) * (w * 3) + (k * tw) * 3;
                        memcpy(dstRow, hf.data() + ty * (tw * 3), tw * 3);
                    }
                }
            }
        }
        if (SaveRgbBufferToPng(fullPath, rgb.data(), w, outH)) {
            LogPrintf("[Screenshot] Saved PNG screenshot (%ux%u): '%s'", w, outH, fullPath);
        }
    } else {
        FILE* f = fopen(fullPath, "wb");
        if (f) {
            uint32_t rowBytes = w * 3;
            uint32_t imgSize  = rowBytes * h;

            BITMAPFILEHEADER bfh = {};
            bfh.bfType    = 0x4D42; // 'BM'
            bfh.bfOffBits = sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER);
            bfh.bfSize    = bfh.bfOffBits + imgSize;

            BITMAPINFOHEADER bih = {};
            bih.biSize        = sizeof(BITMAPINFOHEADER);
            bih.biWidth       = (LONG)w;
            bih.biHeight      = (LONG)h; // Bottom-up BMP
            bih.biPlanes      = 1;
            bih.biBitCount    = 24;
            bih.biCompression = BI_RGB;
            bih.biSizeImage   = imgSize;

            fwrite(&bfh, 1, sizeof(bfh), f);
            fwrite(&bih, 1, sizeof(bih), f);

            std::vector<uint8_t> rowBuf(rowBytes);
            for (int y = (int)h - 1; y >= 0; --y) {
                const uint8_t* srcRow = bgra.data() + y * (w * 4);
                for (uint32_t x = 0; x < w; ++x) {
                    rowBuf[x * 3 + 0] = srcRow[x * 4 + 0]; // B
                    rowBuf[x * 3 + 1] = srcRow[x * 4 + 1]; // G
                    rowBuf[x * 3 + 2] = srcRow[x * 4 + 2]; // R
                }
                fwrite(rowBuf.data(), 1, rowBytes, f);
            }
            fclose(f);
            LogPrintf("[Screenshot] Saved backbuffer screenshot: '%s'", fullPath);
        }
    }
}

// ============================================================================
// Per-Frame Bot Update (Called on Xbox Game Thread)
// ============================================================================

volatile uint32_t g_lastBotTickMs    = 0;
volatile uint32_t g_rollingFrameDtMs = 16;

void UpdateAutoBotTick() {
    if (g_cliAutoBotTrain && (g_swapFrameCount % 25) == 0 && g_swapFrameCount > 0) {
        CaptureFilmstripHistoryFrame();
    }
    if (s_hasPendingScreenshot) {
        s_hasPendingScreenshot = false;
        SaveBackbufferScreenshotFile(s_pendingScreenshotName);
    }

    uint32_t nowMs = GetTickCount();
    if (g_lastBotTickMs != 0) {
        uint32_t dt = nowMs - g_lastBotTickMs;
        g_rollingFrameDtMs = (g_rollingFrameDtMs * 3 + dt) / 4;
    }
    g_lastBotTickMs = nowMs;
    s_botFrame++;
    memset(&s_botPad, 0, sizeof(s_botPad));

    if (!s_modeInitialized) {
        s_modeInitialized = true;
        const char* cmdLine = GetCommandLineA();
        if (cmdLine && strstr(cmdLine, "--autobot-train") != nullptr) {
            s_botMode = BOT_MODE_TRAIN;
        } else {
            s_botMode = BOT_MODE_NORMAL;
        }
        LogPrintf("[AutoBot] Initialized in %s mode", (s_botMode == BOT_MODE_TRAIN) ? "TRAIN (Multimodal)" : "NORMAL (Standalone Learned)");

        // Generate default full-level route + mechanism plans for Level 1 and Level 2 on startup
        CreateDirectoryA("autobot_io", nullptr);
        {
            const float l1Start[3] = { -2.72f,  16.61f, 0.02f };
            const float l1End[3]   = { 13.23f, -12.00f, 0.00f };
            LearnedRoutePlan plan1;
            if (PlanLearnedLevelRoute(1, l1Start, l1End, &plan1, 0u)) {
                DumpLearnedRoutePlan(plan1, "autobot_io\\route_plan_level1.txt");
            }
            const float l2Start[3] = { -82.61f, -171.00f,  0.12f };
            const float l2End[3]   = { -33.88f,  -94.38f, -5.88f };
            LearnedRoutePlan plan2;
            if (PlanLearnedLevelRoute(2, l2Start, l2End, &plan2, 0u)) {
                DumpLearnedRoutePlan(plan2, "autobot_io\\route_plan_level2.txt");
            }

            // Demonstrate mechanism-state-aware routing from the exact same Main Hangar position to Core Drill #1:
            // Case A: Generator (MECH_8) & Mainframe (MECH_10) are UNSOLVED / reset after checkpoint death (only MECH_1..3 solved)
            // Case B: Generator (MECH_8) & Mainframe (MECH_10) are ALREADY SOLVED (MECH_1..10 solved -> direct Main Hangar floor shortcut!)
            const float midHangarPos[3] = { 15.98f, -34.41f, -0.08f };
            const float drill1Pos[3]    = { 12.63f, -30.03f, -68.00f };
            uint32_t maskOnlyBoothSolved = (1u << 19) | (1u << 1) | (1u << 2) | (1u << 3);
            uint32_t maskMainframeSolved = (1u << 19) | (1u << 1) | (1u << 2) | (1u << 3) | (1u << 4) | (1u << 5) |
                                           (1u << 6) | (1u << 7) | (1u << 8) | (1u << 9) | (1u << 10);
            LearnedRoutePlan planReset, planHacked;
            if (PlanLearnedLevelRoute(1, midHangarPos, drill1Pos, &planReset, maskOnlyBoothSolved)) {
                DumpLearnedRoutePlan(planReset, "autobot_io\\route_plan_level1_respawn_reset.txt");
            }
            if (PlanLearnedLevelRoute(1, midHangarPos, drill1Pos, &planHacked, maskMainframeSolved)) {
                DumpLearnedRoutePlan(planHacked, "autobot_io\\route_plan_level1_already_hacked.txt");
            }
        }
    }

    // Hotkey polling when game window is focused: F5 toggles Bot ON/OFF, F7 dumps live route plan
    if (GetForegroundWindow() == g_hWnd) {
        bool f5Down = (GetAsyncKeyState(VK_F5) & 0x8000) != 0;
        if (f5Down && !s_f5WasDown) {
            g_autoBotEnabled = !g_autoBotEnabled;
            LogPrintf("[AutoBot] Toggled via F5 -> %s", g_autoBotEnabled ? "ENABLED" : "DISABLED");
            if (g_autoBotEnabled) {
                s_normalAlignedOnStart = false;
                s_normalRoutePlanned   = false;
                s_normalStageIdx       = 0;
                s_normalStageWpCursor  = 0;
                s_normalNavPath.clear();
                s_normalNavCursor    = 0;
                s_normalLastGoalNode = -1;
            } else {
                snprintf(s_statusText, sizeof(s_statusText), "AI Bot: OFF (F5)");
            }
        }
        s_f5WasDown = f5Down;

        bool f7Down = (GetAsyncKeyState(VK_F7) & 0x8000) != 0;
        if (f7Down && !s_f7WasDown) {
            void* wPtr = *reinterpret_cast<void**>(0x00812D78);
            void* pPtr = s_fnGetLocalPlayer();
            if (IsValidPtr(wPtr, 0x40) && IsValidPtr(pPtr, 0x530)) {
                const char* curLvl = reinterpret_cast<const char*>(reinterpret_cast<uintptr_t>(wPtr) + 0x08);
                float curPos[3] = {};
                GetThingWorldPos(pPtr, curPos);
                float finalGoal[3] = { 13.23f, -12.00f, 0.00f };
                if (_strnicmp(curLvl, "1_2_1", 5) == 0) {
                    finalGoal[0] = -33.88f; finalGoal[1] = -94.38f; finalGoal[2] = -5.88f;
                } else if (!s_learnedObjectives.empty()) {
                    const LearnedObjective& lastObj = s_learnedObjectives.back();
                    finalGoal[0] = lastObj.targetPos[0];
                    finalGoal[1] = lastObj.targetPos[1];
                    finalGoal[2] = lastObj.targetPos[2];
                }
                LearnedRoutePlan livePlan;
                if (PlanLearnedLevelRoute(curLvl, curPos, finalGoal, &livePlan)) {
                    DumpLearnedRoutePlan(livePlan, "autobot_io\\route_plan_live.txt");
                }
            }
        }
        s_f7WasDown = f7Down;

        bool f12Down = (GetAsyncKeyState(VK_F12) & 0x8000) != 0;
        if (f12Down && !s_f12WasDown) {
            char shotName[64];
            snprintf(shotName, sizeof(shotName), "shot_manual_%u.png", s_botFrame);
            RequestBackbufferScreenshot(shotName);
        }
        s_f12WasDown = f12Down;
    } else {
        s_f5WasDown  = false;
        s_f6WasDown  = false;
        s_f7WasDown  = false;
        s_f12WasDown = false;
    }

    void* world = *reinterpret_cast<void**>(0x00812D78);
    if (world && world != s_lastWorldPtr) {
        const char* lvlName = reinterpret_cast<const char*>(reinterpret_cast<uintptr_t>(world) + 0x08);
        if (!IsMenuLevelName(lvlName)) {
            if (s_startedLevel1 && _strnicmp(lvlName, "1_2_1", 5) == 0) {
                if (s_currentRunDir[0]) {
                    LogRawReplayEvent("MISSION_COMPLETE", s_prevSamplePos, "\"result\":\"WIN\"");
                    SaveRawWalkedGraphSnapshot("MISSION_COMPLETE");
                }
                LogPrintf("[AutoBot] MISSION 1 COMPLETE! Saved Raw Replay in '%s'", s_currentRunDir);
            }
            s_lastWorldPtr        = world;
            s_cachedEnemy         = nullptr;
            s_activatedSwitches.clear();
            s_searchedCorpses.clear();
            s_activeLevelFrames   = 0;
            s_seenIntroCutscene   = false;
            s_postIntroFrames     = 0;
            s_normalRoutePlanned  = false;
            s_normalStageIdx      = 0;
            s_normalStageWpCursor = 0;
            s_waitingForLlm       = false;
            if (_strnicmp(lvlName, "1_3_1b", 6) == 0) {
                s_startedLevel1  = true;
                s_inLevel1Escape = false;
            } else if (_strnicmp(lvlName, "1_2_1", 5) != 0) {
                s_startedLevel1  = false;
                s_inLevel1Escape = false;
            }
            float startP[3] = {};
            void* pl = s_fnGetLocalPlayer();
            if (IsValidPtr(pl, 0x530)) GetThingWorldPos(pl, startP);
            StartNewRawReplayRun(lvlName, startP);
            if (s_botMode == BOT_MODE_NORMAL) {
                if (!LoadCompiledLearnedLevel(lvlName)) {
                    LogPrintf("[AutoBot] No compiled file found for '%s', remaining in NORMAL mode.", lvlName);
                }
            }
        }
    }

    if (!g_autoBotEnabled) {
        s_botState    = BOT_STATE_IDLE;
        s_cachedEnemy = nullptr;
        snprintf(s_statusText, sizeof(s_statusText), "AI Bot: OFF (F5)");
        return;
    }

    // ------------------------------------------------------------------------
    // 1. Check Menu / Shell / Loadscreen / End-of-Level / Death State
    // ------------------------------------------------------------------------
    const uint8_t* shell       = reinterpret_cast<const uint8_t*>(0x00871050);
    uint8_t        shellFlags  = *reinterpret_cast<const uint8_t*>(shell + 0x38);
    uint32_t       shellMenuId = *reinterpret_cast<const uint32_t*>(shell + 0x3C);
    uint32_t       menuOn      = *reinterpret_cast<const uint32_t*>(shell + 0x6E30) | ((shellFlags & 0x02) ? 1u : 0u);
    uint32_t       shellState  = *reinterpret_cast<const uint32_t*>(shell + 0x6EC0);
    uint32_t       menuId      = *reinterpret_cast<const uint32_t*>(shell + 0x6ED8);
    if (menuId == 0) menuId = (shellMenuId != 0) ? shellMenuId : shellState;
    const void*    virtKb      = *reinterpret_cast<const void* const*>(shell + 0x6E40);
    const char*    levelName   = IsValidPtr(world, 0x40) ? reinterpret_cast<const char*>(reinterpret_cast<uintptr_t>(world) + 0x08) : "";

    bool transitionedToMission2 = (s_startedLevel1 && _strnicmp(levelName, "1_2_1", 5) == 0);
    if (transitionedToMission2 || (menuOn != 0 && menuId == 10)) {
        s_cachedEnemy = nullptr;
        s_eolFrames++;
        snprintf(s_statusText, sizeof(s_statusText), "AI Bot [%s]: MISSION COMPLETE!",
            (s_botMode == BOT_MODE_TRAIN) ? "TRAIN" : "NORMAL");
        if (s_eolFrames == 1) {
            LogRawReplayEvent("MISSION_COMPLETE", s_prevSamplePos, "\"result\":\"WIN\"");
            SaveRawWalkedGraphSnapshot("MISSION_COMPLETE");
            LogPrintf("[AutoBot] MISSION 1 COMPLETE! Saved Raw Replay in '%s'", s_currentRunDir);
        }
        if (!s_capturedEolShot && s_eolFrames == 20) {
            s_capturedEolShot = true;
            RequestBackbufferScreenshot("autobot_05_mission_complete.png");
        }
        if (s_eolFrames == 40 && strstr(GetCommandLineA(), "--autobot-exit") != nullptr) {
            PostMessageA(g_hWnd, WM_CLOSE, 0, 0);
            return;
        }
        if (menuOn != 0 && menuId == 10) {
            if (s_eolFrames > 240 && (s_botFrame % 24) < 4) {
                s_botPad.bAnalogButtons[0] = 255;
                s_botPad.wButtons |= 0x0010;
            }
            return;
        }
    } else {
        s_eolFrames = 0;
    }

    if (!world || IsMenuLevelName(levelName) || menuOn != 0 || shellState == 0x1E || shellState == 0x1F || shellState == 0x45 || shellState == 0x46) {
        s_botState    = BOT_STATE_MENU;
        s_cachedEnemy = nullptr;

        if (IsValidPtr(virtKb, 0xB8)) {
            if ((s_botFrame % 12) < 3) {
                s_botPad.wButtons |= 0x0010;      // Start (Done on virtual keyboard)
                s_botPad.bAnalogButtons[0] = 255; // A
            }
            return;
        }
        if (menuId == 0x45 || menuId == 0x46 || shellState == 0x45 || shellState == 0x46 || shellState == 0x1E || shellState == 0x1F) {
            s_wasDeadOrReloading = true;
            s_waitingForLlm      = false;
            snprintf(s_statusText, sizeof(s_statusText), "AI Bot [%s]: Death/Continue Menu",
                (s_botMode == BOT_MODE_TRAIN) ? "TRAIN" : "NORMAL");
            if ((s_botFrame % 10) < 3) {
                s_botPad.bAnalogButtons[0] = 255;
            }
            return;
        }
        snprintf(s_statusText, sizeof(s_statusText), "AI Bot [%s]: Menu/Loadscreen (0x%X)",
            (s_botMode == BOT_MODE_TRAIN) ? "TRAIN" : "NORMAL", shellState);
        uint32_t phase = s_botFrame % 24;
        if (phase < 4) {
            s_botPad.bAnalogButtons[0] = 255;
        } else if (phase >= 12 && phase < 16 && shellState != 0x18) {
            s_botPad.wButtons |= 0x0010;
        }
        return;
    }

    // ------------------------------------------------------------------------
    // 2. Check Active Cutscene & Modal Briefing Popup (Pure Gamepad Skip)
    // ------------------------------------------------------------------------
    int cutsceneActiveCount      = *reinterpret_cast<const int*>(0x00818A60);
    int numLoadedCutscenes       = *reinterpret_cast<const int*>(0x00818A88);
    const uintptr_t* cutsceneArr = *reinterpret_cast<const uintptr_t* const*>(0x00818A7C);
    bool isCutscenePlaying       = (cutsceneActiveCount > 0);
    if (!isCutscenePlaying && numLoadedCutscenes > 0 && IsValidPtr(cutsceneArr, numLoadedCutscenes * 4)) {
        for (int i = 0; i < numLoadedCutscenes && i < 64; ++i) {
            const uint8_t* cut = reinterpret_cast<const uint8_t*>(cutsceneArr[i]);
            if (IsValidPtr(cut, 0x20) && *reinterpret_cast<const int*>(cut + 0x0C) != 0) {
                isCutscenePlaying = true;
                break;
            }
        }
    }

    if (isCutscenePlaying) {
        if (s_activeInteractGuid != 0 && s_interactStartedAnim) {
            RecordSwitchActivated(s_activeInteractGuid);
            s_interactStartedAnim = false;
            s_activeInteractGuid  = 0;
        }
        s_botState    = BOT_STATE_CUTSCENE;
        s_cachedEnemy = nullptr;
        s_cutsceneFrames++;
        if (_strnicmp(levelName, "1_3_1b", 6) == 0) s_seenIntroCutscene = true;
        snprintf(s_statusText, sizeof(s_statusText), "AI Bot [%s]: Cutscene",
            (s_botMode == BOT_MODE_TRAIN) ? "TRAIN" : "NORMAL");
        if (s_cutsceneFrames > 8 && (s_botFrame % 6) < 2) {
            s_botPad.wButtons |= 0x0010;      // Start button skips cutscenes
            s_botPad.bAnalogButtons[0] = 255; // A button
        }
        return;
    }
    s_cutsceneFrames = 0;

    int            gameTimePaused = *reinterpret_cast<const int*>(0x0088041C);
    const uint8_t* uiBriefing     = reinterpret_cast<const uint8_t*>(0x007C2F50);
    uint32_t       briefActive    = *reinterpret_cast<const uint32_t*>(uiBriefing + 0x74);
    uint8_t        briefMode      = *reinterpret_cast<const uint8_t*>(uiBriefing + 0x80);
    if (gameTimePaused > 0 || (briefActive != 0 && briefMode > 1)) {
        if ((s_botFrame % 6) < 2) {
            s_botPad.bAnalogButtons[0] = 255; // A button dismisses modal briefing popup
            s_botPad.wButtons |= 0x0010;      // Start
        }
        return;
    }

    // ------------------------------------------------------------------------
    // 3. Perceive Nova (tPlayer, cPlayerControl, cPlayerInfo, tSimCamera)
    // ------------------------------------------------------------------------
    uint8_t* player = reinterpret_cast<uint8_t*>(s_fnGetLocalPlayer());
    if (!IsValidPtr(player, 0x530)) {
        s_cachedEnemy = nullptr;
        return;
    }

    s_activeLevelFrames++;
    if (s_seenIntroCutscene) s_postIntroFrames++;

    uint32_t playerGuid = *reinterpret_cast<const uint32_t*>(player + 0x34);
    float playerPos[3] = {};
    GetThingWorldPos(player, playerPos);

    float*       pHealth    = reinterpret_cast<float*>(player + 0x36C);
    const float* pMaxHealth = reinterpret_cast<const float*>(player + 0x370);

    // Lock Nova's Health (Invincibility), Psionic Energy, and Weapon Ammo/Clip in TRAIN mode and temporary NORMAL verification mode.
    if (s_botMode == BOT_MODE_TRAIN || s_botMode == BOT_MODE_NORMAL) {
        *reinterpret_cast<uint32_t*>(0x00828738) = (playerPos[2] >= -72.0f) ? 1u : 0u;
        if (playerPos[2] >= -72.0f && *pHealth > 0.0f && *pMaxHealth > 0.0f) {
            *pHealth = *pMaxHealth;
        }
        uint32_t maxPsi = *reinterpret_cast<const uint32_t*>(player + 0x480);
        if (maxPsi > 0) {
            *reinterpret_cast<uint32_t*>(player + 0x47C) = maxPsi;
        }
        uint32_t maxAmmo = *reinterpret_cast<const uint32_t*>(player + 0x448);
        if (maxAmmo == 0) maxAmmo = 300;
        *reinterpret_cast<uint32_t*>(player + 0x438) = maxAmmo;
        uint32_t maxClip = *reinterpret_cast<const uint32_t*>(player + 0x444);
        if (maxClip > 0) {
            *reinterpret_cast<uint32_t*>(player + 0x434) = maxClip;
        }
    }

    const uint8_t* ctrl  = *reinterpret_cast<const uint8_t* const*>(player + 0x3F8);
    const uint8_t* pInfo = *reinterpret_cast<const uint8_t* const*>(player + 0x520);
    const uint8_t* cam   = IsValidPtr(pInfo, 0x10) ? *reinterpret_cast<const uint8_t* const*>(pInfo + 0x04) : nullptr;

    // Check if Nova died (*pHealth <= 0.0f)
    if (*pHealth <= 0.0f) {
        if (!s_wasDeadOrReloading) {
            LogRawReplayEvent("PLAYER_DIED", playerPos, "\"result\":\"DEATH\"");
            SaveRawWalkedGraphSnapshot("PLAYER_DIED");
            LogPrintf("[AutoBot] Nova died at (%.2f, %.2f, %.2f) -> saved Raw Replay snapshot & continuing...",
                playerPos[0], playerPos[1], playerPos[2]);
        }
        s_wasDeadOrReloading = true;
        s_waitingForLlm      = false;
        s_cachedEnemy        = nullptr;
        s_searchingCorpse    = nullptr;
        s_searchHoldFrames   = 0;
        if ((s_botFrame % 10) < 3) {
            s_botPad.bAnalogButtons[0] = 255;
            s_botPad.wButtons |= 0x0010;
        }
        return;
    }

    if (s_wasDeadOrReloading || player != s_lastPlayerPtr) {
        bool wasDead = s_wasDeadOrReloading;
        if (wasDead) {
            StartNewRawReplayRun(levelName, playerPos);
        }
        s_wasDeadOrReloading    = false;
        s_lastPlayerPtr         = player;
        s_prevFrameHp           = *pHealth;
        s_cachedEnemy           = nullptr;
        s_searchingCorpse       = nullptr;
        s_searchHoldFrames      = 0;
        s_ignoredEnemyGuid      = 0;
        s_ignoreEnemyUntilFrame = 0;
        s_enemyStaleFrames      = 0;
        s_activatedSwitches.clear();
        s_searchedCorpses.clear();
        s_normalRoutePlanned    = false;
        s_normalStageIdx        = 0;
        s_normalStageWpCursor   = 0;
        s_normalNavPath.clear();
        s_normalNavCursor       = 0;
        s_normalLastGoalNode    = -1;
        if (wasDead && s_botMode == BOT_MODE_TRAIN) {
            s_waitingForLlm               = true;
            s_stateExportedForCurrentWait = false;
            s_waitLlmStartFrame           = s_botFrame;
            s_waitLlmFrozenHp             = *pHealth;
            strcpy(s_lastCmdResult, "RESPAWNED_AFTER_DEATH");
            RequestBackbufferScreenshot("autobot_io\\vision_sheet.png");
        } else {
            s_waitingForLlm               = false;
            s_stateExportedForCurrentWait = false;
        }
    }

    float playerEyePos[3] = { playerPos[0], playerPos[1], playerPos[2] + 1.5f };
    float eyePos[3]       = { playerPos[0], playerPos[1], playerPos[2] + 1.5f };
    uint8_t curCtrlState  = IsValidPtr(ctrl, 0x60) ? ctrl[0x5E] : 0;
    if (IsValidPtr(cam, 0x250) && (curCtrlState == 0x1B || curCtrlState == 0x1C)) {
        // Match UpdateAim (0x00468BEC): when in Shoot Mode, if cam->m_lerping ([cam+0x1A4]) != 0, use lerpTargetPos ([cam+0x08]), else [cam+0x240]
        uint32_t camLerping = *reinterpret_cast<const uint32_t*>(cam + 0x1A4);
        const float* cp = reinterpret_cast<const float*>(cam + (camLerping ? 0x08 : 0x240));
        if (fabsf(cp[0]) > 0.01f || fabsf(cp[1]) > 0.01f) {
            eyePos[0] = cp[0]; eyePos[1] = cp[1]; eyePos[2] = cp[2];
        }
    }
    float muzzlePos[3] = { playerPos[0], playerPos[1], playerPos[2] + 1.30f };

    // ------------------------------------------------------------------------
    // 4. Play Terran Hacking Mini-Game via Pure Gamepad (cHackSys at 0x00877E38)
    // ------------------------------------------------------------------------
    static uint32_t s_ctrlMenuTicks = 0;
    const uint8_t* hackSys          = reinterpret_cast<const uint8_t*>(0x00877E38);
    uint8_t        hackState        = *reinterpret_cast<const uint8_t*>(hackSys + 0x22);
    if (IsHackSystemActive()) {
        s_botState    = BOT_STATE_HACKING;
        s_cachedEnemy = nullptr;
        s_hackFrames++;

        uint32_t targetGuid = *reinterpret_cast<const uint32_t*>(0x00877E3C);
        float    transTimer = *reinterpret_cast<const float*>(hackSys + 0x14);
        uint8_t  numTargets = *reinterpret_cast<const uint8_t*>(0x00877E68);
        bool     menuFrozen = (s_fnHackSysControlFreeze(hackSys, nullptr) != 0);

        if (hackState != 2) {
            s_ctrlMenuTicks = 0;
        }
        if (transTimer <= 0.0f) {
            if (hackState == 1 || hackState == 4) {
                // State 1 (Main Hack Menu) or State 4 (Instructions): press A to start hacking once ControlFreeze clears
                if (!menuFrozen && (s_botFrame % 6) < 2) {
                    s_botPad.bAnalogButtons[0] = 255; // A
                }
            } else if (hackState == 3) {
                // State 3: Active cHackGameTerran 8x5 grid puzzle
                const uint8_t* pGame = *reinterpret_cast<const uint8_t* const*>(hackSys + 0x28);
                if (IsValidPtr(pGame, 0x290)) {
                    uint8_t gameState = pGame[0x08];
                    if (gameState == 1) {
                        uint8_t selRow = pGame[0x284];
                        uint8_t selCol = pGame[0x285];
                        float   curX   = *reinterpret_cast<const float*>(pGame + 0x274);
                        float   curY   = *reinterpret_cast<const float*>(pGame + 0x278);

                        // If current cell is unlocked (flag 0x02), flip it to lock the column
                        if (selRow >= 1 && selRow <= 6 && selCol < 5) {
                            const uint8_t* selCell = pGame + 0x40 + (selRow * 5 + selCol) * 0x0C;
                            if ((selCell[0] & 0x02) != 0 && (s_botFrame % 4) < 2) {
                                if ((selCell[0] & 0x01) == 0) {
                                    s_botPad.bAnalogButtons[7] = 255; // R-Trigger: AttemptSetToOne
                                } else {
                                    s_botPad.bAnalogButtons[6] = 255; // L-Trigger: AttemptSetToZero
                                }
                            }
                        }

                        // Steer cursor toward the nearest unlocked cell in rows 1..6, cols 0..4
                        float bestDistSq = 1e30f;
                        float bestX      = curX;
                        float bestY      = curY;
                        for (int r = 1; r <= 6; ++r) {
                            for (int c = 0; c < 5; ++c) {
                                const uint8_t* cell = pGame + 0x40 + (r * 5 + c) * 0x0C;
                                if ((cell[0] & 0x02) != 0) {
                                    float cx = *reinterpret_cast<const float*>(cell + 0x04);
                                    float cy = *reinterpret_cast<const float*>(cell + 0x08);
                                    float dx = cx - curX;
                                    float dy = cy - curY;
                                    float d2 = dx * dx + dy * dy;
                                    if (d2 < bestDistSq) {
                                        bestDistSq = d2;
                                        bestX      = cx;
                                        bestY      = cy;
                                    }
                                }
                            }
                        }

                        float dx = bestX - curX;
                        float dy = bestY - curY;
                        if (fabsf(dx) > 4.0f) {
                            s_botPad.sThumbLX = (dx > 0.0f) ? 28000 : -28000;
                        }
                        if (fabsf(dy) > 4.0f) {
                            // Screen Y increases downward; ThumbLY > 0 moves cursor up (negative dy)
                            s_botPad.sThumbLY = (dy < 0.0f) ? 28000 : -28000;
                        }
                    } else if (gameState == 0 || gameState == 2 || gameState == 3) {
                        if ((s_botFrame % 6) < 2) {
                            s_botPad.bAnalogButtons[0] = 255; // A
                        }
                    }
                }
            } else if (hackState == 2 && !menuFrozen) {
                // State 2: Hacked System Control Menu — wait until cHackSys::ControlFreeze clears (e.g. active .nco briefing ends),
                // then toggle target(s) with A and exit with B.
                s_ctrlMenuTicks++;
                uint32_t finalToggleTick = (numTargets > 1) ? 16u : 4u;
                if (s_ctrlMenuTicks == 4 || (numTargets > 1 && s_ctrlMenuTicks == 16)) {
                    s_botPad.bAnalogButtons[0] = 255; // A: Toggle current target
                    if (s_ctrlMenuTicks == finalToggleTick && targetGuid != 0) {
                        RecordSwitchActivated(targetGuid);
                        char ev[96] = {};
                        snprintf(ev, sizeof(ev), "\"hack_guid\":\"0x%X\"", targetGuid);
                        LogRawReplayEvent("HACK_COMPLETED", playerPos, ev);
                        LogPrintf("[AutoBot] Completed Terran Hack on console 0x%X!", targetGuid);
                    }
                } else if (numTargets > 1 && s_ctrlMenuTicks == 10) {
                    s_botPad.wButtons |= 0x0002;      // D-Pad Down: select 2nd target
                } else if (s_ctrlMenuTicks >= 24 && (s_ctrlMenuTicks % 6) < 2) {
                    s_botPad.bAnalogButtons[1] = 255; // B: ReturnToGame
                }
            }
        }
        return;
    }
    s_ctrlMenuTicks = 0;
    s_hackFrames = 0;

    // Wait 35 active frames after level load so the intro cutscene triggers cleanly and opens the spawn door
    if (s_activeLevelFrames < 35) {
        return;
    }

    // ------------------------------------------------------------------------
    // 5. Mode 1 (TRAIN MODE) Multimodal Synchronization & Think-Pause
    // ------------------------------------------------------------------------
    if (s_botMode == BOT_MODE_TRAIN) {
        // Check if a new command was written to autobot_io/command.txt
        TryPollMultimodalCommand();

        if (s_lastExecutedCmdId == 0 && !s_waitingForLlm) {
            s_waitingForLlm               = true;
            s_stateExportedForCurrentWait = false;
            s_waitLlmStartFrame           = s_botFrame;
            s_waitLlmFrozenHp             = *pHealth;
            strcpy(s_lastCmdResult, "INIT_READY");
            RequestBackbufferScreenshot("autobot_io\\vision_sheet.png");
        }

        if (s_waitingForLlm) {
            if (!s_stateExportedForCurrentWait && s_botFrame >= s_waitLlmStartFrame + 2) {
                s_stateExportedForCurrentWait = true;
                WriteMultimodalStateAndRequestVision(world, player, const_cast<uint8_t*>(cam), const_cast<uint8_t*>(ctrl), playerPos, eyePos, s_lastCmdResult);
                LogPrintf("[AutoBot-Train] Exported Step #%u state.json + vision_sheet.png -> waiting for multimodal command...", s_stepCounter);
            }
            s_botState = BOT_STATE_WAIT_LLM;
            // Hold Nova stationary and preserve HP while paused waiting for LLM I/O (do NOT advance s_activeGameTicks!)
            *pHealth = s_waitLlmFrozenHp;
            s_botPad.sThumbLX = 0;
            s_botPad.sThumbLY = 0;
            snprintf(s_statusText, sizeof(s_statusText),
                "AI Bot [TRAIN]: Step #%u Waiting for Multimodal LLM at (%.1f, %.1f, %.1f)...",
                s_stepCounter, playerPos[0], playerPos[1], playerPos[2]);
            return;
        }
    }

    // Active Gameplay Frame! Increment fixed-time-interval clock
    s_activeGameTicks++;

    if (*pHealth < s_prevFrameHp - 0.1f) {
        float dmg = s_prevFrameHp - *pHealth;
        s_totalDamageTakenThisRun += dmg;
        if (s_cachedEnemy && IsValidReadablePtr(s_cachedEnemy, 0x374)) {
            uint32_t eGuid = *reinterpret_cast<const uint32_t*>(s_cachedEnemy + 0x34);
            EnemyTacticalEntry* te = FindOrCreateEnemyTactic(eGuid, SafeGetThingName(s_cachedEnemy), nullptr);
            te->totalDmgTakenFrom += dmg;
            if (te->totalDmgTakenFrom > 55.0f && te->policy == POLICY_UNKNOWN) {
                te->policy = POLICY_BOSS_KITE;
            }
        }
    }
    s_prevFrameHp = *pHealth;

    if ((s_botFrame % 4) == 0) {
        AutoCollectNearbyPowerupsAndOpenDoors(world, player, ctrl, playerPos);
    }

    // ------------------------------------------------------------------------
    // 6. Determine Current Navigation Goal, Combat Stance & Interactive Target
    // ------------------------------------------------------------------------
    CombatStance activeStance      = STANCE_DEFENSIVE;
    uint32_t     activeInteract    = 0;
    uint32_t     activeCorpse      = 0;
    uint32_t     activeRegion      = 0;
    uint32_t     forcedTargetEnemy = 0;
    bool         hasMoveGoal       = false;
    float        nextMoveTarget[3] = { playerPos[0], playerPos[1], playerPos[2] };

    if (s_botMode == BOT_MODE_TRAIN) {
        s_cmdElapsedFrames++;
        activeStance      = s_curCmd.stance;
        activeInteract    = s_curCmd.interactGuid;
        activeCorpse      = s_curCmd.searchCorpseGuid;
        activeRegion      = s_curCmd.triggerRegionGuid;
        forcedTargetEnemy = s_curCmd.targetEnemyGuid;
        snprintf(s_goalDesc, sizeof(s_goalDesc), "%s", s_curCmd.desc);

        if (s_curCmd.jumpOnce && s_cmdElapsedFrames == 2) {
            PressStdInputButton(ReadCtrlActionBtn(ctrl, 0x1A8, 4)); // Jump once
        }

        size_t numWps = s_curCmd.waypointsXYZ.size() / 3;
        while (s_cmdWaypointCursor < numWps) {
            float wp[3] = {
                s_curCmd.waypointsXYZ[s_cmdWaypointCursor * 3 + 0],
                s_curCmd.waypointsXYZ[s_cmdWaypointCursor * 3 + 1],
                s_curCmd.waypointsXYZ[s_cmdWaypointCursor * 3 + 2]
            };
            if (Dist2D(playerPos, wp) < 1.35f && fabsf(playerPos[2] - wp[2]) < 2.2f) {
                s_cmdWaypointCursor++;
            } else {
                break;
            }
        }

        if (s_cmdWaypointCursor < numWps) {
            nextMoveTarget[0] = s_curCmd.waypointsXYZ[s_cmdWaypointCursor * 3 + 0];
            nextMoveTarget[1] = s_curCmd.waypointsXYZ[s_cmdWaypointCursor * 3 + 1];
            nextMoveTarget[2] = s_curCmd.waypointsXYZ[s_cmdWaypointCursor * 3 + 2];
            s_goalPos[0]      = s_curCmd.waypointsXYZ[(numWps - 1) * 3 + 0];
            s_goalPos[1]      = s_curCmd.waypointsXYZ[(numWps - 1) * 3 + 1];
            s_goalPos[2]      = s_curCmd.waypointsXYZ[(numWps - 1) * 3 + 2];
            hasMoveGoal       = true;
        }
    } else {
        // BOT_MODE_NORMAL: Stage-decomposed navigation & mechanism cracking via PlanLearnedLevelRoute
        float finalGoalPos[3] = { 13.23f, -12.00f, 0.00f };
        if (!s_learnedNodes.empty()) {
            finalGoalPos[0] = s_learnedNodes.back().x;
            finalGoalPos[1] = s_learnedNodes.back().y;
            finalGoalPos[2] = s_learnedNodes.back().z;
        }

        if (s_normalRoutePlanned && s_normalStageIdx < s_normalRoutePlan.stageCount) {
            const LearnedRouteStage& chkSt = s_normalRoutePlan.stages[s_normalStageIdx];
            bool isElevatorStage = (chkSt.hasMechanismAction &&
                strcmp(chkSt.mechanism.actionType, "RIDE_ELEVATOR") == 0);
            uint32_t chkWpIdx = chkSt.waypointStartIdx +
                ((s_normalStageWpCursor < chkSt.waypointCount) ? s_normalStageWpCursor : 0);
            if (!isElevatorStage && chkWpIdx < s_normalRoutePlan.waypointCount &&
                fabsf(playerPos[2] - s_normalRoutePlan.waypoints[chkWpIdx].pos[2]) > 25.0f)
            {
                s_normalRoutePlanned = false;
            }
        }
        if (!s_normalRoutePlanned) {
            if (PlanLearnedLevelRoute(s_currentLevel, playerPos, finalGoalPos, &s_normalRoutePlan, MECH_STATE_AUTO_LIVE)) {
                s_normalRoutePlanned  = true;
                s_normalStageIdx      = 0;
                s_normalStageWpCursor = 0;
                DumpLearnedRoutePlan(s_normalRoutePlan, "autobot_io\\route_plan_live.txt");
                LogPrintf("[AutoBot] Planned live route from (%.2f, %.2f, %.2f) -> (%.2f, %.2f, %.2f): %u stages, %u waypoints, %u mechanisms",
                    playerPos[0], playerPos[1], playerPos[2],
                    finalGoalPos[0], finalGoalPos[1], finalGoalPos[2],
                    s_normalRoutePlan.stageCount, s_normalRoutePlan.waypointCount, s_normalRoutePlan.mechanismCount);
            }
        }

        if (s_normalRoutePlanned && s_normalStageIdx < s_normalRoutePlan.stageCount) {
            bool isClimbingNow = (IsValidPtr(ctrl, 0x60) &&
                (ctrl[0x5E] == 0x13 || ctrl[0x5E] == 0x14 || ctrl[0x5E] == 0x15 ||
                 ctrl[0x5E] == 0x16 || ctrl[0x5E] == 0x11 || ctrl[0x5E] == 0x12));

            // Advance at most 1 completed stage waypoint / mechanism per tick
            if (s_normalStageIdx < s_normalRoutePlan.stageCount) {
                const LearnedRouteStage& st = s_normalRoutePlan.stages[s_normalStageIdx];
                if (s_normalStageWpCursor >= st.waypointCount && s_normalStageIdx + 1 < s_normalRoutePlan.stageCount) {
                    s_normalStageIdx++;
                    s_normalStageWpCursor = 0;
                }
                const LearnedRouteStage& curStage = s_normalRoutePlan.stages[s_normalStageIdx];
                if (s_normalStageWpCursor < curStage.waypointCount) {
                    uint32_t wpIdx = curStage.waypointStartIdx + s_normalStageWpCursor;
                    const LearnedRouteWaypoint& wp = s_normalRoutePlan.waypoints[wpIdx];
                    bool isStageFinalWp = (s_normalStageWpCursor + 1 >= curStage.waypointCount);
                    bool isNearStageEndForMech = isStageFinalWp ||
                        (s_normalStageWpCursor + 2 >= curStage.waypointCount && Dist2D(playerPos, curStage.toPos) < 3.2f);

                    uint32_t stepInteractGuid = 0;
                    uint32_t stepRegionGuid   = 0;
                    if (isNearStageEndForMech && curStage.hasMechanismAction) {
                        if (strcmp(curStage.mechanism.actionType, "ENTER_REGION") == 0) {
                            if (isStageFinalWp) stepRegionGuid = curStage.mechanism.guid;
                        } else if (strcmp(curStage.mechanism.actionType, "CLIMB_LADDER") != 0) {
                            stepInteractGuid = curStage.mechanism.guid;
                        }
                    }

                    float distWp   = Dist2D(playerPos, wp.pos);
                    float dzWp     = fabsf(playerPos[2] - wp.pos[2]);
                    bool  stepDone = false;

                    if (IsRideElevatorMechanism(stepInteractGuid)) {
                        float destZ = GetElevatorDestinationZ(stepInteractGuid, wp.pos[2]);
                        float tolZ  = (destZ > wp.pos[2]) ? 0.18f : 0.45f;
                        uint32_t reqAct = GetRequiredActivationsForCurrentStage(stepInteractGuid);
                        if (GetSwitchActivationCount(stepInteractGuid) >= reqAct && fabsf(playerPos[2] - destZ) < tolZ) {
                            stepDone       = true;
                            isStageFinalWp = true;
                        }
                    } else if (const uint8_t* scCon = (stepInteractGuid != 0) ? FindScriptByOwnerGuid(stepInteractGuid, "cConsole") : nullptr) {
                        if (*reinterpret_cast<const uint32_t*>(scCon + 0x2C) != 0 && distWp < 4.5f) {
                            stepDone       = true;
                            isStageFinalWp = true;
                        }
                    } else if (stepInteractGuid != 0) {
                        uint32_t reqAct = GetRequiredActivationsForCurrentStage(stepInteractGuid);
                        if (GetSwitchActivationCount(stepInteractGuid) >= reqAct &&
                            (IsShootObstacleMechanism(stepInteractGuid) || distWp < 4.5f || Dist2D(playerPos, curStage.toPos) < 4.5f))
                        {
                            stepDone       = true;
                            isStageFinalWp = true;
                        }
                    } else if (isStageFinalWp && curStage.hasMechanismAction && strcmp(curStage.mechanism.actionType, "CLIMB_LADDER") == 0) {
                        if ((distWp < 1.85f && dzWp < 2.2f) || isClimbingNow) stepDone = true;
                    } else if (strcmp(wp.moveType, "LADDER") == 0) {
                        // Generic ladder top dismount: complete once Nova reaches top floor height and finishes climbing
                        if (playerPos[2] >= wp.pos[2] - 0.30f && !isClimbingNow && distWp < 3.0f) stepDone = true;
                    } else if (strcmp(wp.moveType, "JUMP") == 0) {
                        // Generic jump/mantle ledge climb: complete once Nova reaches ledge height and finishes mantle/jump
                        if (playerPos[2] >= wp.pos[2] - 0.30f && !isClimbingNow && (!IsValidPtr(ctrl, 0x60) || ctrl[0x5E] != 10) && distWp < 2.8f) stepDone = true;
                    } else if (strcmp(wp.moveType, "DROP") == 0) {
                        // Generic drop landing: complete only after Nova has landed on the lower deck (not while falling in mid-air)
                        bool isFallingNow = (IsValidPtr(ctrl, 0x60) && ctrl[0x5E] == 0x07);
                        if (dzWp < 1.0f && !isFallingNow && distWp < 1.85f) stepDone = true;
                    } else if (dzWp < 1.65f) {
                        float tightRadius = (stepRegionGuid != 0) ? 0.8f : 0.95f;
                        if (distWp < tightRadius) {
                            stepDone = true;
                        } else if (distWp < 1.75f) {
                            if (isStageFinalWp) {
                                stepDone = true;
                            } else {
                                const LearnedRouteWaypoint& nextWp = s_normalRoutePlan.waypoints[wpIdx + 1];
                                const float* prevPos = (s_normalStageWpCursor > 0)
                                    ? s_normalRoutePlan.waypoints[wpIdx - 1].pos
                                    : curStage.fromPos;
                                const char* prevMove = (s_normalStageWpCursor > 0)
                                    ? s_normalRoutePlan.waypoints[wpIdx - 1].moveType
                                    : "WALK";
                                const char* nextNextMove = (s_normalStageWpCursor + 2 < curStage.waypointCount)
                                    ? s_normalRoutePlan.waypoints[wpIdx + 2].moveType
                                    : "WALK";
                                bool nearVerticalTransition =
                                    (strcmp(prevMove, "WALK") != 0 && strcmp(prevMove, "START") != 0) ||
                                    strcmp(nextWp.moveType, "WALK") != 0 ||
                                    strcmp(nextNextMove, "WALK") != 0;

                                float inX  = wp.pos[0] - prevPos[0];
                                float inY  = wp.pos[1] - prevPos[1];
                                float outX = nextWp.pos[0] - wp.pos[0];
                                float outY = nextWp.pos[1] - wp.pos[1];
                                float inLen  = sqrtf(inX * inX + inY * inY);
                                float outLen = sqrtf(outX * outX + outY * outY);
                                float turnDot = (inLen > 0.1f && outLen > 0.1f)
                                    ? ((inX * outX + inY * outY) / (inLen * outLen))
                                    : 1.0f;

                                float fromWaist[3] = { playerPos[0], playerPos[1], playerPos[2] + 0.90f };
                                float toWaist[3]   = { nextWp.pos[0], nextWp.pos[1], nextWp.pos[2] + 0.90f };
                                float fromKnee[3]  = { playerPos[0], playerPos[1], playerPos[2] + 0.45f };
                                float toKnee[3]    = { nextWp.pos[0], nextWp.pos[1], nextWp.pos[2] + 0.45f };
                                if (!nearVerticalTransition && turnDot > 0.85f &&
                                    CheckClearLineOfSight(fromWaist, toWaist) &&
                                    CheckClearLineOfSight(fromKnee, toKnee))
                                {
                                    stepDone = true;
                                }
                            }
                        }
                    }

                    if (stepDone) {
                        if (!isStageFinalWp) {
                            s_normalStageWpCursor++;
                        } else {
                            if (curStage.hasMechanismAction) {
                                LogPrintf("[RoutePlanner] Stage %d/%u complete: Node %d -> Node %d | Cracked MECH_%d (0x%X %s)",
                                    curStage.stageIndex, s_normalRoutePlan.stageCount, curStage.fromNodeId, curStage.toNodeId,
                                    curStage.mechanism.mechId, curStage.mechanism.guid, curStage.mechanism.actionType);
                                if (strcmp(curStage.mechanism.actionType, "RIDE_ELEVATOR") == 0 &&
                                    (uint32_t)curStage.stageIndex + 1 >= s_normalRoutePlan.stageCount)
                                {
                                    s_inLevel1Escape = true;
                                }
                            } else {
                                LogPrintf("[RoutePlanner] Final Stage %d/%u complete: Reached Destination Node %d (%.2f, %.2f, %.2f)",
                                    curStage.stageIndex, s_normalRoutePlan.stageCount, curStage.toNodeId,
                                    curStage.toPos[0], curStage.toPos[1], curStage.toPos[2]);
                            }
                            if (s_normalStageIdx + 1 < s_normalRoutePlan.stageCount) {
                                s_normalStageIdx++;
                                s_normalStageWpCursor = 0;
                            } else {
                                s_normalStageWpCursor = curStage.waypointCount;
                            }
                        }
                    }
                }
            }

            // Generic Doorway & Corner Stall Recovery:
            // 1) If Nova just completed an OPEN_SWITCH stage and is physically blocked at the doorway threshold for > 95 frames
            //    without advancing toward the post-doorway waypoint, step back 1 stage and re-toggle the doorway switch.
            // 2) If Nova cut a corner too early (or was displaced during combat) and is physically stalled from reaching the current
            //    intermediate waypoint for > 110 navigation frames, step back 1 waypoint to clear the corner cleanly.
            if (s_lastTrackedStageIdx != s_normalStageIdx || s_lastTrackedWpCursor != s_normalStageWpCursor) {
                s_lastTrackedStageIdx = s_normalStageIdx;
                s_lastTrackedWpCursor = s_normalStageWpCursor;
                s_stageWpBestDist     = 1e30f;
                s_stageWpStallFrames  = 0;
            }
            {
                const LearnedRouteStage& checkSt = s_normalRoutePlan.stages[s_normalStageIdx];
                uint32_t checkWpOff = (s_normalStageWpCursor < checkSt.waypointCount)
                    ? s_normalStageWpCursor
                    : (checkSt.waypointCount > 0 ? checkSt.waypointCount - 1 : 0);
                const LearnedRouteWaypoint& checkWp = s_normalRoutePlan.waypoints[checkSt.waypointStartIdx + checkWpOff];
                float curDistToWp = Dist2D(playerPos, checkWp.pos);
                if (curDistToWp + 0.18f < s_stageWpBestDist) {
                    s_stageWpBestDist    = curDistToWp;
                    s_stageWpStallFrames = 0;
                } else if (s_botState == BOT_STATE_NAVIGATE && !isClimbingNow) {
                    s_stageWpStallFrames++;
                }

                if (s_stageWpStallFrames > 95 && s_normalStageIdx > 0 && s_normalStageWpCursor <= 2) {
                    const LearnedRouteStage& prevStage = s_normalRoutePlan.stages[s_normalStageIdx - 1];
                    const uint8_t* prevScBtn = (prevStage.hasMechanismAction && strcmp(prevStage.mechanism.actionType, "OPEN_SWITCH") == 0)
                        ? FindScriptByOwnerGuid(prevStage.mechanism.guid, "cButton")
                        : nullptr;
                    if (prevScBtn != nullptr &&
                        Dist2D(playerPos, prevStage.toPos) < 3.5f &&
                        !IsDoorSwitchAlreadyOpen(world, prevStage.mechanism.guid, prevStage.toPos, nullptr, prevScBtn))
                    {
                        LogPrintf("[AutoBot] Doorway blocked after MECH_%d (0x%X) for %u frames -> stepping back to Stage %d to toggle switch!",
                            prevStage.mechanism.mechId, prevStage.mechanism.guid, s_stageWpStallFrames, prevStage.stageIndex);
                        ForceResetSwitchForRetry(world, prevStage.mechanism.guid);
                        s_normalStageIdx--;
                        s_normalStageWpCursor = (prevStage.waypointCount > 0) ? (prevStage.waypointCount - 1) : 0;
                        s_lastTrackedStageIdx = s_normalStageIdx;
                        s_lastTrackedWpCursor = s_normalStageWpCursor;
                        s_stageWpBestDist     = 1e30f;
                        s_stageWpStallFrames  = 0;
                    }
                }
                bool isCheckWpFinal = (checkWpOff + 1 >= checkSt.waypointCount);
                if (s_stageWpStallFrames > 110 && s_normalStageWpCursor > 0 && !isClimbingNow &&
                    !(isCheckWpFinal && checkSt.hasMechanismAction))
                {
                    const LearnedRouteWaypoint& prevWp = s_normalRoutePlan.waypoints[checkSt.waypointStartIdx + checkWpOff - 1];
                    if (fabsf(playerPos[2] - checkWp.pos[2]) < 1.8f && fabsf(playerPos[2] - prevWp.pos[2]) < 1.8f) {
                        LogPrintf("[AutoBot] Stalled toward Stage %d WP %u (Node %d) for %u frames -> stepping back 1 waypoint to clear obstacle!",
                            checkSt.stageIndex, checkWpOff + 1, checkWp.nodeId, s_stageWpStallFrames);
                        s_normalStageWpCursor--;
                        s_lastTrackedWpCursor = s_normalStageWpCursor;
                        s_stageWpBestDist     = 1e30f;
                        s_stageWpStallFrames  = 0;
                    }
                }
            }

            const LearnedRouteStage& activeSt = s_normalRoutePlan.stages[s_normalStageIdx];
            uint32_t curWpOffset = (s_normalStageWpCursor < activeSt.waypointCount)
                ? s_normalStageWpCursor
                : (activeSt.waypointCount > 0 ? activeSt.waypointCount - 1 : 0);
            const LearnedRouteWaypoint& activeWp = s_normalRoutePlan.waypoints[activeSt.waypointStartIdx + curWpOffset];
            bool isFinalWpOfStage = (curWpOffset + 1 >= activeSt.waypointCount);

            activeStance   = STANCE_DEFENSIVE;
            activeInteract = 0;
            activeRegion   = 0;
            if ((isFinalWpOfStage || (curWpOffset + 2 >= activeSt.waypointCount && Dist2D(playerPos, activeSt.toPos) < 3.2f)) &&
                activeSt.hasMechanismAction)
            {
                if (strcmp(activeSt.mechanism.actionType, "ENTER_REGION") == 0) {
                    activeRegion = activeSt.mechanism.guid;
                } else if (strcmp(activeSt.mechanism.actionType, "CLIMB_LADDER") != 0) {
                    activeInteract = activeSt.mechanism.guid;
                }
            }

            s_goalPos[0]      = activeWp.pos[0];
            s_goalPos[1]      = activeWp.pos[1];
            s_goalPos[2]      = activeWp.pos[2];
            nextMoveTarget[0] = activeWp.pos[0];
            nextMoveTarget[1] = activeWp.pos[1];
            nextMoveTarget[2] = activeWp.pos[2];
            hasMoveGoal       = true;

            // Generic RIDE_ELEVATOR Cabin Centering:
            // When approaching the final waypoint (elevator interior button) or waiting inside the elevator cabin after pressing
            // the button, target the flat cabin floor center 1.15m along the button's inward surface normal (ip + sFwd * 1.15f)
            // so Nova stays off the interior wall's lower bevel and ActorFindFloor (normal.z >= 0.70) never triggers STATE_FALL (0x07).
            if (activeSt.hasMechanismAction &&
                strcmp(activeSt.mechanism.actionType, "RIDE_ELEVATOR") == 0 &&
                (isFinalWpOfStage || GetSwitchActivationCount(activeSt.mechanism.guid) >= 1))
            {
                float cabinCenter[3] = {};
                if (ComputeElevatorCabinCenterFromButton(activeSt.mechanism.guid, playerPos[2], cabinCenter)) {
                    nextMoveTarget[0] = cabinCenter[0];
                    nextMoveTarget[1] = cabinCenter[1];
                    nextMoveTarget[2] = cabinCenter[2];
                }
            }

            if (activeSt.hasMechanismAction) {
                snprintf(s_goalDesc, sizeof(s_goalDesc), "Stage %d/%u WP %u/%u (Node %d -> MECH_%d 0x%X)",
                    activeSt.stageIndex, s_normalRoutePlan.stageCount,
                    curWpOffset + 1, activeSt.waypointCount,
                    activeWp.nodeId, activeSt.mechanism.mechId, activeSt.mechanism.guid);
            } else {
                snprintf(s_goalDesc, sizeof(s_goalDesc), "Stage %d/%u WP %u/%u (Node %d -> Goal)",
                    activeSt.stageIndex, s_normalRoutePlan.stageCount,
                    curWpOffset + 1, activeSt.waypointCount, activeWp.nodeId);
            }
        }
    }

    // Handle Cloak Bypass Stance via bound Cloak button (ctrl + 0x1D0, default D-Pad Left)
    if (activeStance == STANCE_CLOAK_BYPASS && (s_botFrame % 60) == 1) {
        PressStdInputButton(ReadCtrlActionBtn(ctrl, 0x1D0, 12));
    }

    // ------------------------------------------------------------------------
    // 7. Tactical Enemy Selection & Active Run-and-Gun Combat State Machine
    // ------------------------------------------------------------------------
    const uint8_t* w = reinterpret_cast<const uint8_t*>(world);
    static void*    s_lastEngagedEnemyPtr = nullptr;
    static uint32_t s_lastEngagedGuid     = 0;
    static uint32_t s_engagedGuidFrames   = 0;
    static char     s_lastEngagedName[32] = {};

    if (s_lastEngagedGuid != 0 && s_lastEngagedEnemyPtr != nullptr) {
        if (!IsValidReadablePtr(s_lastEngagedEnemyPtr, 0x374) ||
            !IsValidReadablePtr(*reinterpret_cast<const void* const*>(s_lastEngagedEnemyPtr), 0x40) ||
            *reinterpret_cast<const float*>(reinterpret_cast<const uint8_t*>(s_lastEngagedEnemyPtr) + 0x36C) <= 0.0f ||
            s_fnIsValidAssistTarget(s_lastEngagedEnemyPtr) == 0)
        {
            s_killCount++;
            EnemyTacticalEntry* te = FindOrCreateEnemyTactic(s_lastEngagedGuid, s_lastEngagedName, nullptr);
            te->timesKilled++;
            char ev[128] = {};
            snprintf(ev, sizeof(ev), "\"guid\":\"0x%X\",\"name\":\"%s\",\"kills\":%u",
                s_lastEngagedGuid, s_lastEngagedName, s_killCount);
            LogRawReplayEvent("ENEMY_KILLED", playerPos, ev);
            LogPrintf("[AutoBot] Killed enemy '%s' (0x%X) at (%.2f, %.2f, %.2f) | Total Kills: %u, Shots Fired: %u",
                s_lastEngagedName, s_lastEngagedGuid, playerPos[0], playerPos[1], playerPos[2], s_killCount, s_totalShotsFired);

            s_lastEngagedGuid     = 0;
            s_lastEngagedEnemyPtr = nullptr;
            s_engagedGuidFrames   = 0;
            s_enemyStaleFrames    = 0;
            s_lastTrackedEnemyHp  = -1.0f;
            s_cachedEnemy         = nullptr;
        }
    }

    bool isClimbing = (IsValidPtr(ctrl, 0x60) &&
        (ctrl[0x5E] == 0x13 || ctrl[0x5E] == 0x14 || ctrl[0x5E] == 0x15 ||
         ctrl[0x5E] == 0x16 || ctrl[0x5E] == 0x11 || ctrl[0x5E] == 0x12 || ctrl[0x5E] == 10));
    bool inElevatorCabinNow = IsPlayerInsideElevatorCabin(playerPos);
    bool atLadderBase = false;
    if (s_botMode == BOT_MODE_NORMAL && s_normalRoutePlanned && s_normalStageIdx < s_normalRoutePlan.stageCount) {
        const LearnedRouteStage& curSt = s_normalRoutePlan.stages[s_normalStageIdx];
        uint32_t wpOff = (s_normalStageWpCursor < curSt.waypointCount)
            ? s_normalStageWpCursor
            : (curSt.waypointCount > 0 ? curSt.waypointCount - 1 : 0);
        const LearnedRouteWaypoint& curWp = s_normalRoutePlan.waypoints[curSt.waypointStartIdx + wpOff];
        if (strcmp(curWp.moveType, "LADDER") == 0 ||
            (curSt.hasMechanismAction && strcmp(curSt.mechanism.actionType, "CLIMB_LADDER") == 0 && Dist2D(playerPos, curSt.toPos) < 4.5f))
        {
            atLadderBase = true;
        }
    }
    bool isHackConsoleTarget = (activeInteract != 0) && IsCurrentStageHackTerminal(activeInteract);
    bool nearActiveMechanism = (activeInteract != 0 && !isHackConsoleTarget && Dist2D(playerPos, s_goalPos) < 2.2f) && (s_stageWpStallFrames < 45);
    bool suppressCombatForNav = (s_botMode == BOT_MODE_NORMAL) &&
                                (isClimbing || inElevatorCabinNow || atLadderBase || nearActiveMechanism || (s_botFrame < s_unstuckUntilFrame + 45));

    if (suppressCombatForNav || ((activeStance == STANCE_IGNORE || activeStance == STANCE_CLOAK_BYPASS) && forcedTargetEnemy == 0)) {
        s_cachedEnemy = nullptr;
    } else {
        s_cachedEnemy = nullptr;
        float bestEnemyScore = 1e30f;
        uint8_t* curThing = IsValidPtr(w, 0x100) ? *reinterpret_cast<uint8_t* const*>(w + 0xE4) : nullptr;
        int scanCount = 0;
        while (IsValidPtr(curThing, 0x140) && scanCount < 2048) {
            if (curThing != player && *reinterpret_cast<const uint32_t*>(curThing + 0x10) == 0 && s_fnIsValidAssistTarget(curThing) != 0) {
                uint32_t eGuid = *reinterpret_cast<const uint32_t*>(curThing + 0x34);
                if (s_botMode == BOT_MODE_TRAIN && forcedTargetEnemy != 0 && eGuid != forcedTargetEnemy) {
                    curThing = *reinterpret_cast<uint8_t* const*>(curThing + 0x1C);
                    scanCount++;
                    continue;
                }
                const char* eName = SafeGetThingName(curThing);
                float ep[3] = {};
                GetThingWorldPos(curThing, ep);
                EnemyTacticalEntry* te = FindOrCreateEnemyTactic(eGuid, eName, ep);

                // Auto-clear any non-Camera/non-Turret monster within 15.5m with Gauss Rifle
                bool isCameraOrTurret = (eName && (strcmp(eName, "Camera") == 0 || strcmp(eName, "Gauss Turret") == 0));
                bool isIgnored = isCameraOrTurret ||
                                 (te && s_botFrame < te->ignoreUntilFrame) ||
                                 (eGuid == s_ignoredEnemyGuid && s_botFrame < s_ignoreEnemyUntilFrame);

                if (!isIgnored || (forcedTargetEnemy != 0 && eGuid == forcedTargetEnemy)) {
                    float d  = Dist3D(playerPos, ep);
                    float dz = fabsf(playerPos[2] - ep[2]);
                    float maxDist = (forcedTargetEnemy != 0 && eGuid == forcedTargetEnemy) ? 45.0f : 15.5f;
                    if (d <= maxDist && (dz < 2.5f || (forcedTargetEnemy != 0 && eGuid == forcedTargetEnemy))) {
                        float tpos[3] = { ep[0], ep[1], ep[2] + 1.05f };
                        GetEnemyTargetPos(curThing, tpos);
                        bool pointBlankLOS = (d < 3.0f && dz < 1.8f);
                        if (pointBlankLOS ||
                            (CheckClearLineOfSight(playerEyePos, tpos) && CheckClearLineOfSight(muzzlePos, tpos)) ||
                            (forcedTargetEnemy != 0 && eGuid == forcedTargetEnemy && d < 25.0f))
                        {
                            // Prefer keeping lock on currently engaged enemy to avoid target-switching camera whip
                            float score = (forcedTargetEnemy != 0 && eGuid == forcedTargetEnemy) ? (d * 0.1f) :
                                          ((eGuid == s_lastEngagedGuid) ? (d * 0.70f) : d);
                            if (score < bestEnemyScore) {
                                bestEnemyScore    = score;
                                s_cachedEnemy     = curThing;
                                s_cachedEnemyDist = d;
                                s_cachedEnemyTargetPos[0] = tpos[0];
                                s_cachedEnemyTargetPos[1] = tpos[1];
                                s_cachedEnemyTargetPos[2] = tpos[2];
                            }
                        }
                    }
                }
            }
            curThing = *reinterpret_cast<uint8_t* const*>(curThing + 0x1C);
            scanCount++;
        }
    }

    bool bestEnemyHasClearLOS = false;
    if (s_cachedEnemy) {
        bool pointBlankLOS = (s_cachedEnemyDist < 3.0f && fabsf(playerPos[2] - s_cachedEnemyTargetPos[2]) < 2.2f);
        bestEnemyHasClearLOS = pointBlankLOS ||
                               (CheckClearLineOfSight(muzzlePos, s_cachedEnemyTargetPos) &&
                                CheckClearLineOfSight(playerEyePos, s_cachedEnemyTargetPos));
        uint32_t eGuid = *reinterpret_cast<const uint32_t*>(s_cachedEnemy + 0x34);
        float    eHp   = *reinterpret_cast<const float*>(s_cachedEnemy + 0x36C);
        if (eGuid == s_lastEngagedGuid) {
            s_engagedGuidFrames++;
        } else {
            s_engagedGuidFrames = 1;
        }
        if (eGuid == s_lastEngagedGuid && fabsf(eHp - s_lastTrackedEnemyHp) < 0.1f) {
            s_enemyStaleFrames++;
        } else {
            s_enemyStaleFrames   = 0;
            s_lastTrackedEnemyHp = eHp;
        }

        if ((s_enemyStaleFrames > 180 || s_engagedGuidFrames > 360) && eGuid != forcedTargetEnemy) {
            // Only ignore an enemy if its HP has not dropped for 3.0s (180 frames) or total lock exceeds 6.0s
            EnemyTacticalEntry* teIgnore = FindOrCreateEnemyTactic(eGuid, SafeGetThingName(s_cachedEnemy), nullptr);
            if (teIgnore) {
                teIgnore->ignoreUntilFrame = s_botFrame + 300;
            }
            s_ignoredEnemyGuid      = eGuid;
            s_ignoreEnemyUntilFrame = s_botFrame + 300;
            s_cachedEnemy           = nullptr;
            s_enemyStaleFrames      = 0;
            s_engagedGuidFrames     = 0;
        }

        if (s_cachedEnemy) {
            s_lastEngagedEnemyPtr = s_cachedEnemy;
            s_lastEngagedGuid     = eGuid;
            snprintf(s_lastEngagedName, sizeof(s_lastEngagedName), "%s", SafeGetThingName(s_cachedEnemy));
        }
    }

    uint8_t* bestEnemy = (isClimbing || suppressCombatForNav) ? nullptr : s_cachedEnemy;

    // ------------------------------------------------------------------------
    // 8. Execute Target Interaction (Always Takes Priority Over Combat, except clear enemies near hack consoles first!)
    // ------------------------------------------------------------------------
    bool handledByInteract = (bestEnemy != nullptr && isHackConsoleTarget && s_cachedEnemyDist < 12.0f)
        ? false
        : ExecuteTargetInteraction(
            world, player, ctrl, cam, shell, playerGuid, playerPos, eyePos,
            activeInteract, activeCorpse, activeRegion);

    // ------------------------------------------------------------------------
    // 9. Execute Combat State Machine or Movement via Pure Gamepad
    // ------------------------------------------------------------------------
    static uint32_t s_noEnemyFrames   = 0;
    static uint8_t  s_lastShotCounter = 0;

    uint32_t shootModeBtn = ReadCtrlActionBtn(ctrl, 0x1E0, 2); // Default Y (Enter/Exit Shoot Mode)
    uint32_t nextWeapBtn  = ReadCtrlActionBtn(ctrl, 0x1E4, 3); // Default Black (Cycle Weapon)
    uint32_t meleeBtn     = ReadCtrlActionBtn(ctrl, 0x1EC, 1); // Default X (Melee Attack)
    uint32_t fireBtn      = ReadCtrlActionBtn(ctrl, 0x1A4, 7); // Default R-Trigger (Fire)
    uint8_t  ctrlState    = IsValidPtr(ctrl, 0x60) ? ctrl[0x5E] : 0;

    void* pLaserSight = IsValidPtr(player, 0x150) ? s_fnFindAttachmentByType(player, nullptr, 3) : nullptr;
    bool  laserLocked = false;

    if (handledByInteract) {
        // Interaction handled steering/buttons for this tick; proceed to sampling & step check
    } else if (bestEnemy) {
        s_botState      = BOT_STATE_COMBAT;
        s_combatFrames++;
        s_noEnemyFrames = 0;

        const char* enemyName   = SafeGetThingName(bestEnemy);
        uint32_t    enemyGuid   = *reinterpret_cast<const uint32_t*>(bestEnemy + 0x34);
        float       enemyPos[3] = {};
        GetThingWorldPos(bestEnemy, enemyPos);
        EnemyTacticalEntry* te  = FindOrCreateEnemyTactic(enemyGuid, enemyName, enemyPos);
        if (s_combatFrames == 1) te->timesEncountered++;

        bool useMeleeMode = (activeStance == STANCE_MELEE_RUSH);
        bool useBossKite  = (activeStance == STANCE_BOSS_KITE || te->policy == POLICY_BOSS_KITE);

        float muzzlePos[3] = {};
        float barrelFwd[3] = {};
        GetPlayerMuzzlePos(player, eyePos, muzzlePos, barrelFwd);

        float yawErr = 0.0f;
        GetEnemyTargetPos(bestEnemy, s_cachedEnemyTargetPos);
        AimCameraTowardPoint(cam, ctrl, muzzlePos, s_cachedEnemyTargetPos, 30.0f, 22.0f, &yawErr);

        float aimDx  = s_cachedEnemyTargetPos[0] - muzzlePos[0];
        float aimDy  = s_cachedEnemyTargetPos[1] - muzzlePos[1];
        float aimDz  = s_cachedEnemyTargetPos[2] - muzzlePos[2];
        float aimLen = sqrtf(aimDx * aimDx + aimDy * aimDy + aimDz * aimDz);
        float barrelDot = 1.0f;
        if (aimLen > 0.001f) {
            barrelDot = (barrelFwd[0] * aimDx + barrelFwd[1] * aimDy + barrelFwd[2] * aimDz) / aimLen;
        }

        if (useMeleeMode && !useBossKite) {
            if ((ctrlState == 0x1B || ctrlState == 0x1C) && (s_botFrame % 10) == 0) {
                PressStdInputButton(shootModeBtn); // Exit Shoot Mode before melee
            }
            const float* moveTgt = (s_cachedEnemyDist <= 3.5f && fabsf(playerPos[2] - enemyPos[2]) < 1.5f) ? enemyPos : nextMoveTarget;
            MovePlayerTowardPoint(player, ctrl, cam, playerPos, moveTgt);

            if (s_cachedEnemyDist <= 2.40f && (s_botFrame - s_lastMeleeFrame) >= 14) {
                PressStdInputButton(meleeBtn); // Press X (Melee Attack)
                s_lastMeleeFrame = s_botFrame;
            }
        } else {
            uint32_t weaponMode = *reinterpret_cast<const uint32_t*>(player + 0x454);
            if (weaponMode != 3 && (s_botFrame % 15) == 0) {
                PressStdInputButton(nextWeapBtn); // Press Black to select Gauss Rifle
            }
            if ((ctrlState == 2 || ctrlState == 3 || ctrlState == 4) && (s_botFrame % 12) == 0) {
                PressStdInputButton(shootModeBtn); // Press Y to enter Z-Target Shoot Mode (0x1B)
            }

            // Clear any stale 3-second LOS cooldown on bestEnemy ([enemy + 0x408]) so TargetSelectGauss selects it immediately
            if (bestEnemyHasClearLOS && IsValidPtr(bestEnemy, 0x410)) {
                *reinterpret_cast<uint32_t*>(const_cast<uint8_t*>(bestEnemy) + 0x408) = 0;
            }

            // Lock Gauss Rifle target GUID, HUD red reticle GUID (0x0087B0C8), and laser sight onto the enemy's live 3D position
            if (IsValidPtr(ctrl, 0x3F0) && (ctrlState == 0x1B || ctrlState == 0x1C) && yawErr < 6.0f &&
                (bestEnemyHasClearLOS || forcedTargetEnemy != 0))
            {
                uint8_t* mutCtrl = const_cast<uint8_t*>(ctrl);
                *reinterpret_cast<uint32_t*>(mutCtrl + 0x234) = enemyGuid;
                *reinterpret_cast<uint32_t*>(mutCtrl + 0x3D8) = enemyGuid;
                *reinterpret_cast<float*>(mutCtrl + 0x240)    = s_cachedEnemyTargetPos[0];
                *reinterpret_cast<float*>(mutCtrl + 0x244)    = s_cachedEnemyTargetPos[1];
                *reinterpret_cast<float*>(mutCtrl + 0x248)    = s_cachedEnemyTargetPos[2];
                *reinterpret_cast<float*>(mutCtrl + 0x278)    = 0.0f; // Zero Gauss Rifle spread cone
                *reinterpret_cast<uint32_t*>(0x0087B0C8)      = enemyGuid; // Turn HUD crosshair RED & draw enemy brackets
                if (IsValidPtr(pLaserSight, 0x130)) {
                    s_fnLaserSightSetTarget(pLaserSight, nullptr, s_cachedEnemyTargetPos, 1);
                    laserLocked = true;
                }
            }

            // Fire only when in Shoot Mode (0x1B / 0x1C), camera is converged (yawErr < 4.5 deg),
            // and Nova's rifle barrel bone has turned within the 60-degree fire cone (barrelDot > 0.52f)
            if ((ctrlState == 0x1B || ctrlState == 0x1C) &&
                (bestEnemyHasClearLOS || forcedTargetEnemy != 0) &&
                yawErr < 4.5f && barrelDot > 0.52f && (s_combatFrames % 7) < 5)
            {
                PressStdInputButton(fireBtn); // R-Trigger burst fire
            }

            if (IsValidPtr(ctrl, 0x258)) {
                uint8_t curShotCounter = ctrl[0x228];
                if (curShotCounter > s_lastShotCounter) {
                    s_totalShotsFired += (curShotCounter - s_lastShotCounter);
                }
                s_lastShotCounter = curShotCounter;
            }

            // Stop advancing along the path when an enemy is engaged: stand ground and eliminate the enemy first!
            if (useBossKite && s_cachedEnemyDist < 6.5f) {
                s_botPad.sThumbLY = -28000;
                s_botPad.sThumbLX = ((s_combatFrames % 60) < 30) ? 24000 : -24000;
            } else if (s_cachedEnemyDist < 2.2f) {
                // Backpedal slightly only if a melee enemy rushes into point-blank range (< 2.2m)
                s_botPad.sThumbLX = 0;
                s_botPad.sThumbLY = -22000;
            } else {
                s_botPad.sThumbLX = 0;
                s_botPad.sThumbLY = 0;
            }
        }
    } else {
        s_botState        = BOT_STATE_NAVIGATE;
        s_combatFrames    = 0;
        s_lastShotCounter = IsValidPtr(ctrl, 0x258) ? ctrl[0x228] : 0;
        s_noEnemyFrames++;

        // Exit Z-Target Shoot Mode (0x1B / 0x1C) via Y button once out of combat for > 18 frames (0.3s)
        if (s_noEnemyFrames > 18 && (ctrlState == 0x1B || ctrlState == 0x1C) && (s_botFrame % 10) == 0) {
            PressStdInputButton(shootModeBtn);
        }

        if (s_botMode == BOT_MODE_TRAIN && s_curCmd.hasLookAt && !hasMoveGoal) {
            float yawErr = 0.0f;
            AimCameraTowardPoint(cam, ctrl, eyePos, s_curCmd.lookAt, 16.0f, 12.0f, &yawErr);
        } else if (hasMoveGoal) {
            // Look ahead 5.5m+ along the stage path so the camera stays steady down the corridor
            // instead of whipping sideways at close intermediate waypoints or bobbing with footsteps.
            float lookPt[3] = { nextMoveTarget[0], nextMoveTarget[1], nextMoveTarget[2] };
            if (s_botMode == BOT_MODE_NORMAL && s_normalRoutePlanned && s_normalStageIdx < s_normalRoutePlan.stageCount) {
                const LearnedRouteStage& st = s_normalRoutePlan.stages[s_normalStageIdx];
                for (uint32_t off = s_normalStageWpCursor; off < st.waypointCount; ++off) {
                    const float* wpPos = s_normalRoutePlan.waypoints[st.waypointStartIdx + off].pos;
                    lookPt[0] = wpPos[0];
                    lookPt[1] = wpPos[1];
                    lookPt[2] = wpPos[2];
                    if (Dist2D(playerPos, lookPt) >= 5.5f) break;
                }
            }
            float aimPt[3] = { lookPt[0], lookPt[1], eyePos[2] - 0.15f };
            if (fabsf(lookPt[2] - playerPos[2]) > 2.2f) {
                aimPt[2] = lookPt[2] + 1.2f;
            }
            if (s_botMode == BOT_MODE_TRAIN && s_curCmd.hasLookAt) {
                aimPt[0] = s_curCmd.lookAt[0];
                aimPt[1] = s_curCmd.lookAt[1];
                aimPt[2] = s_curCmd.lookAt[2];
            }
            bool ridingElevatorCabin = false;
            if (s_botMode == BOT_MODE_NORMAL && s_normalStageIdx < s_normalRoutePlan.stageCount) {
                const LearnedRouteStage& activeSt = s_normalRoutePlan.stages[s_normalStageIdx];
                if (activeSt.hasMechanismAction &&
                    strcmp(activeSt.mechanism.actionType, "RIDE_ELEVATOR") == 0 &&
                    GetSwitchActivationCount(activeSt.mechanism.guid) >= 1)
                {
                    ridingElevatorCabin = true;
                    // Look horizontally toward the exit corridor (the first waypoint of the next stage) while riding the elevator
                    if (s_normalStageIdx + 1 < s_normalRoutePlan.stageCount) {
                        const LearnedRouteStage& nextSt = s_normalRoutePlan.stages[s_normalStageIdx + 1];
                        if (nextSt.waypointCount > 0) {
                            const float* exitWp = s_normalRoutePlan.waypoints[nextSt.waypointStartIdx].pos;
                            aimPt[0] = exitWp[0];
                            aimPt[1] = exitWp[1];
                        }
                    }
                    aimPt[2] = eyePos[2] - 0.10f;
                }
            }
            float yawErr = 0.0f;
            if (!isClimbing && Dist2D(playerPos, aimPt) > 1.8f) {
                AimCameraTowardPoint(cam, ctrl, eyePos, aimPt, 16.0f, 10.0f, &yawErr);
            }
            if (ridingElevatorCabin && Dist2D(playerPos, nextMoveTarget) < 0.35f) {
                s_botPad.sThumbLX = 0;
                s_botPad.sThumbLY = 0;
            } else {
                MovePlayerTowardPoint(player, ctrl, cam, playerPos, nextMoveTarget);
            }
        }
    }

    if (!laserLocked && IsValidPtr(pLaserSight, 0x130)) {
        *reinterpret_cast<uint32_t*>(reinterpret_cast<uint8_t*>(pLaserSight) + 0x118) = 0;
    }

    // ------------------------------------------------------------------------
    // 10. Obstacle Detection, Fixed-Time-Interval Sampling & Step Completion
    // ------------------------------------------------------------------------
    static float    s_anchorPos[3]  = {};
    static uint32_t s_anchorTick    = 0;
    bool            isBlockedByWall = false;

    bool  justDidMelee        = ((s_botFrame - s_lastMeleeFrame) < 22);
    float elevSafeCenter[3]   = {};
    bool  inElevatorCabin     = IsPlayerInsideElevatorCabin(playerPos, elevSafeCenter);
    if (inElevatorCabin && ctrlState == 0x07 && Dist2D(playerPos, elevSafeCenter) > 0.20f) {
        float* mutPos = reinterpret_cast<float*>(player + 0xB0);
        mutPos[0]    = elevSafeCenter[0];
        mutPos[1]    = elevSafeCenter[1];
        playerPos[0] = elevSafeCenter[0];
        playerPos[1] = elevSafeCenter[1];
        uint8_t* phys = *reinterpret_cast<uint8_t* const*>(player + 0x168);
        if (IsValidPtr(phys, 0x20)) {
            uint8_t* hkrb = *reinterpret_cast<uint8_t* const*>(phys + 0x08);
            if (IsValidPtr(hkrb, 0x20)) {
                uint8_t* hkmot = *reinterpret_cast<uint8_t* const*>(hkrb + 0x08);
                if (IsValidPtr(hkmot, 0x90)) {
                    *reinterpret_cast<float*>(hkmot + 0x60) = elevSafeCenter[0];
                    *reinterpret_cast<float*>(hkmot + 0x64) = elevSafeCenter[1];
                    *reinterpret_cast<float*>(hkmot + 0x70) = elevSafeCenter[0];
                    *reinterpret_cast<float*>(hkmot + 0x74) = elevSafeCenter[1];
                }
            }
        }
    }
    bool inInteractAnim  = (ctrlState == 0x21 || ctrlState == 0x33 || ctrlState == 0x37);
    if (hasMoveGoal && !isClimbing && !inElevatorCabin && !inInteractAnim && bestEnemy == nullptr &&
        (s_botMode == BOT_MODE_NORMAL || s_cmdElapsedFrames > 2) && !justDidMelee) {
        if (Dist3D(playerPos, s_anchorPos) > 0.55f) {
            s_anchorPos[0] = playerPos[0];
            s_anchorPos[1] = playerPos[1];
            s_anchorPos[2] = playerPos[2];
            s_anchorTick   = s_activeGameTicks;
        } else if (s_activeGameTicks - s_anchorTick > (handledByInteract ? 180u : 45u) &&
                   (Dist2D(playerPos, s_goalPos) > 1.4f || handledByInteract))
        {
            isBlockedByWall = true;
            if (s_botFrame >= s_unstuckUntilFrame) {
                s_unstuckUntilFrame = s_botFrame + 24;
                s_unstuckDir        = -s_unstuckDir;
                LogRawReplayEvent("OBSTACLE_BLOCKED", playerPos, "\"reason\":\"stuck_on_geometry\"");
            }
            if (s_botFrame < s_unstuckUntilFrame) {
                if ((ctrlState == 0x1B || ctrlState == 0x1C) && (s_botFrame % 6) == 0) {
                    PressStdInputButton(shootModeBtn);
                }
                s_botPad.sThumbLX = (int16_t)(s_unstuckDir * 28000);
                s_botPad.sThumbLY = -20000;
            }
        }
    } else {
        s_anchorPos[0] = playerPos[0];
        s_anchorPos[1] = playerPos[1];
        s_anchorPos[2] = playerPos[2];
        s_anchorTick   = s_activeGameTicks;
    }

    if (s_botMode == BOT_MODE_NORMAL && (s_activeGameTicks % 300) == 0) {
        LogPrintf("[AutoBot-Progress] %s | Pos=(%.2f, %.2f, %.2f) ctrl=0x%X Kills=%u Shots=%u",
            s_goalDesc, playerPos[0], playerPos[1], playerPos[2], ctrlState, s_killCount, s_totalShotsFired);
    }

    float curYaw   = IsValidPtr(cam, 0x250) ? *reinterpret_cast<const float*>(cam + 0x20C) : 0.0f;
    float curPitch = IsValidPtr(cam, 0x250) ? *reinterpret_cast<const float*>(cam + 0x204) : 0.0f;
    uint32_t curAmmo = *reinterpret_cast<const uint32_t*>(player + 0x438);
    uint8_t  ctrlSt  = IsValidPtr(ctrl, 0x60) ? ctrl[0x5E] : 0;

    // Record Fixed-Time-Interval Sample (every 12 active ticks = 0.20s)
    RecordFixedTimeIntervalSample(playerPos, curYaw, curPitch, *pHealth, curAmmo, ctrlSt, isBlockedByWall);

    // In TRAIN Mode, check if the current multimodal command burst has finished or hit an event
    if (s_botMode == BOT_MODE_TRAIN && !s_waitingForLlm) {
        size_t numWps = s_curCmd.waypointsXYZ.size() / 3;
        bool allWpsReached = (numWps > 0 && s_cmdWaypointCursor >= numWps);
        bool interactDone  = false;
        if (IsRideElevatorMechanism(s_curCmd.interactGuid)) {
            float destZ = GetElevatorDestinationZ(s_curCmd.interactGuid, playerPos[2]);
            interactDone = (GetSwitchActivationCount(s_curCmd.interactGuid) > s_cmdInitialInteractCount &&
                            fabsf(playerPos[2] - destZ) < 2.0f);
        } else if (s_curCmd.interactGuid != 0) {
            interactDone = (GetSwitchActivationCount(s_curCmd.interactGuid) > s_cmdInitialInteractCount);
        }
        bool targetEnemyDead = false;
        if (s_curCmd.targetEnemyGuid != 0 && s_cmdElapsedFrames >= 4) {
            void* tePtr = s_fnFindGUID(s_curCmd.targetEnemyGuid);
            if (!IsValidReadablePtr(tePtr, 0x374) ||
                *reinterpret_cast<const float*>(reinterpret_cast<const uint8_t*>(tePtr) + 0x36C) <= 0.0f ||
                s_fnIsValidAssistTarget(tePtr) == 0) {
                targetEnemyDead = true;
            }
        }
        bool timeExpired   = (s_cmdElapsedFrames >= s_curCmd.maxFrames);
        bool blockedStop   = (isBlockedByWall && (s_activeGameTicks - s_anchorTick) > 75 && s_cmdElapsedFrames > 75);

        bool wpsAndTasksDone = allWpsReached &&
                               (s_curCmd.interactGuid == 0 || interactDone) &&
                               (s_curCmd.targetEnemyGuid == 0 || targetEnemyDead);
        bool standaloneTaskDone = (numWps == 0) &&
                                  ((s_curCmd.interactGuid != 0 && interactDone) ||
                                   (s_curCmd.targetEnemyGuid != 0 && targetEnemyDead));

        if (wpsAndTasksDone || standaloneTaskDone || timeExpired || blockedStop) {
            if (wpsAndTasksDone && (interactDone || targetEnemyDead)) strcpy(s_lastCmdResult, "WAYPOINTS_AND_TASK_COMPLETED");
            else if (wpsAndTasksDone)                                 strcpy(s_lastCmdResult, "REACHED_WAYPOINTS");
            else if (standaloneTaskDone && targetEnemyDead)           strcpy(s_lastCmdResult, "TARGET_ENEMY_KILLED");
            else if (standaloneTaskDone && interactDone)              strcpy(s_lastCmdResult, "INTERACT_COMPLETED");
            else if (blockedStop)                                     strcpy(s_lastCmdResult, "BLOCKED_BY_OBSTACLE");
            else                                                      strcpy(s_lastCmdResult, "DURATION_COMPLETED");

            s_waitingForLlm               = true;
            s_stateExportedForCurrentWait = false;
            s_waitLlmStartFrame           = s_botFrame;
            s_waitLlmFrozenHp             = *pHealth;
            RequestBackbufferScreenshot("autobot_io\\vision_sheet.png");
            SaveRawWalkedGraphSnapshot("STEP_PAUSE");
            LogPrintf("[AutoBot-Train] Cmd #%u finished (%s) at Pos=(%.2f, %.2f, %.2f) HP=%.1f Ammo=%u -> capturing vision_sheet.png...",
                s_curCmd.cmdId, s_lastCmdResult, playerPos[0], playerPos[1], playerPos[2], *pHealth, curAmmo);
        }
    }

    snprintf(s_statusText, sizeof(s_statusText),
        "AI Bot [%s]: %s | Pos(%.0f,%.0f,%.0f) HP:%.0f Ammo:%u Kills:%u",
        (s_botMode == BOT_MODE_TRAIN) ? "TRAIN" : "NORMAL",
        s_goalDesc, playerPos[0], playerPos[1], playerPos[2], *pHealth, curAmmo, s_killCount);
}

bool ApplyAutoBotInput(void* pXboxInputState) {
    if (!g_autoBotEnabled || !pXboxInputState) return false;
    BotInputState* st = reinterpret_cast<BotInputState*>(pXboxInputState);
    st->Gamepad = s_botPad;
    return true;
}

const char* GetAutoBotStatusText() {
    return s_statusText;
}
