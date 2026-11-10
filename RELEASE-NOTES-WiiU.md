# FBNeo for Wii U — Release Notes

A port of FinalBurn Neo (libretro) to the Wii U, focused on CV1000 (Cave) arcade emulation.

## Highlights

- **CV1000 at a locked 60 fps** — the sprite blitter runs in parallel across two cores
  (core0 + core2), bit-exact with the reference core.
- **Compressed save states** — fast save/load, per game.
- **Full-screen menu** with large legible fonts and controller navigation.
- **SDL2 audio** synced to the audio queue.

## Known issues

- **Threaded blitter artifacts** — splitting the sprite blitter across threads
  (core0 + core2) introduced some rendering artifacts, seen in the **deathsml**
  stage-select menu. Mitigated with a single-threaded texture-upload pre-pass; worth
  re-checking on newly added games.
- **Residual audio scratch in the heaviest scenes** — a performance tail (frame-time
  dips starve the audio queue), not an audio-backend bug.

## Controls

- **Menu:** Up/Down or left stick to move, A to start, Y for help, hold +/- to quit.
- **In game:** hold **+ and − together** to return to the menu (HOME does not exit).
