#include "ghost_runtime.h"
#include <direct.h>

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

void EnsureDirExists(const char* dirPath) {
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
