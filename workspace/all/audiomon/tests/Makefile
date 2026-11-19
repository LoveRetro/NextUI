CXX ?= g++
CXXFLAGS += -std=c++11 -Wall -Wextra -I../../common -I../../../tg5040/libmsettings
CXXFLAGS += $(shell pkg-config --cflags dbus-1 libudev)
LDLIBS += $(shell pkg-config --libs dbus-1 libudev)

.PHONY: test compile clean

test: usb_audio_test
	./usb_audio_test

usb_audio_test: usb_audio_test.cpp ../audiomon.cpp
	$(CXX) $(CXXFLAGS) -o $@ $< $(LDLIBS)

compile: audiomon.o

audiomon.o: ../audiomon.cpp
	$(CXX) $(CXXFLAGS) -I../../../tg5040/platform -DPLATFORM=\"tg5040\" -c -o $@ $<

clean:
	rm -f usb_audio_test audiomon.o
