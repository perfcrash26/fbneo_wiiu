/*
 * fbneo_player - a minimal SDL2 libretro frontend for the statically-linked FBNeo core, so a game
 * can actually be played. The generic frontend shell (window, controllers, config, the on-screen
 * picker, the rotation-aware blit, the help screen) lives in picker/ and has no FBNeo/libretro
 * knowledge; this file is the thin glue: the libretro callbacks and the per-game load/run loop.
 *
 * Paths on Wii U:  roms  <content= in config>, default fs:/vol/external01/wiiu/fbneo_player/roms/<set>.zip
 *                  config fs:/vol/external01/wiiu/fbneo_player/config.txt   (created on first run)
 *                  font   fs:/vol/content/Roboto-Regular.ttf         (bundled in the wuhb)
 */
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <stdarg.h>
#include <sys/stat.h>
#include <SDL.h>
#include <coreinit/time.h>
#include <gx2/display.h>
#include <gx2/enum.h>
#include <gx2/event.h>
#include <gx2/swap.h>
#include <whb/log.h>
#include <whb/log_cafe.h>
#include "libretro.h"
#include "picker/picker.h"

#define DATA_DIR    "fs:/vol/external01/wiiu/fbneo_player"	/* standard Wii U per-app data path */
#define FONT_FILE   "fs:/vol/content/Roboto-Regular.ttf"	/* bundled in the wuhb */
#define HISCORE_SRC "fs:/vol/content/hiscore.dat"	/* FBNeo's metadata/hiscore.dat, bundled in the wuhb */
#define SYS_DIR     DATA_DIR "/system"
#define SAVE_DIR    DATA_DIR "/saves"
#define STATE_DIR   DATA_DIR "/states"
#define FBNEO_WIIU_VER "1.0.0.3-U1"	/* shown in the picker header; tracks the release tag */

/* ---- logging: "[fbneo +s.ms] player: ..." to OSReport (dev builds also log over UDP) ---- */

static OSTime g_log_epoch;	/* set in main() before the first plog() */

static void plog(const char *fmt, ...)
{
    char buf[512];
    va_list ap;
    uint64_t ms = OSTicksToMilliseconds(OSGetSystemTime() - g_log_epoch);
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    WHBLogPrintf("[fbneo +%u.%03u] player: %s", (unsigned)(ms / 1000), (unsigned)(ms % 1000), buf);
}

/* ---- core and shim interfaces ---- */

extern int nFrameskip;          /* FBNeo core: render 1 of every nFrameskip frames (default 1 = every frame) */
extern int g_blit_split;        /* epic12.cpp: CV1000 blit on two cores (config blitsplit, default 1) */
extern int g_comp_split;        /* epic12.cpp: 32-bit composite on the blit cores (config compsplit, default 1) */
extern int g_ymz_async;         /* ymz770.cpp: YMZ770 rendering on core 0 (config ymzasync, default 1) */
extern const char *fbneo_system(const char *name);	/* fbneo_names.cpp: system label for the list */
extern const char *fbneo_title(const char *name);	/* fbneo_names.cpp: full game name */
extern int fbneo_is_cv1000(const char *name);		/* fbneo_names.cpp: Cave CV1000 hardware */

/* ---- settings (config.txt, read once in config_load) ---- */

static int g_frameskip;         /* frameskip=N: skip N renders between shown frames (0 = off) */
/* autoskip=N (0 = off): libretro audio-driven frameskip. The core skips its draw (composite + present)
 * while the audio queue is below N% of the target depth; emulation itself is never skipped. */
static int g_autoskip;
static char g_autoskip_thr[8];
/* depth32=auto|1|0, auto = on for CV1000 only: there it skips the 16-bit composite; other drivers' 32-bit
 * frames leave the alpha byte 0, which the Wii U SDL renderer shows as black */
static int g_depth32_cfg = -1;
static int g_vsync_cfg = -1;    /* vsync=auto|1|0 (-1 = auto) */
static int g_hiscores = 1;      /* hiscores=1|0: core option fbneo-hiscores (needs system/fbneo/hiscore.dat) */
static int g_aspect_auto = 1;   /* aspect=auto|pixel: the game's intended aspect (4:3) or square pixels */

/* ---- per-game state ---- */

static enum retro_pixel_format pixfmt = RETRO_PIXEL_FORMAT_0RGB1555;
static unsigned rotation;       /* libretro SET_ROTATION (90-degree CCW steps) */
static int g_depth32;           /* core option fbneo-allow-depth-32 for this game */
static int g_vsync = 1;         /* vsync pacing in effect for this game */
static double g_game_fps;       /* this game's own refresh rate */
static Uint32 g_target_bytes;   /* audio queue target */
static SDL_AudioDeviceID adev;
static retro_audio_buffer_status_callback_t g_buf_status_cb;
static unsigned g_frame_no;     /* frames run, for the autofire phase */
static int want_quit, want_menu;

/* ---- high scores ---- */

/* hiscore.dat goes where the core looks (system/fbneo/); copied from the wuhb once, so a user's own
 * newer copy is kept */
static void hiscore_install(void)
{
    const char *dst = SYS_DIR "/fbneo/hiscore.dat";
    struct stat st;
    FILE *in, *out;
    char buf[4096];
    size_t n;
    mkdir(SYS_DIR "/fbneo", 0777);
    if (stat(dst, &st) == 0) return;
    in = fopen(HISCORE_SRC, "rb");
    if (!in) { plog("hiscore: no bundled hiscore.dat"); return; }
    out = fopen(dst, "wb");
    if (out) {
        while ((n = fread(buf, 1, sizeof(buf), in)) > 0) fwrite(buf, 1, n, out);
        fclose(out);
        plog("hiscore: installed %s", dst);
    }
    fclose(in);
}

/* ---- libretro environment ---- */

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
    case RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY: *(const char **)data = SYS_DIR; return true;
    case RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY:   *(const char **)data = SAVE_DIR; return true;
    case RETRO_ENVIRONMENT_SET_PIXEL_FORMAT:     pixfmt = *(const enum retro_pixel_format *)data; return true;
    case RETRO_ENVIRONMENT_GET_LOG_INTERFACE:    ((struct retro_log_callback *)data)->log = core_log; return true;
    case RETRO_ENVIRONMENT_GET_AUDIO_VIDEO_ENABLE: *(int *)data = 3; return true;
    case RETRO_ENVIRONMENT_SET_ROTATION:         rotation = *(const unsigned *)data; return true;
    case RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE:  *(bool *)data = false; return true;
    case RETRO_ENVIRONMENT_GET_CORE_OPTIONS_VERSION: *(unsigned *)data = 0; return true;
    case RETRO_ENVIRONMENT_GET_VARIABLE: {
        struct retro_variable *v = (struct retro_variable *)data;
        v->value = NULL;
        if (g_autoskip && !strcmp(v->key, "fbneo-frameskip-type")) v->value = "Manual";
        else if (g_autoskip && !strcmp(v->key, "fbneo-frameskip-manual-threshold")) v->value = g_autoskip_thr;
        else if (!strcmp(v->key, "fbneo-allow-depth-32")) v->value = g_depth32 ? "enabled" : "disabled";
        else if (!strcmp(v->key, "fbneo-hiscores")) v->value = g_hiscores ? "enabled" : "disabled";
        return v->value != NULL;
    }
    case RETRO_ENVIRONMENT_GET_SAVESTATE_CONTEXT:
        /* our save states are ordinary ones (no runahead/rewind): lets the core keep hiscore.dat working */
        if (data) *(int *)data = RETRO_SAVESTATE_CONTEXT_NORMAL;
        return true;
    case RETRO_ENVIRONMENT_SET_AUDIO_BUFFER_STATUS_CALLBACK:
        /* only claim support with autoskip on: otherwise the core ignores the fixed frameskip= setting */
        g_buf_status_cb = data ? ((const struct retro_audio_buffer_status_callback *)data)->callback : NULL;
        return g_autoskip != 0;
    case RETRO_ENVIRONMENT_SET_MINIMUM_AUDIO_LATENCY:
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

/* ---- pacing ----
 * Two modes, chosen per game (vsync=auto|1|0):
 *  - audio-queue pacing (vsync off): after a short prebuffer the loop waits whenever the audio queue is above
 *    its target, so emulation runs at the game's own rate, locked to the audio clock.
 *  - vsync pacing: swap interval 1, and before presenting frame N we wait until frame N-1 has flipped (never
 *    for frame N itself, so a slow frame isn't rounded down to 30 fps). The audio rate is nudged by up to
 *    +-VSYNC_TOL from the queue fill so the queue neither drains nor grows; the queue wait stays only as a
 *    safety cap. This absorbs a small game/TV mismatch only, so auto turns vsync on when the game's rate is
 *    within VSYNC_TOL of the TV's (CPS1/2/3 ~59.6 Hz, CV1000 60 Hz) and off otherwise (Neo Geo 59.19 Hz,
 *    54-57 Hz sets, 50 Hz TV modes). */
#define VSYNC_TOL        0.007   /* max game/TV rate mismatch vsync pacing absorbs */
#define NUDGE_GAIN       0.01    /* audio-rate change per unit of queue error; 0.7% mismatch settles at ~1.7x target */
#define QUEUE_FRAMES     4       /* audio queue target, in frames of audio (~67 ms) */
#define PREBUFFER_FRAMES 4       /* frames queued before the audio device starts */
#define VSYNC_CAP_X      3       /* vsync pacing: wait only when the queue is above this many targets */
#define QUEUE_WAIT_MS    40      /* longest wait for the queue to drain per frame */
#define AUDIO_SAMPLES    1024    /* SDL audio device buffer, in sample frames */

static double tv_refresh_hz(void)
{
    return GX2GetSystemTVScanMode() == GX2_TV_SCAN_MODE_576I ? 50.0 : 59.94;   /* 576i = the only 50 Hz mode */
}

static void vsync_choose(double fps)   /* sets g_vsync and g_game_fps for a game running at fps */
{
    double tv = tv_refresh_hz(), d = fps / tv - 1.0;
    g_game_fps = fps;
    g_vsync = g_vsync_cfg >= 0 ? g_vsync_cfg : (d <= VSYNC_TOL && d >= -VSYNC_TOL);
    plog("vsync=%d (%s; game %.3f Hz, TV %.2f Hz, %+.2f%%)", g_vsync,
         g_vsync_cfg < 0 ? "auto" : "config", fps, tv, 100.0 * d);
}

static void vsync_wait_prev(void)
{
    uint32_t swaps = 0, flips = 0;
    OSTime lf = 0, lv = 0;
    for (int guard = 0; guard < 4; guard++) {
        GX2GetSwapStatus(&swaps, &flips, &lf, &lv);
        if (flips >= swaps) return;       /* nothing pending: the previous frame is on screen */
        GX2WaitForVsync();
    }
}

/* ---- video ---- */

static Uint32 sdl_fmt_for(enum retro_pixel_format f)
{
    if (f == RETRO_PIXEL_FORMAT_XRGB8888) return SDL_PIXELFORMAT_ARGB8888;
    if (f == RETRO_PIXEL_FORMAT_RGB565)   return SDL_PIXELFORMAT_RGB565;
    return SDL_PIXELFORMAT_ARGB1555; /* 0RGB1555 */
}

static void video_refresh(const void *data, unsigned w, unsigned h, size_t pitch)
{
    if (g_vsync) vsync_wait_prev();
    picker_present(data, w, h, (unsigned)pitch, sdl_fmt_for(pixfmt), picker_rotate() ? rotation : 0);
}

/* ---- audio ---- */

/* audio rate nudge (vsync pacing): linear-interpolating resampler */
#define RS_MAX_FRAMES 4096       /* output buffer, in sample frames */
#define RS_MAX_IN     4000       /* larger batches skip the nudge: stretched by <=0.8% they must fit RS_MAX_FRAMES */
static double rs_pos;
static int16_t rs_last[2];
static int16_t rs_out[2 * RS_MAX_FRAMES];

static size_t rs_resample(const int16_t *d, size_t n, double ratio, const int16_t **out)
{
    const double step = 1.0 / ratio;   /* input samples per output sample */
    size_t o = 0;
    double p = rs_pos;
    while (p < (double)n && o < RS_MAX_FRAMES) {
        int i = (int)p; double f = p - i;
        for (int c = 0; c < 2; c++) {
            int a = i ? d[2 * (i - 1) + c] : rs_last[c], b = d[2 * i + c];
            rs_out[2 * o + c] = (int16_t)(a + (b - a) * f);
        }
        o++; p += step;
    }
    rs_pos = p - (double)n;
    if (n) { rs_last[0] = d[2 * (n - 1)]; rs_last[1] = d[2 * (n - 1) + 1]; }
    *out = rs_out;
    return o;
}

static void audio_sample(int16_t l, int16_t r)
{
    int16_t s[2] = { l, r };
    if (adev) SDL_QueueAudio(adev, s, sizeof(s));
}

static size_t audio_batch(const int16_t *d, size_t frames)
{
    if (adev && g_vsync && g_target_bytes && frames <= RS_MAX_IN) {
        double fill = (double)SDL_GetQueuedAudioSize(adev), tgt = (double)g_target_bytes;
        double adj = NUDGE_GAIN * (tgt - fill) / tgt;   /* below target: stretch (more samples), above: shrink */
        if (adj > VSYNC_TOL + 0.001) adj = VSYNC_TOL + 0.001;
        if (adj < -VSYNC_TOL - 0.001) adj = -VSYNC_TOL - 0.001;
        const int16_t *o;
        size_t n = rs_resample(d, frames, 1.0 + adj, &o);
        SDL_QueueAudio(adev, o, n * 2 * sizeof(int16_t));
        return frames;
    }
    if (adev) SDL_QueueAudio(adev, d, frames * 2 * sizeof(int16_t));
    return frames;
}

/* ---- input: button remap + autofire ----
 * The pad's game buttons by name: a b x y l r zl zr l3 r3. "map_<pressed>=<acts as>" makes a button act as
 * another (e.g. map_y=a: Y does what A does; A still works unless it is remapped too). "turbo=a,x" gives
 * those buttons autofire while held, at turbo_hz presses per second (default 15). Both apply to both
 * players. + and - (Start/Coin) and the d-pad are not remappable. */
#define GAME_TRIGGER_ON 8000     /* ZL/ZR axis value that counts as a game-button press */
enum { PB_A, PB_B, PB_X, PB_Y, PB_L, PB_R, PB_ZL, PB_ZR, PB_L3, PB_R3, PB_N };
static const char *const pb_name[PB_N] = { "a", "b", "x", "y", "l", "r", "zl", "zr", "l3", "r3" };
static int g_map[PB_N];          /* pressed button -> the button it acts as */
static unsigned g_turbo_mask;    /* buttons with autofire */
static int g_turbo_hz = 15;
static unsigned g_turbo_period;  /* autofire period in frames, set per game */

static int pb_index(const char *s, size_t n)
{
    for (int i = 0; i < PB_N; i++) if (strlen(pb_name[i]) == n && !strncasecmp(s, pb_name[i], n)) return i;
    return -1;
}

static void input_config(void)
{
    char v[64], key[16];
    for (int i = 0; i < PB_N; i++) {
        g_map[i] = i;
        snprintf(key, sizeof(key), "map_%s", pb_name[i]);
        picker_cfg_str(key, "", v, sizeof(v));
        int t = v[0] ? pb_index(v, strlen(v)) : -1;
        if (t >= 0) { g_map[i] = t; plog("remap: %s acts as %s", pb_name[i], pb_name[t]); }
    }
    picker_cfg_str("turbo", "", v, sizeof(v));
    for (char *p = v; *p; ) {
        size_t n = strcspn(p, ", ");
        int i = pb_index(p, n);
        if (i >= 0) g_turbo_mask |= 1u << i;
        p += n; while (*p == ',' || *p == ' ') p++;
    }
    g_turbo_hz = picker_cfg_int("turbo_hz", 15);
    if (g_turbo_hz < 1) g_turbo_hz = 1;
    if (g_turbo_hz > 30) g_turbo_hz = 30;
    if (g_turbo_mask) plog("turbo mask=%03x at %d Hz", g_turbo_mask, g_turbo_hz);
}

static void turbo_set_period(double fps)   /* per game, from its refresh rate */
{
    g_turbo_period = (unsigned)((fps > 1.0 ? fps : 60.0) / g_turbo_hz + 0.5);
    if (g_turbo_period < 2) g_turbo_period = 2;
}

static int pb_down(SDL_GameController *g, int i)   /* raw state of a game button */
{
    static const SDL_GameControllerButton btn[PB_N] = {
        SDL_CONTROLLER_BUTTON_A, SDL_CONTROLLER_BUTTON_B, SDL_CONTROLLER_BUTTON_X, SDL_CONTROLLER_BUTTON_Y,
        SDL_CONTROLLER_BUTTON_LEFTSHOULDER, SDL_CONTROLLER_BUTTON_RIGHTSHOULDER, SDL_CONTROLLER_BUTTON_INVALID,
        SDL_CONTROLLER_BUTTON_INVALID, SDL_CONTROLLER_BUTTON_LEFTSTICK, SDL_CONTROLLER_BUTTON_RIGHTSTICK };
    if (i == PB_ZL) return SDL_GameControllerGetAxis(g, SDL_CONTROLLER_AXIS_TRIGGERLEFT) > GAME_TRIGGER_ON;
    if (i == PB_ZR) return SDL_GameControllerGetAxis(g, SDL_CONTROLLER_AXIS_TRIGGERRIGHT) > GAME_TRIGGER_ON;
    return SDL_GameControllerGetButton(g, btn[i]) != 0;
}

static int pb_acts(SDL_GameController *g, int target)   /* is anything pressed that acts as `target`? */
{
    int phase_on = (g_frame_no % g_turbo_period) < g_turbo_period / 2;   /* autofire: pressed for the first half */
    for (int i = 0; i < PB_N; i++)
        if (g_map[i] == target && pb_down(g, i) && (!(g_turbo_mask & (1u << i)) || phase_on)) return 1;
    return 0;
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
    /* game buttons go through the remap/autofire table. Face buttons match by position: SDL's A/B/X/Y
     * (bottom/right/left/top) are libretro's B/A/Y/X. */
    switch (id) {
    case RETRO_DEVICE_ID_JOYPAD_B:  return pb_acts(g, PB_A);
    case RETRO_DEVICE_ID_JOYPAD_A:  return pb_acts(g, PB_B);
    case RETRO_DEVICE_ID_JOYPAD_Y:  return pb_acts(g, PB_X);
    case RETRO_DEVICE_ID_JOYPAD_X:  return pb_acts(g, PB_Y);
    case RETRO_DEVICE_ID_JOYPAD_L:  return pb_acts(g, PB_L);
    case RETRO_DEVICE_ID_JOYPAD_R:  return pb_acts(g, PB_R);
    case RETRO_DEVICE_ID_JOYPAD_L2: return pb_acts(g, PB_ZL);
    case RETRO_DEVICE_ID_JOYPAD_R2: return pb_acts(g, PB_ZR);
    case RETRO_DEVICE_ID_JOYPAD_L3: return pb_acts(g, PB_L3);
    case RETRO_DEVICE_ID_JOYPAD_R3: return pb_acts(g, PB_R3);
    default: break;
    }
    {   /* left analog stick doubles the d-pad for directions */
        Sint16 ax = SDL_GameControllerGetAxis(g, SDL_CONTROLLER_AXIS_LEFTX);
        Sint16 ay = SDL_GameControllerGetAxis(g, SDL_CONTROLLER_AXIS_LEFTY);
        if (id == RETRO_DEVICE_ID_JOYPAD_LEFT  && ax < -PICKER_STICK_DZ) return 1;
        if (id == RETRO_DEVICE_ID_JOYPAD_RIGHT && ax >  PICKER_STICK_DZ) return 1;
        if (id == RETRO_DEVICE_ID_JOYPAD_UP    && ay < -PICKER_STICK_DZ) return 1;
        if (id == RETRO_DEVICE_ID_JOYPAD_DOWN  && ay >  PICKER_STICK_DZ) return 1;
    }
    switch (id) {
    case RETRO_DEVICE_ID_JOYPAD_UP:     b = SDL_CONTROLLER_BUTTON_DPAD_UP; break;
    case RETRO_DEVICE_ID_JOYPAD_DOWN:   b = SDL_CONTROLLER_BUTTON_DPAD_DOWN; break;
    case RETRO_DEVICE_ID_JOYPAD_LEFT:   b = SDL_CONTROLLER_BUTTON_DPAD_LEFT; break;
    case RETRO_DEVICE_ID_JOYPAD_RIGHT:  b = SDL_CONTROLLER_BUTTON_DPAD_RIGHT; break;
    case RETRO_DEVICE_ID_JOYPAD_START:  b = SDL_CONTROLLER_BUTTON_START; break;
    case RETRO_DEVICE_ID_JOYPAD_SELECT: b = SDL_CONTROLLER_BUTTON_BACK; break;
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
    snprintf(out, n, "%s/%s.state", STATE_DIR, set);
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
    if (!buf) { picker_message("State save failed (memory):", set); return; }
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

/* ---- in-game hotkeys ----
 * Hold BOTH triggers (ZL+ZR), then Y = quicksave, A = quickload, X = cycle the screens (TV + GamePad /
 * TV only / GamePad only). Edge-detected; the both-triggers gate makes an accidental trigger during play
 * very unlikely. */
static void hotkeys_poll(const char *set)
{
    static const char *const scr_name[3] = { "Screens: TV + GamePad", "Screens: TV only", "Screens: GamePad only" };
    SDL_GameController *g = picker_pad(0);
    static int prev_save, prev_load, prev_scr;
    int save = 0, load = 0, scr = 0;
    if (g &&
        SDL_GameControllerGetAxis(g, SDL_CONTROLLER_AXIS_TRIGGERLEFT)  > PICKER_TRIGGER_ON &&
        SDL_GameControllerGetAxis(g, SDL_CONTROLLER_AXIS_TRIGGERRIGHT) > PICKER_TRIGGER_ON) {
        save = SDL_GameControllerGetButton(g, SDL_CONTROLLER_BUTTON_Y);
        load = SDL_GameControllerGetButton(g, SDL_CONTROLLER_BUTTON_A);
        scr  = SDL_GameControllerGetButton(g, SDL_CONTROLLER_BUTTON_X);
    }
    if (save && !prev_save) do_savestate(set);
    if (load && !prev_load) do_loadstate(set);
    if (scr && !prev_scr) {
        picker_set_screens((picker_screens() + 1) % 3);
        picker_set_hint(scr_name[picker_screens()], 2000);
        plog("screens=%d", picker_screens());
    }
    prev_save = save; prev_load = load; prev_scr = scr;
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
    g_depth32 = g_depth32_cfg >= 0 ? g_depth32_cfg : fbneo_is_cv1000(set);   /* read by the core during load */
    plog("loading %s (depth32=%d)", path, g_depth32);
    if (!retro_load_game(&game)) {
        plog("retro_load_game FAILED %s", set);
        picker_message("Failed to load (wrong romset version?):", set);
        return 0;
    }
    retro_get_system_av_info(&av);
    plog("av: %ux%u fps=%.2f rate=%.0f", av.geometry.base_width, av.geometry.base_height,
         av.timing.fps, av.timing.sample_rate);
    vsync_choose(av.timing.fps);
    turbo_set_period(av.timing.fps);
    picker_set_aspect(g_aspect_auto ? av.geometry.aspect_ratio : 0.0);   /* 0 = square pixels */
    plog("aspect=%.3f (%s)", av.geometry.aspect_ratio, g_aspect_auto ? "auto" : "pixel");
    nFrameskip = g_frameskip + 1;   /* set per-game after load; skips the core draw on skipped frames */
    picker_set_hint("Hold  +  and  -  together to return to the menu", 5000);   /* HOME doesn't exit */

    SDL_zero(want);
    want.freq = (int)av.timing.sample_rate;
    want.format = AUDIO_S16SYS;
    want.channels = 2;
    want.samples = AUDIO_SAMPLES;
    adev = SDL_OpenAudioDevice(NULL, 0, &want, NULL, 0);   /* opens paused; started after a prebuffer */

    {
        int    rate = want.freq > 1 ? want.freq : 48000;
        double fps  = av.timing.fps > 1 ? av.timing.fps : 60.0;
        Uint32 frame_bytes  = (Uint32)(rate / fps + 1) * want.channels * sizeof(int16_t);
        Uint32 target_bytes = frame_bytes * QUEUE_FRAMES;
        Uint32 cap = g_vsync ? VSYNC_CAP_X * target_bytes : target_bytes;   /* queue wait threshold */
        int    audio_on = 0, warm = 0;
        g_target_bytes = target_bytes;
        rs_pos = 0; rs_last[0] = rs_last[1] = 0;
        if (picker_renderer()) SDL_RenderSetVSync(picker_renderer(), g_vsync ? 1 : 0);
        while (!want_quit && !want_menu) {
            Uint32 t0 = SDL_GetTicks();
            input_poll();
            hotkeys_poll(set);
            if (g_buf_status_cb && adev) {   /* autoskip: tell the core how full the audio queue is */
                Uint32 qn = SDL_GetQueuedAudioSize(adev);
                unsigned occ = audio_on ? (unsigned)((Uint64)qn * 100 / target_bytes) : 100;
                g_buf_status_cb(audio_on != 0, occ > 100 ? 100 : occ, audio_on && qn < 2 * frame_bytes);
            }
            retro_run();
            g_frame_no++;
            if (adev) {
                if (!audio_on && ++warm >= PREBUFFER_FRAMES) { SDL_PauseAudioDevice(adev, 0); audio_on = 1; }
                /* wait while the queue is above the threshold (pacing, or with vsync a safety cap); a game
                 * that can't keep up never gets ahead, so it runs flat out */
                for (int ms = 0; audio_on && !want_quit && !want_menu && ms < QUEUE_WAIT_MS &&
                                 SDL_GetQueuedAudioSize(adev) > cap; ms++)
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

/* ---- config.txt ---- */

static int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

static int cfg_tristate(const char *key)   /* "auto" -> -1, else 0/1 */
{
    char v[16];
    picker_cfg_str(key, "auto", v, sizeof(v));
    return !strcmp(v, "auto") ? -1 : (atoi(v) != 0);
}

static void config_load(void)
{
    char v[16], filter[16];

    /* display */
    g_aspect_auto = strcmp(picker_cfg_str("aspect", "auto", v, sizeof(v)), "pixel") != 0;
    picker_set_filter(!strcmp(picker_cfg_str("filter", "smooth", filter, sizeof(filter)), "sharp"));
    picker_cfg_str("screens", "both", v, sizeof(v));
    picker_set_screens(!strcmp(v, "tv") ? 1 : !strcmp(v, "gamepad") ? 2 : 0);
    plog("aspect=%s filter=%s screens=%d", g_aspect_auto ? "auto" : "pixel", filter, picker_screens());

    /* input and high scores */
    input_config();
    g_hiscores = picker_cfg_int("hiscores", 1) != 0;
    if (g_hiscores) hiscore_install();

    /* pacing */
    g_vsync_cfg = cfg_tristate("vsync");
    g_frameskip = clampi(picker_cfg_int("frameskip", 0), 0, 9);
    g_autoskip = clampi(picker_cfg_int("autoskip", 0), 0, 100);
    snprintf(g_autoskip_thr, sizeof(g_autoskip_thr), "%d", g_autoskip);
    plog("vsync=%s frameskip=%d autoskip=%d", g_vsync_cfg < 0 ? "auto" : (g_vsync_cfg ? "1" : "0"),
         g_frameskip, g_autoskip);

    /* CV1000: 32-bit output and the multi-core switches (on by default; 0 = one core, for troubleshooting) */
    g_depth32_cfg = cfg_tristate("depth32");
    g_blit_split = picker_cfg_int("blitsplit", 1) != 0;
    g_comp_split = picker_cfg_int("compsplit", 1) != 0;
    g_ymz_async = picker_cfg_int("ymzasync", 1) != 0;
    plog("depth32=%s blitsplit=%d compsplit=%d ymzasync=%d", g_depth32_cfg < 0 ? "auto" : (g_depth32_cfg ? "1" : "0"),
         g_blit_split, g_comp_split, g_ymz_async);

}

int main(int argc, char **argv)
{
    static const char *const fbneo_controls[] = {
        "Move: D-pad or left stick",
        "Buttons: A B X Y face, L R shoulders, L2 R2 triggers",
        "Start: +   Coin/Select: -",
        "Return to this menu: hold + and - together",
        "Hold ZL+ZR, then:  Y save state   A load state   X screens (TV/GamePad)",
        "2 players: GamePad, Pro Controller, or Wiimote+Classic",
        NULL
    };
    static const picker_cfg cfg = {
        "FBNeo " FBNEO_WIIU_VER, "rom", DATA_DIR, FONT_FILE, "zip", fbneo_system,
        "Romsets must match FBNeo v1.0.0.3 (~Oct-2026 set); Neo Geo needs neogeo.zip",
        fbneo_controls, fbneo_title
    };
    char autoload[64] = {0};
    int autoloaded = 0;
    int rc;
    (void)argc; (void)argv;
    g_log_epoch = OSGetSystemTime();
    WHBLogCafeInit();	/* log lines to OSReport */
    plog("start");

    rc = picker_init(&cfg);
    if (rc) { plog("picker_init failed (%d): %s", rc, SDL_GetError()); return 1; }
    mkdir(SYS_DIR, 0777);    /* picker_init created DATA_DIR; ok if these already exist */
    mkdir(SAVE_DIR, 0777);
    mkdir(STATE_DIR, 0777);
    config_load();

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
