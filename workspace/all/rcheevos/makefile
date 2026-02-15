###########################################################
# rcheevos static library build for NextUI
###########################################################

ifeq (,$(PLATFORM))
PLATFORM=$(UNION_PLATFORM)
endif

ifeq (,$(PLATFORM))
	$(error please specify PLATFORM, eg. PLATFORM=tg5040 make)
endif

# Desktop builds use native compiler
ifneq ($(PLATFORM), desktop)
ifeq (,$(CROSS_COMPILE))
	$(error missing CROSS_COMPILE for this toolchain)
endif

ifeq (,$(PREFIX))
	$(error missing PREFIX for this toolchain)
endif
endif

###########################################################

include ../../$(PLATFORM)/platform/makefile.env

###########################################################

TARGET = rcheevos
SRCDIR = src/src
INCDIR = src/include
OBJDIR = build/$(PLATFORM)/obj

ifeq ($(PLATFORM), desktop)
CC = gcc
AR = ar
else
CC = $(CROSS_COMPILE)gcc
AR = $(CROSS_COMPILE)ar
endif

# rcheevos source files
SOURCES = \
	$(SRCDIR)/rc_client.c \
	$(SRCDIR)/rc_compat.c \
	$(SRCDIR)/rc_util.c \
	$(SRCDIR)/rc_version.c \
	$(SRCDIR)/rc_libretro.c \
	$(SRCDIR)/rcheevos/alloc.c \
	$(SRCDIR)/rcheevos/condition.c \
	$(SRCDIR)/rcheevos/condset.c \
	$(SRCDIR)/rcheevos/consoleinfo.c \
	$(SRCDIR)/rcheevos/format.c \
	$(SRCDIR)/rcheevos/lboard.c \
	$(SRCDIR)/rcheevos/memref.c \
	$(SRCDIR)/rcheevos/operand.c \
	$(SRCDIR)/rcheevos/richpresence.c \
	$(SRCDIR)/rcheevos/runtime.c \
	$(SRCDIR)/rcheevos/runtime_progress.c \
	$(SRCDIR)/rcheevos/trigger.c \
	$(SRCDIR)/rcheevos/value.c \
	$(SRCDIR)/rcheevos/rc_validate.c \
	$(SRCDIR)/rapi/rc_api_common.c \
	$(SRCDIR)/rapi/rc_api_editor.c \
	$(SRCDIR)/rapi/rc_api_info.c \
	$(SRCDIR)/rapi/rc_api_runtime.c \
	$(SRCDIR)/rapi/rc_api_user.c \
	$(SRCDIR)/rhash/aes.c \
	$(SRCDIR)/rhash/cdreader.c \
	$(SRCDIR)/rhash/hash.c \
	$(SRCDIR)/rhash/hash_disc.c \
	$(SRCDIR)/rhash/hash_encrypted.c \
	$(SRCDIR)/rhash/hash_rom.c \
	$(SRCDIR)/rhash/hash_zip.c \
	$(SRCDIR)/rhash/md5.c

# Object files go in platform-specific directory
OBJECTS = $(patsubst $(SRCDIR)/%.c,$(OBJDIR)/%.o,$(SOURCES))

CFLAGS += $(OPT) -fomit-frame-pointer
CFLAGS += -I$(INCDIR) -I$(SRCDIR) -std=gnu99
# libretro.h is in minarch/libretro-common/include
CFLAGS += -I../minarch/libretro-common/include
CFLAGS += -DRC_DISABLE_LUA
CFLAGS += -DRC_CLIENT_SUPPORTS_HASH
CFLAGS += -fPIC

PRODUCT = build/$(PLATFORM)/lib$(TARGET).a

# rcheevos version to use (can be overridden)
RCHEEVOS_VERSION ?= 40d916de00fe757bab40fb4db41a7912193a48e3

.PHONY: build clean install clone

# Clone rcheevos source if not present
clone:
	@if [ ! -f "src/include/rc_client.h" ]; then \
		echo "Cloning rcheevos..."; \
		rm -rf src; \
		git clone https://github.com/RetroAchievements/rcheevos.git src; \
		cd src && git checkout $(RCHEEVOS_VERSION); \
	fi

# Build target: first clone, then compile (recursive make ensures sources exist)
build: clone
	$(MAKE) $(PRODUCT) PLATFORM=$(PLATFORM)

$(PRODUCT): $(OBJECTS)
	mkdir -p build/$(PLATFORM)
	$(AR) rcs $@ $(OBJECTS)

$(OBJDIR)/%.o: $(SRCDIR)/%.c
	mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c -o $@ $<

install: $(PRODUCT)
	mkdir -p "$(PREFIX_LOCAL)/include/rcheevos"
	mkdir -p "$(PREFIX_LOCAL)/lib"
	cp $(INCDIR)/*.h "$(PREFIX_LOCAL)/include/rcheevos/"
	cp $(PRODUCT) "$(PREFIX_LOCAL)/lib/"

clean:
	rm -rf build
	rm -f $(PREFIX_LOCAL)/lib/lib$(TARGET).a
	rm -rf $(PREFIX_LOCAL)/include/rcheevos
