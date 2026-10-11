###########################################################

ifeq (,$(PLATFORM))
PLATFORM=$(UNION_PLATFORM)
endif

ifeq (,$(PLATFORM))
	$(error please specify PLATFORM, eg. PLATFORM=h700 make)
endif

ifeq (,$(CROSS_COMPILE))
	$(error missing CROSS_COMPILE for this toolchain)
endif

###########################################################

include ../platform/makefile.env

###########################################################

INCDIR = -I. -I../../all/common -I../platform
SOURCE = poweroff_next.c ../../all/common/utils.c ../../all/common/config.c

CC = $(CROSS_COMPILE)gcc
# Override CFLAGS and LDFLAGS to not include SDL2 dependencies
CFLAGS = $(OPT) -g -std=gnu11 -Wall -Wextra
CFLAGS += $(INCDIR) -DPLATFORM=\"$(PLATFORM)\"
LDFLAGS = -lpthread -ldl

BUILDDIR = build/$(PLATFORM)

# One source, two binaries: reboot_next skips the PMIC power-off sequence.
all:
	mkdir -p $(BUILDDIR)
	$(CC) $(SOURCE) -o $(BUILDDIR)/poweroff_next.elf $(CFLAGS) $(LDFLAGS)
	$(CC) $(SOURCE) -o $(BUILDDIR)/reboot_next.elf $(CFLAGS) -DREBOOT_NEXT $(LDFLAGS)

clean:
	rm -rf $(BUILDDIR)
