# FBNeo Player (Wii U)

A standalone port of **FinalBurn Neo** (the libretro arcade core) to the Nintendo Wii U, with an
SDL2 front-end. Focused on Capcom (CPS1 / CPS2 / CPS3), Neo Geo, and Cave CV1000 arcade games.

**Version 1.0.0.3-U1**

See: AI / Claude Disclosure below

## Compatibility & speed

| Hardware | Speed |
|---|---|
| CPS1 | full speed |
| CPS2 | full speed |
| Neo Geo | full speed |
| CPS3 | usually full speed, with short slowdowns in heavy parts |
| CV1000 (Cave) | **mostly playable, but not full speed** — dense scenes drop frames and the audio turns scratchy under load |

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
| `system/` | BIOS / system files |

`config.txt` settings: `content=` (ROM folder, default `roms/`), `frameskip=N` (0–9, default 0),
`rotate=on|off` (rotate vertical games; default on), `autoload=<set>` (boot straight into one game).

## Optimizations

The headline emulation-speed work, per platform:

- **CV1000 — threaded sprite blitter.** The Cave blitter (`epic12`) originally ran inline on the
  emulation thread. It was first moved onto its own core, then **split across two cores** so the two
  halves of each frame render in parallel. This is what took CV1000 from a slideshow to playable.
- **CPS3 — character-DMA bulk fill.** Replaced byte-by-byte RLE character decompression with a bulk
  fill, trimming the worst DMA-heavy frames.
- **Neo Geo — palette rebuild fix.** The full palette was being rebuilt every frame; now it is
  recalculated only when it actually changes.

Supporting CV1000 CPU-side and audio work along the way: an SH-3 interpreter dispatch table, a
bit-exact timer fast-path, and — most important for usability — **re-syncing the frame loop to the
audio queue**, which stopped the underruns that had made the sound unusable.

## Features

- **Game picker** with on-screen help, controller navigation, and per-system labels
- **Save states** — compressed and near-instant, with an in-game quicksave / load combo
- **Frameskip** option (via config)
- **Two players** — GamePad, Pro Controller, or Wiimote + Classic Controller
- Screen rotation for vertical (TATE) games, full-screen scaling on 1080p displays, and a bundled font



## Controls

| Action | Menu | In game |
|---|---|---|
| Move | D-pad or left stick (Up/Down) | D-pad or left stick |
| Select / play | A or B | — |
| Game buttons | — | A, B, X, Y, L, R, ZL, ZR |
| Start / Coin | — | **+** start · **−** coin |
| Help | Y | — |
| Leave help page | A or B | — |
| Save state | — | hold ZL + ZR, press Y |
| Load state | — | hold ZL + ZR, press A |
| Back to menu | — | hold **+** and **−** together (~0.5 s) |
| Quit the app | hold **+** and **−** together | — |

Player 1 uses the first controller (GamePad, Pro Controller, or Wii Remote + Classic Controller),
player 2 the second. Save states use player 1's controller. HOME does not exit; use **+** and **−**.

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
- **Wii U port & optimizations** — a contributor, with Claude (see below).
## Known issues

- **Threaded-blitter artifacts** — the two-core blitter split introduced some rendering artifacts,
  seen in the **deathsml** stage-select menu; mitigated with a single-threaded texture-upload
  pre-pass, and worth re-checking on newly added games.
- **CV1000 is not full speed** — heavy scenes drop frames, and the audio stays clear only under light
  CPU load; under heavy load it becomes scratchy. This is a performance tail, not an audio-backend bug.
  Total CPU use is no longer the bottleneck: since splitting the blitter into 2 threads across 2 cores,
  there is plenty of unused capacity summed across the three cores. What limits each frame is the
  per-core speed of its steps, which run in order (emulated CPU, then the sprite blit, then the
  composite), so their times add up. The unused capacity sits in the gaps where a core waits on
  another core's step. The blit, the longest step, is compute-bound: its time is set by how fast a
  core executes the per-pixel blend, not by memory bandwidth or latency. Short of more threads or a CPU
  recompiler, only optimization of blitter code paths will improve performance further.

## Future work

**Goal: CV1000 normal gameplay with clear sound, near full speed on most games.** Our rough estimate
of the odds is **~60%**: the blitter-bound games (e.g. Deathsmiles, DoDonPachi Dai-Fukkatsu) are already
close and should get there with inner-loop blitter work, while the CPU-heavy games (e.g. Muchi Muchi
Pork, Espgaluda II) likely also need a CPU recompiler.

Hardware profiling points the way:

- **Where the work runs under peak load** (Deathsmiles heavy gameplay, measured on the console):
  core 1 runs the emulated CPU and the frame composite (~59% busy); core 2 draws the top half of the
  sprite blit (~61%); core 0, which the system also uses, draws the bottom half (~46%). The GPU only
  scales and presents the finished frame (~3 ms per frame); its load wasn't measured but is light.
  The cores aren't pegged because each frame is a chain: CPU, then the blit, then the composite. So
  there are limited options for more parallelism: mainly a third blit worker on core 1, which mostly
  waits during the blit (item 1), and the sound chips (item 4). The remaining gains are from
  optimizing the work.

- **CV1000's gameplay bottleneck is the sprite blitter, and it is compute-bound** — it moves only
  ~5–8% of the console's memory bandwidth, so it is not limited by memory speed or latency. Cache
  prefetch and a work-stealing tile queue were tested or analyzed and don't help: the two-core split
  is already well balanced, and the frame composite already runs on the third core. The cost is the
  per-pixel alpha blend itself (dependent palette lookups on an in-order CPU).
- **Elsewhere the ceiling is the interpreted CPU cores** — SH-3 on CV1000, SH-2 on CPS3 and Psikyo.

Next steps:

1. **Use the idle CPU time** — replace the two fixed blit bands with a shared queue of small tiles
   that each core pulls from whenever it's free: cores 0 and 2 from the start of the blit, and core 1
   (the emulation core) as soon as it starts waiting for the blit. Core 0, which the system also uses,
   simply takes fewer tiles when the system needs it, so no fixed limit is needed. Estimated ~10–18%
   off the frames that lag, pending a measurement of how much of core 1's wait overlaps the blit.
2. **Inner-loop blitter optimization** — software-pipeline the alpha blend (two pixels interleaved so
   the dependent palette lookups overlap) and trim the hottest blend path. Estimated ~20–30% off the
   CV1000 blit, which should reduce the frame drops and audio scratch. Combines with item 1.
3. **CPU recompiler (JIT)** — a dynamic recompiler for the SH-2 / SH-3 cores, the biggest lever for
   the interpreter-bound families (CPS3, Psikyo, and CV1000's CPU side). A large undertaking, gated on
   a feasibility pass.
4. **Sound chips on a separate core** — move audio generation off the CPU timeslice. A large,
   structural change, since FBNeo renders audio inside the CPU timeslice loop.
5. **More controllers** — support for 4-player games (the Wii U takes a GamePad plus up to four
   Wii Remotes / Pro Controllers, so up to 5 inputs).

## AI / Claude Disclosure

The Wii U port in this fork — the threaded CV1000 blitter, the CPS3 and Neo Geo optimizations, the
SDL2 player, and the build and profiling tooling — was developed by a contributor working with
**Claude (Anthropic's AI coding assistant, via Claude Code)**. Claude wrote and edited the code, ran
the build-and-test cycles, and analyzed the hardware profiling; every change was directed, reviewed,
and tested on real hardware. The FinalBurn Neo emulator and its libretro core are the work of Team
FBNeo (see Credits).
