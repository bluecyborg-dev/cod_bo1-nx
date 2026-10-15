# Call of Duty: Black Ops (Xbox 360) for Nintendo Switch: static recompilation host application.
#
# The game code is not in this repository: run tools/codegen.sh first (it produces generated/, the code recompiled
# from your own default.xex, and the CMake glue generated/rexglue.cmake). A cross build cannot run rexglue itself.
# Build with tools/build_nro.sh. Output: bo1.nro ("Call of Duty: Black Ops" in the Homebrew Menu).

cmake_minimum_required(VERSION 3.25)
project(bo1 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# The SDK is built from source in this repository unless another tree is given.
set(REXSDK_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../sdk" CACHE PATH "Path to the ReXGlue SDK source tree")

include(generated/rexglue.cmake)

add_executable(bo1
    src/main.cpp
    # Engine hooks (Com_Error logging, ...): addresses of our default.xex.
    src/bo1_engine.cpp
    # Native renderer (GPL-3.0, from project8-sw / nfsmw-nx; see src/native/README.md). Selected at run time with
    # bo1_renderer = "native"; the emulated GPU stays the default.
    src/native/bo1_native_system.cpp
    src/native/native_renderer.cpp
    src/native/native_textures.cpp
    src/native/native_geometry_shaders.cpp)
target_include_directories(bo1 PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
    # The SDK's prebuilt SPIR-V (the gamma-applying swap shader) and glslang's SPIR-V builder, which the translator
    # headers include.
    "${REXSDK_DIR}/src/graphics"
    "${REXSDK_DIR}/thirdparty/glslang"
    "${REXSDK_DIR}/thirdparty/xxHash")
# The renderer's hashes compile xxHash into the app (the Mac SDK is a shared library that doesn't export it).
target_compile_definitions(bo1 PRIVATE XXH_INLINE_ALL)
# On the desktop the GPU code is a shared plugin; the renderer links it directly (the Switch links it statically).
if(NOT REXGLUE_PLATFORM_SWITCH)
    target_link_libraries(bo1 PRIVATE rexgpu-xenos)
endif()

# First bring-up: ReXGlue's Xenos GPU emulation draws the game.
rexglue_setup_target(bo1 GPU_PLUGINS xenos)
# rex/rex_app.h includes ImGui; the SDK only adds its include path for the Switch target. The macOS build (used to
# debug start-up on the Mac) needs it too.
if(NOT REXGLUE_PLATFORM_SWITCH AND TARGET imgui)
    target_include_directories(bo1 PRIVATE $<TARGET_PROPERTY:imgui,INTERFACE_INCLUDE_DIRECTORIES>)
endif()

# The SDK compiles itself with these, but they do not reach the generated game code, which is built in this
# directory. GCC's default -ffp-contract=fast fuses separate PPC fmul/fadd into AArch64 FMA and results drift from
# the Xbox 360; the recompiled code reads guest memory through casts.
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    foreach(target bo1 bo1_recomp)
        if(TARGET ${target})
            target_compile_options(${target} PRIVATE -ffp-contract=off -fno-strict-aliasing)
        endif()
    endforeach()
    # No errno side effect on sqrt/etc. and no FP-trap assumptions in the recompiled code (no guest code reads the
    # host FP exception flags).
    if(TARGET bo1_recomp)
        target_compile_options(bo1_recomp PRIVATE -fno-math-errno -fno-trapping-math)
        # Keep guest register copies out of vector registers (project8-sw): GCC merged pairs of 64-bit guest-context
        # copies into one q-register load/store, and a page fault between them on Horizon came back with q31
        # cleared (a loading-screen crash on a guest pointer that went through q31 as zero).
        target_compile_options(bo1_recomp PRIVATE -fno-tree-slp-vectorize -fno-tree-vectorize)
    endif()
endif()

if(REXGLUE_PLATFORM_SWITCH)
    # On the SD card: sdmc:/switch/bo1-nx/bo1-nx.nro with bo1.toml and game_root/ beside it.
    set(BO1_VERSION "0.0.1" CACHE STRING "NRO display version")
    set(BO1_ICON "${CMAKE_CURRENT_SOURCE_DIR}/../extras/icon.jpg")
    if(NOT EXISTS "${BO1_ICON}")
        set(BO1_ICON "")  # libnx default icon
    endif()
    rexglue_switch_add_nro(bo1
        NAME "Call of Duty: Black Ops"
        AUTHOR "bo1-nx"
        VERSION "${BO1_VERSION}"
        ICON "${BO1_ICON}")
endif()
