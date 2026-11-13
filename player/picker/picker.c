/* picker - self-contained SDL2 content-picker shell for Wii U (wut). See picker.h / README.md. */
#include "picker.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>	/* strcasecmp */
#include <dirent.h>
#include <sys/stat.h>
#include <SDL_ttf.h>
#ifdef __WIIU__
#include <gx2/display.h>   /* GX2SetTVEnable / GX2SetDRCEnable: screen selection */
#endif

#define MAXSETS 1024
#define LOGICAL_W 1280             /* everything draws in this logical space; SDL scales it to the TV */
#define LOGICAL_H 720
#define HOLD_FRAMES 30             /* + and - held this many polls (~0.5 s) = leave */

static const picker_cfg *g_cfg;
static SDL_Window   *g_win;
static SDL_Renderer *g_ren;
static TTF_Font     *g_font;
static TTF_Font     *g_font_big;   /* larger font for the game-list rows (legibility on a TV) */
static SDL_Texture  *g_tex;
static unsigned      g_tex_w, g_tex_h;
static Uint32        g_tex_fmt;
static SDL_GameController *g_pads[2];   /* [0]=player 1, [1]=player 2 */
static int           g_exit_hold;

static char g_content[768];
static char g_autoload[64];
static int  g_rotate = 1;

static char *g_sets[MAXSETS];		/* item names (malloc'd, any length) */
static char *g_titles[MAXSETS];		/* display titles (malloc'd) when cfg->title_for is set, else NULL */
static char  g_label[MAXSETS][28];
static int   g_nsets;
static char *g_fit[MAXSETS];		/* shown text cut to the row width (fit_title cache) */
static int   g_fit_w[MAXSETS];
static char  g_last[64];		/* last item started: the list reopens on it */

static double g_aspect;      /* intended display aspect of the presented image, 0 = square pixels */
static int    g_sharp;       /* nearest-neighbour scaling */
static int    g_screens;     /* 0 both, 1 TV only, 2 GamePad only */

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

/* Text textures live until the frame is presented: the Wii U renderer draws at present time, so a
 * texture destroyed right after SDL_RenderCopy can have its memory reused by the next text (seen as one
 * row showing another row's text). */
#define MAX_FRAME_TEX 96
static SDL_Texture *g_frame_tex[MAX_FRAME_TEX];
static int g_nframe_tex;
static void present(void)
{
    SDL_RenderPresent(g_ren);
    while (g_nframe_tex > 0) SDL_DestroyTexture(g_frame_tex[--g_nframe_tex]);
}

static void draw_text_f(TTF_Font *font, int x, int y, SDL_Color c, const char *s)
{
    SDL_Surface *surf;
    SDL_Texture *t;
    SDL_Rect dst;
    if (!font || !s || !*s) return;
    surf = TTF_RenderUTF8_Blended(font, s, c);
    if (!surf) return;
    t = SDL_CreateTextureFromSurface(g_ren, surf);
    dst.x = x; dst.y = y; dst.w = surf->w; dst.h = surf->h;
    SDL_FreeSurface(surf);
    if (!t) return;
    if (g_nframe_tex == MAX_FRAME_TEX) present();   /* not expected: more text than one frame holds */
    SDL_RenderCopy(g_ren, t, NULL, &dst);
    g_frame_tex[g_nframe_tex++] = t;
}
static void draw_text(int x, int y, SDL_Color c, const char *s) { draw_text_f(g_font, x, y, c, s); }

/* in-game quit hint: a banner shown over the game for a few seconds when a set starts, so players
 * learn how to get back to the menu (HOME doesn't exit). Set by the host via picker_set_hint(). */
static Uint32 g_hint_until = 0;
static char   g_hint_text[96];

void picker_set_hint(const char *text, unsigned ms)
{
    if (!text || !*text) { g_hint_until = 0; return; }
    snprintf(g_hint_text, sizeof(g_hint_text), "%s", text);
    g_hint_until = SDL_GetTicks() + ms;
}

static void draw_hint(void)
{
    int tw = 0, th = 0;
    SDL_Rect bar;
    SDL_Color fg = { 255, 235, 120, 255 };
    if (!g_font || !g_hint_text[0] || SDL_GetTicks() >= g_hint_until) return;
    if (TTF_SizeUTF8(g_font, g_hint_text, &tw, &th) != 0) return;
    bar.w = LOGICAL_W; bar.x = 0; bar.h = th + 20; bar.y = LOGICAL_H - bar.h - 22;   /* banner across the bottom */
    SDL_SetRenderDrawBlendMode(g_ren, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(g_ren, 0, 0, 0, 175);
    SDL_RenderFillRect(g_ren, &bar);
    draw_text_f(g_font, (LOGICAL_W - tw) / 2, bar.y + 10, fg, g_hint_text);
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
            if (down ? (ay > PICKER_STICK_DZ) : (ay < -PICKER_STICK_DZ)) return 1;
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
        if (++g_exit_hold >= HOLD_FRAMES && want_menu) *want_menu = 1;
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
        if (g_tex) SDL_SetTextureScaleMode(g_tex, g_sharp ? SDL_ScaleModeNearest : SDL_ScaleModeLinear);
    }
    SDL_UpdateTexture(g_tex, NULL, data, (int)pitch);

    SDL_RenderGetLogicalSize(g_ren, &ww, &wh);          /* draw into the logical space (SDL scales to TV) */
    if (ww == 0 || wh == 0) SDL_GetRendererOutputSize(g_ren, &ww, &wh);  /* fallback if logical size unset */
    rot90 = (rot_ccw == 1 || rot_ccw == 3);   /* letterbox to the DISPLAYED aspect */
    sa = rot90 ? (double)h / (double)w : (double)w / (double)h;   /* square pixels */
    if (g_aspect > 0.0)   /* intended aspect, in the shown image's orientation (wide stays wide, tall tall) */
        sa = ((sa >= 1.0) == (g_aspect >= 1.0)) ? g_aspect : 1.0 / g_aspect;
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
    draw_hint();                 /* quit banner over the game for the first few seconds */
    present();
}

void picker_set_aspect(double aspect) { g_aspect = aspect > 0.0 ? aspect : 0.0; }

void picker_set_filter(int sharp)
{
    g_sharp = sharp != 0;
    if (g_tex) SDL_SetTextureScaleMode(g_tex, g_sharp ? SDL_ScaleModeNearest : SDL_ScaleModeLinear);
}

void picker_set_screens(int mode)
{
    g_screens = (mode >= 0 && mode <= 2) ? mode : 0;
#ifdef __WIIU__
    GX2SetTVEnable(g_screens != 2);    /* SDL draws the same image to both; this only blanks one */
    GX2SetDRCEnable(g_screens != 1);
#endif
}
int picker_screens(void) { return g_screens; }

SDL_Renderer *picker_renderer(void) { return g_ren; }

void picker_message(const char *line1, const char *line2)
{
    SDL_Color c = { 255, 120, 120, 255 };
    Uint32 end = SDL_GetTicks() + 1600;
    SDL_Event e;
    while (SDL_GetTicks() < end) {
        while (SDL_PollEvent(&e))
            if (e.type == SDL_QUIT) { SDL_PushEvent(&e); return; }   /* leave the quit for the caller's loop */
        SDL_SetRenderDrawColor(g_ren, 18, 18, 28, 255);
        SDL_RenderClear(g_ren);
        draw_text(80, 300, c, line1);
        draw_text(80, 344, c, line2);
        present();
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
    mkdirs(g_content);   /* make sure the content dir exists on first run */
}

/* Generic: read an integer "key=N" from config.txt, or def if absent. Valid after picker_init.
 * Lets a host app read its own config keys (e.g. frameskip) without the picker knowing about them. */
int picker_cfg_int(const char *key, int def)
{
    char path[800], line[800];
    size_t kl = strlen(key);
    FILE *f;
    int v = def;
    config_path(path, sizeof(path));
    f = fopen(path, "r");
    if (!f) return def;
    while (fgets(line, sizeof(line), f))
        if (!strncmp(line, key, kl) && line[kl] == '=')
            v = atoi(line + kl + 1);
    fclose(f);
    return v;
}

/* Generic: copy the value of "key=..." (trailing whitespace stripped) into buf, or def if absent. */
const char *picker_cfg_str(const char *key, const char *def, char *buf, size_t size)
{
    char path[800], line[800];
    size_t kl = strlen(key);
    FILE *f;
    snprintf(buf, size, "%s", def);
    config_path(path, sizeof(path));
    f = fopen(path, "r");
    if (!f) return buf;
    while (fgets(line, sizeof(line), f))
        if (!strncmp(line, key, kl) && line[kl] == '=') {
            size_t n;
            snprintf(buf, size, "%s", line + kl + 1);
            n = strlen(buf);
            while (n && (buf[n - 1] == '\n' || buf[n - 1] == '\r' || buf[n - 1] == ' ' || buf[n - 1] == '\t'))
                buf[--n] = 0;
        }
    fclose(f);
    return buf;
}

typedef struct { char *name, *title; } set_ent;
static int set_cmp(const void *a, const void *b)   /* case-insensitive, by title when there is one */
{
    const set_ent *x = (const set_ent *)a, *y = (const set_ent *)b;
    int c = strcasecmp(x->title ? x->title : x->name, y->title ? y->title : y->name);
    return c ? c : strcasecmp(x->name, y->name);
}

/* what the list shows and sorts by for item i */
static const char *disp(int i) { return g_titles[i] ? g_titles[i] : g_sets[i]; }

static void scan_content(void)
{
    DIR *d;
    struct dirent *e;
    size_t extlen = strlen(g_cfg->ext);
    int i;
    static set_ent ents[MAXSETS];
    for (i = 0; i < g_nsets; i++) {   /* names and titles are malloc'd */
        free(g_sets[i]); g_sets[i] = NULL;
        free(g_titles[i]); g_titles[i] = NULL;
    }
    for (i = 0; i < MAXSETS; i++) { free(g_fit[i]); g_fit[i] = NULL; }
    g_nsets = 0;
    d = opendir(g_content);
    if (!d) return;
    while ((e = readdir(d)) != NULL) {
        const char *n = e->d_name;
        size_t L = strlen(n), stem;
        char *name;
        if (L <= extlen + 1 || n[L - extlen - 1] != '.' || strcasecmp(n + L - extlen, g_cfg->ext) != 0)
            continue;
        stem = L - extlen - 1;                       /* name without ".<ext>" - any length */
        if (g_nsets == MAXSETS) { SDL_Log("picker: more than %d items, list truncated", MAXSETS); break; }
        name = (char *)malloc(stem + 1);
        if (!name) continue;
        memcpy(name, n, stem);
        name[stem] = 0;
        g_sets[g_nsets++] = name;
    }
    closedir(d);
    for (i = 0; i < g_nsets; i++) {
        const char *t = g_cfg->title_for ? g_cfg->title_for(g_sets[i]) : NULL;
        ents[i].name = g_sets[i];
        ents[i].title = (t && *t) ? strdup(t) : NULL;   /* copy: the host may return a shared buffer */
    }
    qsort(ents, g_nsets, sizeof(ents[0]), set_cmp);
    for (i = 0; i < g_nsets; i++) { g_sets[i] = ents[i].name; g_titles[i] = ents[i].title; }
    if (g_cfg->label_for) {
        for (i = 0; i < g_nsets; i++) {                 /* label after sort so indices match */
            const char *s = g_cfg->label_for(g_sets[i]);
            snprintf(g_label[i], sizeof(g_label[0]), "%s", (s && *s) ? s : "");
        }
    }
}

static int pad_axis(SDL_GameControllerAxis ax)   /* a trigger (ZL/ZR) pressed on either pad */
{
    int i;
    for (i = 0; i < 2; i++)
        if (g_pads[i] && SDL_GameControllerGetAxis(g_pads[i], ax) > PICKER_TRIGGER_ON) return 1;
    return 0;
}

static int letter_of(int i)   /* first letter of the shown text, upper case; digits/others = '#' */
{
    const unsigned char *s = (const unsigned char *)disp(i);
    while (*s && !((*s >= 'a' && *s <= 'z') || (*s >= 'A' && *s <= 'Z') || (*s >= '0' && *s <= '9'))) s++;
    if (*s >= 'a' && *s <= 'z') return *s - 32;
    if (*s >= 'A' && *s <= 'Z') return *s;
    return '#';
}

/* from sel, the first item of the next (dir 1) or previous (dir -1) letter group, wrapping */
static int letter_jump(int sel, int dir)
{
    int k = letter_of(sel), j = sel, n;
    for (n = 0; n < g_nsets; n++) {
        j = (j + dir + g_nsets) % g_nsets;
        if (letter_of(j) != k) break;
    }
    if (n == g_nsets) return sel;                       /* one letter only */
    k = letter_of(j);
    while (j > 0 && letter_of(j - 1) == k) j--;         /* start of that group */
    return j;
}

/* the shown text of item i cut to maxw pixels with "...", cached per item and width */
static const char *fit_title(TTF_Font *font, int i, int maxw)
{
    const char *s = disp(i);
    char tmp[256];
    int w = 0, h = 0;
    size_t n;
    if (g_fit[i] && g_fit_w[i] == maxw) return g_fit[i];
    free(g_fit[i]); g_fit[i] = NULL;
    snprintf(tmp, sizeof(tmp), "%s", s);
    if (font && TTF_SizeUTF8(font, tmp, &w, &h) == 0 && w > maxw) {
        n = strlen(tmp);
        while (n > 1) {
            n--;
            while (n > 1 && (tmp[n] & 0xC0) == 0x80) n--;   /* don't cut inside a UTF-8 character */
            snprintf(tmp + n, sizeof(tmp) - n, "...");
            if (TTF_SizeUTF8(font, tmp, &w, &h) == 0 && w <= maxw) break;
        }
    }
    g_fit[i] = strdup(tmp);
    g_fit_w[i] = maxw;
    return g_fit[i] ? g_fit[i] : s;
}

/* ---- help / empty-state screen ---- */

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
    draw_text(120, y, white, "Up/Down move   L/R page   ZL/ZR letter   A start   Y help   hold +/- quit"); y += 40;
    if (g_cfg->controls) {
        draw_text(90, y, head, "In game:"); y += 32;
        for (i = 0; g_cfg->controls[i]; i++) { draw_text(120, y, white, g_cfg->controls[i]); y += 32; }
    }
    y += 8;
    draw_text(90, y, dim, "HOME does not exit - hold +/- in this menu to quit."); y += 34;
    draw_text(90, y + 14, dim, "Press A to continue");
    present();
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
    /* list rows span ~96..682 of the logical 720, clear of the overscan edge and the header; the dimmed
     * name/label column ends at META_R */
    const int VIS = 12, ROWH = 48, X = 90, Y0 = 96, META_R = 1190;
    TTF_Font *lf = g_font_big ? g_font_big : g_font;
    SDL_Color head = {120,200,255,255}, white = {225,225,225,255}, hi = {255,215,70,255}, dim = {140,140,155,255};
    int sel = 0, top = 0, a_prev = 1, y_prev = 1, qhold = 0, armed = 0, dir_prev = 0;
    Uint32 nav_next = 0;
    char buf[128];
    SDL_Event ev;

    scan_content();
    if (g_nsets == 0) { help_screen(); return NULL; }   /* empty -> help, then fall back to caller */
    {
        int i;
        for (i = 0; i < g_nsets; i++) if (!strcmp(g_sets[i], g_last)) { sel = i; break; }
        top = sel >= VIS ? sel - VIS / 2 : 0;
        if (top > g_nsets - VIS) top = g_nsets > VIS ? g_nsets - VIS : 0;
    }

    for (;;) {
        Uint32 now;
        int a, y;
        while (SDL_PollEvent(&ev)) {
            if (ev.type == SDL_QUIT) return NULL;
            if (ev.type == SDL_CONTROLLERDEVICEADDED) open_pads();
        }
        if (pad_btn(SDL_CONTROLLER_BUTTON_START) && pad_btn(SDL_CONTROLLER_BUTTON_BACK)) {
            if (armed && ++qhold >= HOLD_FRAMES) return NULL;    /* hold +/- (once released) to quit */
        } else { qhold = 0; armed = 1; }

        y = pad_btn(SDL_CONTROLLER_BUTTON_Y);           /* Y = help */
        if (y && !y_prev) help_screen();
        y_prev = y;

        now = SDL_GetTicks();
        {   /* up/down step, L/R page, ZL/ZR next/previous letter: once on the press, then repeating
             * after 400 ms (every 80 ms for steps, 250 ms for pages and letters) */
            int act = pad_dir(1) ? 1 : pad_dir(0) ? -1
                    : pad_btn(SDL_CONTROLLER_BUTTON_RIGHTSHOULDER) ? 2 : pad_btn(SDL_CONTROLLER_BUTTON_LEFTSHOULDER) ? -2
                    : pad_axis(SDL_CONTROLLER_AXIS_TRIGGERRIGHT) ? 3 : pad_axis(SDL_CONTROLLER_AXIS_TRIGGERLEFT) ? -3 : 0;
            int go = act && (act != dir_prev || now >= nav_next);
            if (go) {
                int a1 = act < 0 ? -act : act, s = act < 0 ? -1 : 1;
                if (a1 == 1) sel = (sel + s + g_nsets) % g_nsets;
                else if (a1 == 2) { sel += s * VIS; if (sel < 0) sel = 0; if (sel >= g_nsets) sel = g_nsets - 1; }
                else sel = letter_jump(sel, s);
                nav_next = now + (act != dir_prev ? 400 : (a1 == 1 ? 80 : 250));
            }
            dir_prev = act;
        }

        a = pad_btn(SDL_CONTROLLER_BUTTON_A) || pad_btn(SDL_CONTROLLER_BUTTON_B);
        if (a && !a_prev) {
            /* draw a "Starting" frame before returning: the host's content load can take several
             * seconds, and nothing repaints during it, so this avoids a black "is it hung?" screen */
            SDL_SetRenderDrawColor(g_ren, 18, 18, 28, 255);
            SDL_RenderClear(g_ren);
            snprintf(buf, sizeof(buf), "Starting  %s", fit_title(lf, sel, 1100));
            draw_text_f(lf, X, 300, hi, buf);                                /* big legible font */
            draw_text_f(lf, X, 352, white, "loading, please wait...");
            present();
            snprintf(g_last, sizeof(g_last), "%s", g_sets[sel]);
            return g_sets[sel];
        }
        a_prev = a;

        if (sel < top) top = sel;
        if (sel >= top + VIS) top = sel - VIS + 1;

        SDL_SetRenderDrawColor(g_ren, 18, 18, 28, 255);
        SDL_RenderClear(g_ren);
        snprintf(buf, sizeof(buf), "%s  -  pick a %s   (A play, L/R page, ZL/ZR letter, Y help, hold +/- quit)",
                 g_cfg->app_name, g_cfg->content_noun);
        draw_text(X, 48, head, buf);
        {
            int i;
            for (i = 0; i < VIS && top + i < g_nsets; i++) {
                int idx = top + i, ry = Y0 + i * ROWH, mw = 0, mh = 0, mx = META_R;
                char meta[128];
                /* right side, small and dim: the item name (when a title is shown) and its label */
                if (g_titles[idx] && g_label[idx][0]) snprintf(meta, sizeof(meta), "%s  %s", g_sets[idx], g_label[idx]);
                else if (g_titles[idx])               snprintf(meta, sizeof(meta), "%s", g_sets[idx]);
                else                                  snprintf(meta, sizeof(meta), "%s", g_label[idx]);
                if (meta[0] && g_font && TTF_SizeUTF8(g_font, meta, &mw, &mh) == 0) {
                    mx = META_R - mw;
                    draw_text(mx, ry + 8, idx == sel ? hi : dim, meta);
                }
                snprintf(buf, sizeof(buf), "%s%s", idx == sel ? "> " : "   ", fit_title(lf, idx, mx - X - 70));
                draw_text_f(lf, X, ry, idx == sel ? hi : white, buf);
            }
            snprintf(buf, sizeof(buf), "%d / %d", sel + 1, g_nsets);
            draw_text(X, Y0 + VIS * ROWH + 10, head, buf);
        }
        present();
        SDL_Delay(16);
    }
}

/* ---- lifecycle ---- */

int picker_init(const picker_cfg *cfg)
{
    g_cfg = cfg;
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMECONTROLLER) != 0) return 1;
    g_win = SDL_CreateWindow(cfg->app_name, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                             LOGICAL_W, LOGICAL_H, SDL_WINDOW_FULLSCREEN);
    if (!g_win) return 2;
    g_ren = SDL_CreateRenderer(g_win, -1, SDL_RENDERER_ACCELERATED);
    if (!g_ren) return 3;
    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "1");
    /* fixed logical space, scaled by SDL to the real TV output (e.g. 1080p); picker_present uses it too */
    SDL_RenderSetLogicalSize(g_ren, LOGICAL_W, LOGICAL_H);
    if (TTF_Init() == 0) {
        g_font = TTF_OpenFont(cfg->font_file, 26);   /* font failure is non-fatal (text just won't draw) */
        g_font_big = TTF_OpenFont(cfg->font_file, 38);  /* game list; falls back to g_font below if NULL */
    }
    open_pads();
    load_config();
    return 0;
}

void picker_quit(void)
{
    int i;
    if (g_tex) { SDL_DestroyTexture(g_tex); g_tex = NULL; g_tex_w = g_tex_h = 0; }
    for (i = 0; i < g_nsets; i++) { free(g_sets[i]); g_sets[i] = NULL; free(g_titles[i]); g_titles[i] = NULL; }
    for (i = 0; i < MAXSETS; i++) { free(g_fit[i]); g_fit[i] = NULL; }
    g_nsets = 0;
    for (i = 0; i < 2; i++) if (g_pads[i]) { SDL_GameControllerClose(g_pads[i]); g_pads[i] = NULL; }
    if (g_font_big) { TTF_CloseFont(g_font_big); g_font_big = NULL; }
    if (g_font) { TTF_CloseFont(g_font); g_font = NULL; }
    TTF_Quit();
    SDL_Quit();
}
