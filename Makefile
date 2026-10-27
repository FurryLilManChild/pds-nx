# pds-nx: Panzer Dragoon Saga for Nintendo Switch
#
# Build from the devkitPro MSYS2 shell (Windows) or any shell with DEVKITPRO set:
#     make -j8
# Output: pds-nx.nro
#
# Needs the devkitPro packages: switch-dev switch-sdl2 switch-mesa switch-libdrm_nouveau
#
# This file is part of pds-nx and is licensed under the GNU GPL v2 or later.

ifeq ($(strip $(DEVKITPRO)),)
$(error DEVKITPRO is not set. Run this from the devkitPro MSYS2 shell)
endif

include $(DEVKITPRO)/libnx/switch_rules

TARGET      := pds-nx
BUILD       := build
APP_TITLE   := Panzer Dragoon Saga
APP_AUTHOR  := pds-nx / Yaba Sanshiro by devMiyax
APP_VERSION := 0.1.0
ICON        := $(firstword $(wildcard switch/icon.jpg) $(LIBNX)/default_icon.jpg)

# --------------------------------------------------------------------------
# Sources: the emulator core (same set the libretro build uses) + frontend
# --------------------------------------------------------------------------
C   := core
LR  := core/libretro
LRC := core/libretro/libretro-common
CHD := core/libchdr
LZ  := $(CHD)/deps/lzma-19.00
FL  := $(CHD)/deps/flac-1.3.3
ZL  := $(CHD)/deps/zlib-1.2.11

SRC_C := \
	$(C)/osdcore.c $(C)/bios.c $(C)/cd-libretro.c $(C)/cheat.c $(C)/coffelf.c \
	$(C)/cs0.c $(C)/cs1.c $(C)/cs2.c $(C)/debug.c $(C)/error.c $(C)/gameinfo.c \
	$(C)/japmodem.c $(C)/junzip.c $(C)/memory.c $(C)/movie.c $(C)/netlink.c \
	$(C)/peripheral.c $(C)/profile.c $(C)/scspdsp.c $(C)/scu.c $(C)/scsp.c \
	$(C)/sh2cache.c $(C)/sh2core.c $(C)/sh2d.c $(C)/sh2iasm.c $(C)/sh2int.c \
	$(C)/smpc.c $(C)/m68kcore.c $(C)/m68kd.c $(C)/vidogl.c $(C)/yglcache.c \
	$(C)/ygles.c $(C)/yglshaderes.c $(C)/snddummy.c $(C)/vidshared.c \
	$(C)/vidsoft.c $(C)/titan/titan.c $(C)/yabause.c $(C)/thr-rthreads.c \
	$(C)/musashi/m68kcpu.c $(C)/musashi/m68kops.c $(C)/musashi/m68kopnz.c \
	$(C)/musashi/m68kopac.c $(C)/musashi/m68kopdm.c $(C)/m68kmusashi.c \
	$(LR)/libretro.c $(LR)/glsym_private.c \
	$(LRC)/compat/compat_posix_string.c $(LRC)/compat/compat_strl.c \
	$(LRC)/compat/fopen_utf8.c $(LRC)/encodings/encoding_utf.c \
	$(LRC)/file/file_path.c $(LRC)/compat/compat_strcasestr.c \
	$(LRC)/vfs/vfs_implementation.c $(LRC)/streams/file_stream.c \
	$(LRC)/streams/file_stream_transforms.c $(LRC)/rthreads/rthreads.c \
	$(LRC)/glsm/glsm.c $(LRC)/glsym/rglgen.c $(LRC)/glsym/glsym_es3.c \
	$(CHD)/src/libchdr_chd.c $(CHD)/src/libchdr_bitstream.c \
	$(CHD)/src/libchdr_huffman.c $(CHD)/src/libchdr_flac.c \
	$(CHD)/src/libchdr_cdrom.c \
	$(FL)/src/stream_decoder.c $(FL)/src/format.c $(FL)/src/bitreader.c \
	$(FL)/src/crc.c $(FL)/src/memory.c $(FL)/src/lpc.c $(FL)/src/cpu.c \
	$(FL)/src/md5.c $(FL)/src/fixed.c \
	$(LZ)/src/LzmaEnc.c $(LZ)/src/LzFind.c $(LZ)/src/LzmaDec.c \
	$(ZL)/adler32.c $(ZL)/crc32.c $(ZL)/inffast.c $(ZL)/inflate.c \
	$(ZL)/inftrees.c $(ZL)/zutil.c \
	switch/nx_jit.c switch/nx_log.c

SRC_CXX := \
	$(C)/Counter.cpp $(C)/ygl_texture.cpp $(C)/vdp1.cpp $(C)/vdp2.cpp \
	$(C)/sh2_dynarec_devmiyax/DynarecSh2CInterface.cpp \
	$(C)/sh2_dynarec_devmiyax/DynarecSh2.cpp \
	switch/main.cpp

SRC_S := $(C)/sh2_dynarec_devmiyax/dynalib_arm64.s

OBJS := $(addprefix $(BUILD)/,$(SRC_C:.c=.o) $(SRC_CXX:.cpp=.o) $(SRC_S:.s=.o))
DEPS := $(OBJS:.o=.d)

# --------------------------------------------------------------------------
# Flags
# --------------------------------------------------------------------------
ARCH := -march=armv8-a+crc+crypto -mtune=cortex-a57 -mtp=soft -fPIE

DEFS := -D__SWITCH__ -DSWITCH=1 -DHAVE_LIBNX -DAARCH64 \
	-D__LIBRETRO__ -DHAVE_LIBGL -DVERSION=\"3.4.2\" -DHAVE_LROUND -D_7ZIP_ST \
	-DFLAC__HAS_OGG=0 -DFLAC__NO_DLL -DNO_CLI -DHAVE_BUILTIN_BSWAP16=1 \
	-DHAVE_BUILTIN_BSWAP32=1 -DHAVE_C99_VARIADIC_MACROS=1 -DHAVE_FLOORF=1 \
	-DHAVE_GETTIMEOFDAY=1 -DHAVE_STDINT_H=1 -DHAVE_SYS_TIME_H=1 \
	-DIMPROVED_SAVESTATES -DPACKAGE_VERSION=\"1.3.3\" \
	-DDYNAREC_DEVMIYAX=1 -DHAVE_GLSYM_PRIVATE -DHAVE_SYS_PARAM_H \
	-DHAVE_MUSASHI=1 -D_OGLES3_ -DHAVE_OPENGLES -DHAVE_OPENGLES3 -DNDEBUG

INCS := -I$(LR) -I$(C) -I$(LRC)/include -I$(C)/musashi -I$(CHD)/include \
	-I$(LZ)/include -I$(FL)/include -I$(FL)/src/include -I$(ZL) -Iswitch \
	-I$(PORTLIBS)/include -I$(LIBNX)/include

OPT := -O3 -ffast-math -fno-tree-tail-merge -ffunction-sections -fdata-sections

CFLAGS   := -g $(OPT) $(ARCH) $(DEFS) $(INCS) -std=gnu11
CXXFLAGS := -g $(OPT) $(ARCH) $(DEFS) $(INCS) -std=gnu++14
ASFLAGS  := -g $(ARCH)
LDFLAGS  := -specs=$(DEVKITPRO)/libnx/switch.specs -g $(ARCH) -Wl,--gc-sections \
	-Wl,-Map,$(BUILD)/$(TARGET).map -L$(PORTLIBS)/lib -L$(LIBNX)/lib
LIBS     := -lSDL2 -lEGL -lGLESv2 -lglapi -ldrm_nouveau -lnx -lstdc++ -lm

# --------------------------------------------------------------------------
# Rules
# --------------------------------------------------------------------------
.PHONY: all clean dist

all: $(TARGET).nro

$(BUILD)/%.o: %.c
	@mkdir -p $(dir $@)
	@echo CC $<
	@$(CC) -MMD -MP $(CFLAGS) -c $< -o $@

$(BUILD)/%.o: %.cpp
	@mkdir -p $(dir $@)
	@echo CXX $<
	@$(CXX) -MMD -MP $(CXXFLAGS) -c $< -o $@

$(BUILD)/%.o: %.s
	@mkdir -p $(dir $@)
	@echo AS $<
	@$(CC) -x assembler-with-cpp $(ASFLAGS) -c $< -o $@

$(TARGET).elf: $(OBJS)
	@echo LD $@
	@$(CXX) $(LDFLAGS) $(OBJS) $(LIBS) -o $@

$(TARGET).nacp:
	@nacptool --create "$(APP_TITLE)" "$(APP_AUTHOR)" "$(APP_VERSION)" $@

$(TARGET).nro: $(TARGET).elf $(TARGET).nacp
	@echo NRO $@
	@elf2nro $< $@ --nacp=$(TARGET).nacp --icon=$(ICON)

# SD card folder ready to copy: dist/switch/pds-nx/
dist: $(TARGET).nro
	@mkdir -p dist/switch/pds-nx/discs
	@cp $(TARGET).nro dist/switch/pds-nx/
	@echo "Copy dist/switch to the root of your SD card."

clean:
	@rm -rf $(BUILD) $(TARGET).elf $(TARGET).nro $(TARGET).nacp dist

-include $(DEPS)
