#pragma once

#include "tone_curve_presets.hpp"
#include "vec2.hpp"

#include <string>
#include <vector>

namespace AutoHdr {

struct CalibrationSettings {
    float maxNits = 1000.0f;
    float gamutExpansion = 1.5f;
    float blackPoint = 0.0f;
    float colorIntensity = 0.33f;
    float referenceNits = 203.0f;
    float highlightSoftness = 0.30f;
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

} // namespace AutoHdr
