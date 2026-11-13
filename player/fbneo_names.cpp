/* C++ shim: look up a game's details (system, full title, hardware) in the FBNeo driver table, so the
 * C player can label the picker and pick per-game settings. FBNeo's burn.h API is extern "C", so these
 * symbols are unmangled; we match that linkage. Lookups go through a name index built once (sorted
 * driver names + bsearch), because the picker asks for every listed set. Call only while no game is
 * loaded (they switch nBurnDrvActive and put it back). */
#include <string.h>
#include <stdlib.h>

extern "C" {
    extern unsigned int nBurnDrvActive;	/* selected driver */
    extern unsigned int nBurnDrvCount;	/* number of drivers */
    char* BurnDrvGetTextA(unsigned int i);
    int BurnDrvGetHardwareCode();
}

#define DRV_NAME     0
#define DRV_FULLNAME 2
#define DRV_SYSTEM   6
#define HARDWARE_PUBLIC_MASK 0x7fff0000u   /* burn.h hardware code: prefix + board family */
#define HARDWARE_CAVE_CV1000 0x06010000u   /* burn.h: HARDWARE_PREFIX_CAVE | 0x00010000 */

struct drv_ent { const char* name; unsigned int idx; };
static drv_ent* g_idx;
static unsigned int g_nidx;

static int ent_cmp(const void* a, const void* b)
{
    return strcmp(((const drv_ent*)a)->name, ((const drv_ent*)b)->name);
}

/* driver index of a set name, or -1 */
static int find(const char* name)
{
    if (!g_idx) {
        unsigned int save = nBurnDrvActive, i;
        g_idx = (drv_ent*)malloc(sizeof(drv_ent) * (nBurnDrvCount ? nBurnDrvCount : 1));
        if (!g_idx) return -1;
        for (i = 0; i < nBurnDrvCount; i++) {
            char* nm;
            nBurnDrvActive = i;
            nm = BurnDrvGetTextA(DRV_NAME);   /* points into the static driver table */
            if (nm) { g_idx[g_nidx].name = nm; g_idx[g_nidx].idx = i; g_nidx++; }
        }
        nBurnDrvActive = save;
        qsort(g_idx, g_nidx, sizeof(drv_ent), ent_cmp);
    }
    drv_ent key = { name, 0 };
    const drv_ent* e = (const drv_ent*)bsearch(&key, g_idx, g_nidx, sizeof(drv_ent), ent_cmp);
    return e ? (int)e->idx : -1;
}

static const char* text_of(const char* name, int what)
{
    int i = find(name);
    if (i < 0) return 0;
    unsigned int save = nBurnDrvActive;
    nBurnDrvActive = (unsigned int)i;
    const char* res = BurnDrvGetTextA(what);
    nBurnDrvActive = save;
    return res;
}

extern "C" const char* fbneo_system(const char* name) { return text_of(name, DRV_SYSTEM); }   /* "CPS-2" */
extern "C" const char* fbneo_title(const char* name)  { return text_of(name, DRV_FULLNAME); } /* full name */

/* 1 if the set runs on Cave CV1000 hardware (HARDWARE_CAVE_CV1000): the player turns on 32-bit output only
 * there, where it saves the 16-bit composite and its frames come out opaque. */
extern "C" int fbneo_is_cv1000(const char* name)
{
    int i = find(name);
    if (i < 0) return 0;
    unsigned int save = nBurnDrvActive;
    nBurnDrvActive = (unsigned int)i;
    int res = ((unsigned)BurnDrvGetHardwareCode() & HARDWARE_PUBLIC_MASK) == HARDWARE_CAVE_CV1000;
    nBurnDrvActive = save;
    return res;
}
