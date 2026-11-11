/*
 * picker - a self-contained SDL2 + SDL2_ttf content picker shell for Wii U (wut) apps.
 *
 * Drop this folder (picker.c, picker.h, Roboto-Regular.ttf) into a project, compile picker.c,
 * bundle the .ttf into the wuhb /content, and write one glue file that fills a picker_cfg and
 * drives main() with the functions below. It has NO dependency on FBNeo, libretro, or any core -
 * it only knows SDL2, SDL2_ttf, and a directory of files.
 *
 * What it provides: fullscreen window + renderer + font, up to 2 game controllers with hot-plug
 * and a START+BACK "leave" combo, a config.txt it creates on first run, a scan of a content
 * directory for one file extension, an on-screen scrolling menu with optional per-item labels,
 * an install/help screen (shown when the folder is empty and from a Help entry), a transient
 * message box, and a rotation-aware framebuffer blit. The host app keeps main() and its own
 * content loop; the picker is a toolkit, not a framework.
 *
 * See README.md for the adoption checklist.
 */
#ifndef PICKER_H
#define PICKER_H

#include <SDL.h>
#include <stdint.h>

typedef struct {
    const char *app_name;      /* header text, e.g. "FBNeo" */
    const char *content_noun;  /* used in help text, e.g. "rom" (shown as "roms") */
    const char *data_dir;      /* base data dir, e.g. "fs:/vol/external01/wiiu/fbneo_player" */
    const char *font_file;     /* TTF path, e.g. "fs:/vol/content/Roboto-Regular.ttf" */
    const char *ext;           /* file extension to list, without the dot, e.g. "zip" */
    const char *(*label_for)(const char *name); /* optional per-item tag (e.g. system), or NULL */
    const char *help_extra;    /* optional extra line on the help screen (e.g. version note), or NULL */
    const char *const *controls; /* optional NULL-terminated lines describing the app's in-game button
                                  * map, shown on the help screen under the generic menu controls; or NULL */
} picker_cfg;

/* Bring up SDL (video+audio+gamecontroller), a fullscreen window+renderer, the font, controllers,
 * and read/create <data_dir>/<app>/config.txt. Returns 0 on success. Keeps a pointer to cfg. */
int  picker_init(const picker_cfg *cfg);
void picker_quit(void);                 /* close font, controllers, SDL */

/* Values from config.txt (valid after picker_init). */
const char *picker_content_dir(void);   /* "content="/"roms=" line, or <data_dir>/<noun>s default */
const char *picker_autoload(void);      /* "autoload=" value, or "" */
int         picker_rotate(void);         /* "rotate=" (1 default; 0 if "off") */
int         picker_cfg_int(const char *key, int def); /* read a host-defined integer "key=" from config.txt */

/* Scan the content dir, show the menu (or the help/empty screen), and return the chosen item's
 * name WITHOUT extension (pointer valid until the next picker_run), or NULL if the user quit. */
const char *picker_run(void);

/* For the host's content loop: pump SDL events + detect the START+BACK hold. Sets *want_quit when
 * the window/app is closing and *want_menu when the user asked to return to the picker. */
void picker_pump(int *want_quit, int *want_menu);
SDL_GameController *picker_pad(int port); /* port 0 or 1, or NULL if not connected */

/* Rotation-aware present of a raw framebuffer. rot_ccw is libretro's 90-degree CCW step count
 * (0..3); pass 0 to disable. sdl_fmt is an SDL_PIXELFORMAT_*. */
void picker_present(const void *data, unsigned w, unsigned h, unsigned pitch,
                    Uint32 sdl_fmt, unsigned rot_ccw);

SDL_Renderer *picker_renderer(void);     /* for host-drawn screens, if needed */
void picker_message(const char *line1, const char *line2); /* ~1.6s on-screen message */

/* show a banner over the game for `ms` milliseconds (e.g. the quit combo); text "" clears it */
void picker_set_hint(const char *text, unsigned ms);

#endif /* PICKER_H */
