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

include ../platform/makefile.env

###########################################################

TARGET = poweroff_next
INCDIR = -I. -I../../all/common -I../platform
SOURCE = $(TARGET).c ../../all/common/utils.c ../../all/common/config.c

CC = $(CROSS_COMPILE)gcc
# Override CFLAGS and LDFLAGS to not include SDL2 dependencies
CFLAGS = $(ARCH) -g -std=gnu11 -Wall -Wextra
CFLAGS += $(INCDIR) -DPLATFORM=\"$(PLATFORM)\"
LDFLAGS = -lpthread -ldl

PRODUCT = build/$(PLATFORM)/$(TARGET).elf

all:
	mkdir -p build/$(PLATFORM)
	$(CC) $(SOURCE) -o $(PRODUCT) $(CFLAGS) $(LDFLAGS)

clean:
	rm -f $(PRODUCT)
