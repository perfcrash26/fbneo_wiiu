/* picker - self-contained SDL2 content-picker shell for Wii U (wut). See picker.h / README.md. */
#include "picker.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>	/* strcasecmp */
#include <dirent.h>
#include <sys/stat.h>
#include <SDL_ttf.h>

#define MAXSETS 1024

static const picker_cfg *g_cfg;
static SDL_Window   *g_win;
static SDL_Renderer *g_ren;
static TTF_Font     *g_font;
static SDL_Texture  *g_tex;
static unsigned      g_tex_w, g_tex_h;
static Uint32        g_tex_fmt;
static SDL_GameController *g_pads[2];   /* [0]=player 1, [1]=player 2 */
static int           g_exit_hold;

static char g_content[768];
static char g_autoload[64];
static int  g_rotate = 1;

static char *g_sets[MAXSETS];		/* item names (malloc'd, any length) */
static char  g_label[MAXSETS][28];
static int   g_nsets;

/* ---- small helpers ---- */

static void mkdirs(const char *path)
{
    char tmp[800];
    size_t i;
    snprintf(tmp, sizeof(tmp), "%s", path);
    for (i = 1; tmp[i]; i++)
        if (tmp[i] == '/' && tmp[i - 1] != ':') {	/* not the "fs:" device separator */
            tmp[i] = 0; mkdir(tmp, 0777); tmp[i] = '/';
        }
    mkdir(tmp, 0777);
}

static void draw_text(int x, int y, SDL_Color c, const char *s)
{
    SDL_Surface *surf;
    SDL_Texture *t;
    SDL_Rect dst;
    if (!g_font || !s || !*s) return;
    surf = TTF_RenderUTF8_Blended(g_font, s, c);
    if (!surf) return;
    t = SDL_CreateTextureFromSurface(g_ren, surf);
    dst.x = x; dst.y = y; dst.w = surf->w; dst.h = surf->h;
    SDL_FreeSurface(surf);
    if (t) { SDL_RenderCopy(g_ren, t, NULL, &dst); SDL_DestroyTexture(t); }
}

/* ---- controllers ---- */

static void open_pads(void)
{
    int i;
    for (i = 0; i < SDL_NumJoysticks(); i++) {
        SDL_GameController *g;
        if (!SDL_IsGameController(i)) continue;
        g = SDL_GameControllerOpen(i);
        if (!g) continue;
        if (g == g_pads[0] || g == g_pads[1]) continue;  /* already assigned */
        if      (!g_pads[0]) g_pads[0] = g;
        else if (!g_pads[1]) g_pads[1] = g;
        else SDL_GameControllerClose(g);                 /* only two players */
    }
}

SDL_GameController *picker_pad(int port) { return (port == 0 || port == 1) ? g_pads[port] : NULL; }

static int pad_btn(SDL_GameControllerButton b)
{
    int i;
    for (i = 0; i < 2; i++)
        if (g_pads[i] && SDL_GameControllerGetButton(g_pads[i], b)) return 1;
    return 0;
}

static int pad_dir(int down)   /* d-pad or left stick, either player */
{
    int i;
    for (i = 0; i < 2; i++) {
        SDL_GameController *g = g_pads[i];
        if (!g) continue;
        if (SDL_GameControllerGetButton(g, down ? SDL_CONTROLLER_BUTTON_DPAD_DOWN
                                                : SDL_CONTROLLER_BUTTON_DPAD_UP)) return 1;
        {
            Sint16 ay = SDL_GameControllerGetAxis(g, SDL_CONTROLLER_AXIS_LEFTY);
            if (down ? (ay > 12000) : (ay < -12000)) return 1;
        }
    }
    return 0;
}

void picker_pump(int *want_quit, int *want_menu)
{
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        if (e.type == SDL_QUIT && want_quit) *want_quit = 1;
        else if (e.type == SDL_CONTROLLERDEVICEADDED) open_pads();
    }
    /* either player holds START+BACK (+ and -) ~0.5s to leave the running content */
    if ((g_pads[0] && SDL_GameControllerGetButton(g_pads[0], SDL_CONTROLLER_BUTTON_START)
                   && SDL_GameControllerGetButton(g_pads[0], SDL_CONTROLLER_BUTTON_BACK)) ||
        (g_pads[1] && SDL_GameControllerGetButton(g_pads[1], SDL_CONTROLLER_BUTTON_START)
                   && SDL_GameControllerGetButton(g_pads[1], SDL_CONTROLLER_BUTTON_BACK))) {
        if (++g_exit_hold >= 30 && want_menu) *want_menu = 1;
    } else {
        g_exit_hold = 0;
    }
}

/* ---- video ---- */

void picker_present(const void *data, unsigned w, unsigned h, unsigned pitch,
                    Uint32 sdl_fmt, unsigned rot_ccw)
{
    SDL_Rect dst;
    int ww, wh, rot90;
    double sa, da;
    if (!data) return;  /* duped frame */

    if (!g_tex || w != g_tex_w || h != g_tex_h || sdl_fmt != g_tex_fmt) {
        if (g_tex) SDL_DestroyTexture(g_tex);
        g_tex = SDL_CreateTexture(g_ren, sdl_fmt, SDL_TEXTUREACCESS_STREAMING, w, h);
        g_tex_w = w; g_tex_h = h; g_tex_fmt = sdl_fmt;
    }
    SDL_UpdateTexture(g_tex, NULL, data, (int)pitch);

    SDL_GetRendererOutputSize(g_ren, &ww, &wh);
    rot90 = (rot_ccw == 1 || rot_ccw == 3);   /* letterbox to the DISPLAYED aspect */
    sa = rot90 ? (double)h / (double)w : (double)w / (double)h;
    da = (double)ww / (double)wh;
    if (da > sa) { dst.h = wh; dst.w = (int)(wh * sa); dst.x = (ww - dst.w) / 2; dst.y = 0; }
    else         { dst.w = ww; dst.h = (int)(ww / sa); dst.x = 0; dst.y = (wh - dst.h) / 2; }

    SDL_RenderClear(g_ren);
    if (rot90) {
        SDL_Rect r;                            /* footprint is dst w/h swapped+centred; rotate fills dst */
        r.w = dst.h; r.h = dst.w;
        r.x = dst.x + (dst.w - r.w) / 2;
        r.y = dst.y + (dst.h - r.h) / 2;
        SDL_RenderCopyEx(g_ren, g_tex, NULL, &r, rot_ccw == 1 ? 270.0 : 90.0, NULL, SDL_FLIP_NONE);
    } else {
        SDL_RenderCopyEx(g_ren, g_tex, NULL, &dst, rot_ccw == 2 ? 180.0 : 0.0, NULL, SDL_FLIP_NONE);
    }
    SDL_RenderPresent(g_ren);
}

SDL_Renderer *picker_renderer(void) { return g_ren; }

void picker_message(const char *line1, const char *line2)
{
    SDL_Color c = { 255, 120, 120, 255 };
    Uint32 end = SDL_GetTicks() + 1600;
    SDL_Event e;
    while (SDL_GetTicks() < end) {
        while (SDL_PollEvent(&e)) {}
        SDL_SetRenderDrawColor(g_ren, 18, 18, 28, 255);
        SDL_RenderClear(g_ren);
        draw_text(80, 300, c, line1);
        draw_text(80, 344, c, line2);
        SDL_RenderPresent(g_ren);
        SDL_Delay(16);
    }
}

/* ---- config + scan ---- */

const char *picker_content_dir(void) { return g_content; }
const char *picker_autoload(void)    { return g_autoload; }
int         picker_rotate(void)      { return g_rotate; }

static void config_path(char *out, size_t n) { snprintf(out, n, "%s/config.txt", g_cfg->data_dir); }

static void load_config(void)
{
    char path[800], line[800];
    FILE *f;
    snprintf(g_content, sizeof(g_content), "%s/%ss", g_cfg->data_dir, g_cfg->content_noun);
    config_path(path, sizeof(path));
    f = fopen(path, "r");
    if (f) {
        while (fgets(line, sizeof(line), f)) {
            line[strcspn(line, "\r\n")] = 0;
            /* "content=" is the generic key; "roms=" accepted as an alias for older configs */
            if      (!strncmp(line, "content=", 8) && line[8]) snprintf(g_content, sizeof(g_content), "%s", line + 8);
            else if (!strncmp(line, "roms=", 5)    && line[5]) snprintf(g_content, sizeof(g_content), "%s", line + 5);
            else if (!strncmp(line, "autoload=", 9))           snprintf(g_autoload, sizeof(g_autoload), "%s", line + 9);
            else if (!strncmp(line, "rotate=", 7))             g_rotate = strncmp(line + 7, "off", 3) != 0;
        }
        fclose(f);
    } else {
        mkdirs(g_cfg->data_dir);
        f = fopen(path, "w");
        if (f) {
            fprintf(f,
                "# %s player config\n"
                "content=%s\n"
                "# rotate=on    # off = fill the screen without rotating (vertical games look sideways)\n"
                "# autoload=NAME  # boot straight into one item\n",
                g_cfg->app_name, g_content);
            fclose(f);
        }
    }
    mkdirs(g_content);   /* improvement: make sure the content dir exists on first run */
}

static int set_cmp(const void *a, const void *b)   /* case-insensitive, over the malloc'd name pointers */
{
    return strcasecmp(*(const char *const *)a, *(const char *const *)b);
}

static void scan_content(void)
{
    DIR *d;
    struct dirent *e;
    size_t extlen = strlen(g_cfg->ext);
    int i;
    for (i = 0; i < g_nsets; i++) { free(g_sets[i]); g_sets[i] = NULL; }   /* names are malloc'd */
    g_nsets = 0;
    d = opendir(g_content);
    if (!d) return;
    while ((e = readdir(d)) != NULL && g_nsets < MAXSETS) {
        const char *n = e->d_name;
        size_t L = strlen(n), stem;
        char *name;
        if (L <= extlen + 1 || n[L - extlen - 1] != '.' || strcasecmp(n + L - extlen, g_cfg->ext) != 0)
            continue;
        stem = L - extlen - 1;                       /* name without ".<ext>" - any length */
        name = (char *)malloc(stem + 1);
        if (!name) continue;
        memcpy(name, n, stem);
        name[stem] = 0;
        g_sets[g_nsets++] = name;
    }
    closedir(d);
    qsort(g_sets, g_nsets, sizeof(g_sets[0]), set_cmp);
    if (g_cfg->label_for) {
        int i;
        for (i = 0; i < g_nsets; i++) {                 /* label after sort so indices match */
            const char *s = g_cfg->label_for(g_sets[i]);
            snprintf(g_label[i], sizeof(g_label[0]), "%s", (s && *s) ? s : "");
        }
    }
}

/* ---- help / empty-state screen (improvement) ---- */

static void draw_help(void)
{
    SDL_Color head = {120,200,255,255}, white = {225,225,225,255}, dim = {160,160,170,255};
    char l[256];
    int y = 86, i;
    SDL_SetRenderDrawColor(g_ren, 18, 18, 28, 255);
    SDL_RenderClear(g_ren);
    snprintf(l, sizeof(l), "%s  -  help", g_cfg->app_name);
    draw_text(90, 40, head, l);

    /* where content goes + how to change the folder */
    snprintf(l, sizeof(l), "Put %s .%s files in this folder:", g_cfg->content_noun, g_cfg->ext);
    draw_text(90, y, white, l); y += 34;
    draw_text(120, y, head, g_content); y += 32;
    draw_text(90, y, dim, "(fs:/vol/external01 is the SD card root)"); y += 38;
    draw_text(90, y, white, "Use a different folder: edit the content= line of"); y += 34;
    snprintf(l, sizeof(l), "%s/config.txt", g_cfg->data_dir); draw_text(120, y, head, l); y += 38;
    if (g_cfg->help_extra && *g_cfg->help_extra) { draw_text(90, y, dim, g_cfg->help_extra); y += 38; }

    /* controls: the generic menu keys, then the app's in-game map */
    draw_text(90, y, head, "Menu:"); y += 32;
    draw_text(120, y, white, "Up/Down or left stick move   A start   Y help   hold +/- quit"); y += 40;
    if (g_cfg->controls) {
        draw_text(90, y, head, "In game:"); y += 32;
        for (i = 0; g_cfg->controls[i]; i++) { draw_text(120, y, white, g_cfg->controls[i]); y += 32; }
    }
    draw_text(90, y + 14, dim, "Press A to continue");
    SDL_RenderPresent(g_ren);
}

static void help_screen(void)
{
    SDL_Event ev;
    int a_prev = 1;
    for (;;) {
        while (SDL_PollEvent(&ev)) if (ev.type == SDL_CONTROLLERDEVICEADDED) open_pads();
        {
            int a = pad_btn(SDL_CONTROLLER_BUTTON_A) || pad_btn(SDL_CONTROLLER_BUTTON_B);
            if (a && !a_prev) return;
            a_prev = a;
        }
        draw_help();
        SDL_Delay(16);
    }
}

/* ---- the menu ---- */

const char *picker_run(void)
{
    const int VIS = 15, ROWH = 36, X = 90, Y0 = 130;
    SDL_Color head = {120,200,255,255}, white = {225,225,225,255}, hi = {255,215,70,255};
    int sel = 0, top = 0, a_prev = 1, y_prev = 1, qhold = 0, armed = 0, dir_prev = 0;
    Uint32 nav_next = 0;
    char buf[128];
    SDL_Event ev;

    scan_content();
    if (g_nsets == 0) { help_screen(); return NULL; }   /* empty -> help, then fall back to caller */

    for (;;) {
        Uint32 now;
        int a, y;
        while (SDL_PollEvent(&ev)) {
            if (ev.type == SDL_QUIT) return NULL;
            if (ev.type == SDL_CONTROLLERDEVICEADDED) open_pads();
        }
        if (pad_btn(SDL_CONTROLLER_BUTTON_START) && pad_btn(SDL_CONTROLLER_BUTTON_BACK)) {
            if (armed && ++qhold >= 30) return NULL;    /* hold +/- (once released) to quit */
        } else { qhold = 0; armed = 1; }

        y = pad_btn(SDL_CONTROLLER_BUTTON_Y);           /* Y = help */
        if (y && !y_prev) help_screen();
        y_prev = y;

        now = SDL_GetTicks();
        {   /* up/down: a step on the press, then repeats after 400 ms every 80 ms */
            int dir = pad_dir(1) ? 1 : (pad_dir(0) ? -1 : 0);
            if (dir && dir != dir_prev)      { sel = (sel + dir + g_nsets) % g_nsets; nav_next = now + 400; }
            else if (dir && now >= nav_next) { sel = (sel + dir + g_nsets) % g_nsets; nav_next = now + 80; }
            dir_prev = dir;
        }

        a = pad_btn(SDL_CONTROLLER_BUTTON_A) || pad_btn(SDL_CONTROLLER_BUTTON_B);
        if (a && !a_prev) return g_sets[sel];
        a_prev = a;

        if (sel < top) top = sel;
        if (sel >= top + VIS) top = sel - VIS + 1;

        SDL_SetRenderDrawColor(g_ren, 18, 18, 28, 255);
        SDL_RenderClear(g_ren);
        snprintf(buf, sizeof(buf), "%s  -  pick a %s   (A play, Up/Down or stick, Y help, hold +/- quit)",
                 g_cfg->app_name, g_cfg->content_noun);
        draw_text(X, 48, head, buf);
        {
            int i;
            for (i = 0; i < VIS && top + i < g_nsets; i++) {
                int idx = top + i;
                const char *pfx = idx == sel ? "> " : "   ";
                if (g_label[idx][0]) snprintf(buf, sizeof(buf), "%s%s   (%s)", pfx, g_sets[idx], g_label[idx]);
                else                 snprintf(buf, sizeof(buf), "%s%s", pfx, g_sets[idx]);
                draw_text(X, Y0 + i * ROWH, idx == sel ? hi : white, buf);
            }
            snprintf(buf, sizeof(buf), "%d / %d", sel + 1, g_nsets);
            draw_text(X, Y0 + VIS * ROWH + 10, head, buf);
        }
        SDL_RenderPresent(g_ren);
        SDL_Delay(16);
    }
}

/* ---- lifecycle ---- */

int picker_init(const picker_cfg *cfg)
{
    g_cfg = cfg;
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMECONTROLLER) != 0) return 1;
    g_win = SDL_CreateWindow(cfg->app_name, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                             1280, 720, SDL_WINDOW_FULLSCREEN);
    if (!g_win) return 2;
    g_ren = SDL_CreateRenderer(g_win, -1, SDL_RENDERER_ACCELERATED);
    if (!g_ren) return 3;
    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "1");
    if (TTF_Init() == 0)
        g_font = TTF_OpenFont(cfg->font_file, 26);   /* font failure is non-fatal (text just won't draw) */
    open_pads();
    load_config();
    return 0;
}

void picker_quit(void)
{
    int i;
    if (g_tex) { SDL_DestroyTexture(g_tex); g_tex = NULL; g_tex_w = g_tex_h = 0; }
    for (i = 0; i < g_nsets; i++) { free(g_sets[i]); g_sets[i] = NULL; }
    g_nsets = 0;
    for (i = 0; i < 2; i++) if (g_pads[i]) { SDL_GameControllerClose(g_pads[i]); g_pads[i] = NULL; }
    if (g_font) { TTF_CloseFont(g_font); g_font = NULL; }
    TTF_Quit();
    SDL_Quit();
}
