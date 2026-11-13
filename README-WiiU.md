# FBNeo Player (Wii U)

Cave CV1000 and CPS3 arcade game support (see table)

A standalone build of **FinalBurn Neo** (the libretro arcade core) optimized for Nintendo Wii U.

**Version 1.0.0.3-U2**

See: AI / Claude Disclosure below

## Compatibility & speed

| Hardware | Speed |
|---|---|
| CPS1 | full speed, smooth (vsync) |
| CPS2 | full speed, smooth (vsync) |
| CPS3 | full speed, smooth (vsync); short slowdowns in the heaviest parts |
| Neo Geo | full speed (runs at its own 59.19 Hz, so not vsync-locked) |
| CV1000 (Cave) | full speed, except heavy scenes |

Romsets must match **FinalBurn Neo 1.0.0.3**. Neo Geo games need `neogeo.zip` (BIOS).

## Installing

Requires a Wii U running the Aroma homebrew environment.

1. Copy `fbneo_player.wuhb` to `sd:/wiiu/apps/fbneo_player/`.
2. Put your romsets (`.zip`, FinalBurn Neo 1.0.0.3) in `sd:/wiiu/fbneo_player/roms/`. Neo Geo games
   also need `neogeo.zip` there.
3. Launch **FBNeo Player** from the Wii U Menu.

On first launch the app creates its folders under `sd:/wiiu/fbneo_player/`:

| Path | Contents |
|---|---|
| `config.txt` | settings (below) |
| `roms/` | your romsets |
| `states/` | save states, one per game |
| `saves/` | in-game saves (high scores, NVRAM) |
| `system/` | BIOS / system files; `system/fbneo/hiscore.dat` is installed on first run |

## Settings (config.txt)

One `key=value` per line in `sd:/wiiu/fbneo_player/config.txt`; restart the app after editing.

| Key | Values (default first) | Effect |
|---|---|---|
| `vsync` | `auto`, `1`, `0` | Vsync pacing. `auto` turns it on for games within 0.7% of the TV's refresh rate (59.94 Hz, or 50 Hz in 576i) and off for the rest, which then run at their own rate. See the note below. |
| `aspect` | `auto`, `pixel` | `auto` shows games at the aspect FinalBurn Neo reports (4:3); `pixel` uses square pixels. |
| `filter` | `smooth`, `sharp` | Scaling: linear (`smooth`) or nearest-neighbour (`sharp`). |
| `screens` | `both`, `tv`, `gamepad` | Which screens show the game at start; hold ZL + ZR and press X to change it in game. |
| `hiscores` | `1`, `0` | Save high scores for games listed in `hiscore.dat`. |
| `map_<button>` | a button name | Makes a button act as another, e.g. `map_y=a` (Y does what A does; A still works). |
| `turbo` | button list | Autofire while held, e.g. `turbo=a,x`. |
| `turbo_hz` | `15` (1–30) | Autofire presses per second. |
| `frameskip` | `0` (0–9) | Draw only 1 of every N+1 frames; can help CPU-bound games. |
| `autoskip` | `0` (0–100) | Automatic frameskip: skip drawing while the audio queue is below N% of its target. |
| `rotate` | `on`, `off` | Rotate vertical (TATE) games. |
| `content` | path | ROM folder (default `roms/`). |
| `autoload` | set name | Boot straight into one game, skipping the list. |
| `depth32` | `auto`, `1`, `0` | 32-bit output. `auto` = CV1000 only (faster there); `1` for all games makes some show black. |

Button names for `map_` and `turbo`: `a b x y l r zl zr l3 r3`. Start (**+**), Coin (**−**) and
the d-pad can't be remapped.

**Vsync note:** with vsync on, a game that can't hold full speed on the console stutters and its sound
crackles, because every late frame waits for the next TV refresh. Shinobi is one: it runs at about 54 fps
with vsync. That is why `vsync` is a setting — use `vsync=0` for such games.

TBD: an in-app settings screen for these options.

## Controls

| Action | Menu | In game |
|---|---|---|
| Move | D-pad or left stick (Up/Down) | D-pad or left stick |
| Page up / down | L / R | — |
| Previous / next letter | ZL / ZR | — |
| Select / play | A or B | — |
| Game buttons | — | A, B, X, Y, L, R, ZL, ZR, L3, R3 |
| Start / Coin | — | **+** start · **−** coin |
| Help | Y | — |
| Leave help page | A or B | — |
| Save state | — | hold ZL + ZR, press Y |
| Load state | — | hold ZL + ZR, press A |
| Screens (TV + GamePad / TV / GamePad) | — | hold ZL + ZR, press X |
| Back to menu | — | hold **+** and **−** together (~0.5 s) |
| Quit the app | hold **+** and **−** together | — |

Player 1 uses the first controller (GamePad, Pro Controller, or Wii Remote + Classic Controller),
player 2 the second. Save states and hotkeys use player 1's controller. HOME does not exit; use **+**
and **−**.

## Optimizations

The headline emulation-speed work, per platform:

- **CV1000 — threaded sprite blitter.** The Cave blitter (`epic12`) originally ran inline on the
  emulation thread. It was first moved onto its own core, then **split across two cores** so the two
  halves of each frame render in parallel. A per-frame safety check finds draws that cross the split
  (one half reading what the other half writes) and orders them with sync points, or runs that frame on
  one core.
- **CV1000 — VRAM row padding.** The emulator stores each VRAM row with 1 KB of padding, so rows no
  longer map to the same CPU cache sets. This is the emulator's own memory layout, not the hardware's;
  video, audio and save states are bit-identical.
- **CV1000 — frame composite and sound on other cores.** The 32-bit frame composite is split across the
  two blit cores, and the YMZ770 sound chip renders on core 0 from a queue of register writes.
- **CPS3 — character-DMA bulk fill.** Replaced byte-by-byte RLE character decompression with a bulk
  fill, trimming the worst DMA-heavy frames.
- **Neo Geo — palette rebuild fix.** The full palette was being rebuilt every frame; now it is
  recalculated only when it actually changes.
- **All games — frame pacing.** Vsync pacing for games near the TV's rate, with the audio rate nudged
  (up to ±0.8%) so the sound queue neither drains nor grows; the others are paced by the audio queue.

Supporting CV1000 CPU-side work along the way: an SH-3 interpreter dispatch table and a bit-exact timer
fast-path. The code changes are Wii U specific (threading, core pinning, etc.) and are not appropriate
for upstream.

## Features

- **Game picker** with full game names, on-screen help, controller navigation, and per-system labels
- **Save states** — compressed and near-instant, with an in-game quicksave / load combo
- **High scores** saved (FinalBurn Neo `hiscore.dat`)
- **Button remapping and autofire**, **frameskip** and other options (see Settings)
- **TV / GamePad screen selection**, 4:3 or square-pixel display, smooth or sharp scaling
- **Two players** — GamePad, Pro Controller, or Wiimote + Classic Controller
- Screen rotation for vertical (TATE) games, full-screen scaling on 1080p displays, and a bundled font

## Building for Wii U

```
docker build -t fbneo-wiiu player
docker run --rm -v "$PWD":/src fbneo-wiiu player/build-wiiu.sh
```

Run both from the repo root. The build files (`Dockerfile`, `build-wiiu.sh`) live in `player/`.
Output: `player/fbneo_player.wuhb`. Install it as described in [Installing](#installing).

The build also writes `player/release/`, the files to publish: the wuhb, `LICENSE-FBNeo.txt`,
`THIRD-PARTY-NOTICES.txt` and `SHA256SUMS`. Both license files are also bundled inside the wuhb.
`player/make-notices.sh` generates them; it fails the build if a library's license text is missing.

## Credits

- **FinalBurn Neo** — Team FBNeo. The libretro core is maintained upstream by the team;
  BarbuDreadMon leads libretro integration, with dink, iq_132, JacKc, and kev.
- **Wii U homebrew lineage** — the RetroArch Wii U port by aliaspider (initial port, Oct 2016) and
  FIX94 (optimizations and release-ready improvements).
- **wut** (Wii U toolchain & library) — the devkitPro / wut team.
- **SDL2** — the SDL team and the SDL2 Wii U port (SDL2-wut) contributors.
- **Libraries & font** — SDL2_ttf, libretro-common, FreeType, HarfBuzz, Brotli, bzip2, libpng, zlib,
  and the Roboto font; see `THIRD-PARTY-NOTICES.txt`. Portions of this software are copyright © The
  FreeType Project (https://freetype.org). All rights reserved.
- **Wii U optimized build** — perfcrash26, with Claude (see below).

## Known issues

- **CV1000 heavy scenes drop frames** — they are now limited by the emulated SH-3 CPU (an interpreter),
  which on its own takes most of the 16.7 ms frame in the heaviest scenes; the sound crackles where the
  frame rate drops.
- **One-core fallback** — frames whose drawing crosses the two-core split too often run the blit on one
  core, which is slower in those scenes.
- **Vsync and slow games** — see the vsync note under Settings (e.g. Shinobi: use `vsync=0`).
- **Rare crash when leaving a game** — seen once in about 135 game exits during testing.
- **Rare audio stall** — seen once after many game switches in one session; if the sound stops,
  restart the app.

## Future work

**Goal: CV1000 normal gameplay with clear sound at full speed on most games**, including the heaviest
scenes.

Hardware profiling points the way:

- **The emulated SH-3 CPU is now the CV1000 bottleneck.** About two thirds of its time goes to the
  interpreter's dispatch loop (fetching and dispatching each instruction), not to the instructions'
  own work. The CPU step has to come down by roughly a third (Deathsmiles) to a half (Ibara's peaks)
  for full speed in the worst scenes.
- **The sprite blit is compute-bound** — the per-pixel alpha blend on an in-order CPU, not memory
  bandwidth.

Next steps:

1. **Faster SH-3 interpreter loop** — a threaded interpreter (less work per dispatched instruction),
   then possibly a recompiler (JIT) for the SH-2 / SH-3 cores, which would also help CPS3 and Psikyo.
2. **Inner-loop blitter optimization** — software-pipeline the alpha blend and trim the hottest blend
   path.
3. **Use the idle CPU time** — a shared queue of small blit tiles that every core pulls from when free,
   instead of two fixed bands.
4. **In-app settings screen** for the `config.txt` options, and an on-screen FPS display.
5. **Display filters** — CRT / scanline shaders on the GPU.
6. **More controllers** — support for 4-player games (the Wii U takes a GamePad plus up to four
   Wii Remotes / Pro Controllers, so up to 5 inputs).

## AI / Claude Disclosure

The Wii U build in this fork — the threaded CV1000 blitter, the CPS3 and Neo Geo optimizations, the
SDL2 player, and the build and profiling tooling — was developed by a contributor working with
**Claude (Anthropic's AI coding assistant, via Claude Code)**. The contributor did the architectural
review, the instrumentation pass and its analysis, and the optimization plan, then set out how Claude
should approach the project and directed the implementation step by step. Claude wrote and edited the
code, ran the build-and-test cycles, and analyzed the hardware profiling; every change was reviewed and
tested on real hardware. The FinalBurn Neo emulator and its libretro core are the work of Team FBNeo
(see Credits).

## What's new in U2

- **Smooth frames on games near 60 Hz** — vsync pacing, chosen per game (`vsync=auto`). CPS1/2/3,
  Toaplan, Pac-Man and CV1000 now present every frame one TV refresh apart.
- **CV1000 faster** — VRAM row padding (fewer cache conflicts), a safety check that keeps the
  two-core blitter correct, the frame composite split across two cores, and the YMZ770 sound chip on
  its own core.
- **Full game names** in the list, sorted by title; L/R to page, ZL/ZR to jump by letter; the list
  reopens on the last game played.
- **High scores** saved for games in FinalBurn Neo's `hiscore.dat` (bundled).
- **4:3 display** by default, a sharp (nearest-neighbour) scaling option, and **TV / GamePad
  screen selection** (hold ZL + ZR, press X).
- **Button remapping and autofire** from `config.txt`.
- Fixed: list rows sometimes showed another row's text; the in-game clock was stuck at a fixed date.
