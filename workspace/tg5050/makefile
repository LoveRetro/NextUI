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
REQUIRES_FILEMGR=other/NextCommander
REQUIRES_7ZIP=other/7zip

all: readmes

early: $(REQUIRES_EVTEST) $(REQUIRES_JSTEST) $(REQUIRES_FILEMGR) $(REQUIRES_7ZIP)
	mkdir -p other
	cd $(REQUIRES_FILEMGR) && make -j
	cd $(REQUIRES_EVTEST) && $(CROSS_COMPILE)gcc -o evtest evtest.c
	cd $(REQUIRES_JSTEST) && $(CROSS_COMPILE)gcc -o jstest jstest.c
	cd $(REQUIRES_7ZIP)/CPP/7zip/Bundles/Alone2 && $(MAKE) -j -f ../../cmpl_gcc.mak O=_o CC=$(CROSS_COMPILE)gcc CXX=$(CROSS_COMPILE)g++ LDFLAGS_STATIC_3="-static-libstdc++ -static-libgcc"

clean:
	cd $(REQUIRES_FILEMGR) && make clean
	rm -rf $(REQUIRES_7ZIP)/CPP/7zip/Bundles/Alone2/_o

###########################################################

$(REQUIRES_FILEMGR):
	git clone --depth 1 https://github.com/LoveRetro/NextCommander.git $(REQUIRES_FILEMGR)
	
$(REQUIRES_EVTEST):
	git clone --depth 1 https://github.com/freedesktop-unofficial-mirror/evtest.git $(REQUIRES_EVTEST)

$(REQUIRES_JSTEST):
	git clone --depth 1 https://github.com/datrh/joyutils.git $(REQUIRES_JSTEST)

$(REQUIRES_7ZIP):
	git clone --depth 1 --branch 26.00 https://github.com/ip7z/7zip.git $(REQUIRES_7ZIP)

include ../all/readmes/makefile
