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

TARGET = settings
INCDIR = -I. -I../common/ -I../../$(PLATFORM)/platform/
SOURCE = -c ../common/utils.c ../common/api.c ../common/config.c ../common/scaler.c ../minarch/http.c ../common/ra_auth.c ../../$(PLATFORM)/platform/platform.c
CXXSOURCE = $(TARGET).cpp menu.cpp wifimenu.cpp btmenu.cpp keyboardprompt.cpp
CXXSOURCE += build/$(PLATFORM)/utils.o build/$(PLATFORM)/api.o build/$(PLATFORM)/config.o build/$(PLATFORM)/scaler.o build/$(PLATFORM)/http.o build/$(PLATFORM)/ra_auth.o build/$(PLATFORM)/platform.o

CC = $(CROSS_COMPILE)gcc
CXX = $(CROSS_COMPILE)g++
CFLAGS += $(OPT)
CFLAGS += $(INCDIR) -DPLATFORM=\"$(PLATFORM)\"
CXXFLAGS += $(CFLAGS) -std=c++17
CFLAGS += -fpermissive
LDFLAGS +=  -lmsettings
ifeq ($(PLATFORM), tg5040)
# drop as soon as we use btagent on all platforms
CXXFLAGS += -DHAS_BTAGENT
CXXSOURCE += btagent.cpp
CXXFLAGS += $$(pkg-config --cflags --libs gio-2.0 glib-2.0)
endif
ifeq ($(PLATFORM), tg5050)
# drop as soon as we use btagent on all platforms
CXXFLAGS += -DHAS_BTAGENT
CXXSOURCE += btagent.cpp
CXXFLAGS += $$(pkg-config --cflags --libs gio-2.0 glib-2.0)
endif

PRODUCT= build/$(PLATFORM)/$(TARGET).elf

all: $(PREFIX_LOCAL)/include/msettings.h
	mkdir -p build/$(PLATFORM)
	$(CC) $(SOURCE) $(CFLAGS) $(LDFLAGS)
	mv utils.o api.o config.o scaler.o http.o ra_auth.o platform.o build/$(PLATFORM)
	$(CXX) $(CXXSOURCE) -o $(PRODUCT) $(CXXFLAGS) $(LDFLAGS) -lstdc++
clean:
	rm -f $(PRODUCT)

$(PREFIX_LOCAL)/include/msettings.h:
	cd ../../$(PLATFORM)/libmsettings && make