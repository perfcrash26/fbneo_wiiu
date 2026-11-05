/*
 * fbneo_player - a minimal SDL2 libretro frontend for the statically-linked FBNeo core,
 * so a game (default sfiii3) can actually be played and watched. Unlike bench.c (headless,
 * scripted input, hashes), this presents video to the screen, plays audio, and reads a live
 * controller. It links the full core via wut's linker, which handles the large core that
 * RetroArch's RPX layout cannot.
 *
 * It shows an on-screen game picker (SDL2_ttf, font bundled in the wuhb /content), reads the
 * rom directory from a config file it creates on first run, and loads/unloads each game so
 * you can return to the picker in-game with + and - (Start+Select).
 *
 * Paths on Wii U (fs: default device; standard per-app data dir):
 *   roms    <roms= in config>, default fs:/vol/external01/wiiu/fbneo/roms/<set>.zip
 *   config  fs:/vol/external01/wiiu/fbneo/player/config.txt  (created if missing)
 *   font    fs:/vol/content/Roboto-Regular.ttf               (bundled in the wuhb)
 */
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <time.h>
#include <SDL.h>
#include <SDL_ttf.h>
#include <dirent.h>
#include <sys/stat.h>
#include "libretro.h"
#ifdef __WIIU__
#include <coreinit/debug.h>
#endif

#ifdef __WIIU__
#define DATA_DIR  "fs:/vol/external01/wiiu/fbneo"	/* standard Wii U per-app data path */
#define FONT_FILE "fs:/vol/content/Roboto-Regular.ttf"	/* bundled in the wuhb */
#else
#define DATA_DIR  "."
#define FONT_FILE "Roboto-Regular.ttf"
#endif
#define ROMS_DIR    DATA_DIR "/roms"
#define SYS_DIR     DATA_DIR "/system"
#define PLAYER_DIR  DATA_DIR "/player"
#define SAVE_DIR    DATA_DIR "/player/saves"
#define CONFIG_FILE DATA_DIR "/player/config.txt"

static void plog(const char *fmt, ...)
{
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
#ifdef __WIIU__
    OSReport("[player] %s\n", buf);
#else
    fprintf(stderr, "[player] %s\n", buf);
#endif
}

/* A fixed wall clock so the core seeds exactly like the bench reference. */
time_t time(time_t *t) { time_t v = 1527811200; if (t) *t = v; return v; }

static enum retro_pixel_format pixfmt = RETRO_PIXEL_FORMAT_0RGB1555;
static const char *g_sys = SYS_DIR, *g_save = SAVE_DIR;
static unsigned rotation;

static SDL_Window   *win;
static SDL_Renderer *ren;
static SDL_Texture  *tex;
static unsigned      tex_w, tex_h;
static int           win_w = 1280, win_h = 720;
static SDL_AudioDeviceID adev;
static SDL_GameController *pads[2];	/* [0]=player 1, [1]=player 2 */
static int           want_quit;		/* quit the app */
static int           want_menu;		/* leave the running game, back to the picker */
static char          g_roms[768];	/* rom dir, from config.txt */
static TTF_Font     *g_font;
#define MAXSETS 1024
static char          sets[MAXSETS][48];
static int           nsets;

/* ---- libretro callbacks ---- */

static void core_log(enum retro_log_level level, const char *fmt, ...)
{
    char buf[1024];
    va_list ap;
    if (level < RETRO_LOG_WARN)
        return;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    buf[strcspn(buf, "\r\n")] = 0;
    plog("core: %s", buf);
}

static bool environment(unsigned cmd, void *data)
{
    switch (cmd) {
    case RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY: *(const char **)data = g_sys; return true;
    case RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY:   *(const char **)data = g_save; return true;
    case RETRO_ENVIRONMENT_SET_PIXEL_FORMAT:     pixfmt = *(const enum retro_pixel_format *)data; return true;
    case RETRO_ENVIRONMENT_GET_LOG_INTERFACE:    ((struct retro_log_callback *)data)->log = core_log; return true;
    case RETRO_ENVIRONMENT_GET_AUDIO_VIDEO_ENABLE: *(int *)data = 3; return true;
    case RETRO_ENVIRONMENT_SET_ROTATION:         rotation = *(const unsigned *)data; return true;
    case RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE:  *(bool *)data = false; return true;
    case RETRO_ENVIRONMENT_GET_CORE_OPTIONS_VERSION: *(unsigned *)data = 0; return true;
    case RETRO_ENVIRONMENT_GET_VARIABLE:         ((struct retro_variable *)data)->value = NULL; return false;
    case RETRO_ENVIRONMENT_SET_VARIABLES:
    case RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS:
    case RETRO_ENVIRONMENT_SET_CONTROLLER_INFO:
    case RETRO_ENVIRONMENT_SET_GEOMETRY:
    case RETRO_ENVIRONMENT_SET_SYSTEM_AV_INFO:
    case RETRO_ENVIRONMENT_SET_MESSAGE:
    case RETRO_ENVIRONMENT_SET_SUBSYSTEM_INFO:
    case RETRO_ENVIRONMENT_SET_SERIALIZATION_QUIRKS:
    case RETRO_ENVIRONMENT_SET_MEMORY_MAPS:
        return true;
    default:
        return false;
    }
}

static Uint32 sdl_fmt_for(enum retro_pixel_format f)
{
    if (f == RETRO_PIXEL_FORMAT_XRGB8888) return SDL_PIXELFORMAT_ARGB8888;
    if (f == RETRO_PIXEL_FORMAT_RGB565)   return SDL_PIXELFORMAT_RGB565;
    return SDL_PIXELFORMAT_ARGB1555; /* 0RGB1555 */
}

static void video_refresh(const void *data, unsigned w, unsigned h, size_t pitch)
{
    SDL_Rect dst;
    int ww, wh;
    double sa, da;
    if (!data) return; /* duped frame */

    if (!tex || w != tex_w || h != tex_h) {
        if (tex) SDL_DestroyTexture(tex);
        tex = SDL_CreateTexture(ren, sdl_fmt_for(pixfmt), SDL_TEXTUREACCESS_STREAMING, w, h);
        tex_w = w; tex_h = h;
        plog("video %ux%u pitch=%u fmt=%d", w, h, (unsigned)pitch, (int)pixfmt);
    }
    SDL_UpdateTexture(tex, NULL, data, (int)pitch);

    SDL_GetRendererOutputSize(ren, &ww, &wh);
    /* letterbox keeping the game's aspect */
    sa = (double)w / (double)h;
    da = (double)ww / (double)wh;
    if (da > sa) { dst.h = wh; dst.w = (int)(wh * sa); dst.x = (ww - dst.w) / 2; dst.y = 0; }
    else         { dst.w = ww; dst.h = (int)(ww / sa); dst.x = 0; dst.y = (wh - dst.h) / 2; }

    SDL_RenderClear(ren);
    SDL_RenderCopy(ren, tex, NULL, &dst);
    SDL_RenderPresent(ren);
}

static void audio_sample(int16_t l, int16_t r)
{
    int16_t s[2] = { l, r };
    if (adev) SDL_QueueAudio(adev, s, sizeof(s));
}

static size_t audio_batch(const int16_t *d, size_t frames)
{
    if (adev) SDL_QueueAudio(adev, d, frames * 2 * sizeof(int16_t));
    return frames;
}

static void open_pads(void)
{
    int i;
    for (i = 0; i < SDL_NumJoysticks(); i++) {
        SDL_GameController *g;
        if (!SDL_IsGameController(i)) continue;
        g = SDL_GameControllerOpen(i);
        if (!g) continue;
        if (g == pads[0] || g == pads[1]) continue;	/* already assigned */
        if      (!pads[0]) pads[0] = g;
        else if (!pads[1]) pads[1] = g;
        else SDL_GameControllerClose(g);		/* only two players */
    }
    plog("controllers: p1=%d p2=%d", pads[0] != NULL, pads[1] != NULL);
}

static int exit_hold;

static void input_poll(void)
{
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        if (e.type == SDL_QUIT) want_quit = 1;
        else if (e.type == SDL_CONTROLLERDEVICEADDED) open_pads();
    }
    /* either player holds + and - together (Start+Select) ~0.5s to leave to the game picker */
    if ((pads[0] && SDL_GameControllerGetButton(pads[0], SDL_CONTROLLER_BUTTON_START)
                 && SDL_GameControllerGetButton(pads[0], SDL_CONTROLLER_BUTTON_BACK)) ||
        (pads[1] && SDL_GameControllerGetButton(pads[1], SDL_CONTROLLER_BUTTON_START)
                 && SDL_GameControllerGetButton(pads[1], SDL_CONTROLLER_BUTTON_BACK))) {
        if (++exit_hold >= 30) want_menu = 1;
    } else {
        exit_hold = 0;
    }
}

static int16_t input_state(unsigned port, unsigned device, unsigned index, unsigned id)
{
    SDL_GameController *g;
    SDL_GameControllerButton b = SDL_CONTROLLER_BUTTON_INVALID;
    (void)index;
    if (port > 1 || device != RETRO_DEVICE_JOYPAD)
        return 0;
    g = pads[port];
    if (!g)
        return 0;
    /* left analog stick doubles the d-pad for directions */
    {
        const int DZ = 12000;
        Sint16 ax = SDL_GameControllerGetAxis(g, SDL_CONTROLLER_AXIS_LEFTX);
        Sint16 ay = SDL_GameControllerGetAxis(g, SDL_CONTROLLER_AXIS_LEFTY);
        if (id == RETRO_DEVICE_ID_JOYPAD_LEFT  && ax < -DZ) return 1;
        if (id == RETRO_DEVICE_ID_JOYPAD_RIGHT && ax >  DZ) return 1;
        if (id == RETRO_DEVICE_ID_JOYPAD_UP    && ay < -DZ) return 1;
        if (id == RETRO_DEVICE_ID_JOYPAD_DOWN  && ay >  DZ) return 1;
    }
    if (id == RETRO_DEVICE_ID_JOYPAD_L2)
        return SDL_GameControllerGetAxis(g, SDL_CONTROLLER_AXIS_TRIGGERLEFT) > 8000;
    if (id == RETRO_DEVICE_ID_JOYPAD_R2)
        return SDL_GameControllerGetAxis(g, SDL_CONTROLLER_AXIS_TRIGGERRIGHT) > 8000;
    switch (id) {
    case RETRO_DEVICE_ID_JOYPAD_UP:     b = SDL_CONTROLLER_BUTTON_DPAD_UP; break;
    case RETRO_DEVICE_ID_JOYPAD_DOWN:   b = SDL_CONTROLLER_BUTTON_DPAD_DOWN; break;
    case RETRO_DEVICE_ID_JOYPAD_LEFT:   b = SDL_CONTROLLER_BUTTON_DPAD_LEFT; break;
    case RETRO_DEVICE_ID_JOYPAD_RIGHT:  b = SDL_CONTROLLER_BUTTON_DPAD_RIGHT; break;
    case RETRO_DEVICE_ID_JOYPAD_B:      b = SDL_CONTROLLER_BUTTON_A; break;
    case RETRO_DEVICE_ID_JOYPAD_A:      b = SDL_CONTROLLER_BUTTON_B; break;
    case RETRO_DEVICE_ID_JOYPAD_Y:      b = SDL_CONTROLLER_BUTTON_X; break;
    case RETRO_DEVICE_ID_JOYPAD_X:      b = SDL_CONTROLLER_BUTTON_Y; break;
    case RETRO_DEVICE_ID_JOYPAD_L:      b = SDL_CONTROLLER_BUTTON_LEFTSHOULDER; break;
    case RETRO_DEVICE_ID_JOYPAD_R:      b = SDL_CONTROLLER_BUTTON_RIGHTSHOULDER; break;
    case RETRO_DEVICE_ID_JOYPAD_START:  b = SDL_CONTROLLER_BUTTON_START; break;
    case RETRO_DEVICE_ID_JOYPAD_SELECT: b = SDL_CONTROLLER_BUTTON_BACK; break;
    case RETRO_DEVICE_ID_JOYPAD_L3:     b = SDL_CONTROLLER_BUTTON_LEFTSTICK; break;
    case RETRO_DEVICE_ID_JOYPAD_R3:     b = SDL_CONTROLLER_BUTTON_RIGHTSTICK; break;
    default: return 0;
    }
    return SDL_GameControllerGetButton(g, b) ? 1 : 0;
}

static void mkdirs(const char *path)
{
    char tmp[800];
    size_t i;
    snprintf(tmp, sizeof(tmp), "%s", path);
    for (i = 1; tmp[i]; i++)
        if (tmp[i] == '/') { tmp[i] = 0; mkdir(tmp, 0777); tmp[i] = '/'; }
    mkdir(tmp, 0777);
}

/* read the rom dir from config.txt; create the file with the default if it is missing */
static void load_config(void)
{
    FILE *f;
    char line[800];
    snprintf(g_roms, sizeof(g_roms), "%s", ROMS_DIR);
    f = fopen(CONFIG_FILE, "r");
    if (f) {
        while (fgets(line, sizeof(line), f)) {
            line[strcspn(line, "\r\n")] = 0;
            if (!strncmp(line, "roms=", 5) && line[5])
                snprintf(g_roms, sizeof(g_roms), "%s", line + 5);
        }
        fclose(f);
        plog("config roms=%s", g_roms);
    } else {
        mkdirs(PLAYER_DIR);
        f = fopen(CONFIG_FILE, "w");
        if (f) {
            fprintf(f, "# fbneo player config\nroms=%s\n", g_roms);
            fclose(f);
            plog("created %s", CONFIG_FILE);
        }
    }
}

static int set_cmp(const void *a, const void *b) { return strcmp((const char *)a, (const char *)b); }

static void scan_roms(void)
{
    DIR *d;
    struct dirent *e;
    nsets = 0;
    d = opendir(g_roms);
    if (!d) { plog("opendir %s failed", g_roms); return; }
    while ((e = readdir(d)) != NULL && nsets < MAXSETS) {
        const char *n = e->d_name;
        size_t L = strlen(n);
        if (L > 4 && n[L-4] == '.' &&
            (n[L-3]=='z'||n[L-3]=='Z') && (n[L-2]=='i'||n[L-2]=='I') && (n[L-1]=='p'||n[L-1]=='P') &&
            L - 4 < sizeof(sets[0])) {
            memcpy(sets[nsets], n, L - 4);
            sets[nsets][L - 4] = 0;
            nsets++;
        }
    }
    closedir(d);
    qsort(sets, nsets, sizeof(sets[0]), set_cmp);
    plog("scanned %d sets in %s", nsets, g_roms);
}

/* ---- simple on-screen UI (SDL2_ttf) ---- */

static void draw_text(int x, int y, SDL_Color c, const char *s)
{
    SDL_Surface *surf;
    SDL_Texture *t;
    SDL_Rect dst;
    if (!g_font || !s || !*s) return;
    surf = TTF_RenderUTF8_Blended(g_font, s, c);
    if (!surf) return;
    t = SDL_CreateTextureFromSurface(ren, surf);
    dst.x = x; dst.y = y; dst.w = surf->w; dst.h = surf->h;
    SDL_FreeSurface(surf);
    if (t) { SDL_RenderCopy(ren, t, NULL, &dst); SDL_DestroyTexture(t); }
}

static int pad_btn(SDL_GameControllerButton b)
{
    int i;
    for (i = 0; i < 2; i++)
        if (pads[i] && SDL_GameControllerGetButton(pads[i], b)) return 1;
    return 0;
}

static int pad_dir(int down)	/* d-pad or left stick, either player */
{
    int i;
    for (i = 0; i < 2; i++) {
        SDL_GameController *g = pads[i];
        if (!g) continue;
        if (SDL_GameControllerGetButton(g, down ? SDL_CONTROLLER_BUTTON_DPAD_DOWN : SDL_CONTROLLER_BUTTON_DPAD_UP)) return 1;
        {
            Sint16 ay = SDL_GameControllerGetAxis(g, SDL_CONTROLLER_AXIS_LEFTY);
            if (down ? (ay > 12000) : (ay < -12000)) return 1;
        }
    }
    return 0;
}

static void show_msg(const char *a, const char *b)
{
    SDL_Color c = { 255, 120, 120, 255 };
    Uint32 end = SDL_GetTicks() + 1600;
    SDL_Event e;
    while (SDL_GetTicks() < end) {
        while (SDL_PollEvent(&e)) {}
        SDL_SetRenderDrawColor(ren, 18, 18, 28, 255);
        SDL_RenderClear(ren);
        draw_text(80, 300, c, a);
        draw_text(80, 344, c, b);
        SDL_RenderPresent(ren);
        SDL_Delay(16);
    }
}

/* game picker: returns an index into sets[], or -1 to quit the app */
static int pick_set(void)
{
    const int VIS = 15, ROWH = 36, X = 90, Y0 = 130;
    SDL_Color head = {120,200,255,255}, white = {225,225,225,255}, hi = {255,215,70,255};
    int sel = 0, top = 0, a_prev = 1, qhold = 0, armed = 0;
    Uint32 nav_next = 0;
    char buf[96];
    SDL_Event ev;
    for (;;) {
        Uint32 now;
        int a;
        while (SDL_PollEvent(&ev)) {
            if (ev.type == SDL_QUIT) { want_quit = 1; return -1; }
            if (ev.type == SDL_CONTROLLERDEVICEADDED) open_pads();
        }
        /* hold + and - (once released) to quit the app */
        if (pad_btn(SDL_CONTROLLER_BUTTON_START) && pad_btn(SDL_CONTROLLER_BUTTON_BACK)) {
            if (armed && ++qhold >= 30) { want_quit = 1; return -1; }
        } else { qhold = 0; armed = 1; }

        now = SDL_GetTicks();
        if (nsets > 0 && now >= nav_next && (pad_dir(0) || pad_dir(1))) {
            sel = pad_dir(1) ? (sel + 1) % nsets : (sel - 1 + nsets) % nsets;
            nav_next = now + 140;
        }
        if (!pad_dir(0) && !pad_dir(1)) nav_next = 0;

        a = pad_btn(SDL_CONTROLLER_BUTTON_A) || pad_btn(SDL_CONTROLLER_BUTTON_B);
        if (a && !a_prev && nsets > 0) return sel;
        a_prev = a;

        if (sel < top) top = sel;
        if (sel >= top + VIS) top = sel - VIS + 1;

        SDL_SetRenderDrawColor(ren, 18, 18, 28, 255);
        SDL_RenderClear(ren);
        draw_text(X, 48, head, "FBNeo  -  pick a game   (A/B play, Up/Down or stick, hold +/- quit)");
        if (nsets == 0) {
            draw_text(X, Y0, white, "No .zip roms found. Put sets in the rom folder:");
            draw_text(X, Y0 + ROWH, white, g_roms);
        } else {
            int i;
            for (i = 0; i < VIS && top + i < nsets; i++) {
                int idx = top + i;
                snprintf(buf, sizeof(buf), "%s%s", idx == sel ? "> " : "   ", sets[idx]);
                draw_text(X, Y0 + i * ROWH, idx == sel ? hi : white, buf);
            }
            snprintf(buf, sizeof(buf), "%d / %d", sel + 1, nsets);
            draw_text(X, Y0 + VIS * ROWH + 10, head, buf);
        }
        SDL_RenderPresent(ren);
        SDL_Delay(16);
    }
}

static int load_and_play(const char *set)
{
    char path[800];
    struct retro_game_info game;
    struct retro_system_av_info av;
    SDL_AudioSpec want;

    pixfmt = RETRO_PIXEL_FORMAT_0RGB1555;
    want_menu = 0;
    snprintf(path, sizeof(path), "%s/%s.zip", g_roms, set);
    memset(&game, 0, sizeof(game));
    game.path = path;
    plog("loading %s", path);
    if (!retro_load_game(&game)) {
        plog("retro_load_game FAILED %s", set);
        show_msg("Failed to load:", set);
        return 0;
    }
    retro_get_system_av_info(&av);
    plog("av: %ux%u fps=%.2f rate=%.0f", av.geometry.base_width, av.geometry.base_height,
         av.timing.fps, av.timing.sample_rate);

    SDL_zero(want);
    want.freq = (int)av.timing.sample_rate;
    want.format = AUDIO_S16SYS;
    want.channels = 2;
    want.samples = 1024;
    adev = SDL_OpenAudioDevice(NULL, 0, &want, NULL, 0);
    if (adev) SDL_PauseAudioDevice(adev, 0);

    while (!want_quit && !want_menu) {
        Uint32 t0 = SDL_GetTicks(), dt, budget;
        input_poll();
        retro_run();
        budget = (Uint32)(1000.0 / (av.timing.fps > 1 ? av.timing.fps : 60.0));
        dt = SDL_GetTicks() - t0;
        if (dt < budget) SDL_Delay(budget - dt);
    }

    if (adev) { SDL_CloseAudioDevice(adev); adev = 0; }
    if (tex)  { SDL_DestroyTexture(tex); tex = 0; tex_w = tex_h = 0; }
    retro_unload_game();
    return 1;
}

int main(int argc, char **argv)
{
    (void)argc; (void)argv;
    plog("start");

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMECONTROLLER) != 0) {
        plog("SDL_Init failed: %s", SDL_GetError());
        return 1;
    }
    atexit(SDL_Quit);	/* release GX2 foreground on any exit so we return to the menu */
    win = SDL_CreateWindow("fbneo", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                           win_w, win_h, SDL_WINDOW_FULLSCREEN);
    if (!win) { plog("CreateWindow: %s", SDL_GetError()); return 1; }
    ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_ACCELERATED);
    if (!ren) { plog("CreateRenderer: %s", SDL_GetError()); return 1; }
    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "1");
    if (TTF_Init() == 0) {
        g_font = TTF_OpenFont(FONT_FILE, 26);
        if (!g_font) plog("font open failed (%s): %s", FONT_FILE, TTF_GetError());
    } else {
        plog("TTF_Init failed: %s", TTF_GetError());
    }
    open_pads();
    load_config();

    retro_set_environment(environment);
    retro_set_video_refresh(video_refresh);
    retro_set_audio_sample(audio_sample);
    retro_set_audio_sample_batch(audio_batch);
    retro_set_input_poll(input_poll);
    retro_set_input_state(input_state);
    retro_init();

    while (!want_quit) {
        int idx;
        scan_roms();
        idx = pick_set();
        if (idx < 0) break;
        load_and_play(sets[idx]);
    }

    plog("quit");
    retro_deinit();
    if (g_font) TTF_CloseFont(g_font);
    TTF_Quit();
    SDL_Quit();
    return 0;
}
