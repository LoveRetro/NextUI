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

TARGET = syncsettings
SOURCE = $(TARGET).c

CC = $(CROSS_COMPILE)gcc
CFLAGS   = $(OPT) -I$(PREFIX_LOCAL)/include -DPLATFORM=\"$(PLATFORM)\"
LDFLAGS	 = -Os -L$(PREFIX_LOCAL)/lib -lmsettings -lrt -ldl -Wl,--gc-sections -s

PRODUCT= build/$(PLATFORM)/$(TARGET).elf

all: $(PREFIX_LOCAL)/include/msettings.h
	mkdir -p build/$(PLATFORM)
	$(CC) $(SOURCE) -o $(PRODUCT) $(CFLAGS) $(LDFLAGS)
clean:
	rm -f $(PRODUCT)

$(PREFIX_LOCAL)/include/msettings.h:
	cd ../../$(PLATFORM)/libmsettings && make
