#include "runtime_ui.hpp"

#include "config.hpp"
#include "input.hpp"
#include "layer_common.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <mutex>
#include <string>

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

bool envFlag(const char *name)
{
    const char *env = std::getenv(name);
    return env && *env && std::string(env) != "0";
}

bool showIntensitySlider()
{
    static const bool show = envFlag("AUTOHDR_SHOW_INTENSITY");
    return show;
}

bool showFinishingSliders()
{
    static const bool show = envFlag("AUTOHDR_SHOW_FINISHING");
    return show;
}

bool splitCurveSliders()
{
    return g_state.draft.splitCurveSliders;
}

void setCoupledCurve(float t)
{
    AutoHdr::applyCoupledExpansion(t, g_state.draft.expansionShape, g_state.draft.expansionScurve);
}

FocusedSlider defaultFocus()
{
    return showIntensitySlider() ? FocusedSlider::Intensity : FocusedSlider::ExpansionShape;
}

FocusedSlider lastSlider()
{
    return showFinishingSliders() ? FocusedSlider::DitherStrength : FocusedSlider::HighlightBlowout;
}

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
    g_state.focused = defaultFocus();
    g_state.dragging = false;
    logf("overlay open intensity=%.2f color=%.2f shape=%.2f scurve=%.2f knee=%.2f floor=%.2f stretch=%.2f white=%.2f sep=%.2f blowout=%.2f soften=%.2f smooth=%.2f",
         g_state.draft.intensity, g_state.draft.colorIntensity, g_state.draft.expansionShape,
         g_state.draft.expansionScurve, g_state.draft.expansionMid, g_state.draft.blackFloor,
         g_state.draft.highlightStretch, g_state.draft.stretchWhiteRef, g_state.draft.separation,
         g_state.draft.highlightBlowout, g_state.draft.highlightSoftness, g_state.draft.debandStrength);
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
    const float intensity = AutoHdr::clampIntensity(
        showIntensitySlider() ? g_state.draft.intensity : g_state.baseline.intensity);
    const float color = AutoHdr::clampColorIntensity(g_state.draft.colorIntensity);
    const float shape = AutoHdr::clampExpansionShape(g_state.draft.expansionShape);
    const float scurve = AutoHdr::clampExpansionScurve(g_state.draft.expansionScurve);
    const float knee = AutoHdr::clampExpansionMid(g_state.draft.expansionMid);
    const float floor = AutoHdr::clampBlackFloor(g_state.draft.blackFloor);
    const float stretch = std::min(AutoHdr::clampHighlightStretch(g_state.draft.highlightStretch), 1.0f);
    const float white = AutoHdr::clampStretchWhiteRef(g_state.draft.stretchWhiteRef);
    const float sep = AutoHdr::clampSeparation(g_state.draft.separation);
    const float blowout = AutoHdr::clampHighlightBlowout(g_state.draft.highlightBlowout);
    const float soften = AutoHdr::clampHighlightSoftness(
        showFinishingSliders() ? g_state.draft.highlightSoftness : g_state.baseline.highlightSoftness);
    const float smooth = AutoHdr::clampDebandStrength(
        showFinishingSliders() ? g_state.draft.debandStrength : g_state.baseline.debandStrength);
    saveOverlaySettings(intensity, color, shape, scurve, knee, floor, stretch, white, sep, blowout, soften,
                        smooth);
    g_state.baseline = activeSettings();
    g_state.draft = g_state.baseline;
    g_state.hasDraft = false;
    g_state.overlayVisible = false;
    g_state.dragging = false;
    disarmVirtualPointerLocked();
    logf("overlay saved intensity=%.2f color=%.2f shape=%.2f scurve=%.2f knee=%.2f floor=%.2f stretch=%.2f white=%.2f sep=%.2f blowout=%.2f soften=%.2f smooth=%.2f",
         intensity, color, shape, scurve, knee, floor, stretch, white, sep, blowout, soften, smooth);
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
        if (showIntensitySlider()) {
            g_state.draft.intensity = AutoHdr::clampIntensity(g_state.draft.intensity + delta);
        }
    } else if (g_state.focused == FocusedSlider::ColorIntensity) {
        g_state.draft.colorIntensity = AutoHdr::clampColorIntensity(g_state.draft.colorIntensity + delta);
    } else if (g_state.focused == FocusedSlider::BlackFloor) {
        g_state.draft.blackFloor = AutoHdr::clampBlackFloor(g_state.draft.blackFloor + delta);
    } else if (g_state.focused == FocusedSlider::HighlightStretch) {
        g_state.draft.highlightStretch =
            std::min(AutoHdr::clampHighlightStretch(g_state.draft.highlightStretch + delta), 1.0f);
    } else if (g_state.focused == FocusedSlider::StretchWhiteRef) {
        g_state.draft.stretchWhiteRef = AutoHdr::clampStretchWhiteRef(g_state.draft.stretchWhiteRef + delta);
    } else if (g_state.focused == FocusedSlider::ExpansionMid) {
        g_state.draft.expansionMid = AutoHdr::clampExpansionMid(g_state.draft.expansionMid + delta);
    } else if (g_state.focused == FocusedSlider::HighlightBlowout) {
        g_state.draft.highlightBlowout = AutoHdr::clampHighlightBlowout(g_state.draft.highlightBlowout + delta);
    } else if (g_state.focused == FocusedSlider::HighlightSoftness) {
        if (showFinishingSliders()) {
            g_state.draft.highlightSoftness =
                AutoHdr::clampHighlightSoftness(g_state.draft.highlightSoftness + delta);
        }
    } else if (g_state.focused == FocusedSlider::DitherStrength) {
        if (showFinishingSliders()) {
            g_state.draft.debandStrength =
                sliderToDebandStrength(debandStrengthToSlider(g_state.draft.debandStrength) + delta);
        }
    } else if (g_state.focused == FocusedSlider::ExpansionScurve) {
        if (splitCurveSliders()) {
            g_state.draft.expansionScurve = AutoHdr::clampExpansionScurve(g_state.draft.expansionScurve + delta);
        } else {
            setCoupledCurve(AutoHdr::expansionCurveT(g_state.draft.expansionShape, g_state.draft.expansionScurve) + delta);
        }
    } else if (splitCurveSliders()) {
        g_state.draft.expansionShape = AutoHdr::clampExpansionShape(g_state.draft.expansionShape + delta);
    } else {
        setCoupledCurve(AutoHdr::expansionCurveT(g_state.draft.expansionShape, g_state.draft.expansionScurve) + delta);
    }
    g_state.hasDraft = true;
}

float adjustStepForFocused(FocusedSlider /*focused*/, bool shift)
{
    return shift ? 0.05f : 0.005f;
}

FocusedSlider nextSlider(FocusedSlider cur)
{
    switch (cur) {
    case FocusedSlider::Intensity:
        return FocusedSlider::ExpansionShape;
    case FocusedSlider::ExpansionShape:
        return splitCurveSliders() ? FocusedSlider::ExpansionScurve : FocusedSlider::ExpansionMid;
    case FocusedSlider::ExpansionScurve:
        return FocusedSlider::ExpansionMid;
    case FocusedSlider::ExpansionMid:
        return FocusedSlider::HighlightStretch;
    case FocusedSlider::HighlightStretch:
        return FocusedSlider::StretchWhiteRef;
    case FocusedSlider::StretchWhiteRef:
        return FocusedSlider::BlackFloor;
    case FocusedSlider::BlackFloor:
        return FocusedSlider::ColorIntensity;
    case FocusedSlider::ColorIntensity:
        return FocusedSlider::HighlightBlowout;
    case FocusedSlider::HighlightBlowout:
        return showFinishingSliders() ? FocusedSlider::HighlightSoftness : defaultFocus();
    case FocusedSlider::HighlightSoftness:
        return showFinishingSliders() ? FocusedSlider::DitherStrength : defaultFocus();
    case FocusedSlider::DitherStrength:
        return defaultFocus();
    }
    return defaultFocus();
}

FocusedSlider prevSlider(FocusedSlider cur)
{
    switch (cur) {
    case FocusedSlider::Intensity:
        return lastSlider();
    case FocusedSlider::ExpansionShape:
        return showIntensitySlider() ? FocusedSlider::Intensity : lastSlider();
    case FocusedSlider::ExpansionScurve:
        return FocusedSlider::ExpansionShape;
    case FocusedSlider::ExpansionMid:
        return splitCurveSliders() ? FocusedSlider::ExpansionScurve : FocusedSlider::ExpansionShape;
    case FocusedSlider::HighlightStretch:
        return FocusedSlider::ExpansionMid;
    case FocusedSlider::StretchWhiteRef:
        return FocusedSlider::HighlightStretch;
    case FocusedSlider::BlackFloor:
        return FocusedSlider::StretchWhiteRef;
    case FocusedSlider::ColorIntensity:
        return FocusedSlider::BlackFloor;
    case FocusedSlider::HighlightBlowout:
        return FocusedSlider::ColorIntensity;
    case FocusedSlider::HighlightSoftness:
        return FocusedSlider::HighlightBlowout;
    case FocusedSlider::DitherStrength:
        return FocusedSlider::HighlightSoftness;
    }
    return defaultFocus();
}

// Panel geometry in pixels (must match overlay.comp).
struct PanelGeom {
    float x0, y0, x1, y1;
    float trackX0, trackX1;
    float intensityY, shapeY, scurveY, kneeY, stretchY, whiteY, floorY, colorY, blowoutY, softenY, smoothY;
    float trackH;
};

PanelGeom makePanel(uint32_t w, uint32_t h)
{
    const bool showIntensity = showIntensitySlider();
    const bool showFinishing = showFinishingSliders();
    const bool splitCurve = splitCurveSliders();
    const float rowShift = showIntensity ? 0.0f : 34.0f;
    const float curveShift = splitCurve ? 0.0f : 34.0f;
    const float pw = std::min(520.0f, static_cast<float>(w) * 0.55f);
    const float ph = 420.0f - (showIntensity ? 0.0f : 34.0f) - (showFinishing ? 0.0f : 68.0f) - curveShift;
    const float x0 = (static_cast<float>(w) - pw) * 0.5f;
    const float y0 = static_cast<float>(h) - ph - 48.0f;
    PanelGeom g{};
    g.x0 = x0;
    g.y0 = y0;
    g.x1 = x0 + pw;
    g.y1 = y0 + ph;
    g.trackX0 = x0 + 140.0f;
    g.trackX1 = x0 + pw - 70.0f;
    // Row order must match overlay.comp: Intensity, Shape/Curve, Scurve, Knee, Stretch, White, Floor, Color,
    // Blowout, Soften, Smooth. Scurve is omitted unless split_curve_sliders is set; rows below it shift up.
    g.intensityY = y0 + 34.0f;
    g.shapeY = y0 + 68.0f - rowShift;
    g.scurveY = y0 + 102.0f - rowShift;
    g.kneeY = y0 + 136.0f - rowShift - curveShift;
    g.stretchY = y0 + 170.0f - rowShift - curveShift;
    g.whiteY = y0 + 204.0f - rowShift - curveShift;
    g.floorY = y0 + 238.0f - rowShift - curveShift;
    g.colorY = y0 + 272.0f - rowShift - curveShift;
    g.blowoutY = y0 + 306.0f - rowShift - curveShift;
    g.softenY = y0 + 340.0f - rowShift - curveShift;
    g.smoothY = y0 + 374.0f - rowShift - curveShift;
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
    const float seedRowY = showIntensitySlider() ? g.intensityY : g.shapeY;
    const float seedY = seedRowY + g.trackH * 0.5f;
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
    const bool showIntensity = showIntensitySlider();
    const bool showFinishing = showFinishingSliders();
    const bool splitCurve = splitCurveSliders();

    const float row0Top = g.intensityY - 12.0f;
    const float row0Bot = g.intensityY + g.trackH + 12.0f;
    const float row1Top = g.shapeY - 12.0f;
    const float row1Bot = g.shapeY + g.trackH + 12.0f;
    const float row2Top = g.scurveY - 12.0f;
    const float row2Bot = g.scurveY + g.trackH + 12.0f;
    const float row3Top = g.kneeY - 12.0f;
    const float row3Bot = g.kneeY + g.trackH + 12.0f;
    const float row4Top = g.stretchY - 12.0f;
    const float row4Bot = g.stretchY + g.trackH + 12.0f;
    const float row5Top = g.whiteY - 12.0f;
    const float row5Bot = g.whiteY + g.trackH + 12.0f;
    const float row6Top = g.floorY - 12.0f;
    const float row6Bot = g.floorY + g.trackH + 12.0f;
    const float row7Top = g.colorY - 12.0f;
    const float row7Bot = g.colorY + g.trackH + 12.0f;
    const float row8Top = g.blowoutY - 12.0f;
    const float row8Bot = g.blowoutY + g.trackH + 12.0f;
    const float row9Top = g.softenY - 12.0f;
    const float row9Bot = g.softenY + g.trackH + 12.0f;
    const float row10Top = g.smoothY - 12.0f;
    const float row10Bot = g.smoothY + g.trackH + 12.0f;
    const bool inPanel = ptr.x >= g.x0 && ptr.x <= g.x1 && ptr.y >= g.y0 && ptr.y <= g.y1;
    const bool inRow0 = showIntensity && inPanel && ptr.y >= row0Top && ptr.y <= row0Bot;
    const bool inRow1 = inPanel && ptr.y >= row1Top && ptr.y <= row1Bot;
    const bool inRow2 = splitCurve && inPanel && ptr.y >= row2Top && ptr.y <= row2Bot;
    const bool inRow3 = inPanel && ptr.y >= row3Top && ptr.y <= row3Bot;
    const bool inRow4 = inPanel && ptr.y >= row4Top && ptr.y <= row4Bot;
    const bool inRow5 = inPanel && ptr.y >= row5Top && ptr.y <= row5Bot;
    const bool inRow6 = inPanel && ptr.y >= row6Top && ptr.y <= row6Bot;
    const bool inRow7 = inPanel && ptr.y >= row7Top && ptr.y <= row7Bot;
    const bool inRow8 = inPanel && ptr.y >= row8Top && ptr.y <= row8Bot;
    const bool inRow9 = showFinishing && inPanel && ptr.y >= row9Top && ptr.y <= row9Bot;
    const bool inRow10 = showFinishing && inPanel && ptr.y >= row10Top && ptr.y <= row10Bot;

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
            g_state.focused = FocusedSlider::ExpansionScurve;
            g_state.dragging = true;
        } else if (inRow3) {
            g_state.focused = FocusedSlider::ExpansionMid;
            g_state.dragging = true;
        } else if (inRow4) {
            g_state.focused = FocusedSlider::HighlightStretch;
            g_state.dragging = true;
        } else if (inRow5) {
            g_state.focused = FocusedSlider::StretchWhiteRef;
            g_state.dragging = true;
        } else if (inRow6) {
            g_state.focused = FocusedSlider::BlackFloor;
            g_state.dragging = true;
        } else if (inRow7) {
            g_state.focused = FocusedSlider::ColorIntensity;
            g_state.dragging = true;
        } else if (inRow8) {
            g_state.focused = FocusedSlider::HighlightBlowout;
            g_state.dragging = true;
        } else if (inRow9) {
            g_state.focused = FocusedSlider::HighlightSoftness;
            g_state.dragging = true;
        } else if (inRow10) {
            g_state.focused = FocusedSlider::DitherStrength;
            g_state.dragging = true;
        } else if (!inPanel) {
            return;
        }
    }

    if (g_state.dragging) {
        const float t = std::clamp((ptr.x - g.trackX0) / std::max(g.trackX1 - g.trackX0, 1.0f), 0.0f, 1.0f);
        if (g_state.focused == FocusedSlider::Intensity) {
            if (showIntensity) {
                g_state.draft.intensity = t;
            }
        } else if (g_state.focused == FocusedSlider::ColorIntensity) {
            g_state.draft.colorIntensity = t;
        } else if (g_state.focused == FocusedSlider::BlackFloor) {
            g_state.draft.blackFloor = t;
        } else if (g_state.focused == FocusedSlider::HighlightStretch) {
            g_state.draft.highlightStretch = t;
        } else if (g_state.focused == FocusedSlider::StretchWhiteRef) {
            g_state.draft.stretchWhiteRef = t;
        } else if (g_state.focused == FocusedSlider::ExpansionMid) {
            g_state.draft.expansionMid = t;
        } else if (g_state.focused == FocusedSlider::HighlightBlowout) {
            g_state.draft.highlightBlowout = t;
        } else if (g_state.focused == FocusedSlider::HighlightSoftness) {
            if (showFinishing) {
                g_state.draft.highlightSoftness = t;
            }
        } else if (g_state.focused == FocusedSlider::DitherStrength) {
            if (showFinishing) {
                g_state.draft.debandStrength = sliderToDebandStrength(t);
            }
        } else if (g_state.focused == FocusedSlider::ExpansionScurve) {
            if (splitCurve) {
                g_state.draft.expansionScurve = t;
            } else {
                setCoupledCurve(t);
            }
        } else if (splitCurve) {
            g_state.draft.expansionShape = t;
        } else {
            setCoupledCurve(t);
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
    AutoHdr::CalibrationSettings settings =
        (g_state.overlayVisible || g_state.hasDraft) ? g_state.draft : activeSettings();
    if (!showIntensitySlider()) {
        settings.intensity = 1.0f;
    }
    if (!showFinishingSliders()) {
        settings.highlightSoftness = 1.0f;
        settings.debandStrength = 1.0f;
    }
    return settings;
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
    s.expansionScurve = g_state.draft.expansionScurve;
    s.expansionMid = g_state.draft.expansionMid;
    s.blackFloor = g_state.draft.blackFloor;
    s.highlightStretch = g_state.draft.highlightStretch;
    s.stretchWhiteRef = g_state.draft.stretchWhiteRef;
    s.highlightBlowout = g_state.draft.highlightBlowout;
    s.highlightSoftness = g_state.draft.highlightSoftness;
    s.debandStrength = g_state.draft.debandStrength;
    s.focused = static_cast<int>(g_state.focused);
    s.panelNits = 203.0f;
    s.pointerValid = g_state.pointerValid;
    s.pointerX = g_state.pointerX;
    s.pointerY = g_state.pointerY;
    s.showIntensity = showIntensitySlider();
    s.showFinishing = showFinishingSliders();
    s.splitCurveSliders = g_state.draft.splitCurveSliders;
    return s;
}

} // namespace AutoHdrVk
