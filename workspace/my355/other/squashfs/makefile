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

all:
	mkdir -p output
	mkdir -p output/bin
	mkdir -p output/lib
	cp $(PREFIX)/bin/*squashfs output/bin
	cp $(PREFIX)/lib/liblz4.so.1 output/lib
	cp $(PREFIX)/lib/liblzma.so.5 output/lib
	cp $(PREFIX)/lib/liblzo2.so.2 output/lib
	cp $(PREFIX)/lib/libzstd.so.1 output/lib

clean:
	rm -rf output