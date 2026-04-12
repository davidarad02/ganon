VERSION := $(shell cat VERSION)

.PHONY: all clean x64 x64d x64r armv5 armv5d armv5r mips32be mips32bed mips32ber

all: x64r x64d armv5r armv5d mips32ber mips32bed

x64r:
	@mkdir -p bin
	cmake -B build-x64 -DCMAKE_BUILD_TYPE=Release -DTARGET_NAME=ganon_$(VERSION)_x64
	cmake --build build-x64
	@rm -rf build-x64

x64d:
	@mkdir -p bin
	cmake -B build-x64d -DCMAKE_BUILD_TYPE=Debug -DTARGET_NAME=ganon_$(VERSION)_x64_debug
	cmake --build build-x64d
	@rm -rf build-x64d

x64: x64r x64d

armv5r:
	@mkdir -p bin
	cmake -B build-armv5 -DCMAKE_BUILD_TYPE=Release -DCMAKE_TOOLCHAIN_FILE=cmake/armv5-toolchain.cmake -DTARGET_NAME=ganon_$(VERSION)_armv5
	cmake --build build-armv5
	@rm -rf build-armv5

armv5d:
	@mkdir -p bin
	cmake -B build-armv5d -DCMAKE_BUILD_TYPE=Debug -DCMAKE_TOOLCHAIN_FILE=cmake/armv5-toolchain.cmake -DTARGET_NAME=ganon_$(VERSION)_armv5_debug
	cmake --build build-armv5d
	@rm -rf build-armv5d

armv5: armv5r armv5d

mips32ber:
	@mkdir -p bin
	cmake -B build-mips32be -DCMAKE_BUILD_TYPE=Release -DCMAKE_TOOLCHAIN_FILE=cmake/mips32be-toolchain.cmake -DTARGET_NAME=ganon_$(VERSION)_mips32be
	cmake --build build-mips32be
	@rm -rf build-mips32be

mips32bed:
	@mkdir -p bin
	cmake -B build-mips32bed -DCMAKE_BUILD_TYPE=Debug -DCMAKE_TOOLCHAIN_FILE=cmake/mips32be-toolchain.cmake -DTARGET_NAME=ganon_$(VERSION)_mips32be_debug
	cmake --build build-mips32bed
	@rm -rf build-mips32bed

mips32be: mips32ber mips32bed

clean:
	rm -rf build-x64 build-x64d build-armv5 build-armv5d build-mips32be build-mips32bed bin