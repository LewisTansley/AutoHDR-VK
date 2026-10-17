#pragma once

#include "tone_curve_presets.hpp"
#include "vec2.hpp"

#include <string>
#include <vector>

namespace AutoHdr {

struct CalibrationSettings {
    float maxNits = 1000.0f;
    float gamutExpansion = 1.5f;
    float blackPoint = 0.0f; // legacy config-only offset; prefer blackFloor overlay slider
    float blackFloor = 0.0f; // 0 = off, 1 = max raised-floor crush (maps to blackPoint * 0.025)
    float colorIntensity = 0.33f;
    float referenceNits = 203.0f;
    float highlightSoftness = 1.0f;
    float intensity = 0.5f; // primary UX: 0 = SDR-like, 1 = full peak effect
    float highlightStretch = 0.45f; // histogram highlight pre-stretch (0=off, 1=legacy max, 2=aggressive)
    float stretchWhiteRef = 0.0f;   // WHITE slider: 0=p100 near-max, 1=p98
    float separation = 0.0f; // SEP slider: Ultra HDR–style highlight punch (0 = off, 1 = full)
    // On-axis default for the combined Curve slider (t = 0.225): shape = 1 - t, scurve = t.
    // Projection of the previous 0.55 / 0 pair. Ignored as a pair when splitCurveSliders is set.
    float expansionShape = 0.775f; // 0 = linear, 1 = exponential (darker mids)
    float expansionScurve = 0.225f; // 0 = off, 1 = S-curve residual (soft highlight shoulder)
    bool splitCurveSliders = false; // false = one Curve slider; true = independent Shape and Scurve
    float expansionMid = 0.5f; // KNEE slider: 0.5 = current mid, <0.5 lowers, >0.5 heightens
    bool dither = true;
    float ditherStrength = 1.25f; // auto present-pass dither strength
    float debandStrength = 1.0f;  // SMOOTH slider: mpv-style spatial deband (0–1)
    float highlightBlowout = 0.0f; // BLOWOUT slider: RenoDX-style highlight desat (0 = off)
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
    return clampBlackFloor(blackFloor) * 0.025f;
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

// Intensity slider 0..1 → linear SDR/HDR blend factor.
inline float intensityToBlend(float intensity)
{
    return clampIntensity(intensity);
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

inline float clampStretchWhiteRef(float value)
{
    if (value < 0.0f) {
        return 0.0f;
    }
    if (value > 1.0f) {
        return 1.0f;
    }
    return value;
}

inline float clampSeparation(float value)
{
    if (value < 0.0f) {
        return 0.0f;
    }
    if (value > 1.0f) {
        return 1.0f;
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

inline float clampExpansionScurve(float value)
{
    if (value < 0.0f) {
        return 0.0f;
    }
    if (value > 1.0f) {
        return 1.0f;
    }
    return value;
}

// Combined Curve slider position. t = 0 is full exponential, t = 1 is full S.
// Off-axis pairs project onto shape = 1 - t, scurve = t.
inline float expansionCurveT(float shape, float scurve)
{
    const float t = (1.0f - clampExpansionShape(shape) + clampExpansionScurve(scurve)) * 0.5f;
    if (t < 0.0f) {
        return 0.0f;
    }
    if (t > 1.0f) {
        return 1.0f;
    }
    return t;
}

inline void applyCoupledExpansion(float t, float &shape, float &scurve)
{
    if (t < 0.0f) {
        t = 0.0f;
    }
    if (t > 1.0f) {
        t = 1.0f;
    }
    shape = 1.0f - t;
    scurve = t;
}

inline void applyCoupledExpansion(CalibrationSettings &settings)
{
    if (settings.splitCurveSliders) {
        return;
    }
    applyCoupledExpansion(expansionCurveT(settings.expansionShape, settings.expansionScurve), settings.expansionShape,
                          settings.expansionScurve);
}

inline float clampExpansionMid(float value)
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

inline float clampDebandStrength(float value)
{
    if (value < 0.0f) {
        return 0.0f;
    }
    if (value > 1.0f) {
        return 1.0f;
    }
    return value;
}

inline float clampHighlightBlowout(float value)
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
