/*
 * fbneo_player - a minimal SDL2 libretro frontend for the statically-linked FBNeo core, so a game
 * can actually be played. The generic frontend shell (window, controllers, config, the on-screen
 * picker, the rotation-aware blit, the help screen) lives in picker/ and has no FBNeo/libretro
 * knowledge; this file is the thin glue: the libretro callbacks and the per-game load/run loop.
 *
 * Paths on Wii U:  roms  <content= in config>, default fs:/vol/external01/wiiu/fbneo/roms/<set>.zip
 *                  config fs:/vol/external01/wiiu/fbneo/config.txt   (created on first run)
 *                  font   fs:/vol/content/Roboto-Regular.ttf         (bundled in the wuhb)
 */
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <time.h>
#include <sys/stat.h>
#include <SDL.h>
#include "libretro.h"
#include "picker/picker.h"
#ifdef __WIIU__
#include <coreinit/time.h>
#include <whb/log.h>
#include <whb/log_cafe.h>
#include <whb/log_udp.h>
static OSTime g_log_epoch;	/* set in main() before the first plog() */
static void log_udp_deinit(void) { WHBLogUdpDeinit(); }	/* void wrapper for atexit */
#endif

#ifdef __WIIU__
#define DATA_DIR  "fs:/vol/external01/wiiu/fbneo"	/* standard Wii U per-app data path */
#define FONT_FILE "fs:/vol/content/Roboto-Regular.ttf"	/* bundled in the wuhb */
#else
#define DATA_DIR  "."
#define FONT_FILE "picker/Roboto-Regular.ttf"
#endif
#define SYS_DIR  DATA_DIR "/system"
#define SAVE_DIR DATA_DIR "/player/saves"

static void plog(const char *fmt, ...)
{
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
#ifdef __WIIU__
    /* Tagged "[fbneo ...]" like bench.c so the shared UDP listener (port 4405) can tell these
     * lines apart; WHBLogPrintf goes to both the Cafe/OSReport console and the UDP broadcast. */
    {
        uint64_t ms = OSTicksToMilliseconds(OSGetSystemTime() - g_log_epoch);
        WHBLogPrintf("[fbneo +%u.%03u] player: %s", (unsigned)(ms / 1000), (unsigned)(ms % 1000), buf);
    }
#else
    fprintf(stderr, "[player] %s\n", buf);
#endif
}

/* A fixed wall clock so the core seeds exactly like the bench reference. */
time_t time(time_t *t) { time_t v = 1527811200; if (t) *t = v; return v; }

static enum retro_pixel_format pixfmt = RETRO_PIXEL_FORMAT_0RGB1555;
static const char *g_sys = SYS_DIR, *g_save = SAVE_DIR;
static unsigned rotation;		/* libretro SET_ROTATION (90-degree CCW steps) */
static SDL_AudioDeviceID adev;
static int want_quit, want_menu;

extern int nFrameskip;   /* FBNeo core: render 1 of every nFrameskip frames (default 1 = every frame) */
static int g_frameskip;  /* config "frameskip=N": skip N renders between shown frames (0 = off) */

extern const char *fbneo_system(const char *name);	/* C++ shim, fbneo_names.cpp */

/* ---- libretro callbacks ---- */

static void core_log(enum retro_log_level level, const char *fmt, ...)
{
    char buf[1024];
    va_list ap;
    if (level < RETRO_LOG_WARN) return;
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
    picker_present(data, w, h, (unsigned)pitch, sdl_fmt_for(pixfmt), picker_rotate() ? rotation : 0);
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

static void input_poll(void) { picker_pump(&want_quit, &want_menu); }

static int16_t input_state(unsigned port, unsigned device, unsigned index, unsigned id)
{
    SDL_GameController *g;
    SDL_GameControllerButton b = SDL_CONTROLLER_BUTTON_INVALID;
    (void)index;
    if (port > 1 || device != RETRO_DEVICE_JOYPAD) return 0;
    g = picker_pad((int)port);
    if (!g) return 0;
    {   /* left analog stick doubles the d-pad for directions */
        const int DZ = 12000;
        Sint16 ax = SDL_GameControllerGetAxis(g, SDL_CONTROLLER_AXIS_LEFTX);
        Sint16 ay = SDL_GameControllerGetAxis(g, SDL_CONTROLLER_AXIS_LEFTY);
        if (id == RETRO_DEVICE_ID_JOYPAD_LEFT  && ax < -DZ) return 1;
        if (id == RETRO_DEVICE_ID_JOYPAD_RIGHT && ax >  DZ) return 1;
        if (id == RETRO_DEVICE_ID_JOYPAD_UP    && ay < -DZ) return 1;
        if (id == RETRO_DEVICE_ID_JOYPAD_DOWN  && ay >  DZ) return 1;
    }
    if (id == RETRO_DEVICE_ID_JOYPAD_L2) return SDL_GameControllerGetAxis(g, SDL_CONTROLLER_AXIS_TRIGGERLEFT) > 8000;
    if (id == RETRO_DEVICE_ID_JOYPAD_R2) return SDL_GameControllerGetAxis(g, SDL_CONTROLLER_AXIS_TRIGGERRIGHT) > 8000;
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

/* ---- save states (FBNeo retro_serialize), per game at <data_dir>/states/<set>.state ----
 * zlib-compressed: CV1000 states embed the 128MB epic12 framebuffer (mostly empty), which both
 * fills the card and makes the SD write take ~10s uncompressed. zlib (linked from the fbneo core;
 * declared here to skip a header-path dependency) shrinks it to a few MB -> near-instant save/load. */
extern int compress2(unsigned char *dest, unsigned long *destLen,
                     const unsigned char *source, unsigned long sourceLen, int level);
extern int uncompress(unsigned char *dest, unsigned long *destLen,
                     const unsigned char *source, unsigned long sourceLen);
extern unsigned long compressBound(unsigned long sourceLen);
#define STATE_MAGIC 0x46425331u   /* "FBS1" - compressed state file */

static void state_path(const char *set, char *out, size_t n)
{
    char dir[700];
    snprintf(dir, sizeof(dir), "%s/states", DATA_DIR);
    mkdir(dir, 0777);   /* ok if it already exists */
    snprintf(out, n, "%s/%s.state", dir, set);
}

static void do_savestate(const char *set)
{
    size_t sz = retro_serialize_size();
    unsigned char *buf = NULL, *cbuf = NULL;
    unsigned long clen;
    char p[800];
    FILE *f;
    if (!sz) { picker_message("Save states not supported:", set); return; }
    buf = malloc(sz);
    if (!buf) return;
    if (!retro_serialize(buf, sz)) { picker_message("State save failed:", set); free(buf); return; }
    clen = compressBound(sz);
    cbuf = malloc(clen);
    if (cbuf && compress2(cbuf, &clen, buf, sz, 1) == 0) {   /* level 1 = fast; the data is very sparse */
        state_path(set, p, sizeof(p));
        f = fopen(p, "wb");
        if (f) {
            unsigned int hdr[3] = { STATE_MAGIC, (unsigned int)sz, (unsigned int)clen };
            fwrite(hdr, sizeof(hdr), 1, f);
            fwrite(cbuf, 1, clen, f);
            fclose(f);
            picker_message("State saved:", set);
            plog("state saved %s (%u -> %lu b)", set, (unsigned)sz, clen);
        } else picker_message("State save failed (write):", set);
    } else picker_message("State compress failed:", set);
    free(buf); free(cbuf);
}

static void do_loadstate(const char *set)
{
    char p[800];
    FILE *f;
    unsigned int hdr[3];
    unsigned char *buf = NULL, *cbuf = NULL;
    unsigned long dlen;
    state_path(set, p, sizeof(p));
    f = fopen(p, "rb");
    if (!f) { picker_message("No saved state:", set); return; }
    if (fread(hdr, sizeof(hdr), 1, f) != 1 || hdr[0] != STATE_MAGIC) {
        picker_message("Old/invalid state (re-save):", set); fclose(f); return;
    }
    buf  = malloc(hdr[1]);   /* original size */
    cbuf = malloc(hdr[2]);   /* compressed size */
    dlen = hdr[1];
    if (buf && cbuf && fread(cbuf, 1, hdr[2], f) == (size_t)hdr[2] &&
        uncompress(buf, &dlen, cbuf, hdr[2]) == 0 && dlen == hdr[1] &&
        retro_unserialize(buf, hdr[1]))
        { picker_message("State loaded:", set); plog("state loaded %s (%u b)", set, hdr[1]); }
    else picker_message("State load failed:", set);
    free(buf); free(cbuf); fclose(f);
}

/* In-game: hold BOTH triggers (ZL+ZR), then Y = quicksave, A = quickload. Edge-detected; the
 * both-triggers gate makes an accidental trigger during play very unlikely. */
static void savestate_poll(const char *set)
{
    SDL_GameController *g = picker_pad(0);
    static int prev_save, prev_load;
    int save = 0, load = 0;
    if (g &&
        SDL_GameControllerGetAxis(g, SDL_CONTROLLER_AXIS_TRIGGERLEFT)  > 16000 &&
        SDL_GameControllerGetAxis(g, SDL_CONTROLLER_AXIS_TRIGGERRIGHT) > 16000) {
        save = SDL_GameControllerGetButton(g, SDL_CONTROLLER_BUTTON_Y);
        load = SDL_GameControllerGetButton(g, SDL_CONTROLLER_BUTTON_A);
    }
    if (save && !prev_save) do_savestate(set);
    if (load && !prev_load) do_loadstate(set);
    prev_save = save; prev_load = load;
}

/* ---- one game ---- */

static int load_and_play(const char *set)
{
    char path[800];
    struct retro_game_info game;
    struct retro_system_av_info av;
    SDL_AudioSpec want;

    pixfmt = RETRO_PIXEL_FORMAT_0RGB1555;
    want_menu = 0;
    snprintf(path, sizeof(path), "%s/%s.zip", picker_content_dir(), set);
    memset(&game, 0, sizeof(game));
    game.path = path;
    plog("loading %s", path);
    if (!retro_load_game(&game)) {
        plog("retro_load_game FAILED %s", set);
        picker_message("Failed to load (wrong romset version?):", set);
        return 0;
    }
    retro_get_system_av_info(&av);
    plog("av: %ux%u fps=%.2f rate=%.0f", av.geometry.base_width, av.geometry.base_height,
         av.timing.fps, av.timing.sample_rate);
    nFrameskip = g_frameskip + 1;   /* set per-game after load; skips the core draw on skipped frames */

    SDL_zero(want);
    want.freq = (int)av.timing.sample_rate;
    want.format = AUDIO_S16SYS;
    want.channels = 2;
    want.samples = 1024;
    adev = SDL_OpenAudioDevice(NULL, 0, &want, NULL, 0);   /* opens paused; started after a prebuffer */

    /* Sync emulation to the audio queue. The old loop paced video to a wall-clock timer while
     * audio played off the independent 48kHz hardware clock; the two drift and the small queue
     * starves, which is what makes the sound buzzy/crackly. Instead: build a short prebuffer,
     * then let the queue depth pace the frame loop so the device never underruns. */
    {
        int    rate = want.freq > 1 ? want.freq : 48000;
        double fps  = av.timing.fps > 1 ? av.timing.fps : 60.0;
        Uint32 frame_bytes  = (Uint32)(rate / fps + 1) * want.channels * sizeof(int16_t);
        Uint32 target_bytes = frame_bytes * 4;   /* ~4 frames (~67 ms) of headroom */
        int    audio_on = 0, warm = 0;

        while (!want_quit && !want_menu) {
            Uint32 t0 = SDL_GetTicks();
            input_poll();
            savestate_poll(set);
            retro_run();

            if (adev) {
                if (!audio_on && ++warm >= 4) { SDL_PauseAudioDevice(adev, 0); audio_on = 1; }
                /* if we've run ahead of the target depth, wait for the device to drain back to it;
                 * this locks the loop to the audio clock. A game that can't keep up never gets ahead,
                 * so it runs flat out (frameskip helps). Guard caps the wait so a bad reading can't stall. */
                for (int guard = 0; audio_on && !want_quit && !want_menu && guard < 40 &&
                                    SDL_GetQueuedAudioSize(adev) > target_bytes; guard++)
                    SDL_Delay(1);
            } else {
                Uint32 budget = (Uint32)(1000.0 / fps), dt = SDL_GetTicks() - t0;
                if (dt < budget) SDL_Delay(budget - dt);   /* no audio device: wall-clock fallback */
            }
        }
    }

    if (adev) { SDL_CloseAudioDevice(adev); adev = 0; }
    retro_unload_game();
    return 1;
}

int main(int argc, char **argv)
{
    static const char *const fbneo_controls[] = {
        "Move: D-pad or left stick",
        "Buttons: A B X Y face, L R shoulders, L2 R2 triggers",
        "Start: +   Coin/Select: -",
        "Return to this menu: hold + and - together",
        "Save state: hold ZL+ZR + Y    Load state: hold ZL+ZR + A",
        "2 players: GamePad, Pro Controller, or Wiimote+Classic",
        NULL
    };
    static const picker_cfg cfg = {
        "FBNeo", "rom", DATA_DIR, FONT_FILE, "zip", fbneo_system,
        "Romsets must match FBNeo v1.0.0.3 (~Oct-2026 set); Neo Geo needs neogeo.zip",
        fbneo_controls
    };
    char autoload[64] = {0};
    int autoloaded = 0;
    int rc;
    (void)argc; (void)argv;
#ifdef __WIIU__
    g_log_epoch = OSGetSystemTime();
    WHBLogCafeInit();	/* mirror log lines to OSReport */
    WHBLogUdpInit();	/* ...and broadcast them to the shared UDP listener (4405) */
    atexit(log_udp_deinit);
#endif
    plog("start");

    rc = picker_init(&cfg);
    if (rc) { plog("picker_init failed (%d): %s", rc, SDL_GetError()); return 1; }

    g_frameskip = picker_cfg_int("frameskip", 0);   /* config frameskip=N (0=off); modest on CPU-bound games */
    if (g_frameskip < 0) g_frameskip = 0;
    if (g_frameskip > 9) g_frameskip = 9;
    plog("frameskip=%d", g_frameskip);

    retro_set_environment(environment);
    retro_set_video_refresh(video_refresh);
    retro_set_audio_sample(audio_sample);
    retro_set_audio_sample_batch(audio_batch);
    retro_set_input_poll(input_poll);
    retro_set_input_state(input_state);
    retro_init();

    snprintf(autoload, sizeof(autoload), "%s", picker_autoload());	/* boot straight into one game, once */

    while (!want_quit) {
        const char *set;
        /* autoload once; a separate flag (not clearing the buffer, which `set` aliases) prevents re-boot */
        if (autoload[0] && !autoloaded) { set = autoload; autoloaded = 1; }
        else {
            set = picker_run();
            if (!set) break;
        }
        load_and_play(set);
    }

    plog("quit");
    retro_deinit();
    picker_quit();
    return 0;
}
