/* C++ shim: look up a game's system/hardware name (e.g. "CPS-2", "Neo Geo") from the FBNeo
 * driver table, so the C player can label the picker. FBNeo's burn.h API is extern "C", so
 * these symbols are unmangled; we match that linkage and iterate the driver list by name. */
#include <string.h>

extern "C" {
    extern unsigned int nBurnDrvActive;	/* selected driver */
    extern unsigned int nBurnDrvCount;	/* number of drivers */
    char* BurnDrvGetTextA(unsigned int i);
}

#define DRV_NAME   0
#define DRV_SYSTEM 6

extern "C" const char* fbneo_system(const char* name)
{
    unsigned int save = nBurnDrvActive, i;
    const char* res = 0;
    for (i = 0; i < nBurnDrvCount; i++) {
        char* nm;
        nBurnDrvActive = i;	/* no game is loaded while the picker runs */
        nm = BurnDrvGetTextA(DRV_NAME);
        if (nm && strcmp(nm, name) == 0) {
            res = BurnDrvGetTextA(DRV_SYSTEM);
            break;
        }
    }
    nBurnDrvActive = save;
    return res;
}
