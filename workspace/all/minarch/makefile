###########################################################

ifeq (,$(PLATFORM))
PLATFORM=$(UNION_PLATFORM)
endif

ifeq (,$(PLATFORM))
$(error please specify PLATFORM, eg. PLATFORM=trimui make)
endif

ifeq (,$(CROSS_COMPILE))
$(error missing CROSS_COMPILE for this toolchain)
endif

###########################################################

include ../../$(PLATFORM)/platform/makefile.env
SDL?=SDL

###########################################################

TARGET = minarch
PRODUCT= build/$(PLATFORM)/$(TARGET).elf
INCDIR = -I. -I./libretro-common/include/ -I../common/ -I../../$(PLATFORM)/platform/
SOURCE = $(TARGET).c ../common/scaler.c ../common/utils.c ../common/config.c ../common/api.c ../common/notification.c ../common/http.c ../../$(PLATFORM)/platform/platform.c

# RA support
ifneq (,$(filter $(PLATFORM),tg5040 tg5050 my355 desktop))
# rcheevos include paths
INCDIR += -I../rcheevos/src/include -I../rcheevos/src/src
# libchdr include path (for CHD disc image support)
INCDIR += -I../libchdr/include
# RA source files
SOURCE += ../common/ra_badges.c ra_integration.c chd_reader.c
endif

CC = $(CROSS_COMPILE)gcc
CFLAGS  += $(OPT)
CFLAGS  += $(INCDIR) -DPLATFORM=\"$(PLATFORM)\" -std=gnu99
LDFLAGS	 += -lmsettings -lsamplerate
LDFLAGS  += -llz4

ifeq ($(PROFILE), 1)
CFLAGS  += -pg
LDFLAGS += -pg
endif

ifeq ($(GPERFTOOLS), 1)
CFLAGS  += -g
LDFLAGS += -lprofiler
endif

ifeq ($(PLATFORM), desktop)
ifeq ($(UNAME_S),Linux)
CFLAGS += `pkg-config --cflags libzip`
LDFLAGS += `pkg-config --libs libzip` -lz
else
LDFLAGS += -lzip -lz
endif # Linux
else # all handheld platfoms
ifeq ($(PLATFORM), my355)
LDFLAGS	 +=  -Llibretro-common -lsrm -lzip -lz
CFLAGS   += -DHAS_SRM
else
LDFLAGS	 +=  -Llibretro-common -lsrm -lzip -lz
CFLAGS   += -DHAS_SRM
LDFLAGS +=  -lasound
endif
endif

# RA support: rcheevos and libchdr linking
ifneq (,$(filter $(PLATFORM),tg5040 tg5050 my355 desktop))
LDFLAGS += -L../rcheevos/build/$(PLATFORM) -lrcheevos
LDFLAGS += -L../libchdr/build/$(PLATFORM) -lchdr
CFLAGS += -DRC_CLIENT_SUPPORTS_HASH
ifeq ($(PLATFORM), desktop)
# Desktop needs rpath for local shared library lookup
LDFLAGS += -Wl,-rpath,'$$ORIGIN'
endif
endif

# CFLAGS  += -Wall -Wno-unused-variable -Wno-unused-function -Wno-format-overflow

ifeq ($(PLATFORM), desktop)
ifeq ($(TSAN), 1)
CFLAGS  += -fsanitize=thread
LDFLAGS += -ltsan
endif
ifeq ($(ASAN), 1)
CFLAGS  += -fsanitize=address -fno-common -fno-omit-frame-pointer
LDFLAGS += -lasan
endif
endif

AR=$(CROSS_COMPILE)ar
ARFLAGS=rc

BUILD_DATE!=date +%Y.%m.%d
BUILD_HASH!=cat ../../hash.txt
CFLAGS += -DBUILD_DATE=\"${BUILD_DATE}\" -DBUILD_HASH=\"${BUILD_HASH}\"

ifeq ($(PLATFORM), desktop)
all: clean libretro-common rcheevos-desktop libchdr-desktop $(PREFIX_LOCAL)/include/msettings.h
	mkdir -p build/$(PLATFORM)
	$(CC) $(SOURCE) -o $(PRODUCT) $(CFLAGS) $(LDFLAGS)
else
all: clean libretro-common libsrm.a rcheevos libchdr $(PREFIX_LOCAL)/include/msettings.h
	mkdir -p build/$(PLATFORM)
	# This is a bandaid fix, needs to be cleaned up if/when we expand to other platforms.
ifeq ($(PLATFORM), my355)
	cp $(PREFIX)/lib/libcrypto.so.3 build/$(PLATFORM)
	cp $(PREFIX)/lib/libsqlite3.so.0 build/$(PLATFORM)
	cp $(PREFIX)/lib/libffi.so.7 build/$(PLATFORM)
	cp $(PREFIX)/lib/libtinyalsa.so.2 build/$(PLATFORM)
endif
	cp $(PREFIX)/lib/libsamplerate.so.0 build/$(PLATFORM)
	cp $(PREFIX)/lib/libzip.so.5 build/$(PLATFORM)
	cp $(PREFIX)/lib/libbz2.so.1.0 build/$(PLATFORM)
	cp $(PREFIX)/lib/liblzma.so.5 build/$(PLATFORM)
	cp $(PREFIX)/lib/libzstd.so.1 build/$(PLATFORM)
	cp $(PREFIX)/lib/liblz4.so.1 build/$(PLATFORM)
	cp ../libchdr/build/$(PLATFORM)/libchdr.so.0 build/$(PLATFORM)
	$(CC) $(SOURCE) -o $(PRODUCT) $(CFLAGS) $(LDFLAGS)
endif

libretro-common:
	git clone https://github.com/libretro/libretro-common

rcheevos:
	cd ../rcheevos && make build PLATFORM=$(PLATFORM)

# Desktop rcheevos build - uses native gcc instead of cross-compiler
rcheevos-desktop:
	cd ../rcheevos && make build PLATFORM=desktop

../libchdr/CMakeLists.txt:
	rm -rf ../libchdr
	git clone https://github.com/rtissera/libchdr ../libchdr
	cp libchdr.makefile ../libchdr/makefile

libchdr: ../libchdr/CMakeLists.txt
	cd ../libchdr && make build PLATFORM=$(PLATFORM)

# Desktop libchdr - build from source
libchdr-desktop: ../libchdr/CMakeLists.txt
	cd ../libchdr && make build PLATFORM=desktop
  
$(PREFIX_LOCAL)/include/msettings.h:
	cd ../../$(PLATFORM)/libmsettings && make

### libsrm stuff
OBJECTS = streams/rzip_stream.o streams/file_stream.o vfs/vfs_implementation.o file/file_path.o file/file_path_io.o compat/compat_strl.o time/rtime.o string/stdstring.o encodings/encoding_utf.o streams/trans_stream.o streams/trans_stream_pipe.o streams/trans_stream_zlib.o

$(OBJECTS) :
	cd libretro-common && $(CC) $(CFLAGS) -DHAVE_ZLIB -c -o $@ $(subst .o,.c,$@) -I include/

libsrm.a: $(OBJECTS)
	cd libretro-common && $(AR) $(ARFLAGS) $@ $?

clean:
	rm -f $(PRODUCT)
	rm -f $(OBJECTS) $(LIBRARY)