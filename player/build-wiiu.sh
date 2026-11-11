#!/bin/sh
# Build FBNeo Player (Wii U). From the repo root, inside the Docker image from player/Dockerfile:
#   docker build -t fbneo-wiiu player
#   docker run --rm -v "$PWD":/src fbneo-wiiu player/build-wiiu.sh
# Output: player/fbneo_player.wuhb, plus player/release/ = the files to publish
# (wuhb + LICENSE-FBNeo.txt + THIRD-PARTY-NOTICES.txt + SHA256SUMS).
set -e
cd "$(dirname "$0")/.."
git config --global --add safe.directory '*' 2>/dev/null || true

echo ">> building libretro core (fbneo_libretro_wiiu.a)"
( cd src/burner/libretro && make -j"$(nproc)" platform=wiiu )

echo ">> building player (fbneo_player.wuhb)"
( cd player && make wuhb )

echo ">> assembling player/release/"
rm -rf player/release && mkdir player/release
cp player/fbneo_player.wuhb player/build_content/LICENSE-FBNeo.txt \
   player/build_content/THIRD-PARTY-NOTICES.txt player/release/
( cd player/release && sha256sum fbneo_player.wuhb LICENSE-FBNeo.txt THIRD-PARTY-NOTICES.txt > SHA256SUMS )

echo "built: player/fbneo_player.wuhb"
cat player/release/SHA256SUMS
