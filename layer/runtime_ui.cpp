#include "runtime_ui.hpp"

#include "config.hpp"
#include "input.hpp"
#include "layer_common.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <mutex>

namespace AutoHdrVk {

namespace {

using Clock = std::chrono::steady_clock;

struct RuntimeState {
    bool seeded = false;
    bool effectOn = true;
    bool overlayVisible = false;
    bool virtualPointerArmed = false;
    bool hasDraft = false;
    AutoHdr::CalibrationSettings baseline{};
    AutoHdr::CalibrationSettings draft{};
    FocusedSlider focused = FocusedSlider::Intensity;
    Clock::time_point lastChord{};
    Clock::time_point lastNav{};
    Clock::time_point lastAdjust{};
    bool prevSuperH = false;
    bool dragging = false;
    bool pointerValid = false;
    float pointerX = 0.0f;
    float pointerY = 0.0f;
};

std::mutex g_mutex;
RuntimeState g_state;

constexpr auto kChordDelay = std::chrono::milliseconds(350);
constexpr auto kNavDelay = std::chrono::milliseconds(180);
constexpr auto kAdjustDelay = std::chrono::milliseconds(50);

float sliderToDebandStrength(float slider)
{
    return AutoHdr::clampDebandStrength(slider);
}

float debandStrengthToSlider(float strength)
{
    return AutoHdr::clampDebandStrength(strength);
}

void seedLocked()
{
    if (g_state.seeded) {
        return;
    }
    g_state.effectOn = isEffectActiveForCurrentProcess();
    g_state.baseline = activeSettings();
    g_state.draft = g_state.baseline;
    g_state.seeded = true;
}

bool chordReady(Clock::time_point now, Clock::time_point &last)
{
    if (now - last < kChordDelay) {
        return false;
    }
    last = now;
    return true;
}

void openOverlayLocked()
{
    g_state.baseline = activeSettings();
    g_state.draft = g_state.baseline;
    g_state.hasDraft = true;
    g_state.overlayVisible = true;
    g_state.virtualPointerArmed = false;
    g_state.focused = FocusedSlider::Intensity;
    g_state.dragging = false;
    logf("overlay open intensity=%.2f color=%.2f shape=%.2f floor=%.2f stretch=%.2f smooth=%.2f",
         g_state.draft.intensity, g_state.draft.colorIntensity, g_state.draft.expansionShape, g_state.draft.blackFloor,
         g_state.draft.highlightStretch, g_state.draft.debandStrength);
}

void disarmVirtualPointerLocked()
{
    if (!g_state.virtualPointerArmed) {
        return;
    }
    setOverlayVirtualPointer(false, 0.0f, 0.0f);
    g_state.virtualPointerArmed = false;
}

void closeOverlayConfirmLocked()
{
    if (!g_state.overlayVisible) {
        return;
    }
    const float intensity = AutoHdr::clampIntensity(g_state.draft.intensity);
    const float color = AutoHdr::clampColorIntensity(g_state.draft.colorIntensity);
    const float shape = AutoHdr::clampExpansionShape(g_state.draft.expansionShape);
    const float floor = AutoHdr::clampBlackFloor(g_state.draft.blackFloor);
    const float stretch = AutoHdr::clampHighlightStretch(g_state.draft.highlightStretch);
    const float smooth = AutoHdr::clampDebandStrength(g_state.draft.debandStrength);
    saveOverlaySettings(intensity, color, shape, floor, stretch, smooth);
    g_state.baseline = activeSettings();
    g_state.draft = g_state.baseline;
    g_state.hasDraft = false;
    g_state.overlayVisible = false;
    g_state.dragging = false;
    disarmVirtualPointerLocked();
    logf("overlay saved intensity=%.2f color=%.2f shape=%.2f floor=%.2f stretch=%.2f smooth=%.2f", intensity, color,
         shape, floor, stretch, smooth);
}

void closeOverlayCancelLocked()
{
    if (!g_state.overlayVisible) {
        return;
    }
    g_state.draft = g_state.baseline;
    g_state.hasDraft = false;
    g_state.overlayVisible = false;
    g_state.dragging = false;
    disarmVirtualPointerLocked();
    logf("overlay cancelled");
}

void adjustFocusedLocked(float delta)
{
    if (g_state.focused == FocusedSlider::Intensity) {
        g_state.draft.intensity = AutoHdr::clampIntensity(g_state.draft.intensity + delta);
    } else if (g_state.focused == FocusedSlider::ColorIntensity) {
        g_state.draft.colorIntensity = AutoHdr::clampColorIntensity(g_state.draft.colorIntensity + delta);
    } else if (g_state.focused == FocusedSlider::BlackFloor) {
        g_state.draft.blackFloor = AutoHdr::clampBlackFloor(g_state.draft.blackFloor + delta);
    } else if (g_state.focused == FocusedSlider::HighlightStretch) {
        g_state.draft.highlightStretch = AutoHdr::clampHighlightStretch(g_state.draft.highlightStretch + delta);
    } else if (g_state.focused == FocusedSlider::DitherStrength) {
        g_state.draft.debandStrength =
            sliderToDebandStrength(debandStrengthToSlider(g_state.draft.debandStrength) + delta);
    } else {
        g_state.draft.expansionShape = AutoHdr::clampExpansionShape(g_state.draft.expansionShape + delta);
    }
    g_state.hasDraft = true;
}

float adjustStepForFocused(FocusedSlider focused, bool shift)
{
    if (focused == FocusedSlider::BlackFloor) {
        return shift ? 0.001f : 0.005f;
    }
    if (focused == FocusedSlider::DitherStrength) {
        return shift ? 0.01f : 0.05f;
    }
    return shift ? 0.01f : 0.05f;
}

FocusedSlider nextSlider(FocusedSlider cur)
{
    switch (cur) {
    case FocusedSlider::Intensity:
        return FocusedSlider::ExpansionShape;
    case FocusedSlider::ExpansionShape:
        return FocusedSlider::HighlightStretch;
    case FocusedSlider::HighlightStretch:
        return FocusedSlider::BlackFloor;
    case FocusedSlider::BlackFloor:
        return FocusedSlider::ColorIntensity;
    case FocusedSlider::ColorIntensity:
        return FocusedSlider::DitherStrength;
    case FocusedSlider::DitherStrength:
        return FocusedSlider::Intensity;
    }
    return FocusedSlider::Intensity;
}

FocusedSlider prevSlider(FocusedSlider cur)
{
    switch (cur) {
    case FocusedSlider::Intensity:
        return FocusedSlider::DitherStrength;
    case FocusedSlider::ExpansionShape:
        return FocusedSlider::Intensity;
    case FocusedSlider::HighlightStretch:
        return FocusedSlider::ExpansionShape;
    case FocusedSlider::BlackFloor:
        return FocusedSlider::HighlightStretch;
    case FocusedSlider::ColorIntensity:
        return FocusedSlider::BlackFloor;
    case FocusedSlider::DitherStrength:
        return FocusedSlider::ColorIntensity;
    }
    return FocusedSlider::Intensity;
}

// Panel geometry in pixels (must match overlay.comp).
struct PanelGeom {
    float x0, y0, x1, y1;
    float trackX0, trackX1;
    float intensityY, shapeY, stretchY, floorY, colorY, smoothY;
    float trackH;
};

PanelGeom makePanel(uint32_t w, uint32_t h)
{
    const float pw = std::min(520.0f, static_cast<float>(w) * 0.55f);
    const float ph = 250.0f;
    const float x0 = (static_cast<float>(w) - pw) * 0.5f;
    const float y0 = static_cast<float>(h) - ph - 48.0f;
    PanelGeom g{};
    g.x0 = x0;
    g.y0 = y0;
    g.x1 = x0 + pw;
    g.y1 = y0 + ph;
    g.trackX0 = x0 + 140.0f;
    g.trackX1 = x0 + pw - 16.0f;
    // Row order must match overlay.comp: Intensity, Shape, Stretch, Floor, Color, Smooth.
    g.intensityY = y0 + 34.0f;
    g.shapeY = y0 + 68.0f;
    g.stretchY = y0 + 102.0f;
    g.floorY = y0 + 136.0f;
    g.colorY = y0 + 170.0f;
    g.smoothY = y0 + 204.0f;
    g.trackH = 14.0f;
    return g;
}

void armVirtualPointerLocked(uint32_t w, uint32_t h)
{
    if (!g_state.overlayVisible || g_state.virtualPointerArmed || w == 0 || h == 0) {
        return;
    }
    const PanelGeom g = makePanel(w, h);
    const float seedX = (g.trackX0 + g.trackX1) * 0.5f;
    const float seedY = g.intensityY + g.trackH * 0.5f;
    setOverlayVirtualPointer(true, seedX, seedY);
    g_state.virtualPointerArmed = true;
}

void handlePointerLocked(uint32_t w, uint32_t h)
{
    if (!g_state.overlayVisible || w == 0 || h == 0) {
        g_state.dragging = false;
        return;
    }
    const PointerState ptr = queryPointer(w, h);
    g_state.pointerValid = ptr.valid;
    g_state.pointerX = ptr.x;
    g_state.pointerY = ptr.y;
    if (!ptr.valid) {
        g_state.dragging = false;
        return;
    }
    const PanelGeom g = makePanel(w, h);

    const float row0Top = g.intensityY - 12.0f;
    const float row0Bot = g.intensityY + g.trackH + 12.0f;
    const float row1Top = g.shapeY - 12.0f;
    const float row1Bot = g.shapeY + g.trackH + 12.0f;
    const float row2Top = g.stretchY - 12.0f;
    const float row2Bot = g.stretchY + g.trackH + 12.0f;
    const float row3Top = g.floorY - 12.0f;
    const float row3Bot = g.floorY + g.trackH + 12.0f;
    const float row4Top = g.colorY - 12.0f;
    const float row4Bot = g.colorY + g.trackH + 12.0f;
    const float row5Top = g.smoothY - 12.0f;
    const float row5Bot = g.smoothY + g.trackH + 12.0f;
    const bool inPanel = ptr.x >= g.x0 && ptr.x <= g.x1 && ptr.y >= g.y0 && ptr.y <= g.y1;
    const bool inRow0 = inPanel && ptr.y >= row0Top && ptr.y <= row0Bot;
    const bool inRow1 = inPanel && ptr.y >= row1Top && ptr.y <= row1Bot;
    const bool inRow2 = inPanel && ptr.y >= row2Top && ptr.y <= row2Bot;
    const bool inRow3 = inPanel && ptr.y >= row3Top && ptr.y <= row3Bot;
    const bool inRow4 = inPanel && ptr.y >= row4Top && ptr.y <= row4Bot;
    const bool inRow5 = inPanel && ptr.y >= row5Top && ptr.y <= row5Bot;

    if (!ptr.leftDown) {
        g_state.dragging = false;
        return;
    }

    if (!g_state.dragging) {
        if (inRow0) {
            g_state.focused = FocusedSlider::Intensity;
            g_state.dragging = true;
        } else if (inRow1) {
            g_state.focused = FocusedSlider::ExpansionShape;
            g_state.dragging = true;
        } else if (inRow2) {
            g_state.focused = FocusedSlider::HighlightStretch;
            g_state.dragging = true;
        } else if (inRow3) {
            g_state.focused = FocusedSlider::BlackFloor;
            g_state.dragging = true;
        } else if (inRow4) {
            g_state.focused = FocusedSlider::ColorIntensity;
            g_state.dragging = true;
        } else if (inRow5) {
            g_state.focused = FocusedSlider::DitherStrength;
            g_state.dragging = true;
        } else if (!inPanel) {
            return;
        }
    }

    if (g_state.dragging) {
        const float t = std::clamp((ptr.x - g.trackX0) / std::max(g.trackX1 - g.trackX0, 1.0f), 0.0f, 1.0f);
        if (g_state.focused == FocusedSlider::Intensity) {
            g_state.draft.intensity = t;
        } else if (g_state.focused == FocusedSlider::ColorIntensity) {
            g_state.draft.colorIntensity = t;
        } else if (g_state.focused == FocusedSlider::BlackFloor) {
            g_state.draft.blackFloor = t;
        } else if (g_state.focused == FocusedSlider::HighlightStretch) {
            g_state.draft.highlightStretch = t * 2.0f;
        } else if (g_state.focused == FocusedSlider::DitherStrength) {
            g_state.draft.debandStrength = sliderToDebandStrength(t);
        } else {
            g_state.draft.expansionShape = t;
        }
        g_state.hasDraft = true;
    }
}

} // namespace

void updateRuntimeUi(uint32_t extentWidth, uint32_t extentHeight, float /*outputMode*/)
{
    pollInput();

    const bool super = isSuperDown();
    const bool shift = isShiftDown();
    const bool hKey = isHDown();
    const bool superH = super && hKey;

    std::lock_guard lock(g_mutex);
    seedLocked();
    const auto now = Clock::now();

    if (superH && !g_state.prevSuperH && chordReady(now, g_state.lastChord)) {
        if (g_state.overlayVisible) {
            closeOverlayConfirmLocked();
        } else {
            openOverlayLocked();
        }
    }
    g_state.prevSuperH = superH;

    if (!g_state.overlayVisible) {
        return;
    }

    if (anyKeyPressed({Key::Escape}) && now - g_state.lastNav >= kNavDelay) {
        g_state.lastNav = now;
        closeOverlayCancelLocked();
        return;
    }

    if ((anyKeyPressed({Key::Tab}) || anyKeyPressed({Key::Down})) && now - g_state.lastNav >= kNavDelay) {
        g_state.lastNav = now;
        g_state.focused = nextSlider(g_state.focused);
    }
    if (anyKeyPressed({Key::Up}) && now - g_state.lastNav >= kNavDelay) {
        g_state.lastNav = now;
        g_state.focused = prevSlider(g_state.focused);
    }

    const float step = adjustStepForFocused(g_state.focused, shift);
    if (anyKeyPressed({Key::Left}) && now - g_state.lastAdjust >= kAdjustDelay) {
        g_state.lastAdjust = now;
        adjustFocusedLocked(-step);
    }
    if (anyKeyPressed({Key::Right}) && now - g_state.lastAdjust >= kAdjustDelay) {
        g_state.lastAdjust = now;
        adjustFocusedLocked(step);
    }

    armVirtualPointerLocked(extentWidth, extentHeight);
    handlePointerLocked(extentWidth, extentHeight);
}

bool runtimeEffectOn()
{
    std::lock_guard lock(g_mutex);
    seedLocked();
    return g_state.effectOn;
}

bool overlayVisible()
{
    std::lock_guard lock(g_mutex);
    seedLocked();
    return g_state.overlayVisible;
}

bool shouldProcessPresent()
{
    std::lock_guard lock(g_mutex);
    seedLocked();
    return g_state.effectOn || g_state.overlayVisible;
}

AutoHdr::CalibrationSettings effectiveSettings()
{
    std::lock_guard lock(g_mutex);
    seedLocked();
    if (g_state.overlayVisible || g_state.hasDraft) {
        return g_state.draft;
    }
    return activeSettings();
}

OverlayDrawState overlayDrawState()
{
    std::lock_guard lock(g_mutex);
    seedLocked();
    OverlayDrawState s{};
    s.visible = g_state.overlayVisible;
    s.intensity = g_state.draft.intensity;
    s.colorIntensity = g_state.draft.colorIntensity;
    s.expansionShape = g_state.draft.expansionShape;
    s.blackFloor = g_state.draft.blackFloor;
    s.highlightStretch = g_state.draft.highlightStretch;
    s.debandStrength = g_state.draft.debandStrength;
    s.focused = static_cast<int>(g_state.focused);
    s.panelNits = 203.0f;
    s.pointerValid = g_state.pointerValid;
    s.pointerX = g_state.pointerX;
    s.pointerY = g_state.pointerY;
    return s;
}

} // namespace AutoHdrVk
