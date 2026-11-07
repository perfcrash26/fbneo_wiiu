/*
 * fbneo_bench: a minimal libretro frontend that runs a list of FBNeo romsets back to back.
 *
 * For every set it runs a fixed number of frames with a scripted input (coin, start, play),
 * and logs one RESULT line: load status, timing, free memory, and video/audio hashes taken
 * at checkpoints. The hashes are computed over pixel and sample values, not raw memory, so a
 * big-endian build and a little-endian reference build print the same hash when they agree.
 *
 * The same file builds for Wii U (wut, static core) and Linux (reference, links the .so).
 *
 * Files (under BENCH_DIR):
 *   list.txt      one romset name per line (# comments allowed)
 *   progress.txt  appended "START <set>" / "END <set>"; a START without an END from an
 *                 earlier launch means that set crashed or hung, it is logged and skipped
 *   results.txt   appended RESULT lines (same as the log)
 *   shots/        <set>_<frame>.bmp at the screenshot checkpoints
 *   config.txt    optional "key=value" lines: frames, shots, roms, system, only, stress
 */
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <malloc.h>

#include "libretro.h"

#ifdef __WIIU__
#include <coreinit/memexpheap.h>
#include <coreinit/memheap.h>
#include <coreinit/alarm.h>
#include <coreinit/cache.h>
#include <coreinit/thread.h>
#include <coreinit/time.h>
#include <whb/log.h>
#include <whb/log_cafe.h>
#include <whb/log_console.h>
#include <whb/log_udp.h>
#include <whb/proc.h>
/* BENCH_SUBDIR lets build variants (e.g. -DBENCH_SUBDIR=\"bench-thready\") keep separate results */
#ifndef BENCH_SUBDIR
#define BENCH_SUBDIR "bench"
#endif
#define BENCH_DIR_DEFAULT "fs:/vol/external01/wiiu/fbneo/" BENCH_SUBDIR
#define ROMS_DIR_DEFAULT  "fs:/vol/external01/wiiu/fbneo/roms"
#define SYS_DIR_DEFAULT   "fs:/vol/external01/wiiu/fbneo/system"
#else
#define BENCH_DIR_DEFAULT "bench"
#define ROMS_DIR_DEFAULT  "roms"
#define SYS_DIR_DEFAULT   "system"
#endif

#define MAX_SETS 4096
#define MAX_CHECKPOINTS 16

static char bench_dir[512] = BENCH_DIR_DEFAULT;
static char roms_dir[512] = ROMS_DIR_DEFAULT;
static char sys_dir[512] = SYS_DIR_DEFAULT;
static char save_dir[700];
static int total_frames = 1200;
static int checkpoints[MAX_CHECKPOINTS] = { 60, 300, 600, 900, 1200 };
static int n_checkpoints = 5;
static int shot_frames[MAX_CHECKPOINTS] = { 600, 1200 };
static int n_shot_frames = 2;
static char only_set[64];

/* ---- time and memory ---- */

static uint64_t now_us(void)
{
#ifdef __WIIU__
    return (uint64_t)OSTicksToMicroseconds(OSGetSystemTime());
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000u + ts.tv_nsec / 1000;
#endif
}

static uint64_t start_us;

/* KB in use by malloc (wut's malloc owns the whole MEM2 heap, so MEM free sizes say nothing) */
static unsigned used_kb(void)
{
    struct mallinfo mi = mallinfo(); /* mallinfo2 is glibc-only */
    /* glibc serves big blocks with mmap, which uordblks leaves out */
    return (unsigned)(((unsigned long)mi.uordblks + (unsigned long)mi.hblkhd) / 1024);
}

/* A fixed wall clock (2018-06-01 00:00:00 UTC, the date FBNeo itself uses for netplay) so RTC
 * chips and BurnRandomInit's seed are the same on every run and on both builds. This replaces
 * the C library's time() for the core as well. */
time_t time(time_t *t)
{
    time_t v = 1527811200;
    if (t)
        *t = v;
    return v;
}

/* The same rand()/srand() on both builds: glibc and newlib produce different sequences, and
 * some chips use rand() for noise (e.g. vlm5030.cpp), so speech came out sign-flipped on the
 * Wii U. Replaces the C library's rand() for the core as well. RAND_MAX is 0x7fffffff on both. */
static uint32_t bench_rand_state = 1;

void srand(unsigned seed)
{
    bench_rand_state = seed;
}

int rand(void)
{
    bench_rand_state = bench_rand_state * 1103515245u + 12345u;
    return (int)((bench_rand_state >> 1) & 0x7fffffff);
}

/* ---- logging: every line goes to the platform log with a [fbneo] tag ---- */

static void blog(const char *fmt, ...)
{
    char buf[1024];
    va_list ap;
    uint64_t t = now_us() - start_us;
    int n = snprintf(buf, sizeof(buf), "[fbneo +%u.%03u] ", (unsigned)(t / 1000000), (unsigned)(t / 1000 % 1000));
    va_start(ap, fmt);
    vsnprintf(buf + n, sizeof(buf) - n, fmt, ap);
    va_end(ap);
#ifdef __WIIU__
    {
        /* the on-screen console overprints wrapped lines, so send short chunks
         * (results.txt on the SD card keeps whole lines) */
        const int width = 78;
        int len = strlen(buf), pos = 0;
        char chunk[96];
        while (pos < len) {
            int n = pos ? width - 4 : width;
            snprintf(chunk, sizeof(chunk), "%s%.*s", pos ? "    " : "", n, buf + pos);
            WHBLogPrint(chunk);
            pos += n;
        }
    }
    WHBLogConsoleDraw();
#else
    puts(buf);
    fflush(stdout);
#endif
}

static void append_line(const char *file, const char *line)
{
    char path[600];
    snprintf(path, sizeof(path), "%s/%s", bench_dir, file);
    /* open/append/close per line so a crash never loses what was written before it */
    FILE *f = fopen(path, "a");
    if (!f)
        return;
    fputs(line, f);
    fputc('\n', f);
    fclose(f);
}

/* ---- hashing over values, independent of host byte order ---- */

#define FNV_INIT 0x811c9dc5u
static inline uint32_t fnv_u8(uint32_t h, uint8_t b) { return (h ^ b) * 0x01000193u; }
static inline uint32_t fnv_u16(uint32_t h, uint16_t v) { return fnv_u8(fnv_u8(h, v & 0xff), v >> 8); }
static inline uint32_t fnv_u32(uint32_t h, uint32_t v) { return fnv_u16(fnv_u16(h, v & 0xffff), v >> 16); }

/* ---- per-game state ---- */

static enum retro_pixel_format pixfmt = RETRO_PIXEL_FORMAT_0RGB1555;
static const void *last_frame;
static unsigned last_w, last_h;
static size_t last_pitch;
static int frame_no;
static int frames_with_video;
static uint32_t audio_hash;
static uint64_t audio_samples, audio_nonzero;
static unsigned rotation;
static char cur_set[64];
static int dump_audio;
static FILE *audio_file;
static int core_inited;

static uint32_t hash_frame(void)
{
    uint32_t h = FNV_INIT;
    unsigned x, y;
    if (!last_frame)
        return 0;
    h = fnv_u32(h, last_w);
    h = fnv_u32(h, last_h);
    for (y = 0; y < last_h; y++) {
        const uint8_t *row = (const uint8_t *)last_frame + y * last_pitch;
        if (pixfmt == RETRO_PIXEL_FORMAT_XRGB8888) {
            const uint32_t *p = (const uint32_t *)row;
            for (x = 0; x < last_w; x++)
                h = fnv_u32(h, p[x] & 0x00ffffffu);
        } else {
            const uint16_t *p = (const uint16_t *)row;
            uint16_t mask = pixfmt == RETRO_PIXEL_FORMAT_RGB565 ? 0xffff : 0x7fff;
            for (x = 0; x < last_w; x++)
                h = fnv_u16(h, p[x] & mask);
        }
    }
    return h;
}

/* 24-bit BMP, built byte by byte in memory (same file on both builds) and written at once */
static uint8_t *put_le32(uint8_t *p, uint32_t v) { p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24; return p + 4; }
static uint8_t *put_le16(uint8_t *p, uint16_t v) { p[0] = v; p[1] = v >> 8; return p + 2; }

static void write_shot(int frame)
{
    char path[700];
    unsigned x, y, pad, rowbytes, size;
    uint8_t *buf, *p;
    FILE *f;
    if (!last_frame)
        return;
    rowbytes = last_w * 3;
    pad = (4 - rowbytes % 4) % 4;
    size = 54 + (rowbytes + pad) * last_h;
    buf = p = (uint8_t *)calloc(1, size);
    if (!buf)
        return;
    *p++ = 'B'; *p++ = 'M';
    p = put_le32(p, size);
    p = put_le32(p, 0);
    p = put_le32(p, 54);
    p = put_le32(p, 40);
    p = put_le32(p, last_w);
    p = put_le32(p, last_h);
    p = put_le16(p, 1);
    p = put_le16(p, 24);
    p = put_le32(p, 0);
    p = put_le32(p, (rowbytes + pad) * last_h);
    p = put_le32(p, 2835); p = put_le32(p, 2835); p = put_le32(p, 0); p = put_le32(p, 0);
    for (y = last_h; y-- > 0;) {
        const uint8_t *row = (const uint8_t *)last_frame + y * last_pitch;
        for (x = 0; x < last_w; x++) {
            uint8_t r, g, b;
            if (pixfmt == RETRO_PIXEL_FORMAT_XRGB8888) {
                uint32_t v = ((const uint32_t *)row)[x];
                r = v >> 16; g = v >> 8; b = v;
            } else if (pixfmt == RETRO_PIXEL_FORMAT_RGB565) {
                uint16_t v = ((const uint16_t *)row)[x];
                r = ((v >> 11) & 31) << 3; g = ((v >> 5) & 63) << 2; b = (v & 31) << 3;
            } else {
                uint16_t v = ((const uint16_t *)row)[x];
                r = ((v >> 10) & 31) << 3; g = ((v >> 5) & 31) << 3; b = (v & 31) << 3;
            }
            *p++ = b; *p++ = g; *p++ = r;
        }
        p += pad;
    }
    snprintf(path, sizeof(path), "%s/shots/%s_%04d.bmp", bench_dir, cur_set, frame);
    f = fopen(path, "wb");
    if (f) {
        fwrite(buf, 1, size, f);
        fclose(f);
    }
    free(buf);
}

/* ---- libretro callbacks ---- */

/* errors the core logs while loading: FBNeo then shows an error screen instead of the game
 * (missing or wrong ROMs) but retro_load_game still succeeds, so count them */
static int load_errors;
static char first_load_error[160];

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
    if (level >= RETRO_LOG_ERROR && frame_no == 0) {
        if (!load_errors++)
            snprintf(first_load_error, sizeof(first_load_error), "%s", buf);
    }
    blog("core: %s", buf);
}

static bool environment(unsigned cmd, void *data)
{
    switch (cmd) {
    case RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY:
        *(const char **)data = sys_dir;
        return true;
    case RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY:
        *(const char **)data = save_dir;
        return true;
    case RETRO_ENVIRONMENT_SET_PIXEL_FORMAT:
        pixfmt = *(const enum retro_pixel_format *)data;
        return true;
    case RETRO_ENVIRONMENT_GET_LOG_INTERFACE:
        ((struct retro_log_callback *)data)->log = core_log;
        return true;
    case RETRO_ENVIRONMENT_GET_AUDIO_VIDEO_ENABLE:
        *(int *)data = 3;
        return true;
    case RETRO_ENVIRONMENT_SET_ROTATION:
        rotation = *(const unsigned *)data;
        return true;
    case RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE:
        *(bool *)data = false;
        return true;
    case RETRO_ENVIRONMENT_GET_CORE_OPTIONS_VERSION:
        *(unsigned *)data = 0;
        return true;
    case RETRO_ENVIRONMENT_GET_VARIABLE:
        ((struct retro_variable *)data)->value = NULL;
        return false;
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

static void video_refresh(const void *data, unsigned w, unsigned h, size_t pitch)
{
    if (!data)
        return; /* duped frame: keep the previous one */
    last_frame = data;
    last_w = w;
    last_h = h;
    last_pitch = pitch;
    frames_with_video++;
}

static void audio_sample(int16_t l, int16_t r)
{
    audio_hash = fnv_u16(fnv_u16(audio_hash, (uint16_t)l), (uint16_t)r);
    audio_samples++;
    audio_nonzero += (l != 0) + (r != 0);
}

static size_t audio_batch(const int16_t *d, size_t frames)
{
    size_t i;
    for (i = 0; i < frames * 2; i++) {
        audio_hash = fnv_u16(audio_hash, (uint16_t)d[i]);
        audio_nonzero += d[i] != 0;
        if (audio_file) {
            /* little-endian on both builds */
            fputc((uint16_t)d[i] & 0xff, audio_file);
            fputc((uint16_t)d[i] >> 8, audio_file);
        }
    }
    audio_samples += frames;
    return frames;
}

static void input_poll(void) {}

/* Scripted input, the same for every game and both builds: insert a coin, press start,
 * then hold right and tap the first two buttons, so most games get past attract mode. */
static int16_t input_state(unsigned port, unsigned device, unsigned index, unsigned id)
{
    int f = frame_no;
    if (port != 0 || device != RETRO_DEVICE_JOYPAD)
        return 0;
    if (id == RETRO_DEVICE_ID_JOYPAD_SELECT)
        return (f >= 420 && f < 426) || (f >= 480 && f < 486);
    if (id == RETRO_DEVICE_ID_JOYPAD_START)
        return (f >= 540 && f < 546) || (f >= 600 && f < 606) || (f >= 660 && f < 666);
    if (f < 720)
        return 0;
    if (id == RETRO_DEVICE_ID_JOYPAD_RIGHT)
        return (f / 120) % 2 == 0;
    if (id == RETRO_DEVICE_ID_JOYPAD_LEFT)
        return (f / 120) % 2 == 1;
    if (id == RETRO_DEVICE_ID_JOYPAD_B)
        return f % 20 < 4;
    if (id == RETRO_DEVICE_ID_JOYPAD_A)
        return f % 30 < 4;
    return 0;
}


/* ---- sampling profiler (Wii U, config profile=1) ----
 * Interrupt-driven. A periodic OSAlarm fires ~1 kHz on the emulation core; wut hands the
 * callback the INTERRUPTED OSContext, so ctx->srr0 is the exact PC the emulator was running
 * when the timer fired (a SIGPROF-style sampler). Counts per PC go to prof/<set>.txt and over
 * UDP, for offline symbolization against the ELF (RPX text loads at its link address).
 *
 * Why not a sampler thread + OSSuspendThread (the previous design): on this console a merely
 * READY thread on a secondary core is scheduled only ~once/0.5 s regardless of core (0/2),
 * priority (0/1/16) or sleep-vs-spin (measured: ~20-60 loop iters in 49 s every way), so it
 * caught only 8 samples. The decrementer-driven alarm never touches the thread scheduler. */
static int profile_on;
static int pace_on;   /* config pace=1: sleep out each frame to 60fps so CV1KUTIL shows true core idle */
#ifdef __WIIU__
#define PROF_SLOTS 65536
static uint32_t prof_pc[PROF_SLOTS], prof_count[PROF_SLOTS];
static uint32_t prof_lr[PROF_SLOTS], prof_lr_count[PROF_SLOTS];
static volatile int prof_active;
static volatile uint32_t prof_samples, prof_fires;	/* samples recorded / total alarm fires */
static OSAlarm prof_alarm;

static int is_code(uint32_t a) { return a >= 0x02000000 && a < 0x10000000; }

static void prof_add(uint32_t *keys, uint32_t *counts, uint32_t key)
{
    uint32_t i = (key >> 2) * 2654435761u % PROF_SLOTS, n;
    for (n = 0; n < PROF_SLOTS; n++, i = (i + 1) % PROF_SLOTS) {
        if (keys[i] == key) {
            counts[i]++;
            return;
        }
        if (!keys[i]) {
            keys[i] = key;
            counts[i] = 1;
            return;
        }
    }
}

/* Alarm callback: runs in interrupt context on the core that owns the alarm (the emulation
 * core, since prof_start() arms it from the emu thread). ctx is the interrupted context, so
 * ctx->srr0 is the emulator's PC. Does only array writes - no OS calls, nothing that blocks. */
static void prof_alarm_cb(OSAlarm *alarm, OSContext *ctx)
{
    (void)alarm;
    prof_fires++;
    if (!prof_active || !ctx)
        return;
    uint32_t pc = ctx->srr0, lr = ctx->lr;
    /* Cemu stores srr0 host-endian; a byte-swapped code address is never 4-byte aligned */
    if ((pc & 3) || !is_code(pc))
        pc = __builtin_bswap32(pc);
    prof_add(prof_pc, prof_count, pc);
    if (is_code(lr))
        prof_add(prof_lr, prof_lr_count, lr);
    prof_samples++;
}

#ifdef CV1K_PROF
/* ---- Level-1 CV1000 profiling: per-subsystem frame accounting + per-core utilization ----
 * The per-subsystem times (cpu / ymz / draw / blitwait / total) are accumulated in d_cv1k.cpp
 * (also built -DCV1K_PROF). Here we add a per-core idle sampler: a lowest-priority (31) spinner
 * on each of cores 0/1/2 that only runs when its core is otherwise idle, so it costs the
 * emulator nothing and its accumulated runtime measures that core's idle time (nulldc's
 * sampler_wiiu.cpp technique). Reset per steady window; emitted as [cv1k-prof] lines. */
extern unsigned long long g_cv1k_prof_cpu, g_cv1k_prof_ymz, g_cv1k_prof_draw, g_cv1k_prof_blitwait, g_cv1k_prof_total;
extern unsigned g_cv1k_prof_frames;

static OSThread cvp_spinner[3];
static uint8_t  cvp_spin_stack[3][16 * 1024] __attribute__((aligned(16)));
static volatile uint64_t cvp_idle[3];   /* ticks each core spent idle (spinner ran) */
static uint64_t cvp_idle_mark[3];
static volatile int cvp_stop;
static int cvp_started;
static OSTime cvp_span0;

static int cvp_spinner_main(int which, const char **argv)
{
    (void)argv;
    OSTime last = OSGetSystemTime();
    const OSTime gap = OSMicrosecondsToTicks(20);   /* a bigger gap means something else ran on this core */
    while (!cvp_stop) {
        const OSTime t = OSGetSystemTime();
        if (t - last < gap)
            cvp_idle[which] += (uint64_t)(t - last);
        last = t;
    }
    return 0;
}

static void cv1k_prof_start(void)
{
    g_cv1k_prof_cpu = g_cv1k_prof_ymz = g_cv1k_prof_draw = g_cv1k_prof_blitwait = g_cv1k_prof_total = 0;
    g_cv1k_prof_frames = 0;
    /* spinners are created once and left running for the whole process (priority 31 costs nothing);
     * no stop/join, which hung on a console in nulldc. Each window reads an idle delta. */
    if (!cvp_started) {
        static const uint32_t aff[3] = { OS_THREAD_ATTRIB_AFFINITY_CPU0, OS_THREAD_ATTRIB_AFFINITY_CPU1,
                                         OS_THREAD_ATTRIB_AFFINITY_CPU2 };
        int c;
        cvp_stop = 0;
        for (c = 0; c < 3; c++)
            if (OSCreateThread(&cvp_spinner[c], cvp_spinner_main, c, NULL,
                               cvp_spin_stack[c] + sizeof(cvp_spin_stack[c]), sizeof(cvp_spin_stack[c]), 31, aff[c]))
                OSResumeThread(&cvp_spinner[c]);
        cvp_started = 1;
    }
    cvp_idle_mark[0] = cvp_idle[0];
    cvp_idle_mark[1] = cvp_idle[1];
    cvp_idle_mark[2] = cvp_idle[2];
    cvp_span0 = OSGetSystemTime();
}

static void cv1k_prof_report(const char *set)
{
    OSTime span = OSGetSystemTime() - cvp_span0;
    double spanus = (double)OSTicksToMicroseconds(span);
    unsigned f = g_cv1k_prof_frames ? g_cv1k_prof_frames : 1;
    double tot  = (double)OSTicksToMicroseconds((OSTime)g_cv1k_prof_total);
    double cpu  = (double)OSTicksToMicroseconds((OSTime)g_cv1k_prof_cpu);
    double ymz  = (double)OSTicksToMicroseconds((OSTime)g_cv1k_prof_ymz);
    double draw = (double)OSTicksToMicroseconds((OSTime)g_cv1k_prof_draw);
    double wait = (double)OSTicksToMicroseconds((OSTime)g_cv1k_prof_blitwait);
    double other = tot - cpu - ymz - draw - wait;
    double den = tot > 0 ? tot : 1;
    uint64_t id0 = cvp_idle[0] - cvp_idle_mark[0], id1 = cvp_idle[1] - cvp_idle_mark[1], id2 = cvp_idle[2] - cvp_idle_mark[2];
    if (other < 0) other = 0;
    if (spanus <= 0) spanus = 1;
    blog("CV1KPROF %s frames=%u frame_ms=%.3f cpu_ms=%.3f ymz_ms=%.3f draw_ms=%.3f blitwait_ms=%.3f other_ms=%.3f",
         set, f, tot / 1000.0 / f, cpu / 1000.0 / f, ymz / 1000.0 / f, draw / 1000.0 / f, wait / 1000.0 / f, other / 1000.0 / f);
    blog("CV1KPROF %s pct_of_frame cpu=%.1f ymz=%.1f draw=%.1f blitwait=%.1f other=%.1f",
         set, 100 * cpu / den, 100 * ymz / den, 100 * draw / den, 100 * wait / den, 100 * other / den);
    blog("CV1KUTIL %s busy_pct core0=%.1f core1=%.1f core2=%.1f",
         set, 100.0 - 100.0 * (double)OSTicksToMicroseconds((OSTime)id0) / spanus,
         100.0 - 100.0 * (double)OSTicksToMicroseconds((OSTime)id1) / spanus,
         100.0 - 100.0 * (double)OSTicksToMicroseconds((OSTime)id2) / spanus);
}
#endif /* CV1K_PROF */

static void prof_start(void)
{
    static int created;
    if (!created) {
        OSCreateAlarm(&prof_alarm);
        created = 1;
    }
#ifdef CV1K_PROF
    cv1k_prof_start();
#endif
    memset(prof_pc, 0, sizeof(prof_pc));
    memset(prof_count, 0, sizeof(prof_count));
    memset(prof_lr, 0, sizeof(prof_lr));
    memset(prof_lr_count, 0, sizeof(prof_lr_count));
    prof_samples = prof_fires = 0;
    __sync_synchronize();
    prof_active = 1;
    __sync_synchronize();
    /* periodic 1 ms alarm, armed from the emulation thread so it fires on that core and the
     * callback sees the emulator's interrupted PC */
    OSTime iv = OSMicrosecondsToTicks(1000);
    OSSetPeriodicAlarm(&prof_alarm, OSGetTime() + iv, iv, prof_alarm_cb);
}

/* Emit the top n entries of a (keys,counts) table over UDP, highest first. Destructive:
 * zeroes each count as it is emitted, so call only after the SD file is written. */
static void prof_emit_top(const char *tag, const char *set, uint32_t *keys, uint32_t *counts, int n)
{
    int k;
    for (k = 0; k < n; k++) {
        int i, best = -1;
        uint32_t bestc = 0;
        for (i = 0; i < PROF_SLOTS; i++)
            if (counts[i] > bestc) { bestc = counts[i]; best = i; }
        if (best < 0)
            break;
        blog("%s %s %02d %08x %u", tag, set, k, (unsigned)keys[best], (unsigned)bestc);
        counts[best] = 0;
    }
}

static void prof_write(const char *set)
{
    char path[700];
    FILE *f;
    int i;
    prof_active = 0;
    OSCancelAlarm(&prof_alarm);
    __sync_synchronize();
    OSSleepTicks(OSMillisecondsToTicks(3));
    /* SD copy (full histogram) first, so the destructive UDP emit below can't lose data */
    snprintf(path, sizeof(path), "%s/prof/%s.txt", bench_dir, set);
    f = fopen(path, "w");
    if (f) {
        fprintf(f, "samples %u fires %u\n", (unsigned)prof_samples, (unsigned)prof_fires);
        for (i = 0; i < PROF_SLOTS; i++)
            if (prof_pc[i])
                fprintf(f, "pc %08x %u\n", (unsigned)prof_pc[i], (unsigned)prof_count[i]);
        for (i = 0; i < PROF_SLOTS; i++)
            if (prof_lr[i])
                fprintf(f, "lr %08x %u\n", (unsigned)prof_lr[i], (unsigned)prof_lr_count[i]);
        fclose(f);
    }
    /* UDP copy, so the result is readable without the SD card: a summary line plus the hot
     * PCs/LRs. prof_fires ~= run_ms means the 1 kHz alarm fired as intended. */
    blog("PROF %s samples=%u fires=%u", set, (unsigned)prof_samples, (unsigned)prof_fires);
    prof_emit_top("PROFPC", set, prof_pc, prof_count, 24);
    prof_emit_top("PROFLR", set, prof_lr, prof_lr_count, 12);
#ifdef CV1K_PROF
    cv1k_prof_report(set);
#endif
}
#else
static void prof_start(void) {}
static void prof_write(const char *set) { (void)set; }
#endif

/* ---- one game ---- */

static int is_checkpoint(int f, const int *list, int n)
{
    int i;
    for (i = 0; i < n; i++)
        if (list[i] == f)
            return 1;
    return 0;
}

static void run_set(const char *set)
{
    char path[700], line[2048], hashes[512];
    struct retro_game_info game;
    struct retro_system_av_info av;
    uint64_t t0, t_load, t_run = 0, worst = 0, t_steady = 0;
    unsigned mem_before, mem_after_load, mem_peak;
    int steady_frames = 0, hl = 0;
    bool ok;

    snprintf(cur_set, sizeof(cur_set), "%s", set);
    /* a save dir per set: NVRAM/EEPROM a set writes on unload must not change its next boot,
     * so a sweep must start from an empty bench dir */
    snprintf(save_dir, sizeof(save_dir), "%s/saves/%s", bench_dir, set);
    mkdir(save_dir, 0777);
    snprintf(line, sizeof(line), "START %s", set);
    append_line("progress.txt", line);
    blog("START %s", set);

    pixfmt = RETRO_PIXEL_FORMAT_0RGB1555;
    last_frame = NULL;
    frame_no = frames_with_video = 0;
    audio_hash = FNV_INIT;
    audio_samples = audio_nonzero = 0;
    rotation = 0;
    hashes[0] = 0;
    load_errors = 0;
    first_load_error[0] = 0;

    mem_before = used_kb();
    /* retro_init once per process: FBNeo does not survive retro_deinit + retro_init */
    if (!core_inited) {
        retro_set_environment(environment);
        retro_set_video_refresh(video_refresh);
        retro_set_audio_sample(audio_sample);
        retro_set_audio_sample_batch(audio_batch);
        retro_set_input_poll(input_poll);
        retro_set_input_state(input_state);
        retro_init();
        core_inited = 1;
    }

    snprintf(path, sizeof(path), "%s/%s.zip", roms_dir, set);
    memset(&game, 0, sizeof(game));
    game.path = path;
    t0 = now_us();
    ok = retro_load_game(&game);
    t_load = now_us() - t0;
    mem_after_load = mem_peak = used_kb();

    if (!ok) {
        snprintf(line, sizeof(line), "RESULT %s status=LOADFAIL load_ms=%u mem_used_kb=%u", set,
                 (unsigned)(t_load / 1000), mem_after_load);
        goto done;
    }

    retro_get_system_av_info(&av);
    /* realtime frame budget (us) for pace mode: how long a frame "should" take at the game's fps */
    uint64_t frame_target_us = (av.timing.fps > 1.0) ? (uint64_t)(1000000.0 / av.timing.fps + 0.5) : 16667;
    if (dump_audio) {
        snprintf(path, sizeof(path), "%s/%s.audio.raw", bench_dir, set);
        audio_file = fopen(path, "wb");
        if (audio_file)
            setvbuf(audio_file, NULL, _IOFBF, 256 * 1024);
    }
    for (frame_no = 1; frame_no <= total_frames; frame_no++) {
        uint64_t a = now_us(), d;
        retro_run();
        d = now_us() - a;
        t_run += d;
        if (d > worst)
            worst = d;
#ifdef __WIIU__
        /* pace mode: after timing the (flat-out) frame, sleep out the rest of the realtime budget so
         * the core actually idles when the game is faster than 60fps - that idle is what CV1KUTIL reads.
         * d (the measured frame time) is untouched, so speed_pct/cpu_ms stay flat-out. */
        if (pace_on && d < frame_target_us)
            OSSleepTicks(OSMicrosecondsToTicks(frame_target_us - d));
#endif
        /* steady state: skip the boot/load frames */
        if (profile_on && frame_no == total_frames / 4)
            prof_start();
        if (frame_no > total_frames / 4) {
            t_steady += d;
            steady_frames++;
        }
        if (is_checkpoint(frame_no, checkpoints, n_checkpoints))
            hl += snprintf(hashes + hl, sizeof(hashes) - hl, " v%d=%08x a%d=%08x", frame_no, (unsigned)hash_frame(),
                           frame_no, (unsigned)audio_hash);
        if (is_checkpoint(frame_no, shot_frames, n_shot_frames))
            write_shot(frame_no);
        if ((frame_no & 63) == 0) {
            unsigned m = used_kb();
            if (m > mem_peak)
                mem_peak = m;
#ifdef __WIIU__
            WHBLogConsoleDraw();
            if (!WHBProcIsRunning())
                break;
#endif
        }
    }
    frame_no--;
    if (audio_file) {
        fclose(audio_file);
        audio_file = NULL;
    }
    if (profile_on)
        prof_write(set);

    {
        double fps = av.timing.fps;
        double steady_ms = steady_frames ? t_steady / 1000.0 / steady_frames : 0;
        double speed = steady_ms > 0 ? (1000.0 / fps) / steady_ms * 100.0 : 0;
        char *c;
        /* keep the message on one line and free of '=' so the RESULT line stays parseable */
        for (c = first_load_error; *c; c++)
            if (*c == ' ' || *c == '=')
                *c = '_';
        snprintf(line, sizeof(line),
                 "RESULT %s status=%s frames=%d fps=%.3f w=%u h=%u fmt=%d rot=%u load_ms=%u avg_ms=%.3f steady_ms=%.3f "
                 "worst_ms=%.2f speed_pct=%.1f mem_used_kb=%u/%u/%u video_frames=%d audio_samples=%llu "
                 "audio_nonzero=%llu%s",
                 set, load_errors ? "ROMERROR" : "OK", frame_no, fps, last_w, last_h, (int)pixfmt, rotation, (unsigned)(t_load / 1000),
                 t_run / 1000.0 / (frame_no ? frame_no : 1), steady_ms, worst / 1000.0, speed, mem_before,
                 mem_after_load, mem_peak, frames_with_video, (unsigned long long)audio_samples,
                 (unsigned long long)audio_nonzero, hashes);
        if (load_errors)
            snprintf(line + strlen(line), sizeof(line) - strlen(line), " load_errors=%d first_error=%s", load_errors,
                     first_load_error);
#ifdef CV1K_PROF
        {
            /* single main-core utilization needed to hold the game's fps: steady frame time / budget.
             * >100 = the emulation needs more than one core's worth (drops frames). pace mode's
             * CV1KUTIL core1 is the capped, directly-measured counterpart (100 when this is >=100). */
            double budget = 1000.0 / fps;
            blog("CV1KNEED %s core1_need_pct=%.0f steady_ms=%.2f budget_ms=%.2f paced=%d (>100 = over one core)",
                 set, budget > 0 ? steady_ms / budget * 100.0 : 0.0, steady_ms, budget, pace_on);
        }
#endif
    }
    /* log the result before unloading, so a crash in the driver's exit code still leaves it */
    blog("%s", line);
    append_line("results.txt", line);
    snprintf(path, sizeof(path), "UNLOADING %s", set);
    append_line("progress.txt", path);
    retro_unload_game();
    goto end;

done:
    blog("%s", line);
    append_line("results.txt", line);
end:
    {
        char endl[128];
        snprintf(endl, sizeof(endl), "END %s mem_used_kb=%u", set, used_kb());
        append_line("progress.txt", endl);
    }
}

/* ---- list, progress and config files ---- */

static char *sets[MAX_SETS];
static int n_sets;

static void strip(char *s)
{
    char *e;
    while (*s == ' ' || *s == '\t')
        memmove(s, s + 1, strlen(s));
    e = s + strlen(s);
    while (e > s && (e[-1] == '\n' || e[-1] == '\r' || e[-1] == ' ' || e[-1] == '\t'))
        *--e = 0;
}

static int parse_ints(const char *s, int *out, int max)
{
    int n = 0;
    while (*s && n < max) {
        out[n++] = atoi(s);
        s = strchr(s, ',');
        if (!s)
            break;
        s++;
    }
    return n;
}

static void read_config(void)
{
    char path[600], buf[600];
    FILE *f;
    snprintf(path, sizeof(path), "%s/config.txt", bench_dir);
    f = fopen(path, "r");
    if (!f)
        return;
    while (fgets(buf, sizeof(buf), f)) {
        char *eq;
        strip(buf);
        if (buf[0] == '#' || !(eq = strchr(buf, '=')))
            continue;
        *eq++ = 0;
        if (!strcmp(buf, "frames"))
            total_frames = atoi(eq);
        else if (!strcmp(buf, "checkpoints"))
            n_checkpoints = parse_ints(eq, checkpoints, MAX_CHECKPOINTS);
        else if (!strcmp(buf, "shots"))
            n_shot_frames = parse_ints(eq, shot_frames, MAX_CHECKPOINTS);
        else if (!strcmp(buf, "roms"))
            snprintf(roms_dir, sizeof(roms_dir), "%s", eq);
        else if (!strcmp(buf, "system"))
            snprintf(sys_dir, sizeof(sys_dir), "%s", eq);
        else if (!strcmp(buf, "dumpaudio"))
            dump_audio = atoi(eq);
        else if (!strcmp(buf, "profile"))
            profile_on = atoi(eq);
        else if (!strcmp(buf, "pace"))
            pace_on = atoi(eq);
        else if (!strcmp(buf, "only"))
            snprintf(only_set, sizeof(only_set), "%s", eq);
    }
    fclose(f);
}

static int find_set(const char *name)
{
    int i;
    for (i = 0; i < n_sets; i++)
        if (sets[i] && !strcmp(sets[i], name))
            return i;
    return -1;
}

int main(int argc, char **argv)
{
    char path[600], buf[256], last_started[64] = "";
    int unloading = 0;
    FILE *f;
    int i, todo = 0, done_count = 0;
    static char done_flag[MAX_SETS];

#ifdef __WIIU__
    WHBProcInit();
    WHBLogCafeInit();
    WHBLogUdpInit();
    WHBLogConsoleInit();
    WHBLogConsoleSetColor(0x00203000);
#else
    if (argc > 1)
        snprintf(bench_dir, sizeof(bench_dir), "%s", argv[1]);
#endif
    start_us = now_us();
    read_config();
    snprintf(path, sizeof(path), "%s/shots", bench_dir);
    mkdir(path, 0777);
    snprintf(path, sizeof(path), "%s/saves", bench_dir);
    mkdir(path, 0777);
    snprintf(path, sizeof(path), "%s/prof", bench_dir);
    if (profile_on)
        mkdir(path, 0777);
    blog("fbneo_bench start: frames=%d profile=%d roms=%s system=%s mem_used_kb=%u", total_frames, profile_on, roms_dir, sys_dir,
         used_kb());

    snprintf(path, sizeof(path), "%s/list.txt", bench_dir);
    f = fopen(path, "r");
    if (!f) {
        blog("no list file %s", path);
        goto out;
    }
    while (n_sets < MAX_SETS && fgets(buf, sizeof(buf), f)) {
        strip(buf);
        if (!buf[0] || buf[0] == '#')
            continue;
        if (only_set[0] && strcmp(buf, only_set))
            continue;
        sets[n_sets++] = strdup(buf);
    }
    fclose(f);

    /* resume: skip finished sets, and report the one an earlier launch died in.
     * A profiling run always re-runs its whole list (you re-profile the same sets), so it
     * ignores progress.txt; otherwise a second run would skip everything (ran=0). */
    snprintf(path, sizeof(path), "%s/progress.txt", bench_dir);
    f = profile_on ? NULL : fopen(path, "r");
    if (f) {
        while (fgets(buf, sizeof(buf), f)) {
            char name[64];
            strip(buf);
            if (sscanf(buf, "START %63s", name) == 1) {
                snprintf(last_started, sizeof(last_started), "%s", name);
                unloading = 0;
                if ((i = find_set(name)) >= 0)
                    done_flag[i] = 1;
            } else if (sscanf(buf, "UNLOADING %63s", name) == 1 && !strcmp(name, last_started)) {
                unloading = 1;
            } else if (sscanf(buf, "END %63s", name) == 1 && !strcmp(name, last_started)) {
                last_started[0] = 0;
            }
        }
        fclose(f);
    }
    if (last_started[0]) {
        char line[160];
        if (unloading)
            snprintf(line, sizeof(line), "EXITCRASH %s (ran, then the previous launch died unloading it)", last_started);
        else
            snprintf(line, sizeof(line), "RESULT %s status=CRASH (previous launch never finished it)", last_started);
        blog("%s", line);
        append_line("results.txt", line);
        snprintf(line, sizeof(line), "END %s crashed", last_started);
        append_line("progress.txt", line);
    }

    for (i = 0; i < n_sets; i++)
        if (!done_flag[i])
            todo++;
    blog("%d sets in list, %d to run", n_sets, todo);

    for (i = 0; i < n_sets; i++) {
#ifdef __WIIU__
        if (!WHBProcIsRunning())
            break;
#endif
        if (done_flag[i])
            continue;
        run_set(sets[i]);
        done_count++;
    }
    blog("BENCH_DONE ran=%d", done_count);

out:
#ifdef __WIIU__
    blog("CEMU_RUN_DONE");
    blog("finished: results are in %s/results.txt, press HOME to exit", bench_dir);
    while (WHBProcIsRunning()) {
        WHBLogConsoleDraw();
        OSSleepTicks(OSMillisecondsToTicks(100));
    }
    WHBLogConsoleFree();
    WHBLogUdpDeinit();
    WHBLogCafeDeinit();
    WHBProcShutdown();
#endif
    return 0;
}
