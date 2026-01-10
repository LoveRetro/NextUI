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

###########################################################

TARGET = audiomon.elf

.PHONY: all clean

CC = $(CROSS_COMPILE)gcc
CCXX= $(CROSS_COMPILE)g++

CXXFLAGS += -Wall -O2 -I. -I../common/ -I../../$(PLATFORM)/platform/ -I$(PREFIX)/include/dbus-1.0 -I$(PREFIX)/lib/dbus-1.0/include
LDFLAGS += $$(pkg-config --libs dbus-1 libudev)
CXXFLAGS += -I$(PREFIX_LOCAL)/include -DPLATFORM=\"$(PLATFORM)\" $$(pkg-config --cflags dbus-1 libudev)
LDFLAGS += -L$(PREFIX_LOCAL)/lib -lmsettings

SRCS = audiomon.cpp
OBJS = $(addprefix build/$(PLATFORM)/,$(SRCS:.cpp=.o))

PRODUCT = build/$(PLATFORM)/$(TARGET)

all: $(PREFIX_LOCAL)/include/msettings.h $(PRODUCT)

build/$(PLATFORM):
	mkdir -p $@

$(PRODUCT): build/$(PLATFORM) $(OBJS)
	$(CXX) -o $@ $(OBJS) $(LDFLAGS)

$(PREFIX_LOCAL)/include/msettings.h:
	cd ../libmsettings && make

%.o: %.cpp
	$(CXX) $(CXXFLAGS) -c $< -o $@

build/$(PLATFORM)/%.o: %.cpp
	$(CXX) $(CXXFLAGS) -c $< -o $@

clean:
	rm -f $(PRODUCT) $(OBJS)
