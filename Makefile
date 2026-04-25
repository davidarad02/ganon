VERSION := $(shell cat VERSION)

# libsodium is auto-built from third_party source for x64 targets
LIBSODIUM_SRC := third_party/libsodium
LIBSODIUM_INSTALL := third_party/libsodium-install

# ngtcp2 + picotls (QUIC skin, x64 native only)
NGTCP2_SRC     := third_party/ngtcp2
NGTCP2_INSTALL := third_party/ngtcp2-install
PICOTLS_SRC    := third_party/picotls
PICOTLS_INSTALL := third_party/picotls-install

# libssh + mbedTLS (cross-target crypto backend)
LIBSSH_SRC     := third_party/libssh
LIBSSH_INSTALL := third_party/libssh-install

MBEDTLS_SRC          := third_party/mbedtls
MBEDTLS_INSTALL      := third_party/mbedtls-install
MBEDTLS_INSTALL_ARM  := third_party/mbedtls-install-arm
MBEDTLS_INSTALL_MIPS := third_party/mbedtls-install-mips32be

LIBSSH_INSTALL_ARM  := third_party/libssh-install-arm
LIBSSH_INSTALL_MIPS := third_party/libssh-install-mips32be

# musl libc — smaller static binary for cross targets
MUSL_SRC          := third_party/musl
MUSL_INSTALL_ARM  := third_party/musl-install-arm
MUSL_INSTALL_MIPS := third_party/musl-install-mips32be

# Size-optimisation flags applied to cross-compiled libraries
CROSS_SIZE_FLAGS := -Os -ffunction-sections -fdata-sections -D_FORTIFY_SOURCE=0

.PHONY: all clean \
        x64 x64d x64r \
        arm armd armr \
        armv7 armv7d armv7r \
        mips32be mips32bed mips32ber \
        release debug \
        libsodium clean-libsodium \
        libssh clean-libssh \
        picotls ngtcp2 clean-quic \
        mbedtls mbedtls-arm mbedtls-mips32be \
        libssh-arm libssh-mips32be \
        clean-cross-libs clean-musl

all: x64r x64d armr armd armv7r armv7d mips32ber mips32bed

release: x64r armr armv7r mips32ber

debug: x64d armd armv7d mips32bed

# ── x64 native dependencies ────────────────────────────────────────────────

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

mbedtls:
	@if [ ! -f "$(MBEDTLS_INSTALL)/lib/libmbedcrypto.a" ]; then \
		echo "Building mbedTLS (x64)..."; \
		cmake -B $(MBEDTLS_SRC)/build-x64 \
			-S $(MBEDTLS_SRC) \
			-DCMAKE_INSTALL_PREFIX=$(PWD)/$(MBEDTLS_INSTALL) \
			-DCMAKE_BUILD_TYPE=Release \
			-DCMAKE_C_FLAGS="-Os -ffunction-sections -fdata-sections" \
			-DENABLE_TESTING=OFF \
			-DENABLE_PROGRAMS=OFF \
			-DBUILD_SHARED_LIBS=OFF \
			-DMBEDTLS_FATAL_WARNINGS=OFF && \
		cmake --build $(MBEDTLS_SRC)/build-x64 -j$$(nproc) && \
		cmake --install $(MBEDTLS_SRC)/build-x64; \
	else \
		echo "mbedTLS already built at $(MBEDTLS_INSTALL)."; \
	fi

picotls: mbedtls
	@if [ ! -f "$(PICOTLS_INSTALL)/lib/libpicotls-mbedtls.a" ]; then \
		echo "Building picotls (x64, mbedTLS backend)..."; \
		cd $(PICOTLS_SRC) && git submodule update --init --recursive 2>/dev/null || true; \
		cmake -B build-x64 \
			-S . \
			-DCMAKE_BUILD_TYPE=Release \
			-DCMAKE_C_FLAGS="-Os -ffunction-sections -fdata-sections" \
			-DWITH_DTRACE=OFF \
			-DWITH_OPENSSL=OFF \
			-DWITH_MBEDTLS=ON \
			-DMBEDTLS_INCLUDE_DIRS=$(PWD)/$(MBEDTLS_INSTALL)/include \
			-DMBEDTLS_LIBRARY=$(PWD)/$(MBEDTLS_INSTALL)/lib/libmbedtls.a \
			-DMBEDTLS_CRYPTO=$(PWD)/$(MBEDTLS_INSTALL)/lib/libmbedcrypto.a \
			-DMBEDTLS_X509=$(PWD)/$(MBEDTLS_INSTALL)/lib/libmbedx509.a && \
		cmake --build build-x64 --target picotls-core picotls-mbedtls -j$$(nproc) && \
		mkdir -p $(PWD)/$(PICOTLS_INSTALL)/lib $(PWD)/$(PICOTLS_INSTALL)/include/picotls && \
		cp build-x64/libpicotls-core.a $(PWD)/$(PICOTLS_INSTALL)/lib/ && \
		cp build-x64/libpicotls-mbedtls.a $(PWD)/$(PICOTLS_INSTALL)/lib/ && \
		cp include/picotls.h $(PWD)/$(PICOTLS_INSTALL)/include/ && \
		cp include/picotls/*.h $(PWD)/$(PICOTLS_INSTALL)/include/picotls/; \
	else \
		echo "picotls already built at $(PICOTLS_INSTALL)."; \
	fi

ngtcp2: picotls
	@if [ ! -f "$(NGTCP2_INSTALL)/lib/libngtcp2.a" ]; then \
		echo "Building ngtcp2 (x64, picotls-mbedtls backend)..."; \
		cmake -B $(NGTCP2_SRC)/build-x64 \
			-S $(NGTCP2_SRC) \
			-DCMAKE_BUILD_TYPE=Release \
			-DCMAKE_C_FLAGS="-Os -ffunction-sections -fdata-sections" \
			-DCMAKE_INSTALL_PREFIX=$(PWD)/$(NGTCP2_INSTALL) \
			-DENABLE_SHARED_LIB=OFF \
			-DENABLE_STATIC_LIB=ON \
			-DBUILD_TESTING=OFF \
			-DENABLE_PICOTLS=ON \
			-DPICOTLS_INCLUDE_DIR=$(PWD)/$(PICOTLS_INSTALL)/include \
			"-DPICOTLS_LIBRARIES=$(PWD)/$(PICOTLS_INSTALL)/lib/libpicotls-mbedtls.a;$(PWD)/$(PICOTLS_INSTALL)/lib/libpicotls-core.a;$(PWD)/$(MBEDTLS_INSTALL)/lib/libmbedcrypto.a" \
			-DENABLE_GNUTLS=OFF \
			-DENABLE_OPENSSL=OFF \
			-DENABLE_WOLFSSL=OFF && \
		cmake --build $(NGTCP2_SRC)/build-x64 -j$$(nproc) && \
		cmake --install $(NGTCP2_SRC)/build-x64; \
	else \
		echo "ngtcp2 already built at $(NGTCP2_INSTALL)."; \
	fi

libssh:
	@if [ ! -f "$(LIBSSH_INSTALL)/lib/libssh.a" ]; then \
		echo "Building libssh (x64) from $(LIBSSH_SRC)..."; \
		cmake -B $(LIBSSH_SRC)/build \
			-S $(LIBSSH_SRC) \
			-DCMAKE_INSTALL_PREFIX=$(PWD)/$(LIBSSH_INSTALL) \
			-DBUILD_STATIC_LIB=ON \
			-DBUILD_SHARED_LIBS=OFF \
			-DWITH_SFTP=OFF \
			-DWITH_EXAMPLES=OFF \
			-DWITH_BENCHMARKS=OFF \
			-DWITH_PCAP=OFF \
			-DWITH_ZLIB=OFF \
			-DWITH_GSSAPI=OFF \
			-DWITH_SERVER=ON \
			-DWITH_DEBUG_CRYPTO=OFF \
			-DWITH_DEBUG_PACKET=OFF \
			-DWITH_DEBUG_CALLTRACE=OFF \
			-DCMAKE_BUILD_TYPE=Release && \
		cmake --build $(LIBSSH_SRC)/build -j$$(nproc) && \
		cmake --install $(LIBSSH_SRC)/build; \
	else \
		echo "libssh (x64) already built at $(LIBSSH_INSTALL)."; \
	fi

# ── cross-target dependencies: musl libc ──────────────────────────────────

musl-arm:
	@if [ ! -f "$(MUSL_INSTALL_ARM)/lib/libc.a" ]; then \
		echo "Building musl libc for ARM..."; \
		mkdir -p $(MUSL_SRC)/build-arm && \
		cd $(MUSL_SRC)/build-arm && \
		CC=arm-linux-gnueabihf-gcc CFLAGS="-Os" \
		../configure \
			--target=arm-linux-gnueabihf \
			--prefix=$(PWD)/$(MUSL_INSTALL_ARM) \
			--disable-shared \
			--enable-optimize=size && \
		$(MAKE) -j$$(nproc) && \
		$(MAKE) install && \
		sed -i \
		  -e 's/%rename cpp_options old_cpp_options/%rename cpp_options musl_old_cpp/' \
		  -e 's/%%(old_cpp_options)/%(musl_old_cpp)/g' \
		  -e 's/ %(old_cpp_options)/ %(musl_old_cpp)/g' \
		  $(PWD)/$(MUSL_INSTALL_ARM)/lib/musl-gcc.specs; \
	else \
		echo "musl (arm) already built at $(MUSL_INSTALL_ARM)."; \
	fi

musl-mips32be:
	@if [ ! -f "$(MUSL_INSTALL_MIPS)/lib/libc.a" ]; then \
		echo "Building musl libc for MIPS32BE..."; \
		mkdir -p $(MUSL_SRC)/build-mips32be && \
		cd $(MUSL_SRC)/build-mips32be && \
		CC=mips-linux-gnu-gcc CFLAGS="-Os -EB" \
		../configure \
			--target=mips-linux-gnu \
			--prefix=$(PWD)/$(MUSL_INSTALL_MIPS) \
			--disable-shared \
			--enable-optimize=size && \
		$(MAKE) -j$$(nproc) && \
		$(MAKE) install && \
		sed -i \
		  -e 's/%rename cpp_options old_cpp_options/%rename cpp_options musl_old_cpp/' \
		  -e 's/%%(old_cpp_options)/%(musl_old_cpp)/g' \
		  -e 's/ %(old_cpp_options)/ %(musl_old_cpp)/g' \
		  $(PWD)/$(MUSL_INSTALL_MIPS)/lib/musl-gcc.specs; \
	else \
		echo "musl (mips32be) already built at $(MUSL_INSTALL_MIPS)."; \
	fi

# ── cross-target dependencies: mbedTLS ────────────────────────────────────
# Compiled with -Os -ffunction-sections so --gc-sections can dead-strip
# unused crypto from the final ganon binary.

mbedtls-arm: musl-arm
	@if [ ! -f "$(MBEDTLS_INSTALL_ARM)/lib/libmbedcrypto.a" ]; then \
		echo "Building mbedTLS for ARM..."; \
		cmake -B $(MBEDTLS_SRC)/build-arm \
			-S $(MBEDTLS_SRC) \
			-DCMAKE_SYSTEM_NAME=Linux \
			-DCMAKE_SYSTEM_PROCESSOR=arm \
			-DCMAKE_C_COMPILER=arm-linux-gnueabihf-gcc \
			-DCMAKE_AR=/usr/bin/arm-linux-gnueabihf-ar \
			-DCMAKE_RANLIB=/usr/bin/arm-linux-gnueabihf-ranlib \
			"-DCMAKE_C_FLAGS=-specs=$(PWD)/$(MUSL_INSTALL_ARM)/lib/musl-gcc.specs $(CROSS_SIZE_FLAGS)" \
			-DCMAKE_INSTALL_PREFIX=$(PWD)/$(MBEDTLS_INSTALL_ARM) \
			-DCMAKE_BUILD_TYPE=Release \
			-DENABLE_TESTING=OFF \
			-DENABLE_PROGRAMS=OFF \
			-DBUILD_SHARED_LIBS=OFF \
			-DMBEDTLS_FATAL_WARNINGS=OFF && \
		cmake --build $(MBEDTLS_SRC)/build-arm -j$$(nproc) && \
		cmake --install $(MBEDTLS_SRC)/build-arm; \
	else \
		echo "mbedTLS (arm) already built at $(MBEDTLS_INSTALL_ARM)."; \
	fi

mbedtls-mips32be: musl-mips32be
	@if [ ! -f "$(MBEDTLS_INSTALL_MIPS)/lib/libmbedcrypto.a" ]; then \
		echo "Building mbedTLS for MIPS32BE..."; \
		cmake -B $(MBEDTLS_SRC)/build-mips32be \
			-S $(MBEDTLS_SRC) \
			-DCMAKE_SYSTEM_NAME=Linux \
			-DCMAKE_SYSTEM_PROCESSOR=mips \
			-DCMAKE_C_COMPILER=mips-linux-gnu-gcc \
			-DCMAKE_AR=/usr/bin/mips-linux-gnu-ar \
			-DCMAKE_RANLIB=/usr/bin/mips-linux-gnu-ranlib \
			"-DCMAKE_C_FLAGS=-specs=$(PWD)/$(MUSL_INSTALL_MIPS)/lib/musl-gcc.specs -EB $(CROSS_SIZE_FLAGS)" \
			-DCMAKE_INSTALL_PREFIX=$(PWD)/$(MBEDTLS_INSTALL_MIPS) \
			-DCMAKE_BUILD_TYPE=Release \
			-DENABLE_TESTING=OFF \
			-DENABLE_PROGRAMS=OFF \
			-DBUILD_SHARED_LIBS=OFF \
			-DMBEDTLS_FATAL_WARNINGS=OFF && \
		cmake --build $(MBEDTLS_SRC)/build-mips32be -j$$(nproc) && \
		cmake --install $(MBEDTLS_SRC)/build-mips32be; \
	else \
		echo "mbedTLS (mips32be) already built at $(MBEDTLS_INSTALL_MIPS)."; \
	fi

# ── cross-target dependencies: libssh ─────────────────────────────────────
# SFTP, debug tracing, and benchmarks disabled to shrink the library.
# Compiled with -ffunction-sections so --gc-sections removes unused code.

libssh-arm: mbedtls-arm
	@if [ ! -f "$(LIBSSH_INSTALL_ARM)/lib/libssh.a" ]; then \
		echo "Building libssh for ARM..."; \
		cmake -B $(LIBSSH_SRC)/build-arm \
			-S $(LIBSSH_SRC) \
			-DCMAKE_SYSTEM_NAME=Linux \
			-DCMAKE_SYSTEM_PROCESSOR=arm \
			-DCMAKE_C_COMPILER=arm-linux-gnueabihf-gcc \
			-DCMAKE_AR=/usr/bin/arm-linux-gnueabihf-ar \
			-DCMAKE_RANLIB=/usr/bin/arm-linux-gnueabihf-ranlib \
			"-DCMAKE_C_FLAGS=$(CROSS_SIZE_FLAGS)" \
			-DCMAKE_INSTALL_PREFIX=$(PWD)/$(LIBSSH_INSTALL_ARM) \
			-DCMAKE_BUILD_TYPE=Release \
			-DBUILD_STATIC_LIB=ON \
			-DBUILD_SHARED_LIBS=OFF \
			-DWITH_MBEDTLS=ON \
			-DMBEDTLS_ROOT_DIR=$(PWD)/$(MBEDTLS_INSTALL_ARM) \
			-DWITH_SFTP=OFF \
			-DWITH_EXAMPLES=OFF \
			-DWITH_BENCHMARKS=OFF \
			-DWITH_PCAP=OFF \
			-DWITH_ZLIB=OFF \
			-DWITH_GSSAPI=OFF \
			-DWITH_SERVER=ON \
			-DWITH_DEBUG_CRYPTO=OFF \
			-DWITH_DEBUG_PACKET=OFF \
			-DWITH_DEBUG_CALLTRACE=OFF && \
		cmake --build $(LIBSSH_SRC)/build-arm -j$$(nproc) && \
		cmake --install $(LIBSSH_SRC)/build-arm; \
	else \
		echo "libssh (arm) already built at $(LIBSSH_INSTALL_ARM)."; \
	fi

libssh-mips32be: mbedtls-mips32be
	@if [ ! -f "$(LIBSSH_INSTALL_MIPS)/lib/libssh.a" ]; then \
		echo "Building libssh for MIPS32BE..."; \
		cmake -B $(LIBSSH_SRC)/build-mips32be \
			-S $(LIBSSH_SRC) \
			-DCMAKE_SYSTEM_NAME=Linux \
			-DCMAKE_SYSTEM_PROCESSOR=mips \
			-DCMAKE_C_COMPILER=mips-linux-gnu-gcc \
			-DCMAKE_AR=/usr/bin/mips-linux-gnu-ar \
			-DCMAKE_RANLIB=/usr/bin/mips-linux-gnu-ranlib \
			"-DCMAKE_C_FLAGS=-EB $(CROSS_SIZE_FLAGS)" \
			-DCMAKE_INSTALL_PREFIX=$(PWD)/$(LIBSSH_INSTALL_MIPS) \
			-DCMAKE_BUILD_TYPE=Release \
			-DBUILD_STATIC_LIB=ON \
			-DBUILD_SHARED_LIBS=OFF \
			-DWITH_MBEDTLS=ON \
			-DMBEDTLS_ROOT_DIR=$(PWD)/$(MBEDTLS_INSTALL_MIPS) \
			-DWITH_SFTP=OFF \
			-DWITH_EXAMPLES=OFF \
			-DWITH_BENCHMARKS=OFF \
			-DWITH_PCAP=OFF \
			-DWITH_ZLIB=OFF \
			-DWITH_GSSAPI=OFF \
			-DWITH_SERVER=ON \
			-DWITH_DEBUG_CRYPTO=OFF \
			-DWITH_DEBUG_PACKET=OFF \
			-DWITH_DEBUG_CALLTRACE=OFF && \
		cmake --build $(LIBSSH_SRC)/build-mips32be -j$$(nproc) && \
		cmake --install $(LIBSSH_SRC)/build-mips32be; \
	else \
		echo "libssh (mips32be) already built at $(LIBSSH_INSTALL_MIPS)."; \
	fi

# ── cross-target dependencies: picotls + ngtcp2 (QUIC) ────────────────────

picotls-arm: mbedtls-arm
	@if [ ! -f "$(PICOTLS_INSTALL)/lib/libpicotls-mbedtls-arm.a" ]; then \
		echo "Building picotls for ARM..."; \
		cmake -B build-arm \
			-S . \
			-DCMAKE_SYSTEM_NAME=Linux \
			-DCMAKE_SYSTEM_PROCESSOR=arm \
			-DCMAKE_C_COMPILER=arm-linux-gnueabihf-gcc \
			-DCMAKE_AR=/usr/bin/arm-linux-gnueabihf-ar \
			-DCMAKE_RANLIB=/usr/bin/arm-linux-gnueabihf-ranlib \
			"-DCMAKE_C_FLAGS=$(CROSS_SIZE_FLAGS)" \
			-DCMAKE_BUILD_TYPE=Release \
			-DWITH_DTRACE=OFF \
			-DWITH_OPENSSL=OFF \
			-DWITH_MBEDTLS=ON \
			-DMBEDTLS_INCLUDE_DIRS=$(PWD)/$(MBEDTLS_INSTALL_ARM)/include \
			"-DMBEDTLS_LIBRARIES=$(PWD)/$(MBEDTLS_INSTALL_ARM)/lib/libmbedtls.a;$(PWD)/$(MBEDTLS_INSTALL_ARM)/lib/libmbedcrypto.a;$(PWD)/$(MBEDTLS_INSTALL_ARM)/lib/libmbedx509.a" && \

		cmake --build $(PICOTLS_SRC)/build-arm --target picotls-core picotls-mbedtls -j$$(nproc) && \
		mkdir -p $(PICOTLS_INSTALL)/lib && \
		cp $(PICOTLS_SRC)/build-arm/libpicotls-core.a $(PICOTLS_INSTALL)/lib/libpicotls-core-arm.a && \
		cp $(PICOTLS_SRC)/build-arm/libpicotls-mbedtls.a $(PICOTLS_INSTALL)/lib/libpicotls-mbedtls-arm.a; \
	else \
		echo "picotls (arm) already built."; \
	fi

ngtcp2-arm: picotls-arm
	@if [ ! -f "$(NGTCP2_INSTALL)/lib/libngtcp2-arm.a" ]; then \
		echo "Building ngtcp2 for ARM..."; \
		cmake -B $(NGTCP2_SRC)/build-arm \
			-S $(NGTCP2_SRC) \
			-DCMAKE_SYSTEM_NAME=Linux \
			-DCMAKE_SYSTEM_PROCESSOR=arm \
			-DCMAKE_C_COMPILER=arm-linux-gnueabihf-gcc \
			-DCMAKE_AR=/usr/bin/arm-linux-gnueabihf-ar \
			-DCMAKE_RANLIB=/usr/bin/arm-linux-gnueabihf-ranlib \
			"-DCMAKE_C_FLAGS=$(CROSS_SIZE_FLAGS)" \
			-DCMAKE_BUILD_TYPE=Release \
			-DCMAKE_INSTALL_PREFIX=$(PWD)/$(NGTCP2_INSTALL) \
			-DENABLE_SHARED_LIB=OFF \
			-DENABLE_STATIC_LIB=ON \
			-DBUILD_TESTING=OFF \
			-DENABLE_PICOTLS=ON \
			-DPICOTLS_INCLUDE_DIR=$(PWD)/$(PICOTLS_INSTALL)/include \
			"-DPICOTLS_LIBRARIES=$(PWD)/$(PICOTLS_INSTALL)/lib/libpicotls-mbedtls-arm.a;$(PWD)/$(PICOTLS_INSTALL)/lib/libpicotls-core-arm.a;$(PWD)/$(MBEDTLS_INSTALL_ARM)/lib/libmbedcrypto.a" \
			-DENABLE_GNUTLS=OFF \
			-DENABLE_OPENSSL=OFF \
			-DENABLE_WOLFSSL=OFF && \
		cmake --build $(NGTCP2_SRC)/build-arm -j$$(nproc) && \
		cp $(NGTCP2_SRC)/build-arm/lib/libngtcp2.a $(NGTCP2_INSTALL)/lib/libngtcp2-arm.a; \
	else \
		echo "ngtcp2 (arm) already built."; \
	fi

picotls-mips32be: mbedtls-mips32be
	@if [ ! -f "$(PICOTLS_INSTALL)/lib/libpicotls-mbedtls-mips32be.a" ]; then \
		echo "Building picotls for MIPS32BE..."; \
		cmake -B build-mips32be \
			-S . \
			-DCMAKE_SYSTEM_NAME=Linux \
			-DCMAKE_SYSTEM_PROCESSOR=mips \
			-DCMAKE_C_COMPILER=mips-linux-gnu-gcc \
			-DCMAKE_AR=/usr/bin/mips-linux-gnu-ar \
			-DCMAKE_RANLIB=/usr/bin/mips-linux-gnu-ranlib \
			"-DCMAKE_C_FLAGS=-EB $(CROSS_SIZE_FLAGS)" \
			-DCMAKE_BUILD_TYPE=Release \
			-DWITH_DTRACE=OFF \
			-DWITH_OPENSSL=OFF \
			-DWITH_MBEDTLS=ON \
			-DMBEDTLS_INCLUDE_DIRS=$(PWD)/$(MBEDTLS_INSTALL_MIPS)/include \
			"-DMBEDTLS_LIBRARIES=$(PWD)/$(MBEDTLS_INSTALL_MIPS)/lib/libmbedtls.a;$(PWD)/$(MBEDTLS_INSTALL_MIPS)/lib/libmbedcrypto.a;$(PWD)/$(MBEDTLS_INSTALL_MIPS)/lib/libmbedx509.a" && \

		cmake --build $(PICOTLS_SRC)/build-mips32be --target picotls-core picotls-mbedtls -j$$(nproc) && \
		mkdir -p $(PICOTLS_INSTALL)/lib && \
		cp $(PICOTLS_SRC)/build-mips32be/libpicotls-core.a $(PICOTLS_INSTALL)/lib/libpicotls-core-mips32be.a && \
		cp $(PICOTLS_SRC)/build-mips32be/libpicotls-mbedtls.a $(PICOTLS_INSTALL)/lib/libpicotls-mbedtls-mips32be.a; \
	else \
		echo "picotls (mips32be) already built."; \
	fi

ngtcp2-mips32be: picotls-mips32be
	@if [ ! -f "$(NGTCP2_INSTALL)/lib/libngtcp2-mips32be.a" ]; then \
		echo "Building ngtcp2 for MIPS32BE..."; \
		cmake -B $(NGTCP2_SRC)/build-mips32be \
			-S $(NGTCP2_SRC) \
			-DCMAKE_SYSTEM_NAME=Linux \
			-DCMAKE_SYSTEM_PROCESSOR=mips \
			-DCMAKE_C_COMPILER=mips-linux-gnu-gcc \
			-DCMAKE_AR=/usr/bin/mips-linux-gnu-ar \
			-DCMAKE_RANLIB=/usr/bin/mips-linux-gnu-ranlib \
			"-DCMAKE_C_FLAGS=-EB $(CROSS_SIZE_FLAGS)" \
			-DCMAKE_BUILD_TYPE=Release \
			-DCMAKE_INSTALL_PREFIX=$(PWD)/$(NGTCP2_INSTALL) \
			-DENABLE_SHARED_LIB=OFF \
			-DENABLE_STATIC_LIB=ON \
			-DBUILD_TESTING=OFF \
			-DENABLE_PICOTLS=ON \
			-DPICOTLS_INCLUDE_DIR=$(PWD)/$(PICOTLS_INSTALL)/include \
			"-DPICOTLS_LIBRARIES=$(PWD)/$(PICOTLS_INSTALL)/lib/libpicotls-mbedtls-mips32be.a;$(PWD)/$(PICOTLS_INSTALL)/lib/libpicotls-core-mips32be.a;$(PWD)/$(MBEDTLS_INSTALL_MIPS)/lib/libmbedcrypto.a" \
			-DENABLE_GNUTLS=OFF \
			-DENABLE_OPENSSL=OFF \
			-DENABLE_WOLFSSL=OFF && \
		cmake --build $(NGTCP2_SRC)/build-mips32be -j$$(nproc) && \
		cp $(NGTCP2_SRC)/build-mips32be/lib/libngtcp2.a $(NGTCP2_INSTALL)/lib/libngtcp2-mips32be.a; \
	else \
		echo "ngtcp2 (mips32be) already built."; \
	fi

# ── ganon targets ─────────────────────────────────────────────────────────

x64r: libsodium libssh ngtcp2
	@mkdir -p bin
	cmake -B build-x64 -DCMAKE_BUILD_TYPE=Release -DTARGET_NAME=ganon_$(VERSION)_x64_release
	cmake --build build-x64
	@rm -rf build-x64

x64d: libsodium libssh ngtcp2
	@mkdir -p bin
	cmake -B build-x64d -DCMAKE_BUILD_TYPE=Debug -DTARGET_NAME=ganon_$(VERSION)_x64_debug
	cmake --build build-x64d
	@rm -rf build-x64d

x64: x64r x64d

armr: libssh-arm ngtcp2-arm
	@mkdir -p bin
	cmake -B build-arm -DCMAKE_BUILD_TYPE=Release -DCMAKE_TOOLCHAIN_FILE=cmake/armv5-musl-toolchain.cmake -DTARGET_NAME=ganon_$(VERSION)_arm_release
	cmake --build build-arm
	@rm -rf build-arm

armd: libssh-arm ngtcp2-arm
	@mkdir -p bin
	cmake -B build-armd -DCMAKE_BUILD_TYPE=Debug -DCMAKE_TOOLCHAIN_FILE=cmake/armv5-musl-toolchain.cmake -DTARGET_NAME=ganon_$(VERSION)_arm_debug
	cmake --build build-armd
	@rm -rf build-armd

arm: armr armd

armv7r: libssh-arm ngtcp2-arm
	@mkdir -p bin
	cmake -B build-armv7 -DCMAKE_BUILD_TYPE=Release -DCMAKE_TOOLCHAIN_FILE=cmake/armv7-musl-toolchain.cmake -DTARGET_NAME=ganon_$(VERSION)_armv7_release
	cmake --build build-armv7
	@rm -rf build-armv7

armv7d: libssh-arm ngtcp2-arm
	@mkdir -p bin
	cmake -B build-armv7d -DCMAKE_BUILD_TYPE=Debug -DCMAKE_TOOLCHAIN_FILE=cmake/armv7-musl-toolchain.cmake -DTARGET_NAME=ganon_$(VERSION)_armv7_debug
	cmake --build build-armv7d
	@rm -rf build-armv7d

armv7: armv7r armv7d

mips32ber: libssh-mips32be ngtcp2-mips32be
	@mkdir -p bin
	cmake -B build-mips32be -DCMAKE_BUILD_TYPE=Release -DCMAKE_TOOLCHAIN_FILE=cmake/mips32be-musl-toolchain.cmake -DTARGET_NAME=ganon_$(VERSION)_mips32be_release
	cmake --build build-mips32be
	@rm -rf build-mips32be

mips32bed: libssh-mips32be ngtcp2-mips32be
	@mkdir -p bin
	cmake -B build-mips32bed -DCMAKE_BUILD_TYPE=Debug -DCMAKE_TOOLCHAIN_FILE=cmake/mips32be-musl-toolchain.cmake -DTARGET_NAME=ganon_$(VERSION)_mips32be_debug
	cmake --build build-mips32bed
	@rm -rf build-mips32bed

mips32be: mips32ber mips32bed

# ── clean ─────────────────────────────────────────────────────────────────

clean:
	rm -rf build-x64 build-x64d build-arm build-armd build-armv7 build-armv7d build-mips32be build-mips32bed
	rm -rf bin/*

clean-libsodium:
	rm -rf $(LIBSODIUM_INSTALL)
	cd $(LIBSODIUM_SRC) && $(MAKE) distclean 2>/dev/null || true

clean-libssh:
	rm -rf $(LIBSSH_INSTALL) $(LIBSSH_SRC)/build

clean-musl:
	rm -rf $(MUSL_INSTALL_ARM) $(MUSL_SRC)/build-arm
	rm -rf $(MUSL_INSTALL_MIPS) $(MUSL_SRC)/build-mips32be

clean-quic:
	rm -rf $(PICOTLS_INSTALL) $(PICOTLS_SRC)/build-x64
	rm -rf $(NGTCP2_INSTALL) $(NGTCP2_SRC)/build-x64

clean-cross-libs:
	rm -rf $(MBEDTLS_INSTALL_ARM) $(MBEDTLS_SRC)/build-arm
	rm -rf $(MBEDTLS_INSTALL_MIPS) $(MBEDTLS_SRC)/build-mips32be
	rm -rf $(LIBSSH_INSTALL_ARM) $(LIBSSH_SRC)/build-arm
	rm -rf $(LIBSSH_INSTALL_MIPS) $(LIBSSH_SRC)/build-mips32be
