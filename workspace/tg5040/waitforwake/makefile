ifeq (,$(CROSS_COMPILE))
$(error missing CROSS_COMPILE for this toolchain)
endif

ifeq (,$(PLATFORM))
PLATFORM=$(UNION_PLATFORM)
endif

TARGET = waitforwake
PRODUCT = $(TARGET).elf

CC = $(CROSS_COMPILE)gcc
CFLAGS	= -Os -Wl,--gc-sections -s
CFLAGS  += -I. -I../../all/common -I../platform/ -I$(PREFIX_LOCAL)/include -L$(PREFIX_LOCAL)/lib -DPLATFORM=\"$(UNION_PLATFORM)\"

all:
	$(CC) $(TARGET).c -o $(PRODUCT) $(CFLAGS)
clean:
	rm -rf $(PRODUCT)
