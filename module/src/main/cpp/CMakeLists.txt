cmake_minimum_required(VERSION 3.18.1)

if (NOT DEFINED MODULE_NAME)
    message(FATAL_ERROR "MODULE_NAME is not set")
else ()
    project(${MODULE_NAME})
endif ()

message("Build type: ${CMAKE_BUILD_TYPE}")

set(CMAKE_CXX_STANDARD 20)

# === Настройки Dobby (LSPosed fork) ===
set(DOBBY_GENERATE_SHARED OFF CACHE BOOL "" FORCE)   # только статическая библиотека
set(DOBBY_DEBUG OFF CACHE BOOL "" FORCE)
set(NearBranch ON CACHE BOOL "" FORCE)
set(Plugin.SymbolResolver ON CACHE BOOL "" FORCE)
set(Plugin.ImportTableReplace OFF CACHE BOOL "" FORCE)
set(Plugin.Android.BionicLinkerUtil OFF CACHE BOOL "" FORCE)
set(BUILD_EXAMPLE OFF CACHE BOOL "" FORCE)
set(BUILD_TEST OFF CACHE BOOL "" FORCE)

include(FetchContent)
FetchContent_Declare(
    dobby
    GIT_REPOSITORY https://github.com/LSPosed/Dobby.git
    GIT_TAG        master
)

FetchContent_GetProperties(dobby)
if(NOT dobby_POPULATED)
    FetchContent_Populate(dobby)
    add_subdirectory(${dobby_SOURCE_DIR} ${dobby_BINARY_DIR})
endif()

include_directories(
    xdl/include
    ${dobby_SOURCE_DIR}/include
)

aux_source_directory(xdl xdl-src)

add_library(${MODULE_NAME} SHARED
    main.cpp
    hack.cpp
    il2cpp_dump.cpp
    ${xdl-src}
)

target_compile_options(${MODULE_NAME} PRIVATE
    -Werror=format
    -fdata-sections
    -ffunction-sections
    -fno-exceptions
    -fno-rtti
    \( < \)<NOT:$<CONFIG:Debug>>:-O2 -fvisibility=hidden -fvisibility-inlines-hidden>
    \( < \)<CONFIG:Debug>:-O0>
)

target_link_options(${MODULE_NAME} PRIVATE
    -ffixed-x18
    -Wl,--hash-style=both
    \( < \)<NOT:$<CONFIG:Debug>>:-Wl,-exclude-libs,ALL -Wl,--gc-sections -Wl,--strip-all>
)

# Линкуем статическую версию dobby
target_link_libraries(${MODULE_NAME}
    log
    dobby
)

if (NOT CMAKE_BUILD_TYPE STREQUAL "Debug")
    add_custom_command(TARGET ${MODULE_NAME} POST_BUILD
        COMMAND ${CMAKE_STRIP} --strip-all --remove-section=.comment
                "\( {CMAKE_LIBRARY_OUTPUT_DIRECTORY}/lib \){MODULE_NAME}.so")
endif ()
