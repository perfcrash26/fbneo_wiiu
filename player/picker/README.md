# picker — SDL2 content-picker shell for Wii U (wut)

A self-contained, copy-pasteable frontend shell: fullscreen window + renderer + font, two game
controllers (GamePad / Pro Controller / Wiimote+Classic, with hot-plug and a START+BACK "leave"
combo), a `config.txt` it creates on first run, a directory scan for one file extension, an
on-screen scrolling menu with optional per-item labels, a built-in install/help screen, a
transient message box, and a rotation-aware framebuffer blit.

It knows nothing about FBNeo, libretro, or any emulator core — it only uses SDL2 and SDL2_ttf.
The host app keeps `main()` and its own content loop and calls the picker as a toolkit.

## Adopt it in another project

1. **Copy this whole folder** (`picker.c`, `picker.h`, `Roboto-Regular.ttf`) into your project.
   Drift between copies is expected and fine — there is no shared dependency.
2. **Build** `picker.c` with your SDL2 + SDL2_ttf (wut) flags and link it in. (It's C; same flags
   as the rest of your frontend. See the fbneo `player/Makefile` for an example rule.)
3. **Bundle the font**: put `Roboto-Regular.ttf` into the wuhb `/content` and pass its runtime
   path (`fs:/vol/content/Roboto-Regular.ttf`) as `cfg.font_file`.
4. **Write one glue file** — fill a `picker_cfg` and drive the loop:

   ```c
   static const picker_cfg cfg = {
       .app_name = "MyApp", .content_noun = "game",
       .data_dir = "fs:/vol/external01/wiiu/myapp",
       .font_file = "fs:/vol/content/Roboto-Regular.ttf",
       .ext = "bin", .label_for = NULL, .help_extra = NULL,
   };
   picker_init(&cfg);
   for (;;) {
       const char *name = picker_run();      /* chosen item, or NULL to quit */
       if (!name) break;
       my_load_and_run(name);                /* your content loop; call picker_pump()/
                                                picker_present() inside it */
   }
   picker_quit();
   ```

## The seam (what you provide)

- `cfg.ext` — the file extension to list (no dot).
- `cfg.label_for(name)` — optional per-item tag shown as `name   (tag)`; return `NULL`/`""` for none.
- `cfg.content_noun` / `cfg.app_name` / `cfg.help_extra` — wording for the header and help screen.
- `cfg.controls` — optional NULL-terminated lines describing your app's in-game button map; shown on the
  help screen under the generic menu controls. The help screen (empty-state and `Y`) is the one place a
  user learns where content goes, how to change the folder, and the controls — there is no separate splash.
- Your content loop: call `picker_pump(&want_quit, &want_menu)` each frame, read inputs via
  `picker_pad(port)`, and show video with `picker_present(data, w, h, pitch, sdl_fmt, rot_ccw)`.

Config lives at `<data_dir>/config.txt`, content defaults to `<data_dir>/<noun>s/`; both the config
key `content=` and the legacy `roms=` are accepted. The content dir is auto-created on first run.

## Notes

- Font load failure is non-fatal (text just doesn't draw) — the app still runs.
- `picker_present` owns one streaming texture and recreates it when geometry/format changes, so you
  can switch content without managing textures.
- Controllers: whatever SDL2-wut exposes as *game controllers*. GamePad and Pro Controller are
  reliable; Wiimote+Classic depends on SDL2-wut presenting it as a mapped controller.
