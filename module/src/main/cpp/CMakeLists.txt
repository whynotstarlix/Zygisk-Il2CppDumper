cmake_minimum_required(VERSION 3.18.1)

if (NOT DEFINED MODULE_NAME)
    message(FATAL_ERROR "MODULE_NAME is not set")
else ()
    project(${MODULE_NAME})
endif ()

message("Build type: ${CMAKE_BUILD_TYPE}")

set(CMAKE_CXX_STANDARD 20)

# Отключаем генерацию shared-библиотеки Dobby
set(DOBBY_GENERATE_SHARED OFF CACHE BOOL "" FORCE)

include(FetchContent)
FetchContent_Declare(
    dobby
    GIT_REPOSITORY https://github.com/jmpews/Dobby.git
    GIT_TAG master
)

# --- РУЧНОЙ КОНТРОЛЬ СКАЧИВАНИЯ И ПАТЧИНГА ---
FetchContent_GetProperties(dobby)
if(NOT dobby_POPULATED)
    # 1. Скачиваем исходники
    FetchContent_Populate(dobby)

    # 2. Ищем все ассемблерные файлы Dobby
    file(GLOB_RECURSE ASM_FILES "${dobby_SOURCE_DIR}/*.asm")
    
    # 3. Патчим синтаксис Apple -> NDK (GNU AS) прямо в памяти
    foreach(ASM_FILE IN LISTS ASM_FILES)
        file(READ "${ASM_FILE}" ASM_CONTENT)
        
        # Переставляем модификатор младших битов перед именем переменной
        string(REGEX REPLACE "([a-zA-Z0-9_]+)@PAGEOFF" ":lo12:\\1" ASM_CONTENT "${ASM_CONTENT}")
        # Удаляем модификатор страницы
        string(REGEX REPLACE "([a-zA-Z0-9_]+)@PAGE" "\\1" ASM_CONTENT "${ASM_CONTENT}")
        
        file(WRITE "${ASM_FILE}" "${ASM_CONTENT}")
    endforeach()

    # 4. Только теперь подключаем исправленный проект к нашей сборке
    add_subdirectory(${dobby_SOURCE_DIR} ${dobby_BINARY_DIR})
endif()
# --------------------------------------------

include_directories(
        xdl/include
        ${dobby_SOURCE_DIR}/include
)

aux_source_directory(xdl xdl-src)

add_library(${MODULE_NAME} SHARED
        main.cpp
        hack.cpp
        il2cpp_dump.cpp
        ${xdl-src})

target_compile_options(${MODULE_NAME} PRIVATE
        -Werror=format
        -fdata-sections
        -ffunction-sections
        -fno-exceptions
        -fno-rtti
        $<$<NOT:$<CONFIG:Debug>>:-O2 -fvisibility=hidden -fvisibility-inlines-hidden>
        $<$<CONFIG:Debug>:-O0>
)

target_link_options(${MODULE_NAME} PRIVATE
        -ffixed-x18
        -Wl,--hash-style=both
        $<$<NOT:$<CONFIG:Debug>>:-Wl,-exclude-libs,ALL -Wl,--gc-sections -Wl,--strip-all>
)

target_link_libraries(${MODULE_NAME} log dobby)

if (NOT CMAKE_BUILD_TYPE STREQUAL "Debug")
    add_custom_command(TARGET ${MODULE_NAME} POST_BUILD
            COMMAND ${CMAKE_STRIP} --strip-all --remove-section=.comment "${CMAKE_LIBRARY_OUTPUT_DIRECTORY}/lib${MODULE_NAME}.so")
endif ()
