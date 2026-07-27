#pragma once

#include "vec2.hpp"

#include <string>
#include <vector>

namespace AutoHdr {

constexpr int kToneCurveLutSize = 1024;

struct ToneCurveEndpoints {
    float peakNits = 1000.0f;
    Vec2 sdrMaxPoint;
    float visualReferenceNits = 100.0f;
};

std::vector<Vec2> buildFullCurve(const ToneCurveEndpoints &endpoints, const std::vector<Vec2> &intermediate);

std::vector<Vec2> sanitizeIntermediatePoints(const std::vector<Vec2> &points, const ToneCurveEndpoints &endpoints);

Vec2 sanitizeSdrMaxPoint(const Vec2 &point, const ToneCurveEndpoints &endpoints, const std::vector<Vec2> &intermediate);

float evaluateToneCurve(const std::vector<Vec2> &fullCurve, float inputNits);

void buildToneCurveLut(const std::vector<Vec2> &fullCurve, float inputSpan, float *lut, int size);

void buildToneCurveSlopeLut(const float *lut, float *slopeLut, int size, float *maxSlopeOut = nullptr);

std::string formatToneCurvePoints(const std::vector<Vec2> &points);

std::vector<Vec2> parseToneCurvePoints(const std::string &encoded);

std::string formatSdrMaxPoint(const Vec2 &point);

Vec2 parseSdrMaxPoint(const std::string &encoded, const Vec2 &fallback);

} // namespace AutoHdr
