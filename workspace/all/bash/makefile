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

BASH_VERSION = 5.2
BASH_SOURCE = bash-$(BASH_VERSION).tar.gz
BUILD_DIR = build/bash-$(BASH_VERSION)
CONFIGURE_ARGS = --without-bash-malloc --host=aarch64-pc-linux-gnu

TARGET = bash
PRODUCT = build/$(PLATFORM)/bash

.PHONY: all
all: $(PRODUCT)

$(BASH_SOURCE):
	@echo "Fetching bash source version $(BASH_VERSION)..."
	mkdir -p build/
	wget https://ftp.gnu.org/gnu/bash/$(BASH_SOURCE) -O $(BASH_SOURCE)

$(BUILD_DIR)/configure: $(BASH_SOURCE)
	@echo "Extracting $(BASH_SOURCE)..."
	tar -xf $(BASH_SOURCE) -C build
	rm $(BASH_SOURCE)

$(PRODUCT): $(BUILD_DIR)/configure
	@echo "Configuring bash..."
	(cd $(BUILD_DIR) && \
	./configure $(CONFIGURE_ARGS))
	
	@echo "Compiling bash..."
	(cd $(BUILD_DIR) && \
	make)
	
	@echo "Copying final executable and cleaning up..."
	mkdir -p build/$(PLATFORM)
	cp $(BUILD_DIR)/$(TARGET) $(PRODUCT)
	chown 1000:1000 $(PRODUCT)
	rm -rf $(BUILD_DIR)

.PHONY: clean
clean:
	rm -f $(PRODUCT)
	rm -rf build/bash-*
