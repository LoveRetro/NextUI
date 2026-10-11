ifeq (,$(CROSS_COMPILE))
$(error missing CROSS_COMPILE for this toolchain)
endif
ifeq (,$(PREFIX))
$(error missing PREFIX for this toolchain)
endif

TARGET=msettings

.PHONY: build
.PHONY: clean

CC = $(CROSS_COMPILE)gcc

CFLAGS = -I../../all/common
LDFLAGS = -ltinyalsa -ldl -lrt -lm -s

OPTM=-Ofast

build: 
	$(CC) -c -Werror -fpic "$(TARGET).c" $(CFLAGS) -Wl,--no-as-needed $(LDFLAGS)
	$(CC) -c -Werror -fpic "../../all/common/displaycal.c" $(CFLAGS) -Wl,--no-as-needed $(LDFLAGS)
	$(CC) -shared -o "lib$(TARGET).so" "$(TARGET).o" "displaycal.o" $(LDFLAGS)
	mkdir -p "$(PREFIX_LOCAL)/include"
	mkdir -p "$(PREFIX_LOCAL)/lib"
	cp "$(TARGET).h" "$(PREFIX_LOCAL)/include"
	cp "lib$(TARGET).so" "$(PREFIX_LOCAL)/lib"

clean:
	rm -f *.o
	rm -f "lib$(TARGET).so"
	rm -f $(PREFIX_LOCAL)/include/$(TARGET).h
	rm -f $(PREFIX_LOCAL)/lib/lib$(TARGET).so
