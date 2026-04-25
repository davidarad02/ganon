set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR arm)

set(CMAKE_C_COMPILER arm-linux-gnueabihf-gcc)
set(CMAKE_AR /usr/bin/arm-linux-gnueabihf-ar CACHE FILEPATH "")
set(CMAKE_RANLIB /usr/bin/arm-linux-gnueabihf-ranlib CACHE FILEPATH "")

set(_MUSL_SPECS "${CMAKE_CURRENT_LIST_DIR}/../third_party/musl-install-arm/lib/musl-gcc.specs")
set(CMAKE_C_FLAGS "-specs=${_MUSL_SPECS} -static" CACHE STRING "")
set(CMAKE_EXE_LINKER_FLAGS "-static" CACHE STRING "")

set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE NEVER)
