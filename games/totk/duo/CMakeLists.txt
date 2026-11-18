# SPDX-License-Identifier: GPL-3.0-or-later
# libduo_host.so: Eden Duo's companion runtime and one game's title module for the native port (every game whose
# gpl_companion is true in games/games.toml: Breath of the Wild and Tears of the Kingdom).
#
#   Android:  cmake -S duo -B <dir> -DCMAKE_TOOLCHAIN_FILE=<ndk>/build/cmake/android.toolchain.cmake
#             -DANDROID_ABI=arm64-v8a -DANDROID_STL=c++_shared -DDUO_TITLE=<title id> -DDUO_MODULE=<source> ...
#             (android/Makefile does this with the game's title_id and its duo.module from games.toml)
#   Desktop:  cmake -S duo -B <dir> -DDUO_HOST_TEST=ON [-DDUO_TITLE=0100F2C0115B6000]
#             (a clang with C++23 <format>; builds duo_host_test for that title, test/README.md)
#
# The sources under vendor/ are Eden Duo's and its companions', compiled as they are (vendor/README.md lists the two
# marked exceptions). shim/ supplies the emulator headers they include; port_*.cpp and duo_host.cpp are the port's.
cmake_minimum_required(VERSION 3.22)
project(duo_host LANGUAGES C CXX)

option(DUO_HOST_TEST "Build the desktop host test (fake memory image, RomFS from a folder)" OFF)
# One title module per library: the modules all export the same eden_dsmod_get_* entry points.
set(DUO_TITLE "01007EF00011E000" CACHE STRING "Title id of the game whose module is linked in (games.toml title_id)")
set(DUO_MODULE "" CACHE STRING "Its main source below vendor/companions (games.toml duo.module; empty: the title's own)")

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_VISIBILITY_PRESET hidden)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(CMAKE_CXX_VISIBILITY_PRESET hidden)
set(CMAKE_VISIBILITY_INLINES_HIDDEN ON)

set(EMU ${CMAKE_CURRENT_SOURCE_DIR}/vendor/emu)
set(MODS ${EMU}/src/core/mods)

# Eden Duo's runtime. Not built: the dev tools (mod_re_tools, mod_console: EDEN_DSMOD_BUILD_DEV_TOOLS=0), and what the
# port replaces: mod_module (port_module.cpp), mod_romfs_sources / mod_user_source / mod_sysfont_source
# (port_sources.cpp), mod_load_plan and mod_launch_gate (the emulator's loader).
set(DUO_RUNTIME
    ${MODS}/engine_ichigo.cpp ${MODS}/engine_il2cpp.cpp ${MODS}/engine_mercury.cpp
    ${MODS}/mod_actions.cpp ${MODS}/mod_assets.cpp ${MODS}/mod_clock.cpp ${MODS}/mod_expr.cpp
    ${MODS}/mod_font_pages.cpp ${MODS}/mod_guest_bridge.cpp ${MODS}/mod_host_values.cpp ${MODS}/mod_input.cpp
    ${MODS}/mod_manifest.cpp ${MODS}/mod_map.cpp ${MODS}/mod_module_host.cpp ${MODS}/mod_module_services.cpp
    ${MODS}/mod_msbt.cpp ${MODS}/mod_nav.cpp ${MODS}/mod_nx_assets.cpp ${MODS}/mod_nx_runtime.cpp
    ${MODS}/mod_pages.cpp ${MODS}/mod_persist.cpp ${MODS}/mod_redraw.cpp ${MODS}/mod_runtime.cpp
    ${MODS}/mod_settings.cpp ${MODS}/mod_sources.cpp ${MODS}/mod_state.cpp ${MODS}/mod_texture_readback.cpp
    ${MODS}/mod_ui.cpp ${MODS}/mod_ui_canvas.cpp ${MODS}/mod_ui_expand.cpp ${MODS}/mod_ui_image.cpp
    ${MODS}/mod_ui_map_widget.cpp ${MODS}/mod_ui_scroll.cpp ${MODS}/mod_ui_text.cpp
    ${MODS}/mod_ui_transitions.cpp ${MODS}/mod_ui_widget_state.cpp
    ${EMU}/src/common/cityhash.cpp
    ${EMU}/src/video_core/textures/astc.cpp
    ${EMU}/externals/bc_decoder/bc_decoder.cpp)

# The title module, linked in instead of loaded (port_module.cpp), with the sources it needs besides its own.
set(COMPANIONS ${CMAKE_CURRENT_SOURCE_DIR}/vendor/companions)
set(MODULES ${COMPANIONS}/native/modules)
set(ZSTD ${CMAKE_CURRENT_SOURCE_DIR}/vendor/third_party/zstd)
set(DUO_MODULE_INCLUDES)
set(DUO_MODULE_FONT 0)   # 1: the module exports eden_dsmod_get_font_extensions (port_module.cpp negotiates it)
set(DUO_MODULE_ACTIONS 0)   # 1: the module takes the runtime's item actions (totk_native_actions.h; port_module.cpp)
if (DUO_TITLE STREQUAL "01007EF00011E000")
    # Breath of the Wild 1.6.0: one translation unit (its botw_*.h headers).
    set(DUO_MODULE_MAIN native/modules/01007EF00011E000.cpp)
    set(DUO_MODULE_EXTRA)
elseif (DUO_TITLE STREQUAL "0100F2C0115B6000")
    # Tears of the Kingdom 1.4.3: game state from TotK Native's reader (totk_reader/, MIT; the runtime's
    # companion_snapshot.c byte for byte, tests/test_companion_state.py), art and the Rodin font decoded from the
    # player's RomFS: zstd frames with ZsDic dictionaries (totk_romfs.cpp, zstd's decompressor) -> SARC -> BNTX / bfotf
    # (Wonder's decoders, wonder_assets / wonder_font with stb_truetype). The same set as the companions' own
    # native/modules/CMakeLists.txt (TOTK_SOURCES, WONDER_ZSTD_SOURCES), with the decoder-only zstd vendored.
    set(DUO_MODULE_MAIN native/modules/0100F2C0115B6000.cpp)
    set(ZSTD_SOURCES
        ${ZSTD}/common/entropy_common.c ${ZSTD}/common/error_private.c ${ZSTD}/common/fse_decompress.c
        ${ZSTD}/common/xxhash.c ${ZSTD}/common/zstd_common.c
        ${ZSTD}/decompress/huf_decompress.c ${ZSTD}/decompress/zstd_ddict.c ${ZSTD}/decompress/zstd_decompress.c
        ${ZSTD}/decompress/zstd_decompress_block.c)
    set_source_files_properties(${ZSTD_SOURCES} PROPERTIES
        COMPILE_DEFINITIONS "ZSTD_DISABLE_ASM=1;ZSTD_MULTITHREAD=0;XXH_NAMESPACE=WONDER_")
    set(DUO_MODULE_EXTRA
        ${MODULES}/totk_layer.cpp ${MODULES}/totk_romfs.cpp ${MODULES}/wonder_assets.cpp ${MODULES}/wonder_font.cpp
        ${MODULES}/totk_reader/companion_snapshot.c ${MODULES}/totk_reader/totk_reader_ro.c
        ${MODULES}/totk_reader/totk_gamedata.c
        ${ZSTD_SOURCES})
    set(DUO_MODULE_INCLUDES ${ZSTD})
    set(DUO_MODULE_FONT 1)
    set(DUO_MODULE_ACTIONS 1)
else()
    message(FATAL_ERROR "DUO_TITLE ${DUO_TITLE}: no title module for that game (games.toml title_id with gpl_companion)")
endif()
if (NOT DUO_MODULE)
    set(DUO_MODULE ${DUO_MODULE_MAIN})
elseif (NOT DUO_MODULE STREQUAL DUO_MODULE_MAIN)
    message(FATAL_ERROR "DUO_MODULE ${DUO_MODULE} is not the module of title ${DUO_TITLE} (${DUO_MODULE_MAIN})")
endif()
set(DUO_MODULE_SOURCES ${COMPANIONS}/${DUO_MODULE} ${DUO_MODULE_EXTRA})

set(DUO_PORT_SOURCES shim/system.cpp shim/stb.cpp port_sources.cpp port_module.cpp duo_host.cpp)

add_library(duo_host SHARED ${DUO_RUNTIME} ${DUO_MODULE_SOURCES} ${DUO_PORT_SOURCES})
target_include_directories(duo_host PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}                      # duo_port.h, duo_host.h
    ${CMAKE_CURRENT_SOURCE_DIR}/shim                 # the emulator, as the runtime includes it
    ${EMU}/src
    ${EMU}/externals/bc_decoder
    ${EMU}/externals/stb
    ${CMAKE_CURRENT_SOURCE_DIR}/vendor/third_party   # nlohmann/json.hpp, ankerl/unordered_dense.h
    ${MODULES}
    ${DUO_MODULE_INCLUDES})
target_compile_definitions(duo_host PRIVATE EDEN_DSMOD_BUILD_DEV_TOOLS=0 DUO_PORT=1 DUO_MODULE_FONT=${DUO_MODULE_FONT}
    DUO_MODULE_ACTIONS=${DUO_MODULE_ACTIONS})
# -fexperimental-library: std::jthread and std::stop_token in the NDK's libc++, as Eden Duo's own build does.
target_compile_options(duo_host PRIVATE $<$<COMPILE_LANGUAGE:CXX>:-fexperimental-library> -Wno-deprecated-declarations
    ${DUO_EXTRA_FLAGS})
target_link_options(duo_host PRIVATE -fexperimental-library)
# zlib: engine_mercury.cpp (another game's container; kept so the runtime links whole). By name, from the sysroot:
# find_package would add the sysroot's include folder ahead of libc++'s own headers.
target_link_libraries(duo_host PRIVATE z)
if (ANDROID)
    # Only the port's two entry points and DuoSurfaceView's JNI methods leave the library. Everything else stays
    # local, among it the Tears of the Kingdom reader's companion_* functions, which libtotk.so also defines.
    target_link_options(duo_host PRIVATE
        -Wl,--version-script=${CMAKE_CURRENT_SOURCE_DIR}/libduo_host.map -Wl,--no-undefined -Wl,--build-id=sha1)
    set_target_properties(duo_host PROPERTIES LINK_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/libduo_host.map)
    target_link_libraries(duo_host PRIVATE android log)
endif()

if (DUO_HOST_TEST)
    # Breath of the Wild: a fake image (or the real main via test/make_fake_image.py). Tears of the Kingdom: the real
    # 1.4.3 main with a fake game state laid out behind it (test/README.md).
    if (DUO_TITLE STREQUAL "0100F2C0115B6000")
        add_executable(duo_host_test test/totk_host_test.cpp)
    else()
        add_executable(duo_host_test test/host_test.cpp)
    endif()
    target_include_directories(duo_host_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${EMU}/externals/stb)
    target_compile_options(duo_host_test PRIVATE -fexperimental-library)
    target_link_options(duo_host_test PRIVATE -fexperimental-library)
    target_link_libraries(duo_host_test PRIVATE duo_host)
    # The test reaches the host's internal hooks (duo_host.h), which the Android library does not export.
    # The Tears of the Kingdom test also hashes flag names with the reader's companion_hash (C).
    set_target_properties(duo_host PROPERTIES CXX_VISIBILITY_PRESET default C_VISIBILITY_PRESET default
                          VISIBILITY_INLINES_HIDDEN OFF)
endif()
