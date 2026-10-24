###########################################################

ifeq (,$(PLATFORM))
PLATFORM=$(UNION_PLATFORM)
endif

ifeq (,$(PLATFORM))
$(error please specify PLATFORM, eg. PLATFORM=rgb30 make)
endif

###########################################################

REQUIRES_EVTEST=other/evtest
REQUIRES_MKBOOTIMG=other/mkbootimg
REQUIRES_RSCEGO=other/rsce-go
REQUIRES_FILEMGR=other/NextCommander

all: readmes
	cd other/squashfs && make

early: $(REQUIRES_EVTEST) $(REQUIRES_MKBOOTIMG) $(REQUIRES_RSCEGO) $(REQUIRES_FILEMGR)
	mkdir -p other
	cd $(REQUIRES_FILEMGR) && make -j
	cd $(REQUIRES_EVTEST) && $(CROSS_COMPILE)gcc -o evtest evtest.c
	cd $(REQUIRES_MKBOOTIMG) && make
	cd $(REQUIRES_RSCEGO) && CGO_ENABLED=0 GOOS=linux GOARCH=arm64 go build -o rsce_tool

clean:
	cd $(REQUIRES_FILEMGR) && make clean

###########################################################

$(REQUIRES_FILEMGR):
	git clone --depth 1 https://github.com/LoveRetro/NextCommander.git $(REQUIRES_FILEMGR)
$(REQUIRES_EVTEST):
	git clone --depth 1 https://github.com/freedesktop-unofficial-mirror/evtest.git $(REQUIRES_EVTEST)
$(REQUIRES_MKBOOTIMG):
	git clone --depth 1 https://github.com/LoveRetro/mkbootimg $(REQUIRES_MKBOOTIMG)
$(REQUIRES_RSCEGO):
	git clone --depth 1 https://github.com/Evsio0n/rsce-go $(REQUIRES_RSCEGO)
	cd $(REQUIRES_RSCEGO) && go mod tidy -e && go mod download

include ../all/readmes/makefile
