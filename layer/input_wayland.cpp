#include "input.hpp"

#include "layer_common.hpp"

#include "relative-pointer-unstable-v1-client-protocol.h"

#include <wayland-client.h>
#include <xkbcommon/xkbcommon.h>

#include <sys/mman.h>
#include <unistd.h>

#include <algorithm>
#include <cstring>
#include <map>
#include <mutex>
#include <set>
#include <vector>

namespace AutoHdrVk {
namespace {

struct DisplayState {
    int ref = 0;
    wl_event_queue *queue = nullptr;
    wl_seat *seat = nullptr;
    wl_keyboard *keyboard = nullptr;
    wl_pointer *pointer = nullptr;
    zwp_relative_pointer_manager_v1 *relativeManager = nullptr;
    zwp_relative_pointer_v1 *relative = nullptr;
    xkb_keymap *keymap = nullptr;
    xkb_state *state = nullptr;
    std::set<uint32_t> pressed;
    std::set<void *> vkSurfaces;
    float pointerX = 0.0f;
    float pointerY = 0.0f;
    bool pointerInSurface = false;
    bool leftDown = false;
    int32_t pointerScale = 1;
    float virtualX = 0.0f;
    float virtualY = 0.0f;
    bool virtualActive = false;
    bool haveVirtual = false;
    uint32_t extentW = 0;
    uint32_t extentH = 0;
};

std::mutex g_mutex;
xkb_context *g_xkbCtx = nullptr;
std::map<wl_display *, DisplayState> g_displays;
std::map<void *, wl_display *> g_surfaceToDisplay;

DisplayState *stateFor(wl_display *display)
{
    auto it = g_displays.find(display);
    return it == g_displays.end() ? nullptr : &it->second;
}

void ensureXkb()
{
    if (!g_xkbCtx) {
        g_xkbCtx = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
    }
}

void clampVirtual(DisplayState &st)
{
    if (st.extentW > 0) {
        st.virtualX = std::clamp(st.virtualX, 0.0f, static_cast<float>(st.extentW - 1));
    }
    if (st.extentH > 0) {
        st.virtualY = std::clamp(st.virtualY, 0.0f, static_cast<float>(st.extentH - 1));
    }
}

void syncVirtualFromAbsolute(DisplayState &st)
{
    if (!st.virtualActive) {
        return;
    }
    st.virtualX = st.pointerX;
    st.virtualY = st.pointerY;
    st.haveVirtual = true;
    clampVirtual(st);
}

void attachRelativePointer(DisplayState *st)
{
    if (!st || !st->pointer || !st->relativeManager || st->relative) {
        return;
    }
    st->relative = zwp_relative_pointer_manager_v1_get_relative_pointer(st->relativeManager, st->pointer);
}

void keyboardKeymap(void *data, wl_keyboard *, uint32_t /*format*/, int32_t fd, uint32_t size)
{
    auto *st = static_cast<DisplayState *>(data);
    ensureXkb();
    char *mapShm = static_cast<char *>(mmap(nullptr, size, PROT_READ, MAP_PRIVATE, fd, 0));
    if (mapShm == MAP_FAILED) {
        close(fd);
        return;
    }
    if (st->keymap) {
        xkb_keymap_unref(st->keymap);
        st->keymap = nullptr;
    }
    if (st->state) {
        xkb_state_unref(st->state);
        st->state = nullptr;
    }
    st->keymap =
        xkb_keymap_new_from_string(g_xkbCtx, mapShm, XKB_KEYMAP_FORMAT_TEXT_V1, XKB_KEYMAP_COMPILE_NO_FLAGS);
    if (st->keymap) {
        st->state = xkb_state_new(st->keymap);
    }
    munmap(mapShm, size);
    close(fd);
}

void keyboardEnter(void *data, wl_keyboard *, uint32_t, wl_surface *, wl_array *keys)
{
    auto *st = static_cast<DisplayState *>(data);
    if (!st->state || !keys || !keys->data) {
        return;
    }
    const uint32_t *key = static_cast<const uint32_t *>(keys->data);
    const size_t count = keys->size / sizeof(uint32_t);
    for (size_t i = 0; i < count; ++i) {
        st->pressed.insert(xkb_state_key_get_one_sym(st->state, key[i] + 8));
    }
}

void keyboardLeave(void *data, wl_keyboard *, uint32_t, wl_surface *)
{
    static_cast<DisplayState *>(data)->pressed.clear();
}

void keyboardKey(void *data, wl_keyboard *, uint32_t, uint32_t, uint32_t key, uint32_t state)
{
    auto *st = static_cast<DisplayState *>(data);
    if (!st->state) {
        return;
    }
    const xkb_keysym_t sym = xkb_state_key_get_one_sym(st->state, key + 8);
    if (state == WL_KEYBOARD_KEY_STATE_PRESSED) {
        st->pressed.insert(sym);
    } else {
        st->pressed.erase(sym);
    }
}

void keyboardModifiers(void *data, wl_keyboard *, uint32_t, uint32_t depressed, uint32_t latched, uint32_t locked,
                       uint32_t group)
{
    auto *st = static_cast<DisplayState *>(data);
    if (!st->state) {
        return;
    }
    xkb_state_update_mask(st->state, depressed, latched, locked, 0, 0, group);
}

void keyboardRepeatInfo(void *, wl_keyboard *, int32_t, int32_t) {}

const wl_keyboard_listener g_keyboardListener = {
    .keymap = keyboardKeymap,
    .enter = keyboardEnter,
    .leave = keyboardLeave,
    .key = keyboardKey,
    .modifiers = keyboardModifiers,
    .repeat_info = keyboardRepeatInfo,
};

void pointerEnter(void *data, wl_pointer *, uint32_t /*serial*/, wl_surface *, wl_fixed_t sx, wl_fixed_t sy)
{
    auto *st = static_cast<DisplayState *>(data);
    st->pointerInSurface = true;
    st->pointerX = static_cast<float>(wl_fixed_to_double(sx));
    st->pointerY = static_cast<float>(wl_fixed_to_double(sy));
    syncVirtualFromAbsolute(*st);
}

void pointerLeave(void *data, wl_pointer *, uint32_t /*serial*/, wl_surface *)
{
    auto *st = static_cast<DisplayState *>(data);
    st->pointerInSurface = false;
    // Keep leftDown while virtual HUD cursor is active — games often leave/lock
    // the pointer when using relative look, and button state still arrives.
    if (!st->virtualActive) {
        st->leftDown = false;
    }
}

void pointerMotion(void *data, wl_pointer *, uint32_t /*time*/, wl_fixed_t sx, wl_fixed_t sy)
{
    auto *st = static_cast<DisplayState *>(data);
    st->pointerX = static_cast<float>(wl_fixed_to_double(sx));
    st->pointerY = static_cast<float>(wl_fixed_to_double(sy));
    syncVirtualFromAbsolute(*st);
}

void pointerButton(void *data, wl_pointer *, uint32_t /*serial*/, uint32_t /*time*/, uint32_t button, uint32_t state)
{
    auto *st = static_cast<DisplayState *>(data);
    // linux/input-event-codes.h BTN_LEFT == 0x110
    constexpr uint32_t kBtnLeft = 0x110;
    if (button == kBtnLeft) {
        st->leftDown = (state == WL_POINTER_BUTTON_STATE_PRESSED);
    }
}

void pointerAxis(void *, wl_pointer *, uint32_t, uint32_t, wl_fixed_t) {}
void pointerFrame(void *, wl_pointer *) {}
void pointerAxisSource(void *, wl_pointer *, uint32_t) {}
void pointerAxisStop(void *, wl_pointer *, uint32_t, uint32_t) {}
void pointerAxisDiscrete(void *, wl_pointer *, uint32_t, int32_t) {}

const wl_pointer_listener g_pointerListener = {
    .enter = pointerEnter,
    .leave = pointerLeave,
    .motion = pointerMotion,
    .button = pointerButton,
    .axis = pointerAxis,
    .frame = pointerFrame,
    .axis_source = pointerAxisSource,
    .axis_stop = pointerAxisStop,
    .axis_discrete = pointerAxisDiscrete,
};

void relativeMotion(void *data, zwp_relative_pointer_v1 *, uint32_t /*utime_hi*/, uint32_t /*utime_lo*/,
                    wl_fixed_t dx, wl_fixed_t dy, wl_fixed_t /*dx_unaccel*/, wl_fixed_t /*dy_unaccel*/)
{
    auto *st = static_cast<DisplayState *>(data);
    if (!st->virtualActive) {
        return;
    }
    st->virtualX += static_cast<float>(wl_fixed_to_double(dx));
    st->virtualY += static_cast<float>(wl_fixed_to_double(dy));
    st->haveVirtual = true;
    clampVirtual(*st);
}

const zwp_relative_pointer_v1_listener g_relativeListener = {
    .relative_motion = relativeMotion,
};

void attachRelativePointerWithListener(DisplayState *st)
{
    attachRelativePointer(st);
    if (st && st->relative) {
        zwp_relative_pointer_v1_add_listener(st->relative, &g_relativeListener, st);
    }
}

void seatCapabilities(void *data, wl_seat *seat, uint32_t caps)
{
    auto *st = static_cast<DisplayState *>(data);
    if ((caps & WL_SEAT_CAPABILITY_KEYBOARD) && !st->keyboard) {
        st->keyboard = wl_seat_get_keyboard(seat);
        if (st->keyboard) {
            wl_keyboard_add_listener(st->keyboard, &g_keyboardListener, st);
        }
    }
    if ((caps & WL_SEAT_CAPABILITY_POINTER) && !st->pointer) {
        st->pointer = wl_seat_get_pointer(seat);
        if (st->pointer) {
            wl_pointer_add_listener(st->pointer, &g_pointerListener, st);
            attachRelativePointerWithListener(st);
        }
    }
}

void seatName(void *, wl_seat *, const char *) {}

const wl_seat_listener g_seatListener = {
    .capabilities = seatCapabilities,
    .name = seatName,
};

void registryGlobal(void *data, wl_registry *registry, uint32_t name, const char *interface, uint32_t version)
{
    auto *st = static_cast<DisplayState *>(data);
    if (std::strcmp(interface, wl_seat_interface.name) == 0 && !st->seat) {
        const uint32_t ver = version < 5 ? version : 5;
        st->seat = static_cast<wl_seat *>(wl_registry_bind(registry, name, &wl_seat_interface, ver));
        if (st->seat) {
            wl_seat_add_listener(st->seat, &g_seatListener, st);
        }
    } else if (std::strcmp(interface, zwp_relative_pointer_manager_v1_interface.name) == 0 &&
               !st->relativeManager) {
        st->relativeManager = static_cast<zwp_relative_pointer_manager_v1 *>(
            wl_registry_bind(registry, name, &zwp_relative_pointer_manager_v1_interface, 1));
    }
}

void registryGlobalRemove(void *, wl_registry *, uint32_t) {}

const wl_registry_listener g_registryListener = {
    .global = registryGlobal,
    .global_remove = registryGlobalRemove,
};

void destroyDisplayState(DisplayState &st)
{
    if (st.relative) {
        zwp_relative_pointer_v1_destroy(st.relative);
        st.relative = nullptr;
    }
    if (st.relativeManager) {
        zwp_relative_pointer_manager_v1_destroy(st.relativeManager);
        st.relativeManager = nullptr;
    }
    if (st.pointer) {
        wl_pointer_destroy(st.pointer);
        st.pointer = nullptr;
    }
    if (st.keyboard) {
        wl_keyboard_destroy(st.keyboard);
        st.keyboard = nullptr;
    }
    if (st.seat) {
        wl_seat_destroy(st.seat);
        st.seat = nullptr;
    }
    if (st.queue) {
        wl_event_queue_destroy(st.queue);
        st.queue = nullptr;
    }
    if (st.state) {
        xkb_state_unref(st.state);
        st.state = nullptr;
    }
    if (st.keymap) {
        xkb_keymap_unref(st.keymap);
        st.keymap = nullptr;
    }
    st.pressed.clear();
    st.virtualActive = false;
    st.haveVirtual = false;
}

} // namespace

void initWaylandInput(wl_display *display)
{
    if (!display) {
        return;
    }
    std::lock_guard lock(g_mutex);
    ensureXkb();
    if (!g_xkbCtx) {
        logf("Wayland input unavailable (xkb_context_new failed)");
        return;
    }
    auto &st = g_displays[display];
    if (st.queue) {
        st.ref++;
        return;
    }
    st.ref = 1;
    st.queue = wl_display_create_queue(display);
    if (!st.queue) {
        g_displays.erase(display);
        return;
    }
    wl_display *wrapped = static_cast<wl_display *>(wl_proxy_create_wrapper(display));
    wl_proxy_set_queue(reinterpret_cast<wl_proxy *>(wrapped), st.queue);
    wl_registry *registry = wl_display_get_registry(wrapped);
    wl_proxy_wrapper_destroy(wrapped);
    if (!registry) {
        destroyDisplayState(st);
        g_displays.erase(display);
        return;
    }
    wl_registry_add_listener(registry, &g_registryListener, &st);
    wl_display_roundtrip_queue(display, st.queue);
    // Seat capabilities arrive after the first roundtrip; attach relative pointer then.
    if (st.pointer && st.relativeManager && !st.relative) {
        attachRelativePointerWithListener(&st);
    }
    wl_display_roundtrip_queue(display, st.queue);
    wl_registry_destroy(registry);
    logf("Wayland keyboard input attached%s", st.relativeManager ? " (relative pointer available)" : "");
}

void unrefWaylandInput(wl_display *display)
{
    if (!display) {
        return;
    }
    std::lock_guard lock(g_mutex);
    auto it = g_displays.find(display);
    if (it == g_displays.end()) {
        return;
    }
    it->second.ref--;
    if (it->second.ref > 0) {
        return;
    }
    destroyDisplayState(it->second);
    g_displays.erase(it);
}

void registerWaylandSurface(void *vkSurface, wl_display *display)
{
    if (!vkSurface || !display) {
        return;
    }
    {
        std::lock_guard lock(g_mutex);
        g_surfaceToDisplay[vkSurface] = display;
    }
    initWaylandInput(display);
    std::lock_guard lock(g_mutex);
    if (auto *st = stateFor(display)) {
        st->vkSurfaces.insert(vkSurface);
    }
}

void unregisterWaylandSurface(void *vkSurface)
{
    if (!vkSurface) {
        return;
    }
    wl_display *display = nullptr;
    {
        std::lock_guard lock(g_mutex);
        auto it = g_surfaceToDisplay.find(vkSurface);
        if (it == g_surfaceToDisplay.end()) {
            return;
        }
        display = it->second;
        g_surfaceToDisplay.erase(it);
        if (auto *st = stateFor(display)) {
            st->vkSurfaces.erase(vkSurface);
        }
    }
    unrefWaylandInput(display);
}

void waylandPoll()
{
    std::lock_guard lock(g_mutex);
    for (auto &entry : g_displays) {
        if (entry.second.queue) {
            wl_display_dispatch_queue_pending(entry.first, entry.second.queue);
        }
    }
}

bool waylandKeysArePressed(const std::vector<uint32_t> &keys)
{
    if (keys.empty()) {
        return false;
    }
    std::lock_guard lock(g_mutex);
    for (auto &entry : g_displays) {
        size_t hit = 0;
        for (uint32_t ks : keys) {
            if (entry.second.pressed.count(ks)) {
                ++hit;
            }
        }
        if (hit == keys.size()) {
            return true;
        }
    }
    return false;
}

bool waylandAnyKeyPressed(const std::vector<uint32_t> &keys)
{
    if (keys.empty()) {
        return false;
    }
    std::lock_guard lock(g_mutex);
    for (auto &entry : g_displays) {
        for (uint32_t ks : keys) {
            if (entry.second.pressed.count(ks)) {
                return true;
            }
        }
    }
    return false;
}

bool waylandActive()
{
    std::lock_guard lock(g_mutex);
    return !g_displays.empty();
}

void waylandSetOverlayVirtualPointer(bool enabled, float seedX, float seedY)
{
    std::lock_guard lock(g_mutex);
    for (auto &entry : g_displays) {
        DisplayState &st = entry.second;
        st.virtualActive = enabled;
        if (enabled) {
            st.virtualX = seedX;
            st.virtualY = seedY;
            st.haveVirtual = true;
            clampVirtual(st);
            if (st.pointer && st.relativeManager && !st.relative) {
                attachRelativePointerWithListener(&st);
            }
        } else {
            st.haveVirtual = false;
            if (!st.pointerInSurface) {
                st.leftDown = false;
            }
        }
    }
}

PointerState waylandQueryPointer(uint32_t extentW, uint32_t extentH)
{
    PointerState out{};
    std::lock_guard lock(g_mutex);
    for (auto &entry : g_displays) {
        DisplayState &st = entry.second;
        st.extentW = extentW;
        st.extentH = extentH;
        if (st.virtualActive && st.haveVirtual) {
            clampVirtual(st);
            out.x = st.virtualX;
            out.y = st.virtualY;
            out.leftDown = st.leftDown;
            out.valid = true;
            return out;
        }
        if (!st.pointerInSurface) {
            continue;
        }
        // wl_pointer coords are surface-local; for Vulkan WSI these usually match the
        // logical surface size. Scale into swapchain pixels when extents differ.
        out.x = st.pointerX;
        out.y = st.pointerY;
        if (extentW > 0 && extentH > 0) {
            // Heuristic: if pointer looks like it's in a smaller logical space, leave as-is;
            // most games report surface coords already in buffer pixels.
            (void)extentW;
            (void)extentH;
        }
        out.leftDown = st.leftDown;
        out.valid = true;
        return out;
    }
    return out;
}

} // namespace AutoHdrVk
