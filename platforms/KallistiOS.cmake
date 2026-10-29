# SEGA Dreamcast (KallistiOS) platform
#
# Build from inside a KOS environment (source /opt/toolchains/dc/kos/environ.sh):
#   mkdir build-dc && cd build-dc
#   kos-cmake -DPLATFORM=KallistiOS -DCMAKE_BUILD_TYPE=Release ..
#   make -j$(nproc)
#
# No kos-ports packages are required: the Dreamcast build decodes no Vorbis
# (music and sfx are raw PCM), so libtremor and libogg are no longer linked.
#
# Produces RSDKv4.elf; package with:
#   mkdcdisc -e RSDKv4.elf -f /path/to/Data.rsdk -o sonic1.cdi -N
#
# To run over dcload-ip / dcload-serial instead, configure a SEPARATE build
# directory with the data root pointed at the host:
#   mkdir build-pc && cd build-pc
#   kos-cmake -DPLATFORM=KallistiOS -DCMAKE_BUILD_TYPE=Release -DDC_DATA_ROOT=/pc/ ..
#   make -j$(nproc)
#   dc-tool-ip -t <dreamcast-ip> -x RSDKv4.elf
# Run dc-tool from the directory holding Data.rsdk AND the loose Data/ tree —
# /pc is its working directory. Note dcload-SERIAL cannot sustain the music
# stream (44100 B/s of raw PCM); that path needs the BBA.
#
# Use a separate build dir rather than retargeting build-dc: DC_DATA_ROOT is a
# CACHE variable, so once "/pc/" is configured into a build directory it STAYS
# there across later reconfigures unless you pass it again or wipe the cache.
# A build-dc that has never been given -DDC_DATA_ROOT is byte-identical to the
# behaviour before this option existed.

if(NOT DEFINED ENV{KOS_BASE})
    message(FATAL_ERROR "KOS environment not found. Source environ.sh and use kos-cmake.")
endif()

add_executable(RetroEngine ${RETRO_FILES}
    RSDKv4/Dreamcast/DCGraphics.cpp
    RSDKv4/Dreamcast/DCAudio.cpp
    RSDKv4/Dreamcast/DCInput.cpp
    RSDKv4/Dreamcast/DCSystem.cpp
    RSDKv4/Dreamcast/DCNative.cpp
    RSDKv4/Dreamcast/DCSave.cpp
)

# Force off features that can't work on the Dreamcast
set(RETRO_MOD_LOADER OFF CACHE BOOL "Mod loader disabled on Dreamcast" FORCE)
set(RETRO_NETWORKING OFF CACHE BOOL "Networking disabled on Dreamcast" FORCE)
set(RETRO_USE_HW_RENDER OFF CACHE BOOL "OpenGL HW renderer disabled on Dreamcast" FORCE)

option(RSDK_DEBUG "Enable debug logging + dev menu" OFF)
option(DC_NO_AUDIO "Disable all audio init (bisection aid)" OFF)

# Root the game data is read from. "/cd/" is the disc; "/pc/" is the dcload
# host directory. This is the SUPPORTED way to switch — a compile-time literal,
# decided once at configure time. Resolving the two roots at runtime was tried
# on 2026-07-29 and reverted the same day: it broke asset loading on disc as
# well as over dcload. Do not reintroduce it.
#
# BASE_PATH is used in literal-concatenation form (BASE_PATH "settings.ini" in
# Userdata.cpp, BASE_PATH "log.txt" in Debug.cpp), so this must stay a plain
# string literal — it cannot become a variable or a function call without
# editing those sites too.
set(DC_DATA_ROOT "/cd/" CACHE STRING "Game data root: /cd/ for a disc image, /pc/ for dcload")

# Game data is read from DC_DATA_ROOT (the disc root by default)
target_compile_definitions(RetroEngine PRIVATE
    BASE_PATH="${DC_DATA_ROOT}"
    RSDK_DEBUG=$<BOOL:${RSDK_DEBUG}>
    DC_NO_AUDIO=$<BOOL:${DC_NO_AUDIO}>
)

# Say which root this build was configured for — a mis-set cache variable is
# otherwise only discoverable by running the binary and watching it fail.
message(STATUS "RSDKv4/DC: game data root is ${DC_DATA_ROOT}")

# Data served over dcload is nothing like a disc. Every dcload call runs with
# IRQs disabled for its whole duration (KOS dcload.c: dcload_syscall does
# irq_disable_scoped), so a read halts the machine rather than just occupying
# the link. The music reader's default 32KB chunks are sized for GD-ROM seek
# cost; over dcload they become a ~20-30ms blackout 1.35x a second, dropping
# two frames each time. Tell the port to read in frame-sized pieces instead.
#
# Set ONLY for /pc/. A /cd/ build never sees this macro and is unchanged.
if(DC_DATA_ROOT MATCHES "^/pc")
    target_compile_definitions(RetroEngine PRIVATE DC_DATA_ON_DCLOAD=1)
    message(STATUS "RSDKv4/DC: dcload transport - music reads in small chunks")
endif()

# libtremor / libogg were dropped once the Dreamcast build stopped decoding
# Vorbis: music is raw PCM streamed by DC_MusicOpen, global sfx are raw PCM
# via DC_LoadRawSfx, stage sfx are PCM .wav into AICA sound RAM. Only libm is
# still needed. If a vorbis header is ever reinstated in RetroEngine.hpp, the
# find_path blocks and -ltremor -logg have to come back with it.
target_link_libraries(RetroEngine -lm)

# Bridge prebuilt GCC runtime calls to mutex_lock on KOS versions where
# that API is now inline (otherwise libgcc's weak failure stub is used).
target_link_libraries(RetroEngine "-Wl,--wrap=mutex_lock")

# NOTE: -ffunction-sections/-fdata-sections + --gc-sections are
# deliberately NOT used: with KOS's linker script, section GC can discard
# .init_array (static constructors) and other KEEP-worthy data, producing
# a binary that boots to the KOS banner and then silently misbehaves.
# The working RSDKv5 DC port doesn't use them either.
set(RELEASE_FLAGS
    -Os
    -fno-exceptions
    -fomit-frame-pointer
)

# Hot paths get -O3: the script interpreter and the software renderer
# dominate the frame time on SH4.
set(CRITICAL_FILES
    RSDKv4/Script.cpp
    RSDKv4/Drawing.cpp
    RSDKv4/Scene.cpp
    RSDKv4/Object.cpp
    RSDKv4/Collision.cpp
    RSDKv4/Math.cpp
    RSDKv4/Scene3D.cpp
    RSDKv4/Sprite.cpp
    RSDKv4/Palette.cpp
    RSDKv4/Animation.cpp
)

target_compile_options(RetroEngine PRIVATE $<$<NOT:$<CONFIG:Debug>>:${RELEASE_FLAGS}>)
set_source_files_properties(${CRITICAL_FILES} PROPERTIES COMPILE_FLAGS "$<$<NOT:$<CONFIG:Debug>>:-O3>")

set_target_properties(RetroEngine PROPERTIES
    CXX_STANDARD 11
    CXX_STANDARD_REQUIRED ON
)
