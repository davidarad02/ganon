if(NOT TARGET_NAME)
    set(TARGET_NAME "ganon")
endif()

add_executable(${TARGET_NAME} main.c)

target_include_directories(${TARGET_NAME} PUBLIC ${CMAKE_SOURCE_DIR}/include)

if(DEFINED OUTPUT_DIR)
    set_target_properties(${TARGET_NAME} PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY ${OUTPUT_DIR})
endif()