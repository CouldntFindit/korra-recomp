/* android_wsi.cpp - DXVK-native's window-system layer for Android.
 *
 * DXVK has a win32 and an SDL2 WSI backend; neither exists here. This file
 * implements the same small contract (src/wsi/*.h) on VK_KHR_android_surface:
 * one surface bound to the shell's ANativeWindow, a fixed 1280x720 backbuffer
 * geometry that the compositor scales to the panel, and an HWND that is a
 * generation token instead of a pointer (docs/ANDROID_BRINGUP.md section 5.3).
 *
 * The Vulkan entry point is the system loader's. Selecting a custom driver
 * (libadrenotools/Turnip) is the S25 fallback and stays a one-line change here.
 */
#include <android/log.h>
#include <android/native_window.h>
#include <sys/system_properties.h>

#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <mutex>
#include <vector>

#include <windows.h>

#include "src/wsi/wsi_presenter.h"
#include "src/wsi/wsi_window.h"
#include "src/wsi/wsi_monitor.h"
#include "src/dxvk/dxvk_platform_exts.h"

namespace {

std::mutex g_lock;
ANativeWindow *g_window = nullptr;
/* The app's one window token (DXVK's HWND). Win32 keeps a single HWND for the
 * whole life of the app; the Android shell destroys and re-creates the
 * ANativeWindow behind it, and DXVK must not see that as a different window -
 * it would build a second presenter (and a second VkSurface on the same
 * ANativeWindow) while the first still exists, which the Xclipse driver
 * refuses with VK_ERROR_OUT_OF_HOST_MEMORY. The bind count is reported
 * separately, for the D3D9 bridge's lost/reset state. */
HWND g_handle = nullptr;
uintptr_t g_generation = 0;   /* number of window binds; 0 while no window */
uint32_t g_screen_w = 1280, g_screen_h = 720;
uint32_t g_buffer_w = 1280, g_buffer_h = 720;

/* A surface keeps its window alive until DXVK destroys the surface. */
std::vector<std::pair<VkSurfaceKHR, ANativeWindow *>> g_surfaces;

HMONITOR the_monitor() { return reinterpret_cast<HMONITOR>(uintptr_t{1}); }

/* Lifecycle trail: the window/surface pair is the object DXVK presents from, so
 * every bind, unbind and surface create/destroy is logged when
 *   adb shell setprop debug.korra.wsi 1
 * is set. Off by default; a property read happens only on those events, never
 * per frame. Logcat tag "Korra" is the same tag the shell uses. */
bool wsi_probe_on() {
    char v[PROP_VALUE_MAX] = {0};
    return __system_property_get("debug.korra.wsi", v) > 0 && v[0] == '1';
}

void wsi_log(const char *fmt, ...) {
    if (!wsi_probe_on()) return;
    char line[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    __android_log_write(ANDROID_LOG_INFO, "Korra", line);
}

/* Is this the app's window token? */
bool is_app_token(HWND window) {
    return window && window == g_handle;
}

}  // namespace

extern "C" void dxvkAndroidSetWindow(ANativeWindow *window) {
    std::lock_guard<std::mutex> guard(g_lock);
    if (window == g_window) return;
    if (window) {
        ANativeWindow_acquire(window);
        g_screen_w = (uint32_t)ANativeWindow_getWidth(window);
        g_screen_h = (uint32_t)ANativeWindow_getHeight(window);
        /* Present at 1280x720 and let SurfaceFlinger scale: the swapchain blit
         * then writes a quarter of the pixels a full-screen one would. */
        if (ANativeWindow_setBuffersGeometry(window, 1280, 720, 0) == 0) {
            g_buffer_w = 1280;
            g_buffer_h = 720;
        }
    }
    if (g_window) {
        wsi_log("[wsi] unbind win=%p token=%llx gen=%llu surfaces=%zu", (void *)g_window,
                (unsigned long long)(uintptr_t)g_handle,
                (unsigned long long)g_generation, g_surfaces.size());
        ANativeWindow_release(g_window);
    }
    g_window = window;
    if (window) {
        if (!g_handle) g_handle = reinterpret_cast<HWND>(uintptr_t{0x1001});
        g_generation++;
        wsi_log("[wsi] bind win=%p %ux%u buf=%ux%u token=%llx gen=%llu surfaces=%zu",
                (void *)window, g_screen_w, g_screen_h, g_buffer_w, g_buffer_h,
                (unsigned long long)(uintptr_t)g_handle,
                (unsigned long long)g_generation, g_surfaces.size());
    }
}

extern "C" HWND dxvkAndroidGetWindowHandle() {
    std::lock_guard<std::mutex> guard(g_lock);
    /* Same token for the app's life; null only while there is no window. */
    return g_window ? g_handle : nullptr;
}

/* Bind counter: 0 while the shell has no window, +1 per window bind. The D3D9
 * bridge compares it with the generation its device was reset against to
 * decide DEVICELOST / DEVICENOTRESET (d3d9_android.cpp). */
extern "C" unsigned dxvkAndroidWindowGeneration() {
    std::lock_guard<std::mutex> guard(g_lock);
    return g_window ? (unsigned)g_generation : 0u;
}

/* The D3D9 bridge asks this before it lets a Present reach DXVK: while the
 * shell has no ANativeWindow there is no surface to present to, and touching
 * the one built from a destroyed window is what kills the process. */
extern "C" int dxvkAndroidWindowAlive() {
    std::lock_guard<std::mutex> guard(g_lock);
    return g_window != nullptr;
}

extern "C" void dxvkAndroidSurfaceDestroyed(VkSurfaceKHR surface) {
    std::lock_guard<std::mutex> guard(g_lock);
    for (auto it = g_surfaces.begin(); it != g_surfaces.end(); ++it) {
        if (it->first == surface) {
            wsi_log("[wsi] surface destroy %llx (surfaces=%zu)",
                    (unsigned long long)(uintptr_t)surface, g_surfaces.size() - 1);
            ANativeWindow_release(it->second);
            g_surfaces.erase(it);
            return;
        }
    }
    wsi_log("[wsi] surface destroy %llx not tracked", (unsigned long long)(uintptr_t)surface);
}

namespace dxvk {

DxvkPlatformExts DxvkPlatformExts::s_instance;

std::string_view DxvkPlatformExts::getName() { return "Android WSI"; }

DxvkNameSet DxvkPlatformExts::getInstanceExtensions() {
    DxvkNameSet names;
    names.add(VK_KHR_SURFACE_EXTENSION_NAME);
    names.add(VK_KHR_ANDROID_SURFACE_EXTENSION_NAME);
    return names;
}

DxvkNameSet DxvkPlatformExts::getDeviceExtensions(uint32_t) { return {}; }
void DxvkPlatformExts::initInstanceExtensions() {}
void DxvkPlatformExts::initDeviceExtensions(const DxvkInstance *) {}

}  // namespace dxvk

namespace dxvk::wsi {

VkResult createSurface(HWND window, const Rc<vk::InstanceFn> &vki, VkSurfaceKHR *surface) {
    std::lock_guard<std::mutex> guard(g_lock);
    if (!g_window || !is_app_token(window)) {
        wsi_log("[wsi] surface create req=%llx cur=%llx win=%p -> SURFACE_LOST",
                (unsigned long long)(uintptr_t)window,
                (unsigned long long)(uintptr_t)g_handle, (void *)g_window);
        return VK_ERROR_SURFACE_LOST_KHR;
    }

    auto create = reinterpret_cast<PFN_vkCreateAndroidSurfaceKHR>(
        vkGetInstanceProcAddr(vki->instance(), "vkCreateAndroidSurfaceKHR"));
    if (!create) return VK_ERROR_EXTENSION_NOT_PRESENT;

    VkAndroidSurfaceCreateInfoKHR info{VK_STRUCTURE_TYPE_ANDROID_SURFACE_CREATE_INFO_KHR};
    info.window = g_window;
    VkResult r = create(vki->instance(), &info, nullptr, surface);
    if (r == VK_SUCCESS) {
        ANativeWindow_acquire(g_window);
        g_surfaces.emplace_back(*surface, g_window);
        wsi_log("[wsi] surface create %llx win=%p token=%llx surfaces=%zu",
                (unsigned long long)(uintptr_t)*surface, (void *)g_window,
                (unsigned long long)(uintptr_t)g_handle, g_surfaces.size());
    } else {
        wsi_log("[wsi] surface create win=%p -> VkResult %d", (void *)g_window, (int)r);
    }
    return r;
}

HMONITOR getDefaultMonitor() { return the_monitor(); }
HMONITOR enumMonitors(uint32_t index) { return index ? nullptr : the_monitor(); }

bool getDisplayName(HMONITOR handle, WCHAR (&name)[32]) {
    if (handle != the_monitor()) return false;
    const wchar_t display[] = L"\\\\.\\DISPLAY1";
    std::fill(std::begin(name), std::end(name), 0);
    std::copy(std::begin(display), std::end(display), name);
    return true;
}

bool getDesktopCoordinates(HMONITOR handle, RECT *rect) {
    if (handle != the_monitor() || !rect) return false;
    std::lock_guard<std::mutex> guard(g_lock);
    *rect = {0, 0, (LONG)g_screen_w, (LONG)g_screen_h};
    return true;
}

bool getCurrentDisplayMode(HMONITOR handle, WsiMode *mode) {
    if (handle != the_monitor() || !mode) return false;
    std::lock_guard<std::mutex> guard(g_lock);
    *mode = {g_buffer_w, g_buffer_h, {60, 1}, 32, false};
    return true;
}

bool getDesktopDisplayMode(HMONITOR handle, WsiMode *mode) {
    return getCurrentDisplayMode(handle, mode);
}

bool getDisplayMode(HMONITOR handle, uint32_t index, WsiMode *mode) {
    if (handle != the_monitor() || !mode) return false;
    static constexpr WsiMode modes[] = {
        {640, 480, {60, 1}, 32, false},   {800, 600, {60, 1}, 32, false},
        {1024, 768, {60, 1}, 32, false},  {1280, 720, {60, 1}, 32, false},
        {1280, 960, {60, 1}, 32, false},  {1600, 1200, {60, 1}, 32, false},
    };
    if (index >= sizeof(modes) / sizeof(modes[0])) return false;
    *mode = modes[index];
    return true;
}

void getWindowSize(HWND window, uint32_t *width, uint32_t *height) {
    std::lock_guard<std::mutex> guard(g_lock);
    const bool valid = g_window && is_app_token(window);
    if (width) *width = valid ? g_buffer_w : 0;
    if (height) *height = valid ? g_buffer_h : 0;
}

void resizeWindow(HWND window, DxvkWindowState *, uint32_t width, uint32_t height) {
    std::lock_guard<std::mutex> guard(g_lock);
    if (g_window && is_app_token(window) && width && height &&
        ANativeWindow_setBuffersGeometry(g_window, (int32_t)width, (int32_t)height, 0) == 0) {
        g_buffer_w = width;
        g_buffer_h = height;
    }
}

bool isWindow(HWND window) {
    std::lock_guard<std::mutex> guard(g_lock);
    return g_window && is_app_token(window);
}

HMONITOR getWindowMonitor(HWND window) { return isWindow(window) ? the_monitor() : nullptr; }

bool setWindowMode(HMONITOR monitor, HWND window, const WsiMode *, bool) {
    return monitor == the_monitor() && isWindow(window);
}

bool enterFullscreenMode(HMONITOR monitor, HWND window, DxvkWindowState *, bool) {
    return monitor == the_monitor() && isWindow(window);
}

bool leaveFullscreenMode(HWND window, DxvkWindowState *) { return isWindow(window); }

bool restoreDisplayMode(HMONITOR monitor) { return monitor == the_monitor(); }

}  // namespace dxvk::wsi
