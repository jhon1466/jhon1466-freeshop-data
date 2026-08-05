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

# Borealis's own libnx entry hooks. Its CMakeLists globs the Switch platform
# directory for "*.cpp" only, and this one file is C - so userAppInit() and
# userAppExit() silently never make it into the library, libnx falls back to
# its empty defaults, and nothing brings up romfs / setsys / set / psm /
# nifm / pl / hidsys. SwitchPlatform's constructor then calls
# setsysGetColorSetId() against an uninitialised service and takes a data
# abort on a null pointer before a single frame is drawn. Compiling it into
# the application instead of patching vendored code keeps the vendor tree
# pristine and re-cloneable.
if(PLATFORM_SWITCH)
    list(APPEND FREESHOP_C_SRC ${BOREALIS_LIBRARY}/lib/platforms/switch/switch_wrapper.c)
endif()

file(GLOB_RECURSE FREESHOP_CXX_SRC CONFIGURE_DEPENDS
    ${CMAKE_CURRENT_SOURCE_DIR}/source/views/*.cpp
)
list(APPEND FREESHOP_CXX_SRC ${CMAKE_CURRENT_SOURCE_DIR}/source/main.cpp)

add_executable(${PROJECT_NAME} ${FREESHOP_C_SRC} ${FREESHOP_CXX_SRC})

target_include_directories(${PROJECT_NAME} PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/source
)

# The same set the Makefile linked, minus SDL2/SDL2_ttf and the font-shaping
# half of their stack (freetype/harfbuzz/png) - Borealis brings its own
# renderer and font handling. z stays: curl's content-encoding and our own
# zip_extract.c both call zlib directly, independent of SDL2_ttf's font
# stack that pulled it in before. Kept in a link group for the same reason
# the Makefile used --start-group: these have circular static dependencies.
target_link_libraries(${PROJECT_NAME} PRIVATE
    borealis
    -Wl,--start-group
    curl mbedtls mbedx509 mbedcrypto
    jansson jpeg zstd miniupnpc z
    -Wl,--end-group
)

target_compile_options(${PROJECT_NAME} PRIVATE -Wall -O2)

# ---- .nro packaging ----
#
# What the devkitPro Makefile's nacptool/elf2nro rules did, expressed
# through the toolchain's own helper so the outputs stay identical.
if(PLATFORM_SWITCH)
    # Borealis is compiled with BRLS_RESOURCES="romfs:/" on Switch (see its
    # CMakeLists), so its font, XML layouts, material icons and i18n files
    # must sit at the *root* of the .nro's romfs - not in a subfolder.
    # Shipping only our own romfs/ meant every asset load failed and the app
    # died during Application::init(). This stages a merged romfs (Borealis's
    # resources first, then ours on top) and packs that instead.
    set(ROMFS_STAGE ${CMAKE_BINARY_DIR}/romfs-stage)
    add_custom_target(${PROJECT_NAME}.nro ALL
        DEPENDS ${PROJECT_NAME}
        COMMAND ${CMAKE_COMMAND} -E rm -rf ${ROMFS_STAGE}
        COMMAND ${CMAKE_COMMAND} -E make_directory ${ROMFS_STAGE}
        COMMAND ${CMAKE_COMMAND} -E copy_directory
                ${CMAKE_CURRENT_SOURCE_DIR}/vendor/borealis/resources ${ROMFS_STAGE}
        COMMAND ${CMAKE_COMMAND} -E copy_directory
                ${CMAKE_CURRENT_SOURCE_DIR}/romfs ${ROMFS_STAGE}
        COMMAND ${NX_NACPTOOL_EXE} --create "FreeShop" "jhon1466" "0.1.0"
                ${CMAKE_BINARY_DIR}/${PROJECT_NAME}.nacp
        COMMAND ${NX_ELF2NRO_EXE} ${CMAKE_BINARY_DIR}/${PROJECT_NAME}.elf
                ${CMAKE_BINARY_DIR}/${PROJECT_NAME}.nro
                --icon=${CMAKE_CURRENT_SOURCE_DIR}/icon.jpg
                --nacp=${CMAKE_BINARY_DIR}/${PROJECT_NAME}.nacp
                --romfsdir=${ROMFS_STAGE}
    )
endif()
