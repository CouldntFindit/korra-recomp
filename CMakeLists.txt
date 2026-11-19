cmake_minimum_required(VERSION 3.20)
project(korra_recomp VERSION 0.1.0 LANGUAGES C)

set(CMAKE_C_STANDARD 17)

# ---------------------------------------------------------------------------
# pcrecomp checkout. Override with -DPCRECOMP=<path> or the PCRECOMP env var.
# The pinned commit and patch series are documented in docs/TOOLCHAIN.md.
# ---------------------------------------------------------------------------
if(NOT PCRECOMP)
    if(DEFINED ENV{PCRECOMP})
        set(PCRECOMP "$ENV{PCRECOMP}")
    else()
        set(PCRECOMP "${CMAKE_CURRENT_SOURCE_DIR}/../pcrecomp")
    endif()
endif()
if(NOT EXISTS "${PCRECOMP}/runtime/native32/native32.c")
    message(FATAL_ERROR "pcrecomp not found at '${PCRECOMP}'. Set -DPCRECOMP=<checkout> (see docs/TOOLCHAIN.md).")
endif()

if(MSVC)
    # The guest image is mapped at its own base (0x400000), so the host must not
    # land there: link high and fixed, exactly like pcrecomp's own host projects.
    add_link_options(/DYNAMICBASE:NO /FIXED /BASE:0x70000000)
    # Lifted code has a deep call graph; give it room.
    add_link_options(/STACK:16777216)

    # Generated code is correct but noisy; keep the logs readable.
    add_compile_options(/W1 /wd4996 /wd4244 /wd4146 /wd4102 /wd4013 /wd4101
                        /wd4267 /wd4018 /wd4305 /wd4310 /wd4054 /wd4065)
    add_compile_options(/MP)
    set(CMAKE_C_FLAGS_RELEASE "${CMAKE_C_FLAGS_RELEASE} /O1")
    # /GL on 613 MB of generated code is not worth the link time.
else()
    message(FATAL_ERROR "The Windows host (native32) is MSVC x86 only. Build with -A Win32.")
endif()

if(CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "Build 32-bit: cmake -B build -G \"Visual Studio 17 2022\" -A Win32")
endif()

# ---------------------------------------------------------------------------
# Sources
# ---------------------------------------------------------------------------
set(HOST_SOURCES
    src/host/main.c
    src/host/shims.c
)

set(PCRECOMP_RUNTIME
    "${PCRECOMP}/runtime/native32/native32.c"
    "${PCRECOMP}/runtime/recomp32/image_loader.c"
    "${PCRECOMP}/runtime/recomp32/recomp_trace.c"
)

file(GLOB GENERATED_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/src/recomp/gen/recomp_*.c")
list(FILTER GENERATED_SOURCES EXCLUDE REGEX "recomp_dispatch\\.c$")
set(DISPATCH "${CMAKE_CURRENT_SOURCE_DIR}/src/recomp/gen/recomp_dispatch.c")
if(NOT EXISTS "${DISPATCH}")
    message(FATAL_ERROR "Generated code missing. Run: python scripts/run_korra_lift.py lift work/LoK.analysis.exe work/catalog.json src/recomp/gen")
endif()

add_executable(korra_recomp ${HOST_SOURCES} ${PCRECOMP_RUNTIME} ${GENERATED_SOURCES} ${DISPATCH})

target_include_directories(korra_recomp PRIVATE
    src/host
    src/recomp/gen
    "${PCRECOMP}/runtime/recomp32"
    "${PCRECOMP}/runtime/native32"
)

target_link_libraries(korra_recomp PRIVATE
    # Only what the host itself uses. The guest's D3D9/D3DX/XInput/DInput
    # imports are resolved at run time by native32_bind, not by the linker.
    kernel32 user32 gdi32 advapi32 shell32 ole32 oleaut32 winmm
    dbghelp synchronization
)

target_compile_definitions(korra_recomp PRIVATE
    _CRT_SECURE_NO_WARNINGS
    WIN32_LEAN_AND_MEAN
)
