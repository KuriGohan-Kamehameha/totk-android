# SPDX-License-Identifier: GPL-3.0-or-later
# libduo_host.so: Eden Duo's companion runtime and the Breath of the Wild module for the native port (BotW build only).
#
#   Android:  cmake -S duo -B <dir> -DCMAKE_TOOLCHAIN_FILE=<ndk>/build/cmake/android.toolchain.cmake
#             -DANDROID_ABI=arm64-v8a -DANDROID_STL=c++_shared ...     (android/Makefile does this for VERSION=botw160)
#   Desktop:  cmake -S duo -B <dir> -DDUO_HOST_TEST=ON                 (a clang with C++23 <format>; builds duo_host_test)
#
# The sources under vendor/ are Eden Duo's and its companions', compiled as they are (vendor/README.md lists the two
# marked exceptions). shim/ supplies the emulator headers they include; port_*.cpp and duo_host.cpp are the port's.
cmake_minimum_required(VERSION 3.22)
project(duo_host LANGUAGES CXX)

option(DUO_HOST_TEST "Build the desktop host test (fake memory image, RomFS from a folder)" OFF)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
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

# The Breath of the Wild title module: one translation unit, linked in instead of loaded (port_module.cpp).
set(DUO_MODULE ${CMAKE_CURRENT_SOURCE_DIR}/vendor/companions/native/modules/01007EF00011E000.cpp)

set(DUO_PORT_SOURCES shim/system.cpp shim/stb.cpp port_sources.cpp port_module.cpp duo_host.cpp)

add_library(duo_host SHARED ${DUO_RUNTIME} ${DUO_MODULE} ${DUO_PORT_SOURCES})
target_include_directories(duo_host PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}                      # duo_port.h, duo_host.h
    ${CMAKE_CURRENT_SOURCE_DIR}/shim                 # the emulator, as the runtime includes it
    ${EMU}/src
    ${EMU}/externals/bc_decoder
    ${EMU}/externals/stb
    ${CMAKE_CURRENT_SOURCE_DIR}/vendor/third_party   # nlohmann/json.hpp, ankerl/unordered_dense.h
    ${CMAKE_CURRENT_SOURCE_DIR}/vendor/companions/native/modules)
target_compile_definitions(duo_host PRIVATE EDEN_DSMOD_BUILD_DEV_TOOLS=0 DUO_PORT=1)
# -fexperimental-library: std::jthread and std::stop_token in the NDK's libc++, as Eden Duo's own build does.
target_compile_options(duo_host PRIVATE -fexperimental-library -Wno-deprecated-declarations ${DUO_EXTRA_FLAGS})
target_link_options(duo_host PRIVATE -fexperimental-library)
# zlib: engine_mercury.cpp (another game's container; kept so the runtime links whole). By name, from the sysroot:
# find_package would add the sysroot's include folder ahead of libc++'s own headers.
target_link_libraries(duo_host PRIVATE z)
if (ANDROID)
    # Only the port's two entry points and DuoSurfaceView's JNI methods leave the library.
    target_link_options(duo_host PRIVATE
        -Wl,--version-script=${CMAKE_CURRENT_SOURCE_DIR}/libduo_host.map -Wl,--no-undefined -Wl,--build-id=sha1)
    set_target_properties(duo_host PROPERTIES LINK_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/libduo_host.map)
    target_link_libraries(duo_host PRIVATE android log)
endif()

if (DUO_HOST_TEST)
    add_executable(duo_host_test test/host_test.cpp)
    target_include_directories(duo_host_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${EMU}/externals/stb)
    target_compile_options(duo_host_test PRIVATE -fexperimental-library)
    target_link_options(duo_host_test PRIVATE -fexperimental-library)
    target_link_libraries(duo_host_test PRIVATE duo_host)
    # The test reaches the host's internal hooks (duo_host.h), which the Android library does not export.
    set_target_properties(duo_host PROPERTIES CXX_VISIBILITY_PRESET default VISIBILITY_INLINES_HIDDEN OFF)
endif()
