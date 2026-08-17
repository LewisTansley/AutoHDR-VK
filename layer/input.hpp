#pragma once

#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <optional>
#include <vector>

struct wl_display;

namespace AutoHdrVk {

// X11 / xkbcommon keysyms (same numeric values).
namespace Key {
constexpr uint32_t SuperL = 0xffeb;
constexpr uint32_t SuperR = 0xffec;
constexpr uint32_t MetaL = 0xffe7;
constexpr uint32_t MetaR = 0xffe8;
constexpr uint32_t ShiftL = 0xffe1;
constexpr uint32_t ShiftR = 0xffe2;
constexpr uint32_t H = 0x0048;
constexpr uint32_t h = 0x0068;
constexpr uint32_t Left = 0xff51;
constexpr uint32_t Up = 0xff52;
constexpr uint32_t Right = 0xff53;
constexpr uint32_t Down = 0xff54;
constexpr uint32_t Escape = 0xff1b;
constexpr uint32_t Tab = 0xff09;
} // namespace Key

struct PointerState {
    float x = 0.0f; // swapchain-pixel coordinates
    float y = 0.0f;
    bool leftDown = false;
    bool valid = false;
};

void pollInput();
bool keysArePressed(const std::vector<uint32_t> &keys);
bool keysArePressed(std::initializer_list<uint32_t> keys);
bool anyKeyPressed(std::initializer_list<uint32_t> keys);
bool isSuperDown();
bool isShiftDown();
bool isHDown();
PointerState queryPointer(uint32_t extentW, uint32_t extentH);

// Wayland: drive a virtual HUD cursor from relative-pointer while overlay is open
// (locked/relative mouse games). No-op on X11-only paths.
void setOverlayVirtualPointer(bool enabled, float seedX, float seedY);

void initWaylandInput(wl_display *display);
void unrefWaylandInput(wl_display *display);
void registerWaylandSurface(void *vkSurface, wl_display *display);
void unregisterWaylandSurface(void *vkSurface);

void registerX11Window(void *vkSurface, unsigned long window);
void unregisterX11Window(void *vkSurface);

} // namespace AutoHdrVk
