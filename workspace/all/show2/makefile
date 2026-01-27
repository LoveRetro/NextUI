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

include ../../$(PLATFORM)/platform/makefile.env
SDL?=SDL

###########################################################

TARGET = show2
PRODUCT = $(TARGET).elf

CXX = $(CROSS_COMPILE)g++
CXXFLAGS = -Os -Wall -std=c++17
LDFLAGS = -lSDL2 -lSDL2_image -lSDL2_ttf -lpthread -lrt -ldl -Wl,--gc-sections -s

all:
	mkdir -p build/$(PLATFORM)
	$(CXX) $(CXXFLAGS) $(TARGET).cpp -o build/$(PLATFORM)/$(PRODUCT) $(LDFLAGS)

clean:
	rm -rf build/$(PLATFORM)/$(PRODUCT)