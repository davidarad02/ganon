if(NOT TARGET_NAME)
    set(TARGET_NAME "ganon")
endif()

add_executable(${TARGET_NAME} main.c)

target_include_directories(${TARGET_NAME} PUBLIC ${CMAKE_SOURCE_DIR}/include)