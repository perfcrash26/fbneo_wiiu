# fbneo_player, see player.c. A minimal SDL2 libretro frontend for the static FBNeo core.
#   make wiiu   -> fbneo_player.rpx   (links ../src/burner/libretro/fbneo_libretro_wiiu.a + SDL2-wut)
LIBRETRO := ../src/burner/libretro
CORE_A   ?= $(LIBRETRO)/fbneo_libretro_wiiu.a
INCLUDES := -I$(LIBRETRO)/libretro-common/include

WUT_ROOT ?= /opt/devkitpro/wut
PORTLIBS ?= /opt/devkitpro/portlibs/wiiu
PPC_CC   := $(DEVKITPPC)/bin/powerpc-eabi-gcc
PPC_CXX  := $(DEVKITPPC)/bin/powerpc-eabi-g++
PPC_ARCH := -mcpu=750 -meabi -mhard-float
SDL2CFG  := $(PORTLIBS)/bin/sdl2-config

# with STATIC_LINKING the core expects the frontend to provide libretro-common
LRC      := $(LIBRETRO)/libretro-common
LRC_SRC  := $(addprefix $(LRC)/,streams/file_stream.c streams/file_stream_transforms.c file/file_path.c \
            file/file_path_io.c file/retro_dirent.c vfs/vfs_implementation.c string/stdstring.c \
            encodings/encoding_utf.c compat/compat_strl.c compat/fopen_utf8.c compat/compat_strcasestr.c time/rtime.c)

PKGCFG     := PKG_CONFIG_PATH=$(PORTLIBS)/lib/pkgconfig:/opt/devkitpro/portlibs/ppc/lib/pkgconfig pkg-config
SDL_CFLAGS := $(shell $(SDL2CFG) --cflags) $(shell $(PKGCFG) --cflags SDL2_ttf)
SDL_LIBS   := $(shell $(PKGCFG) --libs --static SDL2_ttf)

.PHONY: wiiu wuhb clean

wiiu: fbneo_player.rpx

# package a Wii U menu app: FBNeo icon + Roboto font bundled in /content
wuhb: fbneo_player.rpx
	wuhbtool fbneo_player.rpx fbneo_player.wuhb \
	    --name "FBNeo Player" --short-name FBNeoPlay --author fbneo_wiiu \
	    --icon=icon.png --content=content

fbneo_player.elf: player.c picker/picker.c picker/picker.h fbneo_names.cpp $(CORE_A)
	$(PPC_CC) -O2 -Wall $(PPC_ARCH) -D__WIIU__ -D__WUT__ $(INCLUDES) $(SDL_CFLAGS) -isystem $(WUT_ROOT)/include -c player.c -o player_wiiu.o
	$(PPC_CC) -O2 -Wall $(PPC_ARCH) -D__WIIU__ -D__WUT__ $(SDL_CFLAGS) -isystem $(WUT_ROOT)/include -c picker/picker.c -o picker_wiiu.o
	$(PPC_CXX) -O2 $(PPC_ARCH) -D__WIIU__ -c fbneo_names.cpp -o fbneo_names.o
	$(PPC_CC) -O2 $(PPC_ARCH) -D__WIIU__ -D__WUT__ -DWIIU $(INCLUDES) -isystem $(WUT_ROOT)/include -c $(LRC_SRC) && \
	    ar rcs lrc_wiiu.a $(notdir $(LRC_SRC:.c=.o)) && rm -f $(notdir $(LRC_SRC:.c=.o))
	$(PPC_CXX) $(PPC_ARCH) -specs=$(WUT_ROOT)/share/wut.specs -L$(WUT_ROOT)/lib player_wiiu.o picker_wiiu.o fbneo_names.o $(CORE_A) lrc_wiiu.a $(SDL_LIBS) -lwut -lm -o $@

fbneo_player.rpx: fbneo_player.elf
	elf2rpl $< $@

clean:
	rm -f player_wiiu.o picker_wiiu.o fbneo_names.o lrc_wiiu.a fbneo_player.elf fbneo_player.rpx fbneo_player.wuhb
