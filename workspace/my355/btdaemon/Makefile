ifeq (,$(CROSS_COMPILE))
$(error missing CROSS_COMPILE for this toolchain)
endif
ifeq (,$(PREFIX))
$(error missing PREFIX for this toolchain)
endif

TARGET = bt_daemon

.PHONY: all clean

CC = $(CROSS_COMPILE)gcc
CCXX= $(CROSS_COMPILE)g++

CXXFLAGS = -Wall -O2 -I$(PREFIX)/include/dbus-1.0 -I$(PREFIX)/lib/dbus-1.0/include
LDFLAGS = -L$(PREFIX)/lib -ldbus-1
CXXFLAGS += -I$(PREFIX_LOCAL)/include
LDFLAGS += -L$(PREFIX_LOCAL)/lib -lmsettings

SRCS = bt_daemon.cpp
OBJS = $(SRCS:.cpp=.o)

all: $(PREFIX_LOCAL)/include/msettings.h $(TARGET)

$(TARGET): $(OBJS)
	$(CXX) -o $@ $^ $(LDFLAGS)

$(PREFIX_LOCAL)/include/msettings.h:
	cd ../libmsettings && make

%.o: %.cpp
	$(CXX) $(CXXFLAGS) -c $< -o $@

clean:
	rm -f $(TARGET) $(OBJS)
