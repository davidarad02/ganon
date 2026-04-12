.PHONY: all clean x64 armv5 mips32be

all: x64 armv5 mips32be

x64:
	@mkdir -p bin
	cmake -B build-x64 -DTARGET_NAME=ganon_x64
	cmake --build build-x64

armv5:
	@mkdir -p bin
	cmake -B build-armv5 -DCMAKE_TOOLCHAIN_FILE=cmake/armv5-toolchain.cmake -DTARGET_NAME=ganon_armv5
	cmake --build build-armv5

mips32be:
	@mkdir -p bin
	cmake -B build-mips32be -DCMAKE_TOOLCHAIN_FILE=cmake/mips32be-toolchain.cmake -DTARGET_NAME=ganon_mips32be
	cmake --build build-mips32be

clean:
	rm -rf build-x64 build-armv5 build-mips32be bin