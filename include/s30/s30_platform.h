#ifndef S30_PLATFORM_H
#define S30_PLATFORM_H

// Interface between the game and the PocketGo S30 platform layer (src/s30).


// Directory with the asset segment files (<name>.bin) and the ROM, set at startup
extern const char* gS30DataDir;
// Save file (the 512-byte N64 EEPROM image)
extern const char* gS30SaveFile;

// The game loop (src/sys/sys_main.c)
void S30_GameMain(void);

// From the shared library (n64/include/s30: s30_gfx.h, s30_display.h; those
// include <stdint.h>, which the game's headers clash with). Frame pacing in
// panel refreshes per game frame, then a frame's display list drawn and shown.
void S30Display_SetUpdateRate(int vblanks);
void pc_process_displaylist(void* displayList);

// Audio: the game's audio thread setup and start (src/s30/ultra.c), before
// the game initializes
void S30_AudioStart(void);

// Asset segments (generated segtab.c): the game loads one by mapping
// <name>.bin at its VRAM (src/s30/ultra.c)
typedef struct {
    unsigned int romStart;
    unsigned int vram;
    const char* name;
} S30AssetSegment;
extern const S30AssetSegment gS30AssetSegments[];
extern const int gS30AssetSegmentCount;

// Overlays (generated segtab.c): linked in; loading one resets its data from
// the copy taken at startup and clears its bss
typedef struct {
    unsigned int romStart;
    unsigned char* dataStart;
    unsigned char* dataEnd;
    unsigned char* bssStart;
    unsigned char* bssEnd;
    unsigned char* pristine; // the data as linked
} S30Overlay;
extern S30Overlay gS30Overlays[];
extern const int gS30OverlayCount;

// Loads the asset segment or overlay whose ROM data starts at romStart.
// Returns 0 if there is none.
int S30_LoadRomSegment(unsigned int romStart);

// Asset texture arrays whose elements aren't 16-bit (generated textab.c),
// per segment and sorted by address. Segments sharing an address overlap, so
// a lookup goes by the segment mapped there.
typedef struct {
    unsigned int start;
    unsigned int end;
    int elemSize; // bytes: 1, 4 or 8
} S30TexRange;
typedef struct {
    const char* name; // the asset segment
    const S30TexRange* ranges;
    int count;
} S30SegTex;
extern const S30SegTex gS30SegTex[];
extern const int gS30SegTexCount;

// Element size of the texture data at addr (2 unless an S30TexRange says otherwise)
int S30_TexElemSize(unsigned long addr);

#endif
