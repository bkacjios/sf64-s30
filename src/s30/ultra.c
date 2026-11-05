/*
 * Star Fox 64 on the PocketGo S30: what of the N64 is the game's own.
 * libultra's OS is the shared library's (n64/src/os.c, s30_os.h).
 *
 * - Asset segments: data/<name>.bin mapped at the segment's VRAM, so
 *   segmented addresses are plain pointers (SEGMENTED_TO_VIRTUAL is the
 *   identity). Overlays are linked in; loading one resets its data and bss.
 * - The libultra data the game references; its audio thread.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include "s30_audio.h"
#include "s30_platform.h"

/* ---------------------------------------------------------------------------
 * Asset segments and overlays
 * ------------------------------------------------------------------------- */

/* The texture ranges of the segment mapped at each segment number (0x01-0x0F;
   16: the RAM-address ones, ast_radio) */
static const S30SegTex* sMappedTex[17];

static const S30SegTex* seg_tex(const char* name) {
    int i;

    for (i = 0; i < gS30SegTexCount; i++) {
        if (strcmp(gS30SegTex[i].name, name) == 0) {
            return &gS30SegTex[i];
        }
    }
    return NULL;
}

static void map_asset(const S30AssetSegment* seg) {
    char path[4096];
    struct stat st;
    uintptr_t base = seg->vram & ~(uintptr_t)0xFFF;
    size_t skip = seg->vram - base, size;
    void* p;
    int fd;

    snprintf(path, sizeof(path), "%s/%s.bin", gS30DataDir, seg->name);
    fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0 || fstat(fd, &st) != 0) {
        perror(path);
        exit(1);
    }
    size = (skip + st.st_size + 0xFFF) & ~(size_t)0xFFF;
    p = mmap((void*)base, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
    if (p == MAP_FAILED || read(fd, (char*)p + skip, st.st_size) != st.st_size) {
        fprintf(stderr, "sf64: cannot load %s at %08x: %s\n", path, seg->vram, strerror(errno));
        exit(1);
    }
    close(fd);
    sMappedTex[seg->vram < 0x10000000 ? seg->vram >> 24 : 16] = seg_tex(seg->name);
}

int S30_LoadRomSegment(uint32_t romStart) {
    int i;

    for (i = 0; i < gS30AssetSegmentCount; i++) {
        if (gS30AssetSegments[i].romStart == romStart) {
            map_asset(&gS30AssetSegments[i]);
            return 1;
        }
    }
    for (i = 0; i < gS30OverlayCount; i++) {
        S30Overlay* ovl = &gS30Overlays[i];

        if (ovl->romStart == romStart) {
            if (ovl->pristine != NULL) {
                memcpy(ovl->dataStart, ovl->pristine, ovl->dataEnd - ovl->dataStart);
            }
            memset(ovl->bssStart, 0, ovl->bssEnd - ovl->bssStart);
            return 1;
        }
    }
    fprintf(stderr, "sf64: no segment at ROM %08x\n", romStart);
    return 0;
}

/* Copies of the overlays' data as linked, taken before the game runs */
void S30_InitOverlays(void) {
    int i;

    for (i = 0; i < gS30OverlayCount; i++) {
        S30Overlay* ovl = &gS30Overlays[i];
        size_t size = ovl->dataEnd - ovl->dataStart;

        if (size != 0) {
            ovl->pristine = malloc(size);
            memcpy(ovl->pristine, ovl->dataStart, size);
        }
    }
}

int S30_TexElemSize(unsigned long addr) {
    const S30SegTex* t = sMappedTex[addr < 0x10000000 ? addr >> 24 : 16];
    int lo = 0, hi;

    if (t == NULL || (addr >= 0x10000000 && addr < 0x80000000)) {
        return 2; // not an asset segment: game data (the frame buffers are u16)
    }
    hi = t->count - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;

        if (addr < t->ranges[mid].start) {
            hi = mid - 1;
        } else if (addr >= t->ranges[mid].end) {
            lo = mid + 1;
        } else {
            return t->ranges[mid].elemSize;
        }
    }
    return 2;
}

/* ---------------------------------------------------------------------------
 * libultra data the game references, and what it calls that the S30 has no
 * use for
 * ------------------------------------------------------------------------- */

uint32_t osMemSize = 0x800000;
uint32_t osViModeNtscLan1[20], osViModePalLan1[20], osViModeMpalLan1[20];
uint8_t rspbootTextStart[1], rspbootTextEnd[1], gspF3DEX_fifoTextStart[1], gspF3DEX_fifoDataStart[1];
uint8_t aspMainTextStart[1], aspMainDataStart[1], aspMainDataEnd[1];
float __libm_qnan_f = NAN;

void RdRam_CheckIPL3(void) {
}

int32_t Mio0_Decompress(void* src, void* dst) {
    fprintf(stderr, "sf64: Mio0_Decompress called\n");
    return 0;
}

/* The game's audio thread: its setup, then a frame of sound at a time
   (AudioThread_CreateTask runs the synthesis on the software mixer,
   src/audio/mixer.c, and passes the result to osAiSetNextBuffer) */
extern void AudioLoad_Init(void);
extern void Audio_InitSounds(void);
extern void* AudioThread_CreateTask(void);

/* One audio frame (the shared library's audio thread calls it with the audio
   lock held; the buffer passed to osAiSetNextBuffer goes to ALSA) */
static void audio_update(void) {
    AudioThread_CreateTask();
}

void S30_AudioStart(void) {
    S30Audio_Open();
    AudioLoad_Init();
    Audio_InitSounds();
    S30Audio_Start(audio_update);
}
