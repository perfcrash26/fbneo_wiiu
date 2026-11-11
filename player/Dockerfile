# Build environment for FBNeo Player (Wii U).
#
# devkitPro's devkitPPC image already ships the Wii U toolchain (wut, wut-tools,
# wuhbtool). We add only the SDL2 Wii U portlibs the player links against.
#
# From the repo root:
#   docker build -t fbneo-wiiu player
#   docker run --rm -v "$PWD":/src fbneo-wiiu player/build-wiiu.sh
FROM devkitpro/devkitppc:latest

RUN dkp-pacman -Syu --noconfirm --needed wiiu-sdl2 wiiu-sdl2_ttf \
 && (yes | dkp-pacman -Scc >/dev/null 2>&1 || true)

WORKDIR /src
