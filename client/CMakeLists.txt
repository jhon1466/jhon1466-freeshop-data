cmake_minimum_required(VERSION 3.15)

# Borealis (the UI framework pipensx renders with - see
# THIRD_PARTY_NOTICES.md) is CMake-only, so the client's own build moved
# from the devkitPro Makefile to CMake to be able to consume it. Borealis's
# toolchain.cmake has to be included *before* project(), since that is what
# pulls in $DEVKITPRO/cmake/Switch.cmake and points the compiler at the
# aarch64 cross toolchain. Same ordering pipensx's own CMakeLists uses.
set(BOREALIS_LIBRARY ${CMAKE_CURRENT_SOURCE_DIR}/vendor/borealis/library)

if(NOT EXISTS ${BOREALIS_LIBRARY}/CMakeLists.txt)
    message(FATAL_ERROR
        "Borealis is missing. It is vendored but not committed (see .gitignore):\n"
        "  git clone --depth 1 --branch moonlight_wiliwili --recurse-submodules \\\n"
        "    --shallow-submodules https://github.com/XITRIX/borealis.git client/vendor/borealis")
endif()

set(PLATFORM_SWITCH ON CACHE BOOL "" FORCE)
include(${BOREALIS_LIBRARY}/cmake/commonOption.cmake)
include(${BOREALIS_LIBRARY}/cmake/toolchain.cmake)

project(freeshop-client LANGUAGES C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_subdirectory(${BOREALIS_LIBRARY} borealis)

# ---- Application sources ----
#
# Everything under source/ except source/ui/ - that directory is the old
# hand-drawn SDL2 UI being replaced screen by screen by Borealis views
# under source/views/, and the two cannot both be linked (they define the
# same entry points and fight over the framebuffer). Sources are globbed
# rather than listed because the Makefile this replaces globbed too, and
# the file set churns constantly during the port.
file(GLOB_RECURSE FREESHOP_C_SRC CONFIGURE_DEPENDS
    ${CMAKE_CURRENT_SOURCE_DIR}/source/net/*.c
    ${CMAKE_CURRENT_SOURCE_DIR}/source/catalog/*.c
    ${CMAKE_CURRENT_SOURCE_DIR}/source/install/*.c
    ${CMAKE_CURRENT_SOURCE_DIR}/source/update/*.c
    ${CMAKE_CURRENT_SOURCE_DIR}/source/saves/*.c
    ${CMAKE_CURRENT_SOURCE_DIR}/source/mtp/*.c
    ${CMAKE_CURRENT_SOURCE_DIR}/source/ftp/*.c
    ${CMAKE_CURRENT_SOURCE_DIR}/source/torrent/*.c
    ${CMAKE_CURRENT_SOURCE_DIR}/source/torrent/dht/*.c
)
list(APPEND FREESHOP_C_SRC ${CMAKE_CURRENT_SOURCE_DIR}/source/i18n.c)

file(GLOB_RECURSE FREESHOP_CXX_SRC CONFIGURE_DEPENDS
    ${CMAKE_CURRENT_SOURCE_DIR}/source/views/*.cpp
)
list(APPEND FREESHOP_CXX_SRC ${CMAKE_CURRENT_SOURCE_DIR}/source/main.cpp)

add_executable(${PROJECT_NAME} ${FREESHOP_C_SRC} ${FREESHOP_CXX_SRC})

target_include_directories(${PROJECT_NAME} PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/source
)

# The same set the Makefile linked, minus SDL2/SDL2_ttf and their font
# stack (freetype/harfbuzz/bz2/png/z) - Borealis brings its own renderer
# and font handling. Kept in a link group for the same reason the Makefile
# used --start-group: these have circular static dependencies.
target_link_libraries(${PROJECT_NAME} PRIVATE
    borealis
    -Wl,--start-group
    curl mbedtls mbedx509 mbedcrypto
    jansson jpeg zstd miniupnpc
    -Wl,--end-group
)

target_compile_options(${PROJECT_NAME} PRIVATE -Wall -O2)

# ---- .nro packaging ----
#
# What the devkitPro Makefile's nacptool/elf2nro rules did, expressed
# through the toolchain's own helper so the outputs stay identical.
if(PLATFORM_SWITCH)
    add_custom_target(${PROJECT_NAME}.nro ALL
        DEPENDS ${PROJECT_NAME}
        COMMAND ${NX_NACPTOOL_EXE} --create "FreeShop" "jhon1466" "0.1.0"
                ${CMAKE_BINARY_DIR}/${PROJECT_NAME}.nacp
        COMMAND ${NX_ELF2NRO_EXE} ${CMAKE_BINARY_DIR}/${PROJECT_NAME}.elf
                ${CMAKE_BINARY_DIR}/${PROJECT_NAME}.nro
                --icon=${CMAKE_CURRENT_SOURCE_DIR}/icon.jpg
                --nacp=${CMAKE_BINARY_DIR}/${PROJECT_NAME}.nacp
                --romfsdir=${CMAKE_CURRENT_SOURCE_DIR}/romfs
    )
endif()
