VERSION := $(shell cat VERSION)

# libsodium is auto-built from third_party source for x64 targets
LIBSODIUM_SRC := third_party/libsodium
LIBSODIUM_INSTALL := third_party/libsodium-install

.PHONY: all clean x64 x64d x64r arm armd armr armv7 armv7d armv7r mips32be mips32bed mips32ber release debug libsodium clean-libsodium

all: x64r x64d armr armd armv7r armv7d mips32ber mips32bed

release: x64r armr armv7r mips32ber

debug: x64d armd armv7d mips32bed

# Build libsodium from vendored source for x64 native builds.
# Cross-compilation targets (ARM, MIPS) fall back to Monocypher.
libsodium:
	@if [ ! -f "$(LIBSODIUM_INSTALL)/lib/libsodium.a" ]; then \
		echo "Building libsodium from $(LIBSODIUM_SRC)..."; \
		cd $(LIBSODIUM_SRC) && ./configure --prefix=$(PWD)/$(LIBSODIUM_INSTALL) \
			--enable-static --disable-shared --disable-tests \
			CFLAGS="-O3 -march=native -mtune=native -fomit-frame-pointer" && \
		$(MAKE) clean && $(MAKE) -j$$(nproc) && $(MAKE) install; \
	else \
		echo "libsodium already built at $(LIBSODIUM_INSTALL)."; \
	fi

x64r: libsodium
	@mkdir -p bin
	cmake -B build-x64 -DCMAKE_BUILD_TYPE=Release -DTARGET_NAME=ganon_$(VERSION)_x64_release
	cmake --build build-x64
	@rm -rf build-x64

x64d: libsodium
	@mkdir -p bin
	cmake -B build-x64d -DCMAKE_BUILD_TYPE=Debug -DTARGET_NAME=ganon_$(VERSION)_x64_debug
	cmake --build build-x64d
	@rm -rf build-x64d

x64: x64r x64d

armr:
	@mkdir -p bin
	cmake -B build-arm -DCMAKE_BUILD_TYPE=Release -DCMAKE_TOOLCHAIN_FILE=cmake/armv5-toolchain.cmake -DTARGET_NAME=ganon_$(VERSION)_arm_release
	cmake --build build-arm
	@rm -rf build-arm

armd:
	@mkdir -p bin
	cmake -B build-armd -DCMAKE_BUILD_TYPE=Debug -DCMAKE_TOOLCHAIN_FILE=cmake/armv5-toolchain.cmake -DTARGET_NAME=ganon_$(VERSION)_arm_debug
	cmake --build build-armd
	@rm -rf build-armd

arm: armr armd

armv7r:
	@mkdir -p bin
	cmake -B build-armv7 -DCMAKE_BUILD_TYPE=Release -DCMAKE_TOOLCHAIN_FILE=cmake/armv7-toolchain.cmake -DTARGET_NAME=ganon_$(VERSION)_armv7_release
	cmake --build build-armv7
	@rm -rf build-armv7

armv7d:
	@mkdir -p bin
	cmake -B build-armv7d -DCMAKE_BUILD_TYPE=Debug -DCMAKE_TOOLCHAIN_FILE=cmake/armv7-toolchain.cmake -DTARGET_NAME=ganon_$(VERSION)_armv7_debug
	cmake --build build-armv7d
	@rm -rf build-armv7d

armv7: armv7r armv7d

mips32ber:
	@mkdir -p bin
	cmake -B build-mips32be -DCMAKE_BUILD_TYPE=Release -DCMAKE_TOOLCHAIN_FILE=cmake/mips32be-toolchain.cmake -DTARGET_NAME=ganon_$(VERSION)_mips32be_release
	cmake --build build-mips32be
	@rm -rf build-mips32be

mips32bed:
	@mkdir -p bin
	cmake -B build-mips32bed -DCMAKE_BUILD_TYPE=Debug -DCMAKE_TOOLCHAIN_FILE=cmake/mips32be-toolchain.cmake -DTARGET_NAME=ganon_$(VERSION)_mips32be_debug
	cmake --build build-mips32bed
	@rm -rf build-mips32bed

mips32be: mips32ber mips32bed

clean:
	rm -rf build-x64 build-x64d build-arm build-armd build-armv7 build-armv7d build-mips32be build-mips32bed
	rm -rf bin/*

clean-libsodium:
	rm -rf $(LIBSODIUM_INSTALL)
	cd $(LIBSODIUM_SRC) && $(MAKE) distclean 2>/dev/null || true