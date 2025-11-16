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
SOURCE = $(TARGET).c

CC = $(CROSS_COMPILE)gcc
CFLAGS += $(ARCH) -fomit-frame-pointer -std=gnu11 -Wall -Wextra
CFLAGS += -DPLATFORM=\"$(PLATFORM)\"
LDFLAGS +=

PRODUCT = build/$(PLATFORM)/$(TARGET).elf

all:
	mkdir -p build/$(PLATFORM)
	$(CC) $(SOURCE) -o $(PRODUCT) $(CFLAGS) $(LDFLAGS)

clean:
	rm -f $(PRODUCT)
