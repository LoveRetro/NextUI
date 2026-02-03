# Makefile wrapper for libchdr (CMake-based project)
# Builds libchdr as a shared library for the target platform

PLATFORM ?= tg5040

BUILD_DIR = build/$(PLATFORM)

# Cross-compilation settings (only for non-desktop platforms)
ifneq ($(PLATFORM),desktop)
ifeq ($(PLATFORM),tg5040)
TOOLCHAIN_FILE = $(BUILD_DIR)/toolchain.cmake
CMAKE_EXTRA = -DCMAKE_TOOLCHAIN_FILE=$(TOOLCHAIN_FILE)
endif
ifeq ($(PLATFORM),tg5050)
TOOLCHAIN_FILE = $(BUILD_DIR)/toolchain.cmake
CMAKE_EXTRA = -DCMAKE_TOOLCHAIN_FILE=$(TOOLCHAIN_FILE)
endif
endif

.PHONY: all build clean

all: build

build: $(BUILD_DIR)/libchdr.so

$(BUILD_DIR)/libchdr.so: | $(BUILD_DIR)
ifeq ($(PLATFORM),tg5040)
	@echo "Creating CMake toolchain file for cross-compilation..."
	@echo 'set(CMAKE_SYSTEM_NAME Linux)' > $(TOOLCHAIN_FILE)
	@echo 'set(CMAKE_SYSTEM_PROCESSOR aarch64)' >> $(TOOLCHAIN_FILE)
	@echo 'set(CMAKE_C_COMPILER aarch64-nextui-linux-gnu-gcc)' >> $(TOOLCHAIN_FILE)
	@echo 'set(CMAKE_CXX_COMPILER aarch64-nextui-linux-gnu-g++)' >> $(TOOLCHAIN_FILE)
	@echo 'set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)' >> $(TOOLCHAIN_FILE)
	@echo 'set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)' >> $(TOOLCHAIN_FILE)
	@echo 'set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)' >> $(TOOLCHAIN_FILE)
endif
ifeq ($(PLATFORM),tg5050)
	@echo "Creating CMake toolchain file for cross-compilation..."
	@echo 'set(CMAKE_SYSTEM_NAME Linux)' > $(TOOLCHAIN_FILE)
	@echo 'set(CMAKE_SYSTEM_PROCESSOR aarch64)' >> $(TOOLCHAIN_FILE)
	@echo 'set(CMAKE_C_COMPILER aarch64-nextui-linux-gnu-gcc)' >> $(TOOLCHAIN_FILE)
	@echo 'set(CMAKE_CXX_COMPILER aarch64-nextui-linux-gnu-g++)' >> $(TOOLCHAIN_FILE)
	@echo 'set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)' >> $(TOOLCHAIN_FILE)
	@echo 'set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)' >> $(TOOLCHAIN_FILE)
	@echo 'set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)' >> $(TOOLCHAIN_FILE)
endif
	cd $(BUILD_DIR) && cmake ../.. \
		-DBUILD_SHARED_LIBS=ON \
		-DINSTALL_STATIC_LIBS=OFF \
		-DCMAKE_BUILD_TYPE=Release \
		-DWITH_SYSTEM_ZLIB=OFF \
		$(CMAKE_EXTRA)
	cd $(BUILD_DIR) && make -j$$(nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)
	@# Copy the .so to expected location (cmake may put it in a subdir)
	@if [ -f $(BUILD_DIR)/libchdr.so ]; then \
		echo "libchdr.so built successfully"; \
	elif [ -f $(BUILD_DIR)/src/libchdr.so ]; then \
		cp $(BUILD_DIR)/src/libchdr.so $(BUILD_DIR)/; \
	fi

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

clean:
	rm -rf $(BUILD_DIR)
