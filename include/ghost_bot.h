#pragma once

#include "ghost_runtime.h"

// ============================================================================
// GhostWin11 Standalone Automated Testing & In-Game Bot Module (ghost_bot.cpp)
// ============================================================================
// Provides a standalone player-input simulation and D3D11 screenshot bot for
// automated testing, menu navigation, level loading, character movement, and
// visual regression verification while keeping the game window 100% visible.
//
// Supported CLI arguments:
//   --bot="<cmd1>; <cmd2>; ..."     Execute semicolon-separated bot commands
//   --bot-script="<filepath.txt>"   Execute newline/semicolon-separated bot script
//
// Supported Bot Commands:
//   skip_videos [settle_ms]         Pulse Start/A during .vid movies, stop as soon as
//                                   3D title screen begins, then wait settle_ms (default 2500)
//   wait <ms>                       Wait <ms> milliseconds
//   wait_frames <count>             Wait <count> rendered 3D frames
//   press <btn> [hold_ms] [wait_ms] Press button (start, back, a, b, x, y, up, down,
//                                   left, right, ltrig, rtrig, black, white, rthumb)
//   stick <lx> <ly> <rx> <ry> <ms> [btn]
//                                   Move left/right analog sticks (-1.0..+1.0) for <ms>
//   walk_forward <ms>               Walk forward (LY = +1.0) for <ms>
//   walk_back <ms>                  Walk backward (LY = -1.0) for <ms>
//   strafe_left <ms>                Strafe left (LX = -1.0) for <ms>
//   strafe_right <ms>               Strafe right (LX = +1.0) for <ms>
//   turn_left <ms>                  Turn camera left (RX = -1.0) for <ms>
//   turn_right <ms>                 Turn camera right (RX = +1.0) for <ms>
//   screenshot <path.png>           Save 640x480 D3D11 backbuffer to PNG file
//   exit / quit                     Close GhostWin11.exe after script finishes

void GhostBot_InitFromCommandLine(const char* cmdLine);
bool GhostBot_IsActive();
bool GhostBot_AutoConfigureLauncher();

void GhostBot_OnVideoOverlayFrame();
void GhostBot_On3DSwapFrame();
void GhostBot_ApplyInputState(
    uint16_t* ioButtons,
    uint8_t   ioAnalogButtons[8],
    int16_t*  ioThumbLX,
    int16_t*  ioThumbLY,
    int16_t*  ioThumbRX,
    int16_t*  ioThumbRY);
