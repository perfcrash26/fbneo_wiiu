#!/bin/sh
# Write the license files that ship with the player into OUTDIR:
#   LICENSE-FBNeo.txt        FBNeo's license, verbatim (src/license.txt)
#   THIRD-PARTY-NOTICES.txt  every other library/font in the wuhb, with its license text
# Library texts come from the devkitPro image when it ships them (so they match the linked
# versions); the rest are kept in player/licenses/. Fails if any text is missing.
# usage: player/make-notices.sh OUTDIR
set -e
OUT="${1:?usage: make-notices.sh OUTDIR}"
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(dirname "$HERE")"
PPC=/opt/devkitpro/portlibs/ppc/licenses
WIIU=/opt/devkitpro/portlibs/wiiu/share/licenses
mkdir -p "$OUT"

cp "$ROOT/src/license.txt" "$OUT/LICENSE-FBNeo.txt"

N="$OUT/THIRD-PARTY-NOTICES.txt"
cat > "$N" <<'EOF'
FBNeo Player (Wii U) - third-party notices

FinalBurn Neo itself is covered by LICENSE-FBNeo.txt. The player also contains
the following software and font, each under its own license, reproduced below.

  SDL2, SDL2_ttf ............ zlib license
  wut (Wii U toolchain) ..... zlib license
  libretro-common ........... MIT license
  FreeType .................. FreeType License (FTL)
  HarfBuzz .................. Old MIT license
  Brotli .................... MIT license
  bzip2 ..................... bzip2 license
  libpng .................... libpng license
  zlib ...................... zlib license
  Roboto font ............... SIL Open Font License 1.1

Portions of this software are copyright (C) The FreeType Project
(https://freetype.org). All rights reserved.
EOF

add() {   # add NAME FILE
    [ -f "$2" ] || { echo "make-notices: missing license text for $1: $2" >&2; exit 1; }
    printf '\n\n==============================================================================\n%s\n==============================================================================\n\n' "$1" >> "$N"
    cat "$2" >> "$N"
}

add "SDL2"            "$WIIU/SDL2/LICENSE.txt"
add "SDL2_ttf"        "$HERE/licenses/SDL2_ttf-LICENSE.txt"
add "wut"             "$HERE/licenses/wut-LICENSE.txt"
add "libretro-common" "$HERE/licenses/libretro-common-LICENSE.txt"
add "FreeType (FTL)"  "$PPC/ppc-freetype/FTL.TXT"
add "HarfBuzz"        "$PPC/ppc-harfbuzz/COPYING"
add "Brotli"          "$PPC/ppc-brotli/LICENSE"
add "bzip2"           "$PPC/ppc-bzip2/LICENSE"
add "libpng"          "$PPC/ppc-libpng/LICENSE"
add "zlib"            "$PPC/ppc-zlib/LICENSE"
add "Roboto font"     "$HERE/licenses/Roboto-OFL.txt"

echo "notices: $OUT/LICENSE-FBNeo.txt $N"
