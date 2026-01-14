ifeq (,$(CROSS_COMPILE))
$(error missing CROSS_COMPILE for this toolchain)
endif
ifeq (,$(PREFIX))
$(error missing PREFIX for this toolchain)
endif

bluez:
	rm -rf ./build
	mkdir -p ./build/.update_bluez/usr/bin
	mkdir -p ./build/.update_bluez/usr/lib
	mkdir -p ./build/.update_bluez/usr/lib64
	# bluez
	cp $(PREFIX)/bin/bluetoothctl build/.update_bluez/usr/bin/
	cp $(PREFIX)/bin/btmon build/.update_bluez/usr/bin/
	cp $(PREFIX)/bin/rctest build/.update_bluez/usr/bin/
	cp $(PREFIX)/bin/l2test build/.update_bluez/usr/bin/
	cp $(PREFIX)/bin/l2ping build/.update_bluez/usr/bin/
	cp $(PREFIX)/bin/bluemoon build/.update_bluez/usr/bin/
	cp $(PREFIX)/bin/hex2hcd build/.update_bluez/usr/bin/
	cp $(PREFIX)/bin/mpris-proxy build/.update_bluez/usr/bin/
	cp $(PREFIX)/bin/btattach build/.update_bluez/usr/bin/
	cp $(PREFIX)/bin/isotest build/.update_bluez/usr/bin/
	cp $(PREFIX)/libexec/bluetooth/bluetoothd build/.update_bluez/usr/bin/
	cp $(PREFIX)/libexec/bluetooth/obexd build/.update_bluez/usr/bin/
	cp $(PREFIX)/lib/libbluetooth.so.3.19.15 build/.update_bluez/usr/lib/
	cp $(PREFIX)/lib/libbluetooth.so.3.19.15 build/.update_bluez/usr/lib64/
	#cp /etc/dbus-1/system.d/bluetooth.conf build/.update_bluez/etc/dbus-1/system.d/bluetooth.conf
	#cp /etc/dbus-1/system.d/bluetooth-mesh.conf build/.update_bluez/etc/dbus-1/system.d/bluetooth-mesh.conf
	#cp /etc/dbus-1/system.d/bluetooth-mesh-adapter.conf build/.update_bluez/etc/dbus-1/system.d/bluetooth-mesh-adapter.conf
	# bluez-alsa
	mkdir -p ./build/.update_bluez/usr/lib/alsa-lib
	mkdir -p ./build/.update_bluez/usr/lib64/alsa-lib
	mkdir -p ./build/.update_bluez/etc/dbus-1/system.d
	mkdir -p ./build/.update_bluez/usr/share/alsa/alsa.conf.d
	cp $(PREFIX)/lib/alsa-lib/libasound_module_ctl_bluealsa.so build/.update_bluez/usr/lib/alsa-lib/libasound_module_ctl_bluealsa.so
	cp $(PREFIX)/lib/alsa-lib/libasound_module_pcm_bluealsa.so build/.update_bluez/usr/lib/alsa-lib/libasound_module_pcm_bluealsa.so
	cp $(PREFIX)/lib/alsa-lib/libasound_module_ctl_bluealsa.so build/.update_bluez/usr/lib64/alsa-lib/libasound_module_ctl_bluealsa.so
	cp $(PREFIX)/lib/alsa-lib/libasound_module_pcm_bluealsa.so build/.update_bluez/usr/lib64/alsa-lib/libasound_module_pcm_bluealsa.so
	cp $(PREFIX)/bin/bluealsa build/.update_bluez/usr/bin/
	cp $(PREFIX)/bin/bluealsa-aplay build/.update_bluez/usr/bin/
	cp $(SYSROOT)/etc/dbus-1/system.d/bluealsa.conf build/.update_bluez/etc/dbus-1/system.d/bluealsa.conf
	cp $(PREFIX)/share/alsa/alsa.conf.d/20-bluealsa.conf build/.update_bluez/usr/share/alsa/alsa.conf.d/20-bluealsa.conf
	# sbc
	cp $(PREFIX)/lib/libsbc.so.1.3.1 build/.update_bluez/usr/lib/
	cp $(PREFIX)/lib/libsbc.so.1.3.1 build/.update_bluez/usr/lib64/
	cp $(PREFIX)/bin/sbcinfo build/.update_bluez/usr/bin/
	cp $(PREFIX)/bin/sbcdec build/.update_bluez/usr/bin/
	cp $(PREFIX)/bin/sbcenc build/.update_bluez/usr/bin/
	# script
	cp support/upgrade_bluez.sh build/post_install.sh
	# zip 
	# zip -r nextui.upgrade_bluez.pakz build/
	cd build && python3 -m zipfile -c nextui.upgrade_bluez.pakz .update_bluez/ post_install.sh

all: bluez
