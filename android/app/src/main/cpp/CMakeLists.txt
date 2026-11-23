# Android native build (M3 scaffold).
#
# Builds libkorra.so: GameActivity glue + the shell (log file, install hand-off,
# frame-pacing hints). Nothing from the recompilation is linked by default, so
# this target builds and runs on a device before the M2 gate, and the post-gate
# session only adds source trees and flips options.
#
# Options:
#   KORRA_LINK_GUEST=ON   compile the lifted C (src/recomp/gen) and the Android
#                         host (src/host_android) into libkorra.so
#   KORRA_WITH_DXVK=ON    compile a prepared dxvk-native tree (see
#                         docs/ANDROID_BRINGUP.md section 5)

cmake_minimum_required(VERSION 3.22.1)

project(korra_android LANGUAGES C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

option(KORRA_LINK_GUEST "Link the lifted guest C and the Android host" OFF)
option(KORRA_WITH_DXVK "Link a prepared dxvk-native tree" OFF)

# android/app/src/main/cpp -> repository root
get_filename_component(KORRA_REPO_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../../../.." ABSOLUTE)
set(KORRA_GEN_DIR "${KORRA_REPO_ROOT}/src/recomp/gen" CACHE PATH "lifted C (gitignored)")
set(KORRA_HOST_ANDROID_DIR "${KORRA_REPO_ROOT}/src/host_android" CACHE PATH "Android host shims")
set(KORRA_PCRECOMP_DIR "${KORRA_REPO_ROOT}/../pcrecomp" CACHE PATH "pinned pcrecomp checkout")
set(KORRA_DXVK_SOURCE "" CACHE PATH "prepared dxvk-native source tree")

find_package(game-activity REQUIRED CONFIG)

add_library(korra SHARED
    jni_bridge.cpp
    log_android.c
    install_android.c
    perf_android.c
)

target_compile_definitions(korra PRIVATE KORRA_ANDROID=1)
target_compile_options(korra PRIVATE -O2 -Wall -Wextra -Wno-unused-parameter)
target_link_libraries(korra PRIVATE
    game-activity::game-activity_static
    android
    log
    dl
)

# GameActivity_onCreate and JNI_OnLoad are resolved by name from Java, so the
# linker must keep them even though no C++ code references them. JNI_OnLoad is
# what registers GameActivity's own native methods (initializeNativeCode and
# friends); without -u it is dropped and the Activity fails with
# UnsatisfiedLinkError at first call.
target_link_options(korra PRIVATE
    "-uGameActivity_onCreate"
    "-uJNI_OnLoad"
    "-Wl,-z,max-page-size=16384"
)

# ---------------------------------------------------------------------------
# The real runtime, after the M2 gate.
# ---------------------------------------------------------------------------
if(KORRA_LINK_GUEST)
    file(GLOB KORRA_GEN_SOURCES CONFIGURE_DEPENDS "${KORRA_GEN_DIR}/*.c")
    if(NOT KORRA_GEN_SOURCES)
        message(FATAL_ERROR
            "KORRA_LINK_GUEST=ON but ${KORRA_GEN_DIR} has no .c files. "
            "Run scripts\\run_korra_lift.py first (see docs/TOOLCHAIN.md).")
    endif()
    file(GLOB KORRA_HOST_SOURCES CONFIGURE_DEPENDS
        "${KORRA_HOST_ANDROID_DIR}/*.c" "${KORRA_HOST_ANDROID_DIR}/*.cpp")
    if(NOT KORRA_HOST_SOURCES)
        message(FATAL_ERROR
            "KORRA_LINK_GUEST=ON but ${KORRA_HOST_ANDROID_DIR} does not exist. "
            "The Win32 host in src/host is not portable; see docs/ANDROID_BRINGUP.md.")
    endif()

    add_library(korra_guest OBJECT ${KORRA_GEN_SOURCES} ${KORRA_HOST_SOURCES}
        # M4 audio: the shared sink contract and its AAudio implementation live
        # in src/host (Windows owns wasapi_sink.c there; this file is
        # `#ifdef __ANDROID__` and compiles to nothing on Windows).
        "${KORRA_REPO_ROOT}/src/host/audio_sink.h"
        "${KORRA_REPO_ROOT}/src/host/aaudio_sink.c")
    target_include_directories(korra_guest PRIVATE
        "${KORRA_GEN_DIR}"
        "${KORRA_HOST_ANDROID_DIR}"
        "${KORRA_REPO_ROOT}/src/host"
        "${KORRA_PCRECOMP_DIR}/runtime/recomp32"
        "${KORRA_PCRECOMP_DIR}/runtime/win32hle")
    # -fwrapv / -fno-strict-aliasing match the MSVC host's semantics: the lifted
    # C performs 32-bit wrapping arithmetic on one untyped guest memory array.
    target_compile_options(korra_guest PRIVATE
        -O1 -fwrapv -fno-strict-aliasing -ffp-contract=off -w)
    # recomp_0053.c is 142 MB of one translation unit. Clang at -O1 needs ~5 GB
    # and >15 minutes for it alone (measured), which dominates every rebuild; -O0
    # keeps bring-up honest and costs only that file's execution speed.
    foreach(gen_src ${KORRA_GEN_SOURCES})
        file(SIZE "${gen_src}" gen_size)
        if(gen_size GREATER 100000000)
            set_source_files_properties("${gen_src}" PROPERTIES COMPILE_OPTIONS "-O0")
            message(STATUS "korra: ${gen_src} is ${gen_size} bytes - compiling at -O0")
        endif()
    endforeach()
    target_compile_definitions(korra_guest PRIVATE KORRA_ANDROID=1)
    target_link_libraries(korra PRIVATE korra_guest)
    # AAudio (API 26+): the M4 output stage. korra_guest is an OBJECT library,
    # so the library belongs on the final target.
    target_link_libraries(korra PRIVATE aaudio)
    target_compile_definitions(korra PRIVATE KORRA_LINK_GUEST=1)
    find_package(Threads REQUIRED)
    target_link_libraries(korra PRIVATE Threads::Threads)
endif()

if(KORRA_WITH_DXVK)
    if(NOT KORRA_DXVK_SOURCE)
        message(FATAL_ERROR "KORRA_WITH_DXVK=ON needs -DKORRA_DXVK_SOURCE=<prepared tree>")
    endif()
    # The build recipe lives in the repo (dxvk_android/); the prepared
    # dxvk-native tree is a local clone and is never committed.
    set(DXVK_SOURCE "${KORRA_DXVK_SOURCE}" CACHE PATH "prepared dxvk-native" FORCE)
    add_subdirectory("${KORRA_REPO_ROOT}/dxvk_android" "${CMAKE_CURRENT_BINARY_DIR}/dxvk")
    target_compile_definitions(korra PRIVATE KORRA_WITH_DXVK=1 VK_USE_PLATFORM_ANDROID_KHR)
    target_link_libraries(korra PRIVATE dxvk_android)
    if(TARGET korra_guest)
        target_compile_definitions(korra_guest PRIVATE KORRA_WITH_DXVK=1)
        target_compile_features(korra_guest PRIVATE cxx_std_17)
        target_include_directories(korra_guest PRIVATE
            "${KORRA_DXVK_SOURCE}"
            "${KORRA_DXVK_SOURCE}/include"
            "${KORRA_DXVK_SOURCE}/include/native"
            "${KORRA_DXVK_SOURCE}/include/native/windows"
            "${KORRA_DXVK_SOURCE}/include/native/directx"
            "${KORRA_DXVK_SOURCE}/src/util")
    endif()
endif()
