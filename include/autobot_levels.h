#pragma once
#include <stdint.h>
#include <stddef.h>

// ============================================================================
// StarCraft: Ghost — Per-Level Ordered Mechanism Sequences
// Each level lists ONLY the ordered mechanism GUIDs to trigger/reach.
// Coordinates, facing normals, mechanism types (cButton / cConsole / cLadder /
// tRegion / destructible), completion states, and navigation paths are all
// queried dynamically from live game memory (tWorld) at runtime.
// ============================================================================

struct AutoBotLevelMechTable {
    const char*     levelName;
    const uint16_t* mechGuids;
    size_t          mechCount;
};

// [1_3_1b_Hive_StationOuter.nil]
static constexpr uint16_t kLevel_1_3_1b_Mechs[] = {
    0x14C, // 1. 出生点房门开关
    0x127, // 2. 警卫亭外门开关
    0x12C, // 3. 警卫亭内机库总开关
    0x12E, // 4. 警卫亭内门开关
    0x32E, // 5. 机库东侧维修门开关
    0x2F8, // 6. 下层隧道爆炸油桶
    0x232, // 7. 西侧发电机房长梯
    0x279, // 8. 发电机控制室门开关
    0x222, // 9. 发电机终端破解 (Stage 1)
    0x2EB, // 10. 发电机控制室东门开关
    0x226, // 11. 主机终端破解 (Stage 2)
    0x225, // 12. 呼叫主电梯开关
    0x259, // 13. 乘主电梯下行至钻井层
    0x1D5, // 14. 钻井终端 1 破解
    0x1D6, // 15. 钻井终端 2 破解
    0x1D8, // 16. 钻井终端 3 破解
    0x1D7, // 17. 钻井终端 4 破解
    0x261, // 18. 乘主电梯上行返回地面
    0x288, // 19. 北侧撤离区
};

static constexpr AutoBotLevelMechTable kAutoBotLevelTables[] = {
    { "1_3_1b_Hive_StationOuter.nil", kLevel_1_3_1b_Mechs, sizeof(kLevel_1_3_1b_Mechs) / sizeof(kLevel_1_3_1b_Mechs[0]) },
};
