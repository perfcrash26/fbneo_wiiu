/*
 * fbneo_player - a minimal SDL2 libretro frontend for the statically-linked FBNeo core,
 * so a game (default sfiii3) can actually be played and watched. Unlike bench.c (headless,
 * scripted input, hashes), this presents video to the screen, plays audio, and reads a live
 * controller. It links the full core via wut's linker, which handles the large core that
 * RetroArch's RPX layout cannot.
 *
 * Paths on Wii U (fs: default device, like the bench):
 *   roms    fs:/vol/external01/wiiu/fbneo/roms/<set>.zip
 *   system  fs:/vol/external01/wiiu/fbneo/system
 *   set     fs:/vol/external01/wiiu/fbneo/player/set.txt  (one line; default "sfiii3")
 */
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <time.h>
#include <SDL.h>
#include "libretro.h"
#ifdef __WIIU__
#include <coreinit/debug.h>
#endif

#ifdef __WIIU__
#define ROMS_DIR "fs:/vol/external01/wiiu/fbneo/roms"
#define SYS_DIR  "fs:/vol/external01/wiiu/fbneo/system"
#define SAVE_DIR "fs:/vol/external01/wiiu/fbneo/player/saves"
#define SET_FILE "fs:/vol/external01/wiiu/fbneo/player/set.txt"
#else
#define ROMS_DIR "roms"
#define SYS_DIR  "system"
#define SAVE_DIR "saves"
#define SET_FILE "set.txt"
#endif

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
static SDL_GameController *pad;
static int           want_quit;

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

static void open_pad(void)
{
    int i;
    for (i = 0; i < SDL_NumJoysticks(); i++) {
        if (SDL_IsGameController(i)) { pad = SDL_GameControllerOpen(i); if (pad) { plog("controller %d opened", i); return; } }
    }
}

static void input_poll(void)
{
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        if (e.type == SDL_QUIT) want_quit = 1;
        else if (e.type == SDL_CONTROLLERDEVICEADDED && !pad) open_pad();
    }
}

static int16_t input_state(unsigned port, unsigned device, unsigned index, unsigned id)
{
    SDL_GameControllerButton b = SDL_CONTROLLER_BUTTON_INVALID;
    (void)index;
    if (port != 0 || device != RETRO_DEVICE_JOYPAD || !pad)
        return 0;
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
    /* L2/R2 from triggers */
    if (id == RETRO_DEVICE_ID_JOYPAD_L2)
        return SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_TRIGGERLEFT) > 8000;
    if (id == RETRO_DEVICE_ID_JOYPAD_R2)
        return SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_TRIGGERRIGHT) > 8000;
    return SDL_GameControllerGetButton(pad, b) ? 1 : 0;
}

static void read_set(char *out, size_t n)
{
    FILE *f;
    snprintf(out, n, "sfiii3");
    f = fopen(SET_FILE, "r");
    if (f) {
        if (fgets(out, (int)n, f)) out[strcspn(out, "\r\n")] = 0;
        fclose(f);
    }
}

int main(int argc, char **argv)
{
    char set[64], path[700];
    struct retro_game_info game;
    struct retro_system_av_info av;
    SDL_AudioSpec want;
    (void)argc; (void)argv;

    plog("start");
    read_set(set, sizeof(set));
    plog("set=%s", set);

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMECONTROLLER) != 0) {
        plog("SDL_Init failed: %s", SDL_GetError());
        return 1;
    }
    win = SDL_CreateWindow("fbneo", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                           win_w, win_h, SDL_WINDOW_FULLSCREEN);
    if (!win) { plog("CreateWindow: %s", SDL_GetError()); return 1; }
    ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_ACCELERATED);
    if (!ren) { plog("CreateRenderer: %s", SDL_GetError()); return 1; }
    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "1");
    open_pad();

    retro_set_environment(environment);
    retro_set_video_refresh(video_refresh);
    retro_set_audio_sample(audio_sample);
    retro_set_audio_sample_batch(audio_batch);
    retro_set_input_poll(input_poll);
    retro_set_input_state(input_state);
    retro_init();

    snprintf(path, sizeof(path), "%s/%s.zip", ROMS_DIR, set);
    memset(&game, 0, sizeof(game));
    game.path = path;
    plog("loading %s", path);
    if (!retro_load_game(&game)) { plog("retro_load_game FAILED"); return 1; }

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
    else plog("audio open failed: %s", SDL_GetError());

    plog("entering run loop");
    while (!want_quit) {
        Uint32 t0 = SDL_GetTicks(), dt, budget;
        input_poll();
        retro_run();
        /* pace to the core's fps (video_refresh already presented) */
        budget = (Uint32)(1000.0 / (av.timing.fps > 1 ? av.timing.fps : 60.0));
        dt = SDL_GetTicks() - t0;
        if (dt < budget) SDL_Delay(budget - dt);
    }

    plog("quit");
    retro_unload_game();
    retro_deinit();
    if (adev) SDL_CloseAudioDevice(adev);
    SDL_Quit();
    return 0;
}
