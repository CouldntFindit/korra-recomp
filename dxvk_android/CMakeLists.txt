# DXVK-native (D3D9) for Android arm64, linked statically into libkorra.so.
#
# The tree this builds is produced by tools/dxvk_prepare.py: the pinned
# dxvk-native checkout (native-1.9.2b, c8dc91f) plus the arm64/Android
# replacements and the generated shader headers. This file is the build recipe,
# re-derived from DXVK's own meson layout (src/{util,dxso,spirv,vulkan,dxvk,d3d9})
# and docs/ANDROID_BRINGUP.md section 5.
#
# No D3D10/11, no DXGI, no OpenVR/OpenXR: Korra is a D3D9 title.

cmake_minimum_required(VERSION 3.22)
project(dxvk_android C CXX)

if(NOT ANDROID)
    message(FATAL_ERROR "dxvk_android builds for Android only")
endif()

set(DXVK_SOURCE "" CACHE PATH "dxvk-native tree prepared by tools/dxvk_prepare.py")
if(NOT EXISTS "${DXVK_SOURCE}/ANDROID_PREPARE_SHA256")
    message(FATAL_ERROR
        "DXVK tree not prepared: run tools/dxvk_prepare.py <clone> (see docs/ANDROID_BRINGUP.md 5.3)")
endif()

file(GLOB UTIL
    "${DXVK_SOURCE}/src/util/*.cpp"
    "${DXVK_SOURCE}/src/util/com/*.cpp"
    "${DXVK_SOURCE}/src/util/config/*.cpp"
    "${DXVK_SOURCE}/src/util/log/*.cpp"
    "${DXVK_SOURCE}/src/util/sha1/*.c"
    "${DXVK_SOURCE}/src/util/sha1/*.cpp"
    "${DXVK_SOURCE}/src/util/sync/*.cpp")
list(FILTER UTIL EXCLUDE REGEX "util_gdi\\.cpp$")
list(APPEND UTIL
    "${DXVK_SOURCE}/src/util/platform/util_env_linux.cpp"
    "${DXVK_SOURCE}/src/util/platform/util_luid_linux.cpp"
    "${DXVK_SOURCE}/src/util/platform/util_string_linux.cpp"
    "${DXVK_SOURCE}/src/util/platform/thread_native.cpp")

file(GLOB GRAPHICS
    "${DXVK_SOURCE}/src/dxso/*.cpp"
    "${DXVK_SOURCE}/src/spirv/*.cpp"
    "${DXVK_SOURCE}/src/vulkan/*.cpp"
    "${DXVK_SOURCE}/src/dxvk/*.cpp"
    "${DXVK_SOURCE}/src/dxvk/hud/*.cpp"
    "${DXVK_SOURCE}/src/d3d9/*.cpp")
list(FILTER GRAPHICS EXCLUDE REGEX "dxvk_open(vr|xr)\\.cpp$")

add_library(dxvk_android STATIC ${UTIL} ${GRAPHICS} android_wsi.cpp)

target_compile_features(dxvk_android PRIVATE cxx_std_17)
# DXVK throws DxvkError and uses RTTI; the shell's -fno-exceptions does not apply
# to this target.
target_compile_options(dxvk_android PRIVATE
    -O2 -fexceptions -frtti -fno-strict-aliasing -Wno-deprecated-declarations
    -Wno-unused-parameter -Wno-missing-field-initializers)
target_compile_definitions(dxvk_android PRIVATE
    DXVK_NATIVE DXVK_PLATFORM_LINUX DXVK_WSI_ANDROID NOMINMAX
    VK_USE_PLATFORM_ANDROID_KHR)
target_include_directories(dxvk_android PRIVATE
    "${DXVK_SOURCE}"
    "${DXVK_SOURCE}/include"
    "${DXVK_SOURCE}/src/util"
    "${DXVK_SOURCE}/include/native"
    "${DXVK_SOURCE}/include/native/windows"
    "${DXVK_SOURCE}/include/native/directx"
    "${DXVK_SOURCE}/generated")

find_library(DXVK_VULKAN_LIBRARY NAMES vulkan libvulkan.so.1 REQUIRED)
target_link_libraries(dxvk_android PUBLIC ${DXVK_VULKAN_LIBRARY} android log)
