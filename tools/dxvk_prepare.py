#!/usr/bin/env python3
"""dxvk_prepare.py - turn a clean dxvk-native checkout into an arm64 Android build tree.

Nothing here is a fork: every change is a string replacement applied to the
pinned tag, recorded in ANDROID_PREPARE_SHA256 so CMake refuses a stale tree
(docs/ANDROID_BRINGUP.md section 5.3).

The replacements are the portable subset the arm64 build actually needs:

  util_bit.h        cstdint; x86 asm/intrinsics behind an x86 guard; arm64 ctz
  sync_spinlock.h   _mm_pause -> yield
  util_vector.h     SSE replaceNaN -> scalar
  d3d9_device.cpp   feature bits asked for are the supported ones. On Xclipse 940
                    textureCompressionBC is NOT reported (BC sampling formats
                    are), so the upstream VK_TRUE would fail vkCreateDevice.
  log.cpp           warnings/errors also go to logcat under "DXVK"
  vulkan_presenter  null-guard vkDestroySwapchainKHR/vkDestroySurfaceKHR

Shader headers: glslc (shipped with the NDK) compiles every built-in GLSL
shader into generated/<name>.h as `const uint32_t <name>[]`, which is what
DXVK's SpirvCodeBuffer(const uint32_t (&)[N]) consumes.

Usage:
  python tools\\dxvk_prepare.py <dxvk-native-checkout> [--glslc PATH]
"""
import argparse
import hashlib
import pathlib
import re
import subprocess
import sys

PIN = "c8dc91fabd00cac11d697ccf07426e798393cd40"


def sha(path: pathlib.Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def replace_once(tree: pathlib.Path, rel: str, old: str, new: str) -> None:
    path = tree / rel
    text = path.read_text(encoding="utf-8")
    if new in text:
        return
    if old not in text:
        raise SystemExit("prepare: pattern not found in %s:\n%s" % (rel, old[:200]))
    path.write_text(text.replace(old, new, 1), encoding="utf-8")


def git_head(tree: pathlib.Path) -> str:
    try:
        out = subprocess.check_output(["git", "-C", str(tree), "rev-parse", "HEAD"],
                                      stderr=subprocess.DEVNULL)
        return out.decode().strip()
    except Exception:
        return ""


def ensure_android_vulkan_header(tree: pathlib.Path) -> None:
    """dxvk-native 1.9.2b vendors Vulkan-Headers without vulkan_android.h.

    Fetch the file from the *same* Vulkan-Headers tag the tree vendors
    (v1.2.189). The NDK's copy is newer (VkFormatFeatureFlags2) and does not
    compile against this vulkan_core.h. The include in vulkan.h is quoted, so it
    must live in that directory.
    """
    dst = tree / "include/vulkan/vulkan_android.h"
    if dst.is_file():
        return
    import urllib.request
    url = ("https://raw.githubusercontent.com/KhronosGroup/Vulkan-Headers/"
           "v1.2.189/include/vulkan/vulkan_android.h")
    data = urllib.request.urlopen(url, timeout=60).read()
    if b"VK_KHR_ANDROID_SURFACE_EXTENSION_NAME" not in data:
        raise SystemExit("prepare: downloaded vulkan_android.h looks wrong")
    dst.write_bytes(data)
    print("prepare: added vulkan_android.h (Vulkan-Headers v1.2.189)")


def ensure_version_header(tree: pathlib.Path) -> None:
    """meson's vcs_tag() step: version.h from version.h.in with the git describe."""
    src = tree / "version.h.in"
    dst = tree / "version.h"
    tag = "native-1.9.2b"
    try:
        out = subprocess.check_output(["git", "-C", str(tree), "describe", "--dirty=+"],
                                      stderr=subprocess.DEVNULL)
        tag = out.decode().strip() or tag
    except Exception:
        pass
    dst.write_text(src.read_text(encoding="utf-8").replace("@VCS_TAG@", tag),
                   encoding="utf-8")
    print("prepare: version.h = %s" % tag)


def patch(tree: pathlib.Path) -> None:
    replace_once(tree, "src/util/util_bit.h", "#pragma once\n#include <cstdint>",
                 "#pragma once\n#include <cstdint>")

    # x86intrin.h is not available on arm64; the SSE2 block is x86-only too.
    replace_once(tree, "src/util/util_bit.h",
                 "#include <x86intrin.h>",
                 "#if defined(__i386__) || defined(__x86_64__)\n#include <x86intrin.h>\n#endif")

    # tzcnt: the x86 asm path has no arm64 equivalent.
    replace_once(tree, "src/util/util_bit.h",
                 "    #elif defined(__GNUC__) || defined(__clang__)\n    uint32_t res;",
                 "    #elif defined(__aarch64__) || defined(__arm__)\n"
                 "    return n ? __builtin_ctz(n) : 32;\n"
                 "    #elif defined(__GNUC__) || defined(__clang__)\n    uint32_t res;")

    # bcmpeq: SSE2 intrinsics are the fast path, and only on x86.
    replace_once(tree, "src/util/util_bit.h",
                 "#if defined(__GNUC__) || defined(__clang__) || defined(_MSC_VER)\n"
                 "    auto ai = reinterpret_cast<const __m128i*>(a);",
                 "#if defined(__i386__) || defined(__x86_64__)\n"
                 "    auto ai = reinterpret_cast<const __m128i*>(a);")

    replace_once(tree, "src/util/sync/sync_spinlock.h", "        _mm_pause();",
                 "        #if defined(__aarch64__) || defined(__arm__)\n"
                 "        asm volatile(\"yield\");\n"
                 "        #else\n        _mm_pause();\n        #endif")

    replace_once(tree, "src/util/util_vector.h",
                 "    __m128 value = _mm_load_ps(a.data);\n"
                 "    __m128 mask  = _mm_cmpeq_ps(value, value);\n"
                 "           value = _mm_and_ps(value, mask);\n"
                 "    _mm_store_ps(result.data, value);",
                 "    for (int i = 0; i < 4; i++)\n"
                 "      result.data[i] = (a.data[i] == a.data[i]) ? a.data[i] : 0.0f;")

    # Only ask for the features the device has. D3D9 code paths that need a
    # missing one degrade instead of failing device creation.
    for name in ("geometryShader", "depthClamp", "fillModeNonSolid",
                 "sampleRateShading", "shaderClipDistance", "shaderCullDistance",
                 "textureCompressionBC", "multiViewport", "independentBlend"):
        replace_once(tree, "src/d3d9/d3d9_device.cpp",
                     "enabled.core.features.%s = VK_TRUE;" % name,
                     "enabled.core.features.%s = supported.core.features.%s;" % (name, name))

    # DXVK's own log reaches stderr, which is redirected into log.txt already;
    # logcat is what a tester's bug report carries, so mirror warn/error.
    replace_once(tree, "src/util/log/log.cpp", '#include "log.h"\n',
                 '#include "log.h"\n#ifdef __ANDROID__\n#include <android/log.h>\n#endif\n')
    replace_once(tree, "src/util/log/log.cpp",
                 "        std::cerr << prefix << line << std::endl;\n",
                 "        std::cerr << prefix << line << std::endl;\n"
                 "#ifdef __ANDROID__\n"
                 "        if (level >= LogLevel::Warn)\n"
                 "          __android_log_print(level >= LogLevel::Error ? ANDROID_LOG_ERROR\n"
                 "                              : ANDROID_LOG_WARN, \"DXVK\", \"%s\", line.c_str());\n"
                 "#endif\n")

    # A half-created swapchain must not be destroyed twice.
    replace_once(tree, "src/vulkan/vulkan_presenter.cpp",
                 "    m_vkd->vkDestroySwapchainKHR(m_vkd->device(), m_swapchain, nullptr);",
                 "    if (m_swapchain != VK_NULL_HANDLE)\n"
                 "      m_vkd->vkDestroySwapchainKHR(m_vkd->device(), m_swapchain, nullptr);")
    replace_once(tree, "src/vulkan/vulkan_presenter.cpp",
                 "    m_vki->vkDestroySurfaceKHR(m_vki->instance(), m_surface, nullptr);",
                 "    if (m_surface != VK_NULL_HANDLE)\n"
                 "      m_vki->vkDestroySurfaceKHR(m_vki->instance(), m_surface, nullptr);\n"
                 "    m_surface = VK_NULL_HANDLE;")


def generate_shaders(tree: pathlib.Path, glslc: pathlib.Path) -> int:
    out = tree / "generated"
    out.mkdir(exist_ok=True)
    count = 0
    for pattern in ("*.vert", "*.frag", "*.comp", "*.geom", "*.tesc", "*.tese"):
        for src in list((tree / "src/dxvk/shaders").glob(pattern)) + \
                   list((tree / "src/dxvk/hud/shaders").glob(pattern)) + \
                   list((tree / "src/d3d9/shaders").glob(pattern)):
            name = src.stem
            tmp = out / (name + ".raw")
            cmd = [str(glslc), "--target-env=vulkan1.1", "-mfmt=c", "-o", str(tmp), str(src)]
            r = subprocess.run(cmd, capture_output=True, text=True)
            if r.returncode != 0:
                raise SystemExit("glslc failed for %s:\n%s" % (src, r.stderr))
            body = tmp.read_text(encoding="utf-8").strip()
            tmp.unlink()
            if body.endswith(";"):
                body = body[:-1]
            (out / (name + ".h")).write_text(
                "/* generated by tools/dxvk_prepare.py from %s */\n"
                "static const uint32_t %s[] = %s;\n" % (src.name, name, body),
                encoding="utf-8")
            count += 1
    return count


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("tree", type=pathlib.Path)
    ap.add_argument("--glslc", type=str, default="")
    args = ap.parse_args()
    tree = args.tree.resolve()
    if not (tree / "meson.build").is_file():
        raise SystemExit("not a dxvk-native checkout: %s" % tree)
    head = git_head(tree)
    if head and head != PIN:
        print("prepare: warning: HEAD is %s, pinned is %s" % (head, PIN))

    patch(tree)
    ensure_android_vulkan_header(tree)
    ensure_version_header(tree)

    glslc = pathlib.Path(args.glslc) if args.glslc else None
    if not glslc:
        # The NDK ships shaderc; find the newest installed one.
        sdk = pathlib.Path("C:/Users/yigit/OneDrive/Belgeler/korradecomp/android-sdk/ndk")
        cands = sorted(sdk.glob("*/shader-tools/windows-x86_64/glslc.exe")) if sdk.is_dir() else []
        glslc = cands[-1] if cands else None
    if glslc and glslc.is_file():
        n = generate_shaders(tree, glslc)
        print("prepare: generated %d shader headers with %s" % (n, glslc))
    else:
        raise SystemExit("prepare: glslc not found; pass --glslc")

    (tree / "ANDROID_PREPARE_SHA256").write_text(sha(pathlib.Path(__file__)) + "\n",
                                                 encoding="ascii")
    print("prepare: pinned %s, prepare hash %s" % (PIN, sha(pathlib.Path(__file__))[:16]))
    return 0


if __name__ == "__main__":
    sys.exit(main())
