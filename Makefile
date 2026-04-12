VERSION := $(shell cat VERSION)

.PHONY: all clean x64 x64d x64r arm armd armr mips32be mips32bed mips32ber release debug

all: x64r x64d armr armd mips32ber mips32bed

release: x64r armr mips32ber

debug: x64d armd mips32bed

x64r:
	@mkdir -p bin
	cmake -B build-x64 -DCMAKE_BUILD_TYPE=Release -DTARGET_NAME=ganon_$(VERSION)_x64_release
	cmake --build build-x64
	@rm -rf build-x64

x64d:
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
	rm -rf build-x64 build-x64d build-arm build-armd build-mips32be build-mips32bed
	rm -rf bin/*