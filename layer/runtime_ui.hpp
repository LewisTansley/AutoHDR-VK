#pragma once

#include "calibration.hpp"

#include <cstdint>

namespace AutoHdrVk {

enum class FocusedSlider : int {
    Intensity = 0,
    ExpansionShape = 1,
    ExpansionScurve = 2,
    ExpansionMid = 3,
    HighlightStretch = 4,
    StretchWhiteRef = 5,
    BlackFloor = 6,
    ColorIntensity = 7,
    HighlightBlowout = 8,
    HighlightSoftness = 9,
    DitherStrength = 10, // SMOOTH: mpv-style deband (0–1)
};

struct OverlayDrawState {
    bool visible = false;
    float intensity = 0.5f;
    float colorIntensity = 0.33f;
    float expansionShape = 0.775f;
    float expansionScurve = 0.225f;
    float expansionMid = 0.5f;
    float blackFloor = 0.0f;
    float highlightStretch = 0.45f;
    float stretchWhiteRef = 0.0f;
    float highlightBlowout = 0.0f;
    float highlightSoftness = 1.0f;
    float debandStrength = 1.0f;
    int focused = 0;
    float panelNits = 203.0f;
    float outputMode = 0.0f; // 0=pq, 1=scrgb, 2=sdr
    bool pointerValid = false;
    float pointerX = 0.0f;
    float pointerY = 0.0f;
    bool showIntensity = false;
    bool showFinishing = false;
    bool splitCurveSliders = false;
};

// Call once per present (before gating). Handles hotkeys + slider input.
void updateRuntimeUi(uint32_t extentWidth, uint32_t extentHeight, float outputMode);

bool runtimeEffectOn();
bool overlayVisible();
bool shouldProcessPresent(); // effect on OR overlay visible

AutoHdr::CalibrationSettings effectiveSettings();
OverlayDrawState overlayDrawState();

} // namespace AutoHdrVk
