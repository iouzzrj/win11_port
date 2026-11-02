#include "ghost_bot.h"
#include "xbox_d3d8_types.h"
#include <d3d11.h>
#include <dxgi.h>
#include <wincodec.h>
#include <ctype.h>
#include <string>
#include <vector>

// ============================================================================
// Standalone Automated Testing & In-Game Bot Implementation
// ============================================================================

enum class BotCmdType {
    SkipVideos,
    WaitInGame,
    WaitMs,
    WaitFrames,
    PressButton,
    StickMove,
    Screenshot,
    ExitApp,
};

struct BotButtonMask {
    uint16_t wButtons;
    uint8_t  analog[8]; // 0=A, 1=B, 2=X, 3=Y, 4=Black, 5=White, 6=LTrig, 7=RTrig
};

struct BotCommand {
    BotCmdType    type       = BotCmdType::WaitMs;
    uint32_t      durationMs = 0;
    uint32_t      afterMs    = 0;
    uint32_t      frameCount = 0;
    BotButtonMask btnMask    = {};
    float         lx         = 0.0f;
    float         ly         = 0.0f;
    float         rx         = 0.0f;
    float         ry         = 0.0f;
    std::string   filePath;
};

static std::vector<BotCommand> s_botCmds;
static size_t                  s_curCmdIdx               = 0;
static bool                    s_botActive               = false;
static bool                    s_cmdStarted              = false;
static DWORD                   s_cmdStartTick            = 0;
static DWORD                   s_skipSettleStartTick     = 0;
static bool                    s_skipReached3D           = false;
static uint32_t                s_cmdStartFrame           = 0;
static volatile uint32_t       s_totalVideoFrames        = 0;
static volatile uint32_t       s_total3dSwapFrames       = 0;
static volatile uint32_t       s_3dFramesSinceLastVideo  = 0;

static BotButtonMask           s_activeBtnMask           = {};
static int16_t                 s_activeThumbLX           = 0;
static int16_t                 s_activeThumbLY           = 0;
static int16_t                 s_activeThumbRX           = 0;
static int16_t                 s_activeThumbRY           = 0;

static void ClearActiveBotInputs() {
    memset(&s_activeBtnMask, 0, sizeof(s_activeBtnMask));
    s_activeThumbLX = 0;
    s_activeThumbLY = 0;
    s_activeThumbRX = 0;
    s_activeThumbRY = 0;
}

static std::string TrimStr(const std::string& s) {
    size_t a = 0;
    while (a < s.size() && (unsigned char)s[a] <= ' ') ++a;
    size_t b = s.size();
    while (b > a && (unsigned char)s[b - 1] <= ' ') --b;
    return s.substr(a, b - a);
}

static std::string ToLowerStr(std::string s) {
    for (char& c : s) {
        if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
    }
    return s;
}

static BotButtonMask ParseButtonToken(const std::string& rawToken) {
    BotButtonMask m = {};
    std::string tok = ToLowerStr(rawToken);
    size_t pos = 0;
    while (pos < tok.size()) {
        size_t plus = tok.find('+', pos);
        std::string part = (plus == std::string::npos) ? tok.substr(pos) : tok.substr(pos, plus - pos);
        part = TrimStr(part);
        if (part == "up" || part == "dpad_up" || part == "sight") {
            m.wButtons |= 0x0001u;
        } else if (part == "down" || part == "dpad_down" || part == "light") {
            m.wButtons |= 0x0002u;
        } else if (part == "left" || part == "dpad_left" || part == "cloak") {
            m.wButtons |= 0x0004u;
        } else if (part == "right" || part == "dpad_right") {
            m.wButtons |= 0x0008u;
        } else if (part == "start" || part == "enter") {
            m.wButtons |= 0x0010u;
        } else if (part == "back" || part == "tab" || part == "esc") {
            m.wButtons |= 0x0020u;
        } else if (part == "lthumb") {
            m.wButtons |= 0x0040u;
        } else if (part == "rthumb" || part == "sniper" || part == "zoom") {
            m.wButtons |= 0x0080u;
        } else if (part == "a" || part == "jump" || part == "space") {
            m.analog[0] = 255u;
        } else if (part == "b" || part == "action" || part == "use") {
            m.analog[1] = 255u;
        } else if (part == "x" || part == "melee" || part == "reload") {
            m.analog[2] = 255u;
        } else if (part == "y" || part == "weapon" || part == "mode") {
            m.analog[3] = 255u;
        } else if (part == "black" || part == "nextgun") {
            m.analog[4] = 255u;
        } else if (part == "white" || part == "grenade") {
            m.analog[5] = 255u;
        } else if (part == "ltrig" || part == "lt" || part == "crouch") {
            m.analog[6] = 255u;
        } else if (part == "rtrig" || part == "rt" || part == "fire" || part == "shoot") {
            m.analog[7] = 255u;
        }
        if (plus == std::string::npos) break;
        pos = plus + 1;
    }
    return m;
}

static std::vector<std::string> SplitWords(const std::string& line) {
    std::vector<std::string> words;
    size_t i = 0;
    while (i < line.size()) {
        while (i < line.size() && (unsigned char)line[i] <= ' ') ++i;
        if (i >= line.size()) break;
        if (line[i] == '"') {
            ++i;
            size_t start = i;
            while (i < line.size() && line[i] != '"') ++i;
            words.push_back(line.substr(start, i - start));
            if (i < line.size() && line[i] == '"') ++i;
        } else {
            size_t start = i;
            while (i < line.size() && (unsigned char)line[i] > ' ') ++i;
            words.push_back(line.substr(start, i - start));
        }
    }
    return words;
}

static void ParseBotScriptText(const std::string& text) {
    size_t start = 0;
    while (start < text.size()) {
        size_t end = text.find_first_of(";\r\n", start);
        std::string stmt = (end == std::string::npos) ? text.substr(start) : text.substr(start, end - start);
        start = (end == std::string::npos) ? text.size() : (end + 1);

        size_t hashPos = stmt.find('#');
        if (hashPos != std::string::npos) {
            stmt = stmt.substr(0, hashPos);
        }
        stmt = TrimStr(stmt);
        if (stmt.empty()) continue;

        std::vector<std::string> w = SplitWords(stmt);
        if (w.empty()) continue;
        std::string op = ToLowerStr(w[0]);

        if (op == "skip_videos" || op == "skip_logos" || op == "wait_title" || op == "wait_menu") {
            BotCommand cmd = {};
            cmd.type       = BotCmdType::SkipVideos;
            cmd.durationMs = (w.size() >= 2) ? (uint32_t)strtoul(w[1].c_str(), nullptr, 10) : 2500u;
            s_botCmds.push_back(cmd);
        } else if (op == "wait_ingame") {
            BotCommand cmd = {};
            cmd.type       = BotCmdType::WaitInGame;
            cmd.durationMs = (w.size() >= 2) ? (uint32_t)strtoul(w[1].c_str(), nullptr, 10) : 1500u;
            s_botCmds.push_back(cmd);
        } else if (op == "wait" || op == "sleep") {
            BotCommand cmd = {};
            cmd.type       = BotCmdType::WaitMs;
            cmd.durationMs = (w.size() >= 2) ? (uint32_t)strtoul(w[1].c_str(), nullptr, 10) : 1000u;
            s_botCmds.push_back(cmd);
        } else if (op == "wait_frames") {
            BotCommand cmd = {};
            cmd.type       = BotCmdType::WaitFrames;
            cmd.frameCount = (w.size() >= 2) ? (uint32_t)strtoul(w[1].c_str(), nullptr, 10) : 60u;
            s_botCmds.push_back(cmd);
        } else if (op == "press") {
            if (w.size() >= 2) {
                BotCommand cmd = {};
                cmd.type       = BotCmdType::PressButton;
                cmd.btnMask    = ParseButtonToken(w[1]);
                cmd.durationMs = (w.size() >= 3) ? (uint32_t)strtoul(w[2].c_str(), nullptr, 10) : 100u;
                cmd.afterMs    = (w.size() >= 4) ? (uint32_t)strtoul(w[3].c_str(), nullptr, 10) : 200u;
                s_botCmds.push_back(cmd);
            }
        } else if (op == "stick" || op == "move") {
            if (w.size() >= 6) {
                BotCommand cmd = {};
                cmd.type       = BotCmdType::StickMove;
                cmd.lx         = (float)atof(w[1].c_str());
                cmd.ly         = (float)atof(w[2].c_str());
                cmd.rx         = (float)atof(w[3].c_str());
                cmd.ry         = (float)atof(w[4].c_str());
                cmd.durationMs = (uint32_t)strtoul(w[5].c_str(), nullptr, 10);
                if (w.size() >= 7) {
                    cmd.btnMask = ParseButtonToken(w[6]);
                }
                s_botCmds.push_back(cmd);
            }
        } else if (op == "walk_forward") {
            BotCommand cmd = {};
            cmd.type       = BotCmdType::StickMove;
            cmd.ly         = 1.0f;
            cmd.durationMs = (w.size() >= 2) ? (uint32_t)strtoul(w[1].c_str(), nullptr, 10) : 1000u;
            s_botCmds.push_back(cmd);
        } else if (op == "walk_back") {
            BotCommand cmd = {};
            cmd.type       = BotCmdType::StickMove;
            cmd.ly         = -1.0f;
            cmd.durationMs = (w.size() >= 2) ? (uint32_t)strtoul(w[1].c_str(), nullptr, 10) : 1000u;
            s_botCmds.push_back(cmd);
        } else if (op == "strafe_left") {
            BotCommand cmd = {};
            cmd.type       = BotCmdType::StickMove;
            cmd.lx         = -1.0f;
            cmd.durationMs = (w.size() >= 2) ? (uint32_t)strtoul(w[1].c_str(), nullptr, 10) : 1000u;
            s_botCmds.push_back(cmd);
        } else if (op == "strafe_right") {
            BotCommand cmd = {};
            cmd.type       = BotCmdType::StickMove;
            cmd.lx         = 1.0f;
            cmd.durationMs = (w.size() >= 2) ? (uint32_t)strtoul(w[1].c_str(), nullptr, 10) : 1000u;
            s_botCmds.push_back(cmd);
        } else if (op == "turn_left") {
            BotCommand cmd = {};
            cmd.type       = BotCmdType::StickMove;
            cmd.rx         = -1.0f;
            cmd.durationMs = (w.size() >= 2) ? (uint32_t)strtoul(w[1].c_str(), nullptr, 10) : 500u;
            s_botCmds.push_back(cmd);
        } else if (op == "turn_right") {
            BotCommand cmd = {};
            cmd.type       = BotCmdType::StickMove;
            cmd.rx         = 1.0f;
            cmd.durationMs = (w.size() >= 2) ? (uint32_t)strtoul(w[1].c_str(), nullptr, 10) : 500u;
            s_botCmds.push_back(cmd);
        } else if (op == "screenshot" || op == "capture") {
            if (w.size() >= 2) {
                BotCommand cmd = {};
                cmd.type     = BotCmdType::Screenshot;
                cmd.filePath = w[1];
                s_botCmds.push_back(cmd);
            }
        } else if (op == "exit" || op == "quit") {
            BotCommand cmd = {};
            cmd.type = BotCmdType::ExitApp;
            s_botCmds.push_back(cmd);
        }
    }
}

static bool ExtractOptionValue(const char* cmdLine, const char* prefix, std::string* outVal) {
    const char* p = strstr(cmdLine, prefix);
    if (!p) return false;
    bool quotedBeforePrefix = (p > cmdLine && *(p - 1) == '"');
    p += strlen(prefix);
    if (*p == '"') {
        ++p;
        const char* end = strchr(p, '"');
        *outVal = end ? std::string(p, (size_t)(end - p)) : std::string(p);
    } else if (quotedBeforePrefix) {
        const char* end = strchr(p, '"');
        *outVal = end ? std::string(p, (size_t)(end - p)) : std::string(p);
    } else {
        const char* end = p;
        while (*end && (unsigned char)*end > ' ') ++end;
        *outVal = std::string(p, (size_t)(end - p));
    }
    return true;
}

void GhostBot_InitFromCommandLine(const char* cmdLine) {
    s_botCmds.clear();
    s_curCmdIdx  = 0;
    s_botActive  = false;
    s_cmdStarted = false;
    if (!cmdLine || !cmdLine[0]) return;

    std::string scriptPath;
    if (ExtractOptionValue(cmdLine, "--bot-script=", &scriptPath) && !scriptPath.empty()) {
        FILE* f = fopen(scriptPath.c_str(), "rb");
        if (f) {
            fseek(f, 0, SEEK_END);
            long sz = ftell(f);
            fseek(f, 0, SEEK_SET);
            if (sz > 0) {
                std::string buf((size_t)sz, '\0');
                fread(&buf[0], 1, (size_t)sz, f);
                ParseBotScriptText(buf);
            }
            fclose(f);
        }
    }

    std::string inlineBot;
    if (ExtractOptionValue(cmdLine, "--bot=", &inlineBot) && !inlineBot.empty()) {
        ParseBotScriptText(inlineBot);
    }

    if (!s_botCmds.empty()) {
        s_botActive = true;
    }
}

bool GhostBot_IsActive() {
    return s_botActive;
}

bool GhostBot_AutoConfigureLauncher() {
    wchar_t userDataDirW[MAX_PATH] = {};
    wchar_t iniPathW[MAX_PATH]     = {};
    GetAppDataAndIniPathsW(userDataDirW, MAX_PATH, iniPathW, MAX_PATH);
    LoadKeyBindingsFromIniW(iniPathW);

    wchar_t gameDirW[MAX_PATH] = {};
    if (iniPathW[0] && GetFileAttributesW(iniPathW) != INVALID_FILE_ATTRIBUTES) {
        GetPrivateProfileStringW(L"Paths", L"GameDir", L"", gameDirW, MAX_PATH, iniPathW);
        TrimTrailingSlashesW(gameDirW);
    }

    if (!IsValidGameDirW(gameDirW)) {
        wchar_t exeDirW[MAX_PATH] = {};
        GetExeDirW(exeDirW, MAX_PATH);
        const wchar_t* relCandidates[] = {
            L"",
            L"..\\StarCraft Ghost Xbox Finn Hillbilly",
            L"..\\..\\StarCraft Ghost Xbox Finn Hillbilly",
            L"..\\..\\..\\StarCraft Ghost Xbox Finn Hillbilly",
        };
        for (const wchar_t* rel : relCandidates) {
            wchar_t candW[MAX_PATH] = {};
            if (rel[0] == L'\0') {
                wcsncpy(candW, exeDirW, MAX_PATH - 1);
            } else {
                JoinPathW(candW, MAX_PATH, exeDirW, rel);
            }
            wchar_t fullW[MAX_PATH] = {};
            if (GetFullPathNameW(candW, MAX_PATH, fullW, nullptr) && IsValidGameDirW(fullW)) {
                wcsncpy(gameDirW, fullW, MAX_PATH - 1);
                break;
            }
        }
    }

    if (!IsValidGameDirW(gameDirW)) {
        return false;
    }

    // Select highest-VRAM discrete GPU or saved adapter from config.ini
    IDXGIFactory* pFactory = nullptr;
    if (SUCCEEDED(CreateDXGIFactory(__uuidof(IDXGIFactory), (void**)&pFactory)) && pFactory) {
        int    bestIdx  = 0;
        SIZE_T bestVram = 0;
        wchar_t bestName[128] = {};
        IDXGIAdapter* pAdapter = nullptr;
        for (UINT i = 0; pFactory->EnumAdapters(i, &pAdapter) != DXGI_ERROR_NOT_FOUND; ++i) {
            DXGI_ADAPTER_DESC desc = {};
            if (SUCCEEDED(pAdapter->GetDesc(&desc))) {
                bool isSw = (desc.VendorId == 0x1414 && desc.DeviceId == 0x8C) ||
                            (wcsstr(desc.Description, L"Basic Render") != nullptr);
                if (!isSw && desc.DedicatedVideoMemory > bestVram) {
                    bestVram = desc.DedicatedVideoMemory;
                    bestIdx  = (int)i;
                    wcsncpy(bestName, desc.Description, 127);
                }
            }
            pAdapter->Release();
        }
        pFactory->Release();
        g_selectedAdapterIndex = bestIdx;
        wcsncpy(g_selectedAdapterName, bestName, 127);
    }

    PersistAndInitUserDataDir(gameDirW);
    return true;
}

static bool SaveBackbufferToPng(const char* filePath) {
    if (!g_d3dDevice || !g_d3dCtx || !g_backBufferTex || !filePath || !filePath[0]) return false;

    D3D11_TEXTURE2D_DESC desc = {};
    g_backBufferTex->GetDesc(&desc);
    desc.Usage          = D3D11_USAGE_STAGING;
    desc.BindFlags      = 0;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    desc.MiscFlags      = 0;

    ID3D11Texture2D* stagingTex = nullptr;
    if (FAILED(g_d3dDevice->CreateTexture2D(&desc, nullptr, &stagingTex)) || !stagingTex) {
        return false;
    }

    g_d3dCtx->CopyResource(stagingTex, g_backBufferTex);

    D3D11_MAPPED_SUBRESOURCE mapped = {};
    if (FAILED(g_d3dCtx->Map(stagingTex, 0, D3D11_MAP_READ, 0, &mapped))) {
        stagingTex->Release();
        return false;
    }

    uint32_t w = desc.Width;
    uint32_t h = desc.Height;
    std::vector<uint32_t> opaquePixels(w * h);
    for (uint32_t y = 0; y < h; ++y) {
        const uint32_t* srcRow = reinterpret_cast<const uint32_t*>(
            reinterpret_cast<const uint8_t*>(mapped.pData) + y * mapped.RowPitch);
        uint32_t* dstRow = &opaquePixels[y * w];
        for (uint32_t x = 0; x < w; ++x) {
            dstRow[x] = srcRow[x] | 0xFF000000u; // Force opaque alpha=255
        }
    }
    g_d3dCtx->Unmap(stagingTex, 0);
    stagingTex->Release();

    HRESULT hrCo = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    bool needUninit = SUCCEEDED(hrCo);

    IWICImagingFactory* pFactory = nullptr;
    HRESULT hr = CoCreateInstance(
        CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
        IID_PPV_ARGS(&pFactory));
    if (FAILED(hr) || !pFactory) {
        if (needUninit) CoUninitialize();
        return false;
    }

    wchar_t pathW[MAX_PATH] = {};
    MultiByteToWideChar(CP_UTF8, 0, filePath, -1, pathW, MAX_PATH);
    if (!pathW[0]) {
        MultiByteToWideChar(CP_ACP, 0, filePath, -1, pathW, MAX_PATH);
    }

    IWICStream* pStream = nullptr;
    bool ok = false;
    if (SUCCEEDED(pFactory->CreateStream(&pStream)) && pStream) {
        if (SUCCEEDED(pStream->InitializeFromFilename(pathW, GENERIC_WRITE))) {
            IWICBitmapEncoder* pEncoder = nullptr;
            if (SUCCEEDED(pFactory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &pEncoder)) && pEncoder) {
                if (SUCCEEDED(pEncoder->Initialize(pStream, WICBitmapEncoderNoCache))) {
                    IWICBitmapFrameEncode* pFrame = nullptr;
                    if (SUCCEEDED(pEncoder->CreateNewFrame(&pFrame, nullptr)) && pFrame) {
                        if (SUCCEEDED(pFrame->Initialize(nullptr))) {
                            pFrame->SetSize(w, h);
                            WICPixelFormatGUID fmt = GUID_WICPixelFormat32bppBGRA;
                            pFrame->SetPixelFormat(&fmt);
                            if (SUCCEEDED(pFrame->WritePixels(
                                    h, w * 4, w * h * 4,
                                    reinterpret_cast<BYTE*>(opaquePixels.data()))))
                            {
                                if (SUCCEEDED(pFrame->Commit()) && SUCCEEDED(pEncoder->Commit())) {
                                    ok = true;
                                }
                            }
                        }
                        pFrame->Release();
                    }
                }
                pEncoder->Release();
            }
        }
        pStream->Release();
    }
    pFactory->Release();
    if (needUninit) CoUninitialize();
    return ok;
}

static int16_t FloatToThumbAxis(float v) {
    if (v < -1.0f) v = -1.0f;
    if (v >  1.0f) v =  1.0f;
    return (int16_t)(v * 32767.0f);
}

static void StepBotStateMachine(bool is3DSwapFrame) {
    if (!s_botActive || s_curCmdIdx >= s_botCmds.size()) {
        ClearActiveBotInputs();
        return;
    }

    DWORD now = GetTickCount();
    while (s_curCmdIdx < s_botCmds.size()) {
        const BotCommand& cmd = s_botCmds[s_curCmdIdx];
        if (!s_cmdStarted) {
            s_cmdStarted          = true;
            s_cmdStartTick        = now;
            s_cmdStartFrame       = s_total3dSwapFrames;
            s_skipReached3D       = false;
            s_skipSettleStartTick = 0;
            ClearActiveBotInputs();
        }

        switch (cmd.type) {
            case BotCmdType::SkipVideos: {
                // mainSequence.m_state at 0x00812DC8:
                //   1 = Cutscene (.vid playing: blizzard_logo.vid -> Nihilistic_Logo.vid)
                //   2 = Frontend (Nova 3D Title Screen / Main Menu)
                //   3 = In-Game Mission
                uint32_t seqState = *reinterpret_cast<const volatile uint32_t*>(0x00812DC8);
                if (!s_skipReached3D) {
                    if (seqState >= 2u) {
                        s_skipReached3D       = true;
                        s_skipSettleStartTick = now;
                        ClearActiveBotInputs();
                    } else {
                        // Pulse Start + A (100ms down, 150ms up) while in state 1 (startup videos)
                        ClearActiveBotInputs();
                        if (seqState == 1u) {
                            DWORD phase = (now - s_cmdStartTick) % 250u;
                            if (phase < 100u) {
                                s_activeBtnMask.wButtons  = 0x0010u; // Start
                                s_activeBtnMask.analog[0] = 255u;    // A
                            }
                        }
                        return;
                    }
                }
                ClearActiveBotInputs();
                if ((now - s_skipSettleStartTick) >= cmd.durationMs) {
                    s_curCmdIdx++;
                    s_cmdStarted = false;
                    continue;
                }
                return;
            }
            case BotCmdType::WaitInGame: {
                // Wait until mainSequence.m_state (0x00812DC8) == 3 (In-Game),
                // pulsing Start + A whenever a cutscene video (state == 1) is playing.
                uint32_t seqState = *reinterpret_cast<const volatile uint32_t*>(0x00812DC8);
                if (!s_skipReached3D) {
                    if (seqState == 3u) {
                        s_skipReached3D       = true;
                        s_skipSettleStartTick = now;
                        ClearActiveBotInputs();
                    } else {
                        ClearActiveBotInputs();
                        if (seqState == 1u) {
                            DWORD phase = (now - s_cmdStartTick) % 250u;
                            if (phase < 100u) {
                                s_activeBtnMask.wButtons  = 0x0010u; // Start
                                s_activeBtnMask.analog[0] = 255u;    // A
                            }
                        }
                        return;
                    }
                }
                ClearActiveBotInputs();
                if ((now - s_skipSettleStartTick) >= cmd.durationMs) {
                    s_curCmdIdx++;
                    s_cmdStarted = false;
                    continue;
                }
                return;
            }
            case BotCmdType::WaitMs: {
                ClearActiveBotInputs();
                if ((now - s_cmdStartTick) >= cmd.durationMs) {
                    s_curCmdIdx++;
                    s_cmdStarted = false;
                    continue;
                }
                return;
            }
            case BotCmdType::WaitFrames: {
                ClearActiveBotInputs();
                if ((s_total3dSwapFrames - s_cmdStartFrame) >= cmd.frameCount) {
                    s_curCmdIdx++;
                    s_cmdStarted = false;
                    continue;
                }
                return;
            }
            case BotCmdType::PressButton: {
                DWORD elapsed = now - s_cmdStartTick;
                if (elapsed < cmd.durationMs) {
                    s_activeBtnMask = cmd.btnMask;
                    return;
                }
                ClearActiveBotInputs();
                if (elapsed < (cmd.durationMs + cmd.afterMs)) {
                    return;
                }
                s_curCmdIdx++;
                s_cmdStarted = false;
                continue;
            }
            case BotCmdType::StickMove: {
                DWORD elapsed = now - s_cmdStartTick;
                if (elapsed < cmd.durationMs) {
                    s_activeBtnMask = cmd.btnMask;
                    s_activeThumbLX = FloatToThumbAxis(cmd.lx);
                    s_activeThumbLY = FloatToThumbAxis(cmd.ly);
                    s_activeThumbRX = FloatToThumbAxis(cmd.rx);
                    s_activeThumbRY = FloatToThumbAxis(cmd.ry);
                    return;
                }
                ClearActiveBotInputs();
                s_curCmdIdx++;
                s_cmdStarted = false;
                continue;
            }
            case BotCmdType::Screenshot: {
                // Only capture on a completed 3D/Swap frame so the backbuffer is freshly rendered
                if (!is3DSwapFrame) return;
                SaveBackbufferToPng(cmd.filePath.c_str());
                s_curCmdIdx++;
                s_cmdStarted = false;
                continue;
            }
            case BotCmdType::ExitApp: {
                ClearActiveBotInputs();
                g_appRunning = false;
                ExitProcess(0);
                return;
            }
        }
    }
}

void GhostBot_OnVideoOverlayFrame() {
    s_totalVideoFrames++;
    s_3dFramesSinceLastVideo = 0;
    if (s_botActive) {
        StepBotStateMachine(false);
    }
}

void GhostBot_On3DSwapFrame() {
    s_total3dSwapFrames++;
    s_3dFramesSinceLastVideo++;
    if (s_botActive) {
        StepBotStateMachine(true);
    }
}

void GhostBot_ApplyInputState(
    uint16_t* ioButtons,
    uint8_t   ioAnalogButtons[8],
    int16_t*  ioThumbLX,
    int16_t*  ioThumbLY,
    int16_t*  ioThumbRX,
    int16_t*  ioThumbRY)
{
    if (!s_botActive) return;
    StepBotStateMachine(false);

    if (ioButtons) {
        *ioButtons |= s_activeBtnMask.wButtons;
    }
    if (ioAnalogButtons) {
        for (int i = 0; i < 8; ++i) {
            if (s_activeBtnMask.analog[i] > ioAnalogButtons[i]) {
                ioAnalogButtons[i] = s_activeBtnMask.analog[i];
            }
        }
    }
    if (ioThumbLX && s_activeThumbLX != 0) *ioThumbLX = s_activeThumbLX;
    if (ioThumbLY && s_activeThumbLY != 0) *ioThumbLY = s_activeThumbLY;
    if (ioThumbRX && s_activeThumbRX != 0) *ioThumbRX = s_activeThumbRX;
    if (ioThumbRY && s_activeThumbRY != 0) *ioThumbRY = s_activeThumbRY;
}
