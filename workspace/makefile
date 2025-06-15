# workspace (all)

###########################################################

ifeq (,$(PLATFORM))
PLATFORM=$(UNION_PLATFORM)
endif

ifeq (,$(PLATFORM))
$(error please specify PLATFORM, eg. PLATFORM=trimui make)
endif

###########################################################

.PHONY: all

all:
ifeq ($(PLATFORM), desktop)
	cd ./$(PLATFORM)/libmsettings && make
	cd ./$(PLATFORM) && make early # eg. other libs
	cd ./all/nextui/ && make
	cd ./all/minarch/ && make
	cd ./all/battery/ && make
	cd ./all/clock/ && make
	cd ./all/libbatmondb/ && make
	cd ./all/batmon/ && make
	cd ./all/libgametimedb/ && make
	cd ./all/gametimectl/ && make
	cd ./all/gametime/ && make
	cd ./all/minput/ && make
	cd ./all/nextval/ && make
	cd ./all/settings/ && make
else 
	cd ./$(PLATFORM)/wifimanager && make
	cd ./$(PLATFORM)/libmsettings && make
	cd ./$(PLATFORM) && make early # eg. other libs
	cd ./$(PLATFORM)/keymon && make
	cd ./all/nextui/ && make
	cd ./all/minarch/ && make
	cd ./all/battery/ && make
	cd ./all/clock/ && make
	cd ./all/libbatmondb/ && make
	cd ./all/batmon/ && make
	cd ./all/libgametimedb/ && make
	cd ./all/gametimectl/ && make
	cd ./all/gametime/ && make
	cd ./all/minput/ && make
	cd ./all/syncsettings/ && make
	cd ./all/nextval/ && make
	cd ./all/settings/ && make
	cd ./all/ledcontrol/ && make
	cd ./all/bootlogo/ && make
endif
ifdef COMPILE_CORES
	make cores
else
	echo "skipping core build"
endif
	cd ./$(PLATFORM) && make

cores:
	cd ./$(PLATFORM)/cores && make

core:
ifndef CORE
	$(error CORE is not set)
endif
	cd ./$(PLATFORM)/cores && make $(CORE)

clean:
ifneq ($(PLATFORM), desktop)
	cd ./$(PLATFORM)/wifimanager && make clean
	cd ./$(PLATFORM)/keymon && make clean
	cd ./all/syncsettings/ && make clean
	cd ./all/ledcontrol/ && make clean
	cd ./all/bootlogo/ && make clean
endif
	cd ./$(PLATFORM)/libmsettings && make clean
	cd ./all/nextui/ && make clean
	cd ./all/minarch/ && make clean
	cd ./all/battery/ && make clean
	cd ./all/clock/ && make clean
	cd ./all/libbatmondb/ && make clean
	cd ./all/batmon/ && make clean
	cd ./all/libgametimedb/ && make clean
	cd ./all/gametimectl/ && make clean
	cd ./all/gametime/ && make clean
	cd ./all/minput/ && make clean
	cd ./all/nextval/ && make clean
	cd ./all/settings/ && make clean
	cd ./$(PLATFORM) && make clean