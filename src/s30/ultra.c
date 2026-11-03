/*
 * libultra for the PocketGo S30: the parts of the N64 OS the game uses, on
 * Linux, and the loading of asset segments and overlays.
 *
 * - Message queues block on a condition variable; threads are pthreads. The
 *   game thread (src/sys/sys_main.c S30_GameMain) talks to the serial thread
 *   (controller, EEPROM) and the timer thread through queues, as on the N64.
 * - Timers (osSetTimer) run on a thread of their own; time is counted in N64
 *   CPU cycles (46.875 MHz) from the monotonic clock.
 * - Controller: the S30 gamepad (src/s30/input.c) as controller 1.
 * - EEPROM: the 4 Kbit save in gS30SaveFile.
 * - PI DMA reads the uncompressed ROM (data/baserom.us.rev1.uncompressed.z64).
 * - Asset segments: data/<name>.bin mapped at the segment's VRAM, so
 *   segmented addresses are plain pointers (SEGMENTED_TO_VIRTUAL is the
 *   identity). Overlays are linked in; loading one resets its data and bss.
 *
 * The N64 structures are declared here as the game lays them out (the game's
 * headers clash with the C library's).
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "s30_audio.h"
#include "s30_input.h"
#include "s30_platform.h"

typedef void* OSMesg;
typedef struct OSMesgQueue {
    void* mtqueue;
    void* fullqueue;
    int32_t validCount;
    int32_t first;
    int32_t msgCount;
    OSMesg* msg;
} OSMesgQueue;

typedef struct {
    uint16_t button;
    int8_t stick_x;
    int8_t stick_y;
    uint8_t errnum;
} OSContPad;

typedef struct {
    uint16_t type;
    uint8_t status;
    uint8_t errnum;
} OSContStatus;

typedef struct {
    uint16_t type;
    uint8_t pri;
    uint8_t status;
    OSMesgQueue* retQueue;
} OSMesgHdr;

typedef struct {
    OSMesgHdr hdr;
    void* dramAddr;
    uint32_t devAddr;
    uint32_t size;
    void* piHandle;
} OSIoMesg;

typedef uint64_t OSTime;

#define OS_MESG_BLOCK 1
#define OS_CPU_COUNTER 46875000ull
#define OS_EVENT_SI 5
#define OS_EVENT_COUNT 15

/* ---------------------------------------------------------------------------
 * Message queues
 * ------------------------------------------------------------------------- */

static pthread_mutex_t sMesgLock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t sMesgCond = PTHREAD_COND_INITIALIZER;

void osCreateMesgQueue(OSMesgQueue* mq, OSMesg* msg, int32_t count) {
    mq->mtqueue = mq->fullqueue = NULL;
    mq->validCount = 0;
    mq->first = 0;
    mq->msgCount = count;
    mq->msg = msg;
}

static int32_t send_mesg(OSMesgQueue* mq, OSMesg msg, int32_t flag, int jam) {
    pthread_mutex_lock(&sMesgLock);
    while (mq->validCount >= mq->msgCount) {
        if (flag != OS_MESG_BLOCK) {
            pthread_mutex_unlock(&sMesgLock);
            return -1;
        }
        pthread_cond_wait(&sMesgCond, &sMesgLock);
    }
    if (jam) {
        mq->first = (mq->first + mq->msgCount - 1) % mq->msgCount;
        mq->msg[mq->first] = msg;
    } else {
        mq->msg[(mq->first + mq->validCount) % mq->msgCount] = msg;
    }
    mq->validCount++;
    pthread_cond_broadcast(&sMesgCond);
    pthread_mutex_unlock(&sMesgLock);
    return 0;
}

int32_t osSendMesg(OSMesgQueue* mq, OSMesg msg, int32_t flag) {
    return send_mesg(mq, msg, flag, 0);
}

int32_t osJamMesg(OSMesgQueue* mq, OSMesg msg, int32_t flag) {
    return send_mesg(mq, msg, flag, 1);
}

int32_t osRecvMesg(OSMesgQueue* mq, OSMesg* msg, int32_t flag) {
    pthread_mutex_lock(&sMesgLock);
    while (mq->validCount == 0) {
        if (flag != OS_MESG_BLOCK) {
            pthread_mutex_unlock(&sMesgLock);
            return -1;
        }
        pthread_cond_wait(&sMesgCond, &sMesgLock);
    }
    if (msg != NULL) {
        *msg = mq->msg[mq->first];
    }
    mq->first = (mq->first + 1) % mq->msgCount;
    mq->validCount--;
    pthread_cond_broadcast(&sMesgCond);
    pthread_mutex_unlock(&sMesgLock);
    return 0;
}

static struct {
    OSMesgQueue* mq;
    OSMesg msg;
} sEvents[OS_EVENT_COUNT];

void osSetEventMesg(int32_t event, OSMesgQueue* mq, OSMesg msg) {
    if (event >= 0 && event < OS_EVENT_COUNT) {
        sEvents[event].mq = mq;
        sEvents[event].msg = msg;
    }
}

static void post_event(int32_t event) {
    if (sEvents[event].mq != NULL) {
        osSendMesg(sEvents[event].mq, sEvents[event].msg, 0);
    }
}

void osViSetEvent(OSMesgQueue* mq, OSMesg msg, uint32_t retraceCount) {
}

OSMesgQueue* __osGetActiveQueue(void) {
    return NULL;
}

/* ---------------------------------------------------------------------------
 * Threads
 * ------------------------------------------------------------------------- */

static struct {
    void* thread;
    void (*entry)(void*);
    void* arg;
} sThreads[16];

void osCreateThread(void* thread, int32_t id, void (*entry)(void*), void* arg, void* sp, int32_t pri) {
    int i;

    for (i = 0; i < 16; i++) {
        if (sThreads[i].thread == NULL || sThreads[i].thread == thread) {
            sThreads[i].thread = thread;
            sThreads[i].entry = entry;
            sThreads[i].arg = arg;
            return;
        }
    }
    fprintf(stderr, "sf64: too many threads\n");
}

static void* thread_main(void* p) {
    int i = (int)(intptr_t)p;

    sThreads[i].entry(sThreads[i].arg);
    return NULL;
}

void osStartThread(void* thread) {
    pthread_t t;
    pthread_attr_t attr;
    int i;

    for (i = 0; i < 16; i++) {
        if (sThreads[i].thread == thread) {
            pthread_attr_init(&attr);
            pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
            pthread_create(&t, &attr, thread_main, (void*)(intptr_t)i);
            pthread_attr_destroy(&attr);
            return;
        }
    }
}

void osSetThreadPri(void* thread, int32_t pri) {
}

/* ---------------------------------------------------------------------------
 * Time and timers
 * ------------------------------------------------------------------------- */

uint64_t osClockRate = 62500000;
static uint64_t sTimeBase; // ns
static OSTime sTimeOffset; // osSetTime

static uint64_t now_ns(void) {
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + ts.tv_nsec;
}

OSTime osGetTime(void) {
    uint64_t ns = now_ns() - sTimeBase;

    return sTimeOffset + ns / 1000 * OS_CPU_COUNTER / 1000000;
}

void osSetTime(OSTime t) {
    sTimeOffset = t - (osGetTime() - sTimeOffset);
}

uint32_t osGetCount(void) {
    return (uint32_t)osGetTime();
}

typedef struct OSTimer {
    struct OSTimer* next;
    struct OSTimer* prev;
    OSTime interval;
    OSTime value;
    OSMesgQueue* mq;
    OSMesg msg;
} OSTimer;

#define MAX_TIMERS 32
static struct {
    OSTimer* timer;
    uint64_t due; // ns
    uint64_t interval; // ns, 0 for one-shot
    OSMesgQueue* mq;
    OSMesg msg;
} sTimers[MAX_TIMERS];
static pthread_mutex_t sTimerLock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t sTimerCond = PTHREAD_COND_INITIALIZER;
static int sTimerThreadStarted;

static uint64_t cycles_to_ns(OSTime c) {
    return c * 1000ull / (OS_CPU_COUNTER / 1000000ull);
}

static void* timer_thread(void* arg) {
    pthread_mutex_lock(&sTimerLock);
    for (;;) {
        uint64_t now = now_ns(), next = UINT64_MAX;
        int i;

        for (i = 0; i < MAX_TIMERS; i++) {
            if (sTimers[i].timer == NULL) {
                continue;
            }
            if (sTimers[i].due <= now) {
                OSMesgQueue* mq = sTimers[i].mq;
                OSMesg msg = sTimers[i].msg;

                if (sTimers[i].interval != 0) {
                    sTimers[i].due += sTimers[i].interval;
                } else {
                    sTimers[i].timer = NULL;
                }
                pthread_mutex_unlock(&sTimerLock);
                if (mq != NULL) {
                    osSendMesg(mq, msg, 0);
                }
                pthread_mutex_lock(&sTimerLock);
                i = -1; // rescan: the list may have changed
                now = now_ns();
                next = UINT64_MAX;
                continue;
            }
            if (sTimers[i].due < next) {
                next = sTimers[i].due;
            }
        }
        if (next == UINT64_MAX) {
            pthread_cond_wait(&sTimerCond, &sTimerLock);
        } else {
            struct timespec ts;

            clock_gettime(CLOCK_MONOTONIC, &ts);
            ts.tv_sec += (next - now) / 1000000000ull;
            ts.tv_nsec += (next - now) % 1000000000ull;
            if (ts.tv_nsec >= 1000000000) {
                ts.tv_sec++;
                ts.tv_nsec -= 1000000000;
            }
            pthread_cond_timedwait(&sTimerCond, &sTimerLock, &ts);
        }
    }
    return NULL;
}

int32_t osSetTimer(OSTimer* timer, OSTime countdown, OSTime interval, OSMesgQueue* mq, OSMesg msg) {
    int i;

    pthread_mutex_lock(&sTimerLock);
    if (!sTimerThreadStarted) {
        pthread_t t;
        pthread_condattr_t ca;

        pthread_condattr_init(&ca);
        pthread_condattr_setclock(&ca, CLOCK_MONOTONIC);
        pthread_cond_init(&sTimerCond, &ca);
        pthread_create(&t, NULL, timer_thread, NULL);
        pthread_detach(t);
        sTimerThreadStarted = 1;
    }
    for (i = 0; i < MAX_TIMERS; i++) {
        if (sTimers[i].timer == NULL || sTimers[i].timer == timer) {
            sTimers[i].timer = timer;
            sTimers[i].due = now_ns() + cycles_to_ns(countdown != 0 ? countdown : interval);
            sTimers[i].interval = cycles_to_ns(interval);
            sTimers[i].mq = mq;
            sTimers[i].msg = msg;
            break;
        }
    }
    if (i == MAX_TIMERS) {
        fprintf(stderr, "sf64: too many timers\n");
    }
    pthread_cond_broadcast(&sTimerCond);
    pthread_mutex_unlock(&sTimerLock);
    return 0;
}

int32_t osStopTimer(OSTimer* timer) {
    int i;

    pthread_mutex_lock(&sTimerLock);
    for (i = 0; i < MAX_TIMERS; i++) {
        if (sTimers[i].timer == timer) {
            sTimers[i].timer = NULL;
        }
    }
    pthread_mutex_unlock(&sTimerLock);
    return 0;
}

/* ---------------------------------------------------------------------------
 * Controller (the S30 gamepad as controller 1; no Rumble Pak)
 * ------------------------------------------------------------------------- */

int32_t osContInit(OSMesgQueue* mq, uint8_t* bitpattern, OSContStatus* status) {
    int i;

    for (i = 0; i < 4; i++) {
        status[i].type = 0x0500; // CONT_TYPE_NORMAL
        status[i].status = 0;
        status[i].errnum = i == 0 ? 0 : 8; // CONT_NO_RESPONSE_ERROR
    }
    *bitpattern = 1;
    return 0;
}

int32_t osContStartReadData(OSMesgQueue* mq) {
    osSendMesg(mq, NULL, 0);
    return 0;
}

void osContGetReadData(OSContPad* pad) {
    S30Pad p;

    S30Input_Read(&p);
    memset(pad, 0, 4 * sizeof(*pad));
    pad[0].button = p.button;
    pad[0].stick_x = p.stick_x;
    pad[0].stick_y = p.stick_y;
}

int32_t osContStartQuery(OSMesgQueue* mq) {
    osSendMesg(mq, NULL, 0);
    return 0;
}

void osContGetQuery(OSContStatus* status) {
    uint8_t bits;

    osContInit(NULL, &bits, status);
}

int32_t osMotorInit(OSMesgQueue* mq, void* pfs, int32_t channel) {
    return 1; // no Rumble Pak
}

int32_t osMotorStart(void* pfs) {
    return 1;
}

int32_t osMotorStop(void* pfs) {
    return 1;
}

/* ---------------------------------------------------------------------------
 * EEPROM (4 Kbit: 64 blocks of 8 bytes)
 * ------------------------------------------------------------------------- */

static uint8_t sEeprom[512];
static int sEepromLoaded;

static void eeprom_load(void) {
    FILE* f;

    if (sEepromLoaded) {
        return;
    }
    sEepromLoaded = 1;
    f = fopen(gS30SaveFile, "rb");
    if (f != NULL) {
        if (fread(sEeprom, 1, sizeof(sEeprom), f) != sizeof(sEeprom)) {
            fprintf(stderr, "sf64: %s: short save file\n", gS30SaveFile);
        }
        fclose(f);
    }
}

int32_t osEepromProbe(OSMesgQueue* mq) {
    return 1; // EEPROM_TYPE_4K
}

int32_t osEepromRead(OSMesgQueue* mq, uint8_t address, uint8_t* buffer) {
    eeprom_load();
    memcpy(buffer, &sEeprom[(address & 63) * 8], 8);
    return 0;
}

int32_t osEepromWrite(OSMesgQueue* mq, uint8_t address, uint8_t* buffer) {
    char tmp[4096];
    FILE* f;

    eeprom_load();
    memcpy(&sEeprom[(address & 63) * 8], buffer, 8);
    snprintf(tmp, sizeof(tmp), "%s.tmp", gS30SaveFile);
    f = fopen(tmp, "wb");
    if (f == NULL || fwrite(sEeprom, 1, sizeof(sEeprom), f) != sizeof(sEeprom) || fclose(f) != 0) {
        perror(gS30SaveFile);
        return -1;
    }
    rename(tmp, gS30SaveFile);
    return 0;
}

/* ---------------------------------------------------------------------------
 * ROM (PI DMA)
 * ------------------------------------------------------------------------- */

static int sRomFd = -1;

static void rom_read(uint32_t devAddr, void* dst, uint32_t size) {
    if (sRomFd < 0) {
        char path[4096];

        snprintf(path, sizeof(path), "%s/baserom.us.rev1.uncompressed.z64", gS30DataDir);
        sRomFd = open(path, O_RDONLY | O_CLOEXEC);
        if (sRomFd < 0) {
            perror(path);
            exit(1);
        }
    }
    if (pread(sRomFd, dst, size, devAddr & 0x0FFFFFFF) != (ssize_t)size) {
        fprintf(stderr, "sf64: ROM read %08x+%x failed\n", devAddr, size);
    }
}

void* osCartRomInit(void) {
    static uint32_t handle[32];
    return handle;
}

void* osDriveRomInit(void) {
    return osCartRomInit();
}

int32_t osPiStartDma(OSIoMesg* mb, int32_t pri, int32_t direction, uint32_t devAddr, void* vAddr, uint32_t nbytes,
                     OSMesgQueue* mq) {
    rom_read(devAddr, vAddr, nbytes);
    if (mq != NULL) {
        osSendMesg(mq, mb, 0);
    }
    return 0;
}

int32_t osEPiStartDma(void* handle, OSIoMesg* mb, int32_t direction) {
    rom_read(mb->devAddr, mb->dramAddr, mb->size);
    if (mb->hdr.retQueue != NULL) {
        osSendMesg(mb->hdr.retQueue, mb, 0);
    }
    return 0;
}

void osCreatePiManager(int32_t pri, OSMesgQueue* cmdQ, OSMesg* cmdBuf, int32_t cmdMsgCnt) {
}

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
 * The rest of the N64: nothing to do
 * ------------------------------------------------------------------------- */

int32_t osTvType = 1; // OS_TV_NTSC
uint32_t osMemSize = 0x800000;
uint32_t osViModeNtscLan1[20], osViModePalLan1[20], osViModeMpalLan1[20];
uint8_t rspbootTextStart[1], rspbootTextEnd[1], gspF3DEX_fifoTextStart[1], gspF3DEX_fifoDataStart[1];
uint8_t aspMainTextStart[1], aspMainDataStart[1], aspMainDataEnd[1];
float __libm_qnan_f = NAN;

void osInitialize(void) {
    sTimeBase = now_ns();
}

void osCreateViManager(int32_t pri) {
}
void osViSetMode(void* mode) {
}
void osViSetSpecialFeatures(uint32_t func) {
}
void osViSwapBuffer(void* frameBufPtr) {
}
void osViBlack(uint8_t active) {
}
void osViRepeatLine(uint8_t active) {
}
void osWritebackDCacheAll(void) {
}
void osInvalDCache(void* vaddr, int32_t nbytes) {
}
void osInvalICache(void* vaddr, int32_t nbytes) {
}
void osDpSetStatus(uint32_t data) {
}
void osSpTaskLoad(void* task) {
}
void osSpTaskStartGo(void* task) {
}
void osSpTaskYield(void) {
}
uint32_t osSpTaskYielded(void* task) {
    return 0;
}
void RdRam_CheckIPL3(void) {
}

/* Audio interface: buffers go to ALSA (src/s30/audio.c), which paces the
   audio thread. Nothing is left playing in the N64 sense: the next buffer is
   always a full frame's worth. */
uint32_t osAiGetLength(void) {
    return 0;
}
int32_t osAiSetFrequency(uint32_t frequency) {
    return frequency;
}
int32_t osAiSetNextBuffer(void* buf, uint32_t size) {
    static int dbg = -1, count, peak;

    if (dbg < 0) {
        dbg = getenv("SF64_AUDIODBG") != NULL;
    }
    if (dbg) { // SF64_AUDIODBG: the loudest sample of each second
        const int16_t* p = buf;
        uint32_t i;

        for (i = 0; i < size / 2; i++) {
            int v = p[i] < 0 ? -p[i] : p[i];
            peak = v > peak ? v : peak;
        }
        if (++count % 60 == 0) {
            printf("audio: peak %d, buffer %u frames\n", peak, (unsigned)(size / 4));
            peak = 0;
        }
    }
    S30Audio_PlayInterleaved(buf, size / 4);
    return 0;
}

/* libultra's printf core: the formatting is the C library's */
typedef char* (*outfun)(char*, const char*, size_t);
int _Printf(outfun prout, char* arg, const char* fmt, va_list args) {
    char buf[1024];
    int n = vsnprintf(buf, sizeof(buf), fmt, args);

    if (n < 0) {
        return n;
    }
    if (n >= (int)sizeof(buf)) {
        n = sizeof(buf) - 1;
    }
    return prout(arg, buf, n) != NULL ? n : -1;
}

/* libm entry points under their libultra names */
float __floorf(float x) {
    return floorf(x);
}
float __ceilf(float x) {
    return ceilf(x);
}
float __roundf(float x) {
    return roundf(x);
}
float __truncf(float x) {
    return truncf(x);
}
float __nearbyintf(float x) {
    return nearbyintf(x);
}
/* SIN_DEG/COS_DEG: without these the link resolved __sinf/__cosf elsewhere
   (a math library internal with another signature), giving 0 */
float __sinf(float x) {
    return sinf(x);
}
float __cosf(float x) {
    return cosf(x);
}
double __floor(double x) {
    return floor(x);
}
double __ceil(double x) {
    return ceil(x);
}
double __round(double x) {
    return round(x);
}
double __trunc(double x) {
    return trunc(x);
}
double __nearbyint(double x) {
    return nearbyint(x);
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
