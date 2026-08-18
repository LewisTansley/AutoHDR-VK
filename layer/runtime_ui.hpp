#pragma once

#include "calibration.hpp"

#include <cstdint>

namespace AutoHdrVk {

enum class FocusedSlider : int {
    Intensity = 0,
    ExpansionShape = 1,
    HighlightStretch = 2,
    BlackFloor = 3,
    ColorIntensity = 4,
    DitherStrength = 5, // SMOOTH: mpv-style deband (0–1)
};

struct OverlayDrawState {
    bool visible = false;
    float intensity = 0.5f;
    float colorIntensity = 0.33f;
    float expansionShape = 0.55f;
    float blackFloor = 0.0f;
    float highlightStretch = 0.45f;
    float debandStrength = 0.7f;
    int focused = 0;
    float panelNits = 203.0f;
    float outputMode = 0.0f; // 0=pq, 1=scrgb, 2=sdr
    bool pointerValid = false;
    float pointerX = 0.0f;
    float pointerY = 0.0f;
};

// Call once per present (before gating). Handles hotkeys + slider input.
void updateRuntimeUi(uint32_t extentWidth, uint32_t extentHeight, float outputMode);

bool runtimeEffectOn();
bool overlayVisible();
bool shouldProcessPresent(); // effect on OR overlay visible

AutoHdr::CalibrationSettings effectiveSettings();
OverlayDrawState overlayDrawState();

} // namespace AutoHdrVk
