#pragma once

#include "tone_curve_presets.hpp"
#include "vec2.hpp"

#include <string>
#include <vector>

namespace AutoHdr {

struct CalibrationSettings {
    float maxNits = 1000.0f;
    float gamutExpansion = 1.0f;
    float blackPoint = 0.0f; // legacy config-only offset; prefer blackFloor overlay slider
    float blackFloor = 0.0f; // 0 = off, 1 = max raised-floor crush (maps to blackPoint * 0.05)
    float colorIntensity = 0.33f;
    float referenceNits = 203.0f;
    float highlightSoftness = 0.30f;
    float intensity = 0.5f; // primary UX: 0 = SDR-like, 1 = full peak effect
    float highlightStretch = 0.45f; // histogram highlight pre-stretch (0 = off, 1 = legacy max, 2 = aggressive)
    float expansionShape = 0.55f; // 0 = linear, 1 = exponential (darker mids)
    bool dither = true;
    float ditherStrength = 1.0f; // 1 = one LSB of output bit depth
    bool perceptualColor = true;
    Vec2 sdrMaxPoint{203.0f, 1000.0f};
    std::vector<Vec2> toneCurvePoints;
    ToneCurvePreset toneCurvePreset = ToneCurvePreset::Linear;
};

inline float clampReferenceNits(float value)
{
    if (value < 80.0f) {
        return 80.0f;
    }
    if (value > 480.0f) {
        return 480.0f;
    }
    return value;
}

inline float clampBlackPoint(float value)
{
    if (value < -0.01f) {
        return -0.01f;
    }
    if (value > 0.01f) {
        return 0.01f;
    }
    return value;
}

inline float clampBlackFloor(float value)
{
    if (value < 0.0f) {
        return 0.0f;
    }
    if (value > 1.0f) {
        return 1.0f;
    }
    return value;
}

// Overlay Floor slider 0..1 → linear black-point offset applied before tone LUT.
inline float blackFloorToBlackPoint(float blackFloor)
{
    return clampBlackFloor(blackFloor) * 0.05f;
}

inline float clampGamutExpansion(float value)
{
    if (value < 0.0f) {
        return 0.0f;
    }
    if (value > 20.0f) {
        return 20.0f;
    }
    return value;
}

inline float clampColorIntensity(float value)
{
    if (value < 0.0f) {
        return 0.0f;
    }
    if (value > 1.0f) {
        return 1.0f;
    }
    return value;
}

inline float clampIntensity(float value)
{
    if (value < 0.0f) {
        return 0.0f;
    }
    if (value > 1.0f) {
        return 1.0f;
    }
    return value;
}

inline float clampHighlightStretch(float value)
{
    if (value < 0.0f) {
        return 0.0f;
    }
    if (value > 2.0f) {
        return 2.0f;
    }
    return value;
}

inline float clampExpansionShape(float value)
{
    if (value < 0.0f) {
        return 0.0f;
    }
    if (value > 1.0f) {
        return 1.0f;
    }
    return value;
}

inline float clampHighlightSoftness(float value)
{
    if (value < 0.0f) {
        return 0.0f;
    }
    if (value > 1.0f) {
        return 1.0f;
    }
    return value;
}

inline float clampDitherStrength(float value)
{
    if (value < 0.0f) {
        return 0.0f;
    }
    if (value > 2.0f) {
        return 2.0f;
    }
    return value;
}

} // namespace AutoHdr
