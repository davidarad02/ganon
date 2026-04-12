if(NOT TARGET_NAME)
    set(TARGET_NAME "ganon")
endif()

if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    set(LOGGING_SOURCES main.c logging.c args.c)
else()
    set(LOGGING_SOURCES main.c logging.c args.c)
endif()

add_executable(${TARGET_NAME} ${LOGGING_SOURCES})

target_include_directories(${TARGET_NAME} PUBLIC ${CMAKE_SOURCE_DIR}/include)

if(DEFINED OUTPUT_DIR)
    set_target_properties(${TARGET_NAME} PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY ${OUTPUT_DIR})
endif()