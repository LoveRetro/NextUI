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

SRCS = bt_daemon.cpp
OBJS = $(SRCS:.cpp=.o)

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CXX) -o $@ $^ $(LDFLAGS)

%.o: %.cpp
	$(CXX) $(CXXFLAGS) -c $< -o $@

clean:
	rm -f $(TARGET) $(OBJS)
