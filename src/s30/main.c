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
#include <errno.h>
#include <fcntl.h>
#include <libgen.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <ucontext.h>
#include <unistd.h>

#include <linux/input.h>

#include "s30_display.h"
#include "s30_gfx.h"
#include "s30_input.h"
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

/* mkdir -p for a path's parent directories */
static void make_parents(const char* path) {
    char* p = strdup(path);
    char* s;

    for (s = strchr(p + 1, '/'); s != NULL; s = strchr(s + 1, '/')) {
        *s = '\0';
        if (mkdir(p, 0755) != 0 && errno != EEXIST) {
            perror(p);
        }
        *s = '/';
    }
    free(p);
}

/* XDG data location for the save */
static const char* default_save(const char* home) {
    const char* xdg = getenv("XDG_DATA_HOME");
    char* path = malloc(strlen(xdg ? xdg : home) + 32);

    if (xdg != NULL && xdg[0] == '/') {
        sprintf(path, "%s/sf64/sf64.sav", xdg);
    } else {
        sprintf(path, "%s/.local/share/sf64/sf64.sav", home);
    }
    make_parents(path);
    return path;
}

static const char* path_beside(const char* exeDir, const char* name) {
    char* p = malloc(strlen(exeDir) + strlen(name) + 2);

    sprintf(p, "%s/%s", exeDir, name);
    return p;
}

static const char* sCrashLog; // crash.log next to the binary
static int sCrashFd = -1;

/* To stderr and the crash log (async-signal-safe calls only) */
static void crash_write(const char* s) {
    write(2, s, strlen(s));
    if (sCrashFd >= 0) {
        write(sCrashFd, s, strlen(s));
    }
}

/* Crash report on stderr and appended to the crash log: fault address,
   registers and the code addresses found on the stack, for addr2line */
static void on_crash(int sig, siginfo_t* info, void* ctx) {
    extern char __executable_start[], etext[];
    const mcontext_t* mc = &((const ucontext_t*)ctx)->uc_mcontext;
    const uintptr_t* sp = (const uintptr_t*)mc->arm_sp;
    char buf[160];
    int i, n = 0;

    sCrashFd = open(sCrashLog, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
    snprintf(buf, sizeof(buf), "crash: signal %d at %p pc %08lx lr %08lx sp %08lx\ncrash: stack", sig,
             info->si_addr, mc->arm_pc, mc->arm_lr, mc->arm_sp);
    crash_write(buf);
    for (i = 0; i < 512 && n < 24; i++) {
        if (sp[i] >= (uintptr_t)__executable_start && sp[i] < (uintptr_t)etext) {
            snprintf(buf, sizeof(buf), " %08lx", (unsigned long)sp[i]);
            crash_write(buf);
            n++;
        }
    }
    crash_write("\n");
    if (sCrashFd >= 0) {
        fsync(sCrashFd);
        close(sCrashFd);
    }
    signal(sig, SIG_DFL);
    raise(sig);
}

int main(int argc, char** argv) {
    char* exe = strdup(argv[0]);
    const char* exeDir = dirname(exe);
    const char* home = getenv("HOME");
    struct sigaction sa;

    setvbuf(stdout, NULL, _IOLBF, 0); // logs survive a crash
    sCrashLog = path_beside(exeDir, "crash.log");
    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = on_crash;
    sa.sa_flags = SA_SIGINFO;
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGBUS, &sa, NULL);
    sigaction(SIGILL, &sa, NULL);
    sigaction(SIGFPE, &sa, NULL);

    gS30DataDir = getenv("SF64_DATA") ? getenv("SF64_DATA") : path_beside(exeDir, "data");
    if (getenv("SF64_SAVE")) {
        gS30SaveFile = getenv("SF64_SAVE");
    } else {
        gS30SaveFile = home != NULL ? default_save(home) : path_beside(exeDir, "sf64.sav");
    }
    printf("sf64: data %s, save %s\n", gS30DataDir, gS30SaveFile);

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
