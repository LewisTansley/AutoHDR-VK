#include "input.hpp"

#include <vector>

namespace AutoHdrVk {

// Implemented in input_x11.cpp / input_wayland.cpp
bool x11KeysArePressed(const std::vector<uint32_t> &keys);
bool x11AnyKeyPressed(const std::vector<uint32_t> &keys);
PointerState x11QueryPointer(uint32_t extentW, uint32_t extentH);
void waylandPoll();
bool waylandKeysArePressed(const std::vector<uint32_t> &keys);
bool waylandAnyKeyPressed(const std::vector<uint32_t> &keys);
bool waylandActive();
PointerState waylandQueryPointer(uint32_t extentW, uint32_t extentH);

void pollInput()
{
    waylandPoll();
}

bool keysArePressed(const std::vector<uint32_t> &keys)
{
    if (waylandActive() && waylandKeysArePressed(keys)) {
        return true;
    }
    return x11KeysArePressed(keys);
}

bool keysArePressed(std::initializer_list<uint32_t> keys)
{
    return keysArePressed(std::vector<uint32_t>(keys));
}

bool anyKeyPressed(std::initializer_list<uint32_t> keys)
{
    const std::vector<uint32_t> v(keys);
    if (waylandActive() && waylandAnyKeyPressed(v)) {
        return true;
    }
    return x11AnyKeyPressed(v);
}

bool isSuperDown()
{
    return anyKeyPressed({Key::SuperL, Key::SuperR, Key::MetaL, Key::MetaR});
}

bool isShiftDown()
{
    return anyKeyPressed({Key::ShiftL, Key::ShiftR});
}

bool isHDown()
{
    return anyKeyPressed({Key::h, Key::H});
}

PointerState queryPointer(uint32_t extentW, uint32_t extentH)
{
    if (waylandActive()) {
        const PointerState wl = waylandQueryPointer(extentW, extentH);
        if (wl.valid) {
            return wl;
        }
    }
    return x11QueryPointer(extentW, extentH);
}

} // namespace AutoHdrVk
