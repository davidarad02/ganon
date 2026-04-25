set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR mips)

set(CMAKE_C_COMPILER mips-linux-gnu-gcc)
set(CMAKE_AR /usr/bin/mips-linux-gnu-ar CACHE FILEPATH "")
set(CMAKE_RANLIB /usr/bin/mips-linux-gnu-ranlib CACHE FILEPATH "")

set(_MUSL_SPECS "${CMAKE_CURRENT_LIST_DIR}/../third_party/musl-install-mips32be/lib/musl-gcc.specs")
set(CMAKE_C_FLAGS "-specs=${_MUSL_SPECS} -EB -static" CACHE STRING "")
set(CMAKE_EXE_LINKER_FLAGS "-EB -static" CACHE STRING "")

set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE NEVER)
