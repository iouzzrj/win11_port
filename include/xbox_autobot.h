#pragma once

#include "ghost_runtime.h"

// ============================================================================
// Self-Contained Learned Level Navigation & Mechanism Planner
// (Isolated in xbox_autobot.h so platform files never depend on bot internals)
// ============================================================================

static const uint32_t MECH_STATE_AUTO_LIVE = 0xFFFFFFFFu;

struct LearnedRouteWaypoint {
    int   nodeId;           // NAV_NODE id (0 for off-graph startPos, -1 for off-graph endPos)
    float pos[3];           // 3D world position (x, y, z)
    char  moveType[16];     // "START", "WALK", "DROP", "JUMP", "LADDER", "ELEVATOR", "GOAL"
    float segmentCostSec;   // Estimated traversal time (seconds) for this step
    int   triggerMechId;    // >0 if a mechanism must be solved upon reaching this waypoint
};

struct LearnedMechanismStep {
    int      mechId;          // Mechanism sequence ID (1..N)
    int      nodeId;          // Associated NAV_NODE ID on the learned graph
    uint32_t guid;            // In-game object/script GUID (e.g., 0x222, 0x2F8, 0x259)
    int      prereqMechId;    // Prerequisite mechanism ID (0 if none)
    bool     isAlreadySolved; // True if already solved in live world / state mask
    char     actionType[24];  // "OPEN_SWITCH", "SHOOT_OBSTACLE", "CLIMB_LADDER", "HACK_TERMINAL", "CALL_ELEVATOR", "RIDE_ELEVATOR", "ENTER_REGION", "CALLDOWN_TARGET"
    float    pos[3];          // 3D position of the mechanism
    char     howToSolve[256]; // Detailed cracking/solving method for Nova
};

struct LearnedRouteStage {
    int                  stageIndex;          // 1-based stage index (Stage 1: Start->Mech1, Stage 2: Mech1->Mech2, ..., Final: ->End)
    float                fromPos[3];          // Start 3D position of this stage
    float                toPos[3];            // Target 3D position of this stage
    int                  fromNodeId;          // Start NAV_NODE ID for this stage
    int                  toNodeId;            // Target NAV_NODE ID for this stage
    float                stageDistanceMeters; // 3D length of this stage's sub-path (meters)
    float                stageCostSec;        // Estimated traversal time for this stage (seconds)
    uint32_t             waypointStartIdx;    // Start index in LearnedRoutePlan::waypoints[]
    uint32_t             waypointCount;       // Number of waypoints in this stage's sub-path
    bool                 hasMechanismAction;  // True if a mechanism must be cracked at the end of this stage
    LearnedMechanismStep mechanism;           // Mechanism to crack at the end of this stage (when hasMechanismAction == true)
};

struct LearnedRoutePlan {
    int               levelId;                // 1 (Mission 1: 1_3_1b_Hive_StationOuter.nil) or 2 (Mission 2: 1_2_1_Miners_Bunker.nil)
    char              levelName[64];          // Resolved level filename
    float             startPos[3];            // Input start 3D position
    float             endPos[3];              // Input end 3D position
    int               startNodeId;            // Anchored start NAV_NODE ID
    int               endNodeId;              // Anchored end NAV_NODE ID
    uint32_t          solvedMechMaskUsed;     // Bitmask (bit m = mechId m) of mechanisms that were already solved at planning time
    float             totalDistanceMeters;    // Total 3D route length across all stages (meters)
    float             totalCostSec;           // Total estimated traversal time across all stages (seconds)
    uint32_t          stageCount;             // Number of decomposed stages (Start->Mech1, Mech1->Mech2, ..., ->End)
    LearnedRouteStage stages[24];             // Ordered stage-by-stage breakdown
    uint32_t          waypointCount;
    LearnedRouteWaypoint waypoints[256];
    uint32_t          mechanismCount;
    LearnedMechanismStep mechanisms[32];
};

void RequestBackbufferScreenshot(const char* filename);

uint32_t QueryLiveSolvedMechanismMask(int levelId, const float currentPlayerPos[3] = nullptr);

bool PlanLearnedLevelRoute(
    int levelId,
    const float startPos[3],
    const float endPos[3],
    LearnedRoutePlan* outPlan,
    uint32_t solvedMechMaskOverride = MECH_STATE_AUTO_LIVE);

bool PlanLearnedLevelRoute(
    const char* levelIdOrName,
    const float startPos[3],
    const float endPos[3],
    LearnedRoutePlan* outPlan,
    uint32_t solvedMechMaskOverride = MECH_STATE_AUTO_LIVE);

void DumpLearnedRoutePlan(
    const LearnedRoutePlan& plan,
    const char* optionalOutFilePath = nullptr);
