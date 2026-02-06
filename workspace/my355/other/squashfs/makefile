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
	cp -L $(PREFIX)/lib/liblzma.so output/lib
	cp -L $(PREFIX)/lib/liblzo2.so output/lib
	cp -L $(PREFIX)/lib/liblz4.so output/lib
	cp -L $(PREFIX)/lib/libzstd.so output/lib

clean:
	rm -rf output