#include "input.hpp"

#include "layer_common.hpp"

#include <X11/Xlib.h>
#include <X11/keysym.h>

#include <algorithm>
#include <map>
#include <mutex>
#include <vector>

namespace AutoHdrVk {
namespace {

std::mutex g_mutex;
Display *g_display = nullptr;
bool g_tried = false;

// Vulkan surface -> X11 Window (Xlib Window id == XCB window id).
std::map<void *, Window> g_surfaceWindows;
Window g_activeWindow = 0;

bool ensureDisplay()
{
    if (g_tried) {
        return g_display != nullptr;
    }
    g_tried = true;
    g_display = XOpenDisplay(nullptr);
    if (!g_display) {
        logf("X11 input unavailable (XOpenDisplay failed)");
        return false;
    }
    // Needed so QueryPointer sees current button state across clients.
    XSynchronize(g_display, False);
    logf("X11 input ready");
    return true;
}

bool keycodePressed(const char map[32], KeyCode kc)
{
    if (kc == 0) {
        return false;
    }
    return (map[kc >> 3] & (1 << (kc & 7))) != 0;
}

Window resolveTargetWindow()
{
    if (g_activeWindow != 0) {
        return g_activeWindow;
    }
    if (!g_surfaceWindows.empty()) {
        return g_surfaceWindows.begin()->second;
    }
    return DefaultRootWindow(g_display);
}

} // namespace

void registerX11Window(void *vkSurface, unsigned long window)
{
    if (!vkSurface || window == 0) {
        return;
    }
    std::lock_guard lock(g_mutex);
    g_surfaceWindows[vkSurface] = static_cast<Window>(window);
    g_activeWindow = static_cast<Window>(window);
    logf("X11 window registered for pointer hit-testing (0x%lx)", window);
}

void unregisterX11Window(void *vkSurface)
{
    if (!vkSurface) {
        return;
    }
    std::lock_guard lock(g_mutex);
    auto it = g_surfaceWindows.find(vkSurface);
    if (it == g_surfaceWindows.end()) {
        return;
    }
    if (g_activeWindow == it->second) {
        g_activeWindow = 0;
    }
    g_surfaceWindows.erase(it);
    if (g_activeWindow == 0 && !g_surfaceWindows.empty()) {
        g_activeWindow = g_surfaceWindows.begin()->second;
    }
}

bool x11KeysArePressed(const std::vector<uint32_t> &keys)
{
    std::lock_guard lock(g_mutex);
    if (!ensureDisplay() || keys.empty()) {
        return false;
    }
    char map[32] = {};
    XQueryKeymap(g_display, map);
    for (uint32_t ks : keys) {
        const KeyCode kc = XKeysymToKeycode(g_display, static_cast<KeySym>(ks));
        if (!keycodePressed(map, kc)) {
            return false;
        }
    }
    return true;
}

bool x11AnyKeyPressed(const std::vector<uint32_t> &keys)
{
    std::lock_guard lock(g_mutex);
    if (!ensureDisplay() || keys.empty()) {
        return false;
    }
    char map[32] = {};
    XQueryKeymap(g_display, map);
    for (uint32_t ks : keys) {
        const KeyCode kc = XKeysymToKeycode(g_display, static_cast<KeySym>(ks));
        if (keycodePressed(map, kc)) {
            return true;
        }
    }
    return false;
}

PointerState x11QueryPointer(uint32_t extentW, uint32_t extentH)
{
    PointerState out{};
    std::lock_guard lock(g_mutex);
    if (!ensureDisplay()) {
        return out;
    }

    const Window target = resolveTargetWindow();
    Window rootRet = 0;
    Window child = 0;
    int rootX = 0;
    int rootY = 0;
    int winX = 0;
    int winY = 0;
    unsigned int mask = 0;
    if (!XQueryPointer(g_display, target, &rootRet, &child, &rootX, &rootY, &winX, &winY, &mask)) {
        return out;
    }

    float scaleX = 1.0f;
    float scaleY = 1.0f;
    XWindowAttributes attrs{};
    if (XGetWindowAttributes(g_display, target, &attrs) && attrs.width > 0 && attrs.height > 0
        && extentW > 0 && extentH > 0) {
        scaleX = static_cast<float>(extentW) / static_cast<float>(attrs.width);
        scaleY = static_cast<float>(extentH) / static_cast<float>(attrs.height);
    }

    out.x = static_cast<float>(winX) * scaleX;
    out.y = static_cast<float>(winY) * scaleY;
    out.leftDown = (mask & Button1Mask) != 0;
    out.valid = true;
    return out;
}

} // namespace AutoHdrVk
