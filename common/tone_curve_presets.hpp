#pragma once

#include "vec2.hpp"

#include <string>
#include <vector>

namespace AutoHdr {

struct CalibrationSettings;

enum class ToneCurvePreset {
    Custom,
    Linear,
    Balanced,
    LiftedShadows,
    SoftShadows,
    VividHighlights,
    HighContrast,
    Exponential,
};

struct PresetCurveParams {
    float referenceNits = 203.0f;
    float peakNits = 1000.0f;
};

std::string presetToString(ToneCurvePreset preset);
ToneCurvePreset presetFromString(const std::string &encoded);

std::vector<ToneCurvePreset> builtInToneCurvePresets();

std::vector<Vec2> generatePresetIntermediatePoints(ToneCurvePreset preset, const PresetCurveParams &params);
// Morph linear (shape=0) → exponential (shape=1) intermediate points for the live LUT.
std::vector<Vec2> generateExpansionShapePoints(float shape, const PresetCurveParams &params);
void applyToneCurvePreset(CalibrationSettings &settings);

} // namespace AutoHdr
