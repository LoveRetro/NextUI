###########################################################

ifeq (,$(PLATFORM))
PLATFORM=$(UNION_PLATFORM)
endif

ifeq (,$(PLATFORM))
$(error please specify PLATFORM, eg. PLATFORM=rgb30 make)
endif

###########################################################

REQUIRES_EVTEST=other/evtest
REQUIRES_JSTEST=other/jstest
REQUIRES_UNZIP60=other/unzip60
REQUIRES_FILEMGR=other/NextCommander
REQUIRES_SDL2=other/sdl2

.PHONY: curl-runtime
FILEMGR_PATCH=patches/NextCommander-h700.patch
FILEMGR_STAMP=.patched-h700-v3

all: readmes

# SDL2 (mali-fbdev, alsa-shared, joystick patch) is prebaked into h700-toolchain
# at PREFIX_LOCAL. Stage it + SDK companion libs into other/sdl2/output for packaging.
# Rebuild only via the toolchain image (support/build-sdl2.sh + support/sdl2-h700.patch).
early: $(REQUIRES_EVTEST) $(REQUIRES_JSTEST) $(REQUIRES_UNZIP60) $(REQUIRES_FILEMGR) curl-runtime
	mkdir -p other
	cd $(REQUIRES_EVTEST) && $(CROSS_COMPILE)gcc -o evtest evtest.c
	cd $(REQUIRES_JSTEST) && $(CROSS_COMPILE)gcc -o jstest jstest.c
	cd $(REQUIRES_UNZIP60) && make -f unix/Makefile.trimuismart unzip
	@test -e "$(PREFIX_LOCAL)/lib/libSDL2.so" -o -e "$(PREFIX_LOCAL)/lib/libSDL2-2.0.so.0" || \
		(echo "error: prebaked SDL2 missing from PREFIX_LOCAL=$(PREFIX_LOCAL); use ghcr.io/loveretro/h700-toolchain" >&2; exit 1)
	mkdir -p $(REQUIRES_SDL2)/output
	cp -a $(PREFIX_LOCAL)/lib/libSDL2*.so* $(REQUIRES_SDL2)/output/
	cp -a $(PREFIX)/lib/libSDL2_image*.so* $(REQUIRES_SDL2)/output/ 2>/dev/null || true
	cp -a $(PREFIX)/lib/libSDL2_ttf*.so* $(REQUIRES_SDL2)/output/ 2>/dev/null || true
	cp -L $(PREFIX)/lib/libtinyalsa.so* $(REQUIRES_SDL2)/output/ 2>/dev/null || true
	# SDL2_image dlopens libpng12.so.0 at runtime; the H700 stock OS only has
	# a 32-bit copy under /mnt/vendor/lib, so bundle the toolchain's 64-bit one
	cp -L $(PREFIX)/lib/libpng12.so.0 $(REQUIRES_SDL2)/output/
	cd $(REQUIRES_FILEMGR) && ([ -f $(FILEMGR_STAMP) ] || (git checkout -- . && git apply --unidiff-zero ../../$(FILEMGR_PATCH) && touch $(FILEMGR_STAMP)))
	cd $(REQUIRES_FILEMGR) && ([ -f output/.platform-h700 ] || make clean PLATFORM=h700)
	cd $(REQUIRES_FILEMGR) && make -j PLATFORM=h700 && touch output/.platform-h700
	cd boot && sh build.sh

curl-runtime: curl/build.sh
	cd curl && sh build.sh

clean:
	cd $(REQUIRES_FILEMGR) && make clean PLATFORM=h700
	cd $(REQUIRES_UNZIP60) && make -f unix/Makefile.trimuismart clean
	rm -rf $(REQUIRES_SDL2)/output
	rm -rf curl/output curl/curl-* curl/musl-* curl/openssl-*
	rm -rf boot/output

###########################################################

$(REQUIRES_FILEMGR):
	git clone --depth 1 https://github.com/LoveRetro/NextCommander.git $(REQUIRES_FILEMGR)
	
$(REQUIRES_EVTEST):
	git clone --depth 1 https://github.com/freedesktop-unofficial-mirror/evtest.git $(REQUIRES_EVTEST)

$(REQUIRES_JSTEST):
	git clone --depth 1 https://github.com/datrh/joyutils.git $(REQUIRES_JSTEST)

$(REQUIRES_UNZIP60):
	git clone --depth 1 https://github.com/shauninman/unzip60.git $(REQUIRES_UNZIP60)

include ../all/readmes/makefile
