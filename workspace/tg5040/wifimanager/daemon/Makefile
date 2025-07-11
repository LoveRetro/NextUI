ifeq (,$(CROSS_COMPILE))
$(error missing CROSS_COMPILE for this toolchain)
endif
ifeq (,$(PREFIX))
$(error missing PREFIX for this toolchain)
endif

CC = $(CROSS_COMPILE)gcc

#INCLUDES += -I$(CONFIG_PREFIX)/usr/include \
#			-I ../core/include \
#			-I .

INCLUDES += -I ../core/include \
			-I .

install:
	-@mkdir -p $(PREFIX)/bin
	-@mkdir -p $(PREFIX)/lib
	-@mkdir -p $(PREFIX)/include
	@cp wifid_cmd.h $(PREFIX)/include

wifi_daemon: wifi_daemon.c wifid_ctrl.c
	$(CC) -o $@ $^ $(CFLAGS) $(INCLUDES) $(LDFLAGS) -L$(PREFIX)/lib -lwifimg
	@cp -f wifi_daemon $(PREFIX)/bin

libwifid.so: wifid_cmd_handle.c wifid_cmd_iface.c
	$(CC) -fPIC -shared $(INCLUDES) $(LDFLAGS) $^ -o $@ $(LIBS)
	@cp $@ $(PREFIX)/lib

####################################################################
all: install wifi_daemon libwifid.so
clean:
	rm -rf  wifi_daemon wifi