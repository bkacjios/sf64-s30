/*
 * Star Fox 64 on the PocketGo S30: process entry.
 *
 * Environment:
 *   SF64_DATA     asset segments and ROM directory (default: "data" next to the binary)
 *   SF64_SAVE     save file (default: $XDG_DATA_HOME/sf64/sf64.sav, i.e.
 *                 ~/.local/share/sf64/sf64.sav; else sf64.sav next to the binary)
 *
 * Crash reports also go to crash.log next to the binary (on the SD card;
 * stderr is the launcher's log in /tmp, lost on reboot).
 */
#define _GNU_SOURCE
#include <libgen.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <linux/input.h>

#include "s30_app.h"
#include "s30_display.h"
#include "s30_gfx.h"
#include "s30_input.h"
#include "s30_os.h"
#include "s30_platform.h"

const char* gS30DataDir;
const char* gS30SaveFile;

extern void osInitialize(void);
extern void S30_InitOverlays(void);
extern uint32_t gSegments[16];

/* The S30's Nintendo face buttons: A -> A (fire), B -> B (bomb), Y -> C-left
   (boost), X -> C-down (brake), L1 -> Z and R1 -> R (tilt, double tap to
   roll), L2 -> C-up (camera), Select -> C-right (answer ROB), Start -> Start,
   D-pad -> N64 D-pad */
static const S30ButtonMap kButtons[] = {
    { BTN_EAST, S30_N64_A },         { BTN_SOUTH, S30_N64_B },          { BTN_WEST, S30_N64_CLEFT },
    { BTN_NORTH, S30_N64_CDOWN },    { BTN_TL, S30_N64_Z },             { BTN_TR, S30_N64_R },
    { BTN_TL2, S30_N64_CUP },        { BTN_SELECT, S30_N64_CRIGHT },    { BTN_START, S30_N64_START },
    { BTN_DPAD_UP, S30_N64_DUP },    { BTN_DPAD_DOWN, S30_N64_DDOWN },  { BTN_DPAD_LEFT, S30_N64_DLEFT },
    { BTN_DPAD_RIGHT, S30_N64_DRIGHT },
};

static int tex_elem_size(uintptr_t addr) {
    return S30_TexElemSize(addr);
}


int main(int argc, char** argv) {
    char* exe = strdup(argv[0]);
    const char* exeDir = dirname(exe);

    setvbuf(stdout, NULL, _IOLBF, 0); // logs survive a crash
    S30App_InstallCrashHandler(S30App_PathIn(exeDir, "crash.log"));

    gS30DataDir = getenv("SF64_DATA") ? getenv("SF64_DATA") : S30App_PathIn(exeDir, "data");
    if (getenv("SF64_SAVE")) {
        gS30SaveFile = getenv("SF64_SAVE");
    } else {
        gS30SaveFile = S30App_SavePath("sf64", "sf64.sav", exeDir);
    }
    printf("sf64: data %s, save %s\n", gS30DataDir, gS30SaveFile);
    {
        // The game loop runs on this thread (S30_GameMain); the timer and
        // serial threads are the game's
        S30OsConfig os = {
            .romPath = S30App_PathIn(gS30DataDir, "baserom.us.rev1.uncompressed.z64"),
            .savePath = gS30SaveFile,
            .saveType = S30_SAVE_EEPROM_4K,
            .threads = 1,
        };
        S30Os_Init(&os);
    }
    osInitialize();
    S30_InitOverlays();
    {
        // Star Fox: F3DEX, fixed-point Mtx, textures as the arrays Torch typed
        S30GfxConfig gfx = { S30_UCODE_F3DEX, gSegments, 0, tex_elem_size, "SF64" };
        S30Gfx_Configure(&gfx);
    }
    S30Display_Init();
    S30Gfx_Init();
    S30Input_Init(kButtons, sizeof(kButtons) / sizeof(kButtons[0]));
    S30_GameMain();
    return 0;
}
