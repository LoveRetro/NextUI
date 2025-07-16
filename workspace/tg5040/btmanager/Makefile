ifeq (,$(CROSS_COMPILE))
$(error missing CROSS_COMPILE for this toolchain)
endif
ifeq (,$(PREFIX))
$(error missing PREFIX for this toolchain)
endif

TARGET_ARCH = "aarch64"
TARGET_CC = $(CROSS_COMPILE)gcc
TARGET_CXX = $(CROSS_COMPILE)g++
TARGET_AR = $(CROSS_COMPILE)ar
TARGET_CFLAGS = -pthread
TARGET_LDFLAGS = 
PKG_INSTALL_DIR = 
PKG_BUILD_DIR = .

TARGET_CFLAGS = $(if $(CONFIG_USE_GLIBC),-pthread)

btmanager:
	make -C $(PKG_BUILD_DIR)/src/ \
		ARCH="$(TARGET_ARCH)" \
		AR="$(TARGET_AR)" \
		CC="$(TARGET_CC)" \
		CXX="$(TARGET_CXX)" \
		CFLAGS="$(TARGET_CFLAGS)" \
		LDFLAGS="$(TARGET_LDFLAGS)" \
		CONFIG_PREFIX="$(PKG_INSTALL_DIR)" \
		CONFIG_IPV6="$(CONFIG_IPV6)" \
		all

bluez:
	rm -rf ./build
	mkdir -p ./build/.update_bluez/usr/bin
	mkdir -p ./build/.update_bluez/usr/lib
	mkdir -p ./build/.update_bluez/usr/lib64
	# bluez
	cp /usr/bin/bluetoothctl build/.update_bluez/usr/bin/
	cp /usr/bin/btmon build/.update_bluez/usr/bin/
	cp /usr/bin/rctest build/.update_bluez/usr/bin/
	cp /usr/bin/l2test build/.update_bluez/usr/bin/
	cp /usr/bin/l2ping build/.update_bluez/usr/bin/
	cp /usr/bin/bluemoon build/.update_bluez/usr/bin/
	cp /usr/bin/hex2hcd build/.update_bluez/usr/bin/
	cp /usr/bin/mpris-proxy build/.update_bluez/usr/bin/
	cp /usr/bin/btattach build/.update_bluez/usr/bin/
	cp /usr/bin/isotest build/.update_bluez/usr/bin/
	cp /usr/libexec/bluetooth/bluetoothd build/.update_bluez/usr/bin/
	cp /usr/libexec/bluetooth/obexd build/.update_bluez/usr/bin/
	cp /usr/lib/libbluetooth.so.3.19.15 build/.update_bluez/usr/lib/
	cp /usr/lib/libbluetooth.so.3.19.15 build/.update_bluez/usr/lib64/
	#cp /etc/dbus-1/system.d/bluetooth.conf build/.update_bluez/etc/dbus-1/system.d/bluetooth.conf
	#cp /etc/dbus-1/system.d/bluetooth-mesh.conf build/.update_bluez/etc/dbus-1/system.d/bluetooth-mesh.conf
	#cp /etc/dbus-1/system.d/bluetooth-mesh-adapter.conf build/.update_bluez/etc/dbus-1/system.d/bluetooth-mesh-adapter.conf
	# bluez-alsa
	mkdir -p ./build/.update_bluez/usr/lib/alsa-lib
	mkdir -p ./build/.update_bluez/usr/lib64/alsa-lib
	mkdir -p ./build/.update_bluez/etc/dbus-1/system.d
	mkdir -p ./build/.update_bluez/usr/share/alsa/alsa.conf.d
	cp /usr/lib/alsa-lib/libasound_module_ctl_bluealsa.so build/.update_bluez/usr/lib/alsa-lib/libasound_module_ctl_bluealsa.so
	cp /usr/lib/alsa-lib/libasound_module_pcm_bluealsa.so build/.update_bluez/usr/lib/alsa-lib/libasound_module_pcm_bluealsa.so
	cp /usr/lib/alsa-lib/libasound_module_ctl_bluealsa.so build/.update_bluez/usr/lib64/alsa-lib/libasound_module_ctl_bluealsa.so
	cp /usr/lib/alsa-lib/libasound_module_pcm_bluealsa.so build/.update_bluez/usr/lib64/alsa-lib/libasound_module_pcm_bluealsa.so
	cp /usr/bin/bluealsa build/.update_bluez/usr/bin/
	cp /usr/bin/bluealsa-aplay build/.update_bluez/usr/bin/
	cp /etc/dbus-1/system.d/bluealsa.conf build/.update_bluez/etc/dbus-1/system.d/bluealsa.conf
	cp /usr/share/alsa/alsa.conf.d/20-bluealsa.conf build/.update_bluez/usr/share/alsa/alsa.conf.d/20-bluealsa.conf
	# remove
	# cp /lib/aarch64-linux-gnu/libncurses.so.6 build/.update_bluez/usr/lib/
	# cp /lib/aarch64-linux-gnu/libtinfo.so.6 build/.update_bluez/usr/lib/
	# script
	cp support/upgrade_bluez.sh build/post_install.sh
	# zip 
	# zip -r nextui.upgrade_bluez.pakz build/
	cd build && python -m zipfile -c nextui.upgrade_bluez.pakz .update_bluez/ post_install.sh

all: btmanager bluez
