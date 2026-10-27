# fbneo_bench, see bench.c.
#   make wiiu   -> fbneo_bench.rpx  (links ../src/burner/libretro/fbneo_libretro_wiiu.a)
#   make linux  -> fbneo_bench      (links CORE_SO, default the unix core next to the .a)
LIBRETRO := ../src/burner/libretro
CORE_A   ?= $(LIBRETRO)/fbneo_libretro_wiiu.a
CORE_SO  ?= $(LIBRETRO)/fbneo_libretro.so
INCLUDES := -I$(LIBRETRO)/libretro-common/include

WUT_ROOT ?= /opt/devkitpro/wut
PPC_CC   := $(DEVKITPPC)/bin/powerpc-eabi-gcc
PPC_CXX  := $(DEVKITPPC)/bin/powerpc-eabi-g++
PPC_ARCH := -mcpu=750 -meabi -mhard-float
# with STATIC_LINKING the core expects the frontend to provide libretro-common
LRC      := $(LIBRETRO)/libretro-common
LRC_SRC  := $(addprefix $(LRC)/,streams/file_stream.c streams/file_stream_transforms.c file/file_path.c \
            file/file_path_io.c file/retro_dirent.c vfs/vfs_implementation.c string/stdstring.c \
            encodings/encoding_utf.c compat/compat_strl.c compat/fopen_utf8.c compat/compat_strcasestr.c time/rtime.c)

.PHONY: wiiu linux clean

wiiu: fbneo_bench.rpx

fbneo_bench.elf: bench.c $(CORE_A)
	$(PPC_CC) -O2 -Wall $(PPC_ARCH) -D__WIIU__ -D__WUT__ $(BENCH_CFLAGS) $(INCLUDES) -isystem $(WUT_ROOT)/include -c bench.c -o bench_wiiu.o
	$(PPC_CC) -O2 $(PPC_ARCH) -D__WIIU__ -D__WUT__ -DWIIU $(INCLUDES) -isystem $(WUT_ROOT)/include -c $(LRC_SRC) && \
	    ar rcs lrc_wiiu.a $(notdir $(LRC_SRC:.c=.o)) && rm -f $(notdir $(LRC_SRC:.c=.o))
	$(PPC_CXX) $(PPC_ARCH) -specs=$(WUT_ROOT)/share/wut.specs -L$(WUT_ROOT)/lib bench_wiiu.o $(CORE_A) lrc_wiiu.a -lwut -lm -o $@

fbneo_bench.rpx: fbneo_bench.elf
	elf2rpl $< $@

linux: fbneo_bench

fbneo_bench: bench.c $(CORE_SO)
	$(CC) -O2 -Wall $(INCLUDES) bench.c $(CORE_SO) -Wl,-rpath,'$$ORIGIN' -lm -o $@

clean:
	rm -f bench_wiiu.o lrc_wiiu.a fbneo_bench.elf fbneo_bench.rpx fbneo_bench
