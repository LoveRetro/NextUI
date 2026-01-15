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
SOURCE = $(TARGET).c manual.c ../common/scaler.c ../common/utils.c ../common/config.c ../common/api.c ../../$(PLATFORM)/platform/platform.c

CC = $(CROSS_COMPILE)gcc
CFLAGS  += $(OPT) -fomit-frame-pointer
CFLAGS  += $(INCDIR) -DPLATFORM=\"$(PLATFORM)\" -std=gnu99
LDFLAGS	 += -lmsettings -lsamplerate
ifeq ($(PLATFORM), desktop)
ifeq ($(UNAME_S),Linux)
CFLAGS += `pkg-config --cflags libzip poppler-glib gio-2.0 gobject-2.0 glib-2.0 cairo` -DENABLE_PDF_MANUAL
LDFLAGS += `pkg-config --libs libzip poppler-glib gio-2.0 gobject-2.0 glib-2.0 cairo`
else
LDFLAGS += -lzip
endif
else
LDFLAGS	 +=  -Llibretro-common -lsrm -lzip
CFLAGS   += -DHAS_SRM
endif
ifeq ($(PLATFORM), tg5040)
TG5040_LIBS = ../../tg5040/libs
ifneq (,$(wildcard $(TG5040_LIBS)/lib/libpoppler-glib.so))
CFLAGS += $(shell PKG_CONFIG_PATH=$(abspath $(TG5040_LIBS)/lib/pkgconfig) pkg-config --cflags poppler-glib cairo) -DENABLE_PDF_MANUAL
LDFLAGS += $(shell PKG_CONFIG_PATH=$(abspath $(TG5040_LIBS)/lib/pkgconfig) pkg-config --libs poppler-glib cairo)
endif
CFLAGS += -DHAS_WIFIMG -DHAS_BTMG
LDFLAGS +=  -lwifimg -lwifid
LDFLAGS +=  -lbtmg -lglib-2.0 -lgio-2.0 -lshared-mainloop -lbluetooth-internal -lasound -ljson-c
endif
# CFLAGS  += -Wall -Wno-unused-variable -Wno-unused-function -Wno-format-overflow
# CFLAGS  += -fsanitize=address -fno-common
# LDFLAGS += -lasan

AR=$(CROSS_COMPILE)ar
ARFLAGS=rc

BUILD_DATE!=date +%Y.%m.%d
BUILD_HASH!=cat ../../hash.txt
CFLAGS += -DBUILD_DATE=\"${BUILD_DATE}\" -DBUILD_HASH=\"${BUILD_HASH}\"

ifeq ($(PLATFORM), desktop)
all: clean libretro-common $(PREFIX_LOCAL)/include/msettings.h
	mkdir -p build/$(PLATFORM)
	$(CC) $(SOURCE) -o $(PRODUCT) $(CFLAGS) $(LDFLAGS)
else
all: clean libretro-common libsrm.a $(PREFIX_LOCAL)/include/msettings.h
	mkdir -p build/$(PLATFORM)
	cp $(PREFIX)/lib/libsamplerate.so.0 build/$(PLATFORM)
	# This is a bandaid fix, needs to be cleaned up if/when we expand to other platforms.
	cp $(PREFIX)/lib/libzip.so.5 build/$(PLATFORM)
	cp $(PREFIX)/lib/libbz2.so.1.0 build/$(PLATFORM)
	cp $(PREFIX)/lib/liblzma.so.5 build/$(PLATFORM)
	cp $(PREFIX)/lib/libzstd.so.1 build/$(PLATFORM)
	$(CC) $(SOURCE) -o $(PRODUCT) $(CFLAGS) $(LDFLAGS)
endif

libretro-common:
	git clone https://github.com/libretro/libretro-common
	
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
