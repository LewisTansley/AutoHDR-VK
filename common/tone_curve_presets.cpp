#include "tone_curve_presets.hpp"

#include "calibration.hpp"
#include "tone_curve.hpp"

#include <algorithm>
#include <cmath>
#include <cctype>

namespace AutoHdr {

namespace {

constexpr float kExponentialRate = 2.4f;

struct BuiltInCalibratedCurve {
    ToneCurvePreset preset;
    std::vector<Vec2> normalizedPoints;
};

const BuiltInCalibratedCurve kCalibratedCurves[] = {
    {ToneCurvePreset::Balanced, {{0.2405f, 0.0492f}, {0.7548f, 0.7492f}}},
    {ToneCurvePreset::LiftedShadows, {{0.2491f, 0.1302f}, {0.7462f, 0.7492f}}},
    {ToneCurvePreset::SoftShadows, {{0.3562f, 0.1397f}, {0.7537f, 0.7490f}}},
    {ToneCurvePreset::VividHighlights, {{0.3605f, 0.1444f}, {0.7033f, 0.7492f}}},
    {ToneCurvePreset::HighContrast, {{0.2533f, 0.0587f}, {0.7044f, 0.7490f}}},
};

const BuiltInCalibratedCurve *calibratedCurveFor(ToneCurvePreset preset)
{
    for (const BuiltInCalibratedCurve &curve : kCalibratedCurves) {
        if (curve.preset == preset) {
            return &curve;
        }
    }
    return nullptr;
}

float exponentialMapping(float t)
{
    const float k = kExponentialRate;
    const float denom = std::exp(k) - 1.0f;
    if (std::abs(denom) < 1e-6f) {
        return t;
    }
    return (std::exp(k * t) - 1.0f) / denom;
}

std::vector<Vec2> generateCalibratedIntermediatePoints(const BuiltInCalibratedCurve &curve, const PresetCurveParams &params)
{
    const float ref = std::max(params.referenceNits, 1e-3f);
    const float peak = std::max(params.peakNits, ref);

    std::vector<Vec2> points;
    points.reserve(curve.normalizedPoints.size());
    for (const Vec2 &point : curve.normalizedPoints) {
        points.push_back({point.x * ref, point.y * peak});
    }

    ToneCurveEndpoints endpoints;
    endpoints.peakNits = peak;
    endpoints.visualReferenceNits = ref;
    endpoints.sdrMaxPoint = {ref, peak};
    return sanitizeIntermediatePoints(points, endpoints);
}

std::string toLower(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return value;
}

} // namespace

std::string presetToString(ToneCurvePreset preset)
{
    switch (preset) {
    case ToneCurvePreset::Linear:
        return "linear";
    case ToneCurvePreset::Balanced:
        return "balanced";
    case ToneCurvePreset::LiftedShadows:
        return "lifted_shadows";
    case ToneCurvePreset::SoftShadows:
        return "soft_shadows";
    case ToneCurvePreset::VividHighlights:
        return "vivid_highlights";
    case ToneCurvePreset::HighContrast:
        return "high_contrast";
    case ToneCurvePreset::Exponential:
        return "exponential";
    case ToneCurvePreset::Custom:
        return "custom";
    }
    return "custom";
}

ToneCurvePreset presetFromString(const std::string &encoded)
{
    const std::string normalized = toLower(encoded);
    if (normalized == "linear") {
        return ToneCurvePreset::Linear;
    }
    if (normalized == "balanced" || normalized == "scurve") {
        return ToneCurvePreset::Balanced;
    }
    if (normalized == "lifted_shadows" || normalized == "scurve_boosted") {
        return ToneCurvePreset::LiftedShadows;
    }
    if (normalized == "soft_shadows" || normalized == "scurve_lifted") {
        return ToneCurvePreset::SoftShadows;
    }
    if (normalized == "vivid_highlights") {
        return ToneCurvePreset::VividHighlights;
    }
    if (normalized == "high_contrast") {
        return ToneCurvePreset::HighContrast;
    }
    if (normalized == "exponential") {
        return ToneCurvePreset::Exponential;
    }
    return ToneCurvePreset::Custom;
}

std::vector<ToneCurvePreset> builtInToneCurvePresets()
{
    return {ToneCurvePreset::Linear,       ToneCurvePreset::Balanced,   ToneCurvePreset::LiftedShadows,
            ToneCurvePreset::SoftShadows,  ToneCurvePreset::VividHighlights, ToneCurvePreset::HighContrast,
            ToneCurvePreset::Exponential};
}

std::vector<Vec2> generatePresetIntermediatePoints(ToneCurvePreset preset, const PresetCurveParams &params)
{
    if (preset == ToneCurvePreset::Custom || preset == ToneCurvePreset::Linear) {
        return {};
    }

    if (const BuiltInCalibratedCurve *curve = calibratedCurveFor(preset)) {
        return generateCalibratedIntermediatePoints(*curve, params);
    }

    if (preset != ToneCurvePreset::Exponential) {
        return {};
    }

    return generateExpansionShapePoints(1.0f, params);
}

std::vector<Vec2> generateExpansionShapePoints(float shape, const PresetCurveParams &params)
{
    const float ref = std::max(params.referenceNits, 1e-3f);
    const float peak = std::max(params.peakNits, ref);
    const float s = std::clamp(shape, 0.0f, 1.0f);

    // Dense enough intermediates for a smooth LUT morph between linear and exponential.
    constexpr float kFractions[] = {0.125f, 0.25f, 0.375f, 0.5f, 0.625f, 0.75f, 0.875f};
    std::vector<Vec2> points;
    points.reserve(sizeof(kFractions) / sizeof(kFractions[0]));
    for (float t : kFractions) {
        const float outNorm = t + (exponentialMapping(t) - t) * s;
        points.push_back({t * ref, outNorm * peak});
    }

    ToneCurveEndpoints endpoints;
    endpoints.peakNits = peak;
    endpoints.visualReferenceNits = ref;
    endpoints.sdrMaxPoint = {ref, peak};
    return sanitizeIntermediatePoints(points, endpoints);
}

void applyToneCurvePreset(CalibrationSettings &settings)
{
    if (settings.toneCurvePreset == ToneCurvePreset::Custom) {
        return;
    }

    const float ref = settings.referenceNits;
    const float peak = settings.maxNits;
    settings.sdrMaxPoint = {ref, peak};

    PresetCurveParams params;
    params.referenceNits = ref;
    params.peakNits = peak;
    settings.toneCurvePoints = generatePresetIntermediatePoints(settings.toneCurvePreset, params);
}

} // namespace AutoHdr
