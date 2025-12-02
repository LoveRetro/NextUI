ifeq (,$(CROSS_COMPILE))
$(error missing CROSS_COMPILE for this toolchain)
endif
ifeq (,$(PREFIX))
$(error missing PREFIX for this toolchain)
endif

target = libbtmg.so

install:
	-mkdir -p $(PREFIX)/lib
	-mkdir -p $(PREFIX)/include
	cp ./include/*.h $(PREFIX)/include
	cp ./lib/aarch64/glibc-gcc8_3_0/$(target) $(PREFIX)/lib

####################################################################
all: install
