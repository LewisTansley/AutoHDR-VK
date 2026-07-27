#include "tone_curve.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <sstream>

namespace AutoHdr {

namespace {

constexpr float kEpsilon = 1e-6f;
constexpr float kSameXTolerance = 1.0f;

float clampf(float value, float minValue, float maxValue)
{
    return std::max(minValue, std::min(value, maxValue));
}

float stepWidthForReference(float referenceNits)
{
    return std::max(1.0f, referenceNits * 0.005f);
}

struct SegmentSlopes {
    std::vector<float> m;
};

SegmentSlopes computeMonotonicSlopes(const std::vector<Vec2> &points)
{
    const int n = static_cast<int>(points.size());
    SegmentSlopes result;
    result.m.resize(static_cast<size_t>(n));

    if (n < 2) {
        if (n == 1) {
            result.m[0] = 0.0f;
        }
        return result;
    }

    std::vector<float> h(static_cast<size_t>(n - 1));
    std::vector<float> delta(static_cast<size_t>(n - 1));
    for (int i = 0; i < n - 1; ++i) {
        h[static_cast<size_t>(i)] = std::max(points[static_cast<size_t>(i + 1)].x - points[static_cast<size_t>(i)].x, kEpsilon);
        delta[static_cast<size_t>(i)] =
            (points[static_cast<size_t>(i + 1)].y - points[static_cast<size_t>(i)].y) / h[static_cast<size_t>(i)];
    }

    result.m[0] = delta[0];
    result.m[static_cast<size_t>(n - 1)] = delta[static_cast<size_t>(n - 2)];

    for (int i = 1; i < n - 1; ++i) {
        if (delta[static_cast<size_t>(i - 1)] * delta[static_cast<size_t>(i)] <= 0.0f) {
            result.m[static_cast<size_t>(i)] = 0.0f;
        } else {
            const float w1 = 2.0f * h[static_cast<size_t>(i)] + h[static_cast<size_t>(i - 1)];
            const float w2 = h[static_cast<size_t>(i)] + 2.0f * h[static_cast<size_t>(i - 1)];
            result.m[static_cast<size_t>(i)] =
                (w1 + w2) / (w1 / delta[static_cast<size_t>(i - 1)] + w2 / delta[static_cast<size_t>(i)]);
        }
    }

    for (int i = 0; i < n - 1; ++i) {
        if (std::abs(delta[static_cast<size_t>(i)]) < kEpsilon) {
            result.m[static_cast<size_t>(i)] = 0.0f;
            result.m[static_cast<size_t>(i + 1)] = 0.0f;
        } else {
            const float alpha = result.m[static_cast<size_t>(i)] / delta[static_cast<size_t>(i)];
            const float beta = result.m[static_cast<size_t>(i + 1)] / delta[static_cast<size_t>(i)];
            const float tau = alpha * alpha + beta * beta;
            if (tau > 9.0f) {
                const float scale = 3.0f / std::sqrt(tau);
                result.m[static_cast<size_t>(i)] = scale * alpha * delta[static_cast<size_t>(i)];
                result.m[static_cast<size_t>(i + 1)] = scale * beta * delta[static_cast<size_t>(i)];
            }
        }
    }

    return result;
}

float evaluateHermiteSegment(float x, float x0, float x1, float y0, float y1, float m0, float m1)
{
    const float h = std::max(x1 - x0, kEpsilon);
    const float t = clampf((x - x0) / h, 0.0f, 1.0f);
    const float t2 = t * t;
    const float t3 = t2 * t;
    const float h00 = 2.0f * t3 - 3.0f * t2 + 1.0f;
    const float h10 = t3 - 2.0f * t2 + t;
    const float h01 = -2.0f * t3 + 3.0f * t2;
    const float h11 = t3 - t2;
    return h00 * y0 + h10 * h * m0 + h01 * y1 + h11 * h * m1;
}

int findSegmentIndex(const std::vector<Vec2> &points, float inputNits)
{
    if (points.empty()) {
        return -1;
    }
    if (inputNits <= points.front().x) {
        return 0;
    }
    if (inputNits >= points.back().x) {
        return static_cast<int>(points.size()) - 2;
    }
    for (size_t i = 0; i + 1 < points.size(); ++i) {
        if (inputNits >= points[i].x && inputNits <= points[i + 1].x) {
            return static_cast<int>(i);
        }
    }
    return static_cast<int>(points.size()) - 2;
}

std::vector<Vec2> expandStepKnotsForEval(const std::vector<Vec2> &points, const ToneCurveEndpoints &endpoints)
{
    std::vector<Vec2> sorted = points;
    std::sort(sorted.begin(), sorted.end(), [](const Vec2 &a, const Vec2 &b) { return a.x < b.x; });

    const float maxX = std::max(endpoints.sdrMaxPoint.x - kEpsilon, kEpsilon);
    const float stepWidth = stepWidthForReference(std::max(endpoints.visualReferenceNits, 1.0f));

    std::vector<Vec2> expanded;
    expanded.reserve(sorted.size() + 4);

    float groupX = 0.0f;
    float groupYMin = 0.0f;
    float groupYMax = 0.0f;
    bool inGroup = false;
    int groupCount = 0;

    auto flushGroup = [&]() {
        if (!inGroup) {
            return;
        }
        if (groupCount > 1 && groupYMax > groupYMin + kEpsilon) {
            const float stepX = clampf(groupX, kEpsilon, maxX);
            const float preX = std::max(kEpsilon, std::min(stepX - stepWidth, stepX - kEpsilon));
            expanded.push_back({preX, groupYMin});
            expanded.push_back({stepX, groupYMax});
        } else {
            expanded.push_back({groupX, groupYMax});
        }
        inGroup = false;
        groupCount = 0;
    };

    for (const Vec2 &point : sorted) {
        const float x = clampf(point.x, kEpsilon, maxX);
        const float y = clampf(point.y, 0.0f, endpoints.peakNits);

        if (!inGroup) {
            groupX = x;
            groupYMin = y;
            groupYMax = y;
            inGroup = true;
            groupCount = 1;
            continue;
        }

        if (std::abs(x - groupX) <= kSameXTolerance) {
            groupYMin = std::min(groupYMin, y);
            groupYMax = std::max(groupYMax, y);
            groupX = std::max(groupX, x);
            ++groupCount;
            continue;
        }

        flushGroup();
        groupX = x;
        groupYMin = y;
        groupYMax = y;
        inGroup = true;
        groupCount = 1;
    }

    flushGroup();
    return expanded;
}

void softenHighlightCurveSlope(float *lut, int size, float inputSpan)
{
    if (!lut || size < 4) {
        return;
    }

    const int startIdx = std::max(1, static_cast<int>(size * 0.45f));
    const float dx = std::max(inputSpan / static_cast<float>(size - 1), kEpsilon);
    const float endTarget = lut[size - 1];

    std::vector<float> slopes;
    slopes.reserve(static_cast<size_t>(size - startIdx));
    for (int i = startIdx + 1; i < size; ++i) {
        slopes.push_back((lut[i] - lut[i - 1]) / dx);
    }
    if (slopes.empty()) {
        return;
    }

    std::sort(slopes.begin(), slopes.end());
    const float medianSlope = slopes[slopes.size() / 2];
    const float maxSlope = std::max(medianSlope * 2.5f, 1.0f);
    const float maxDelta = maxSlope * dx;

    for (int i = startIdx + 1; i < size; ++i) {
        if (lut[i] - lut[i - 1] > maxDelta) {
            lut[i] = lut[i - 1] + maxDelta;
        }
    }

    const float endActual = lut[size - 1];
    if (endActual + kEpsilon < endTarget) {
        const float restore = endTarget - endActual;
        const int restoreSpan = size - 1 - startIdx;
        for (int i = startIdx; i < size; ++i) {
            const float t = static_cast<float>(i - startIdx) / static_cast<float>(std::max(restoreSpan, 1));
            lut[i] += restore * t;
        }
    }
}

} // namespace

std::vector<Vec2> buildFullCurve(const ToneCurveEndpoints &endpoints, const std::vector<Vec2> &intermediate)
{
    const std::vector<Vec2> expanded = expandStepKnotsForEval(intermediate, endpoints);
    const std::vector<Vec2> mids = sanitizeIntermediatePoints(expanded, endpoints);
    const Vec2 sdrMax = sanitizeSdrMaxPoint(endpoints.sdrMaxPoint, endpoints, mids);

    std::vector<Vec2> full;
    full.reserve(mids.size() + 2);
    full.push_back({0.0f, 0.0f});
    for (const Vec2 &point : mids) {
        full.push_back(point);
    }
    full.push_back(sdrMax);
    return full;
}

std::vector<Vec2> sanitizeIntermediatePoints(const std::vector<Vec2> &points, const ToneCurveEndpoints &endpoints)
{
    std::vector<Vec2> sorted = points;
    std::sort(sorted.begin(), sorted.end(), [](const Vec2 &a, const Vec2 &b) { return a.x < b.x; });

    const float maxX = std::max(endpoints.sdrMaxPoint.x - kEpsilon, kEpsilon);
    std::vector<Vec2> result;
    result.reserve(sorted.size());

    float lastX = 0.0f;
    float lastY = 0.0f;

    for (const Vec2 &point : sorted) {
        float x = clampf(point.x, kEpsilon, maxX);
        float y = clampf(point.y, 0.0f, endpoints.peakNits);

        if (x <= lastX + kEpsilon) {
            x = lastX + kEpsilon;
        }
        if (y < lastY) {
            y = lastY;
        }
        if (x >= maxX) {
            continue;
        }

        result.push_back({x, y});
        lastX = x;
        lastY = y;
    }

    return result;
}

Vec2 sanitizeSdrMaxPoint(const Vec2 &point, const ToneCurveEndpoints &endpoints, const std::vector<Vec2> &intermediate)
{
    float minX = kEpsilon;
    float minY = 0.0f;
    if (!intermediate.empty()) {
        minX = std::max(minX, intermediate.back().x + kEpsilon);
        minY = std::max(minY, intermediate.back().y);
    }

    const float maxInputX = std::max(endpoints.visualReferenceNits, kEpsilon);
    float x = clampf(point.x, minX, maxInputX);
    float y = clampf(point.y, minY, endpoints.peakNits);
    if (y < minY) {
        y = minY;
    }
    return {x, y};
}

float evaluateToneCurve(const std::vector<Vec2> &fullCurve, float inputNits)
{
    if (fullCurve.empty()) {
        return inputNits;
    }
    if (fullCurve.size() == 1) {
        return fullCurve.front().y;
    }

    if (inputNits <= fullCurve.front().x) {
        return fullCurve.front().y;
    }
    if (inputNits >= fullCurve.back().x) {
        return fullCurve.back().y;
    }

    if (fullCurve.size() == 2) {
        const float x0 = fullCurve[0].x;
        const float y0 = fullCurve[0].y;
        const float x1 = fullCurve[1].x;
        const float y1 = fullCurve[1].y;
        const float t = clampf((inputNits - x0) / std::max(x1 - x0, kEpsilon), 0.0f, 1.0f);
        return y0 + t * (y1 - y0);
    }

    const SegmentSlopes slopes = computeMonotonicSlopes(fullCurve);
    const int segment = findSegmentIndex(fullCurve, inputNits);
    const int i = std::max(0, std::min(segment, static_cast<int>(fullCurve.size()) - 2));

    return evaluateHermiteSegment(inputNits, fullCurve[static_cast<size_t>(i)].x, fullCurve[static_cast<size_t>(i + 1)].x,
                                  fullCurve[static_cast<size_t>(i)].y, fullCurve[static_cast<size_t>(i + 1)].y,
                                  slopes.m[static_cast<size_t>(i)], slopes.m[static_cast<size_t>(i + 1)]);
}

void buildToneCurveLut(const std::vector<Vec2> &fullCurve, float inputSpan, float *lut, int size)
{
    if (!lut || size <= 0) {
        return;
    }

    const float span = std::max(inputSpan, kEpsilon);
    for (int i = 0; i < size; ++i) {
        const float u = static_cast<float>(i) / static_cast<float>(size - 1);
        const float inputNits = u * span;
        lut[i] = evaluateToneCurve(fullCurve, inputNits);
    }

    softenHighlightCurveSlope(lut, size, span);
}

void buildToneCurveSlopeLut(const float *lut, float *slopeLut, int size, float *maxSlopeOut)
{
    if (!lut || !slopeLut || size <= 0) {
        return;
    }

    float maxSlope = 0.0f;
    for (int i = 0; i < size; ++i) {
        float tangent = 0.0f;
        if (size == 1) {
            tangent = 0.0f;
        } else if (i == 0) {
            tangent = lut[1] - lut[0];
        } else if (i == size - 1) {
            tangent = lut[size - 1] - lut[size - 2];
        } else {
            tangent = (lut[i + 1] - lut[i - 1]) * 0.5f;
        }
        slopeLut[i] = tangent;
        maxSlope = std::max(maxSlope, std::abs(tangent));
    }

    if (maxSlopeOut) {
        *maxSlopeOut = maxSlope;
    }
}

std::string formatToneCurvePoints(const std::vector<Vec2> &points)
{
    std::ostringstream oss;
    for (size_t i = 0; i < points.size(); ++i) {
        if (i > 0) {
            oss << ',';
        }
        oss << static_cast<int>(std::lround(points[i].x)) << ':' << static_cast<int>(std::lround(points[i].y));
    }
    return oss.str();
}

std::vector<Vec2> parseToneCurvePoints(const std::string &encoded)
{
    std::vector<Vec2> points;
    std::stringstream ss(encoded);
    std::string token;
    while (std::getline(ss, token, ',')) {
        const auto colon = token.find(':');
        if (colon == std::string::npos) {
            continue;
        }
        try {
            const float x = std::stof(token.substr(0, colon));
            const float y = std::stof(token.substr(colon + 1));
            points.push_back({x, y});
        } catch (...) {
        }
    }
    return points;
}

std::string formatSdrMaxPoint(const Vec2 &point)
{
    return std::to_string(static_cast<int>(std::lround(point.x))) + ':'
        + std::to_string(static_cast<int>(std::lround(point.y)));
}

Vec2 parseSdrMaxPoint(const std::string &encoded, const Vec2 &fallback)
{
    const auto colon = encoded.find(':');
    if (colon == std::string::npos) {
        return fallback;
    }
    try {
        return {std::stof(encoded.substr(0, colon)), std::stof(encoded.substr(colon + 1))};
    } catch (...) {
        return fallback;
    }
}

} // namespace AutoHdr
