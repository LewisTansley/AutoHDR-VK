// Bit-depth dither at present/quantize time (blue-noise + PQ-aware linear scaling).

const float DITHER_PQ_N = 0.1593017578125;
const float DITHER_PQ_RCP_N = 6.2773946361256;
const float DITHER_PQ_M = 78.84375;
const float DITHER_PQ_RCP_M = 0.0126833133693186;
const float DITHER_PQ_C1 = 0.8359375;
const float DITHER_PQ_C2 = 18.8515625;
const float DITHER_PQ_C3 = 18.6875;
const float DITHER_PQ_MAX_NITS = 10000.0;
const float DITHER_BLUE_NOISE_SIZE = 64.0;

layout(set = 0, binding = 3) uniform sampler2D uBlueNoise;

float ditherIgn(vec2 p)
{
    return fract(52.9829189 * fract(dot(p, vec2(0.06711056, 0.00583715))));
}

float ditherTriangular(vec2 p, vec2 offset)
{
    return (ditherIgn(p) + ditherIgn(p + offset)) - 1.0;
}

vec3 ditherIgnTriangular3(vec2 p)
{
    return vec3(
        ditherTriangular(p, vec2(5.2, 1.3)),
        ditherTriangular(p + vec2(17.0, 9.0), vec2(3.7, 8.1)),
        ditherTriangular(p + vec2(31.0, 23.0), vec2(11.4, 2.9)));
}

float sampleBlueNoise1(uvec2 pixel, uvec2 offset)
{
    vec2 uv = (vec2((pixel + offset) % 64u) + 0.5) / DITHER_BLUE_NOISE_SIZE;
    return texture(uBlueNoise, uv).r;
}

vec3 sampleDitherNoise3(uvec2 pixel, float useBlueNoise)
{
    vec2 p = vec2(pixel) + 0.5;
    if (useBlueNoise > 0.5) {
        float n0 = sampleBlueNoise1(pixel, uvec2(0u, 0u));
        float n1 = sampleBlueNoise1(pixel, uvec2(17u, 29u));
        float n2 = sampleBlueNoise1(pixel, uvec2(41u, 11u));
        return vec3(
            (n0 + sampleBlueNoise1(pixel, uvec2(7u, 3u))) - 1.0,
            (n1 + sampleBlueNoise1(pixel, uvec2(23u, 5u))) - 1.0,
            (n2 + sampleBlueNoise1(pixel, uvec2(31u, 19u))) - 1.0);
    }
    return ditherIgnTriangular3(p);
}

vec3 ditherPqToLinear(vec3 pq)
{
    vec3 t = pow(max(pq, vec3(0.0)), vec3(DITHER_PQ_RCP_M));
    vec3 nd = max(t - DITHER_PQ_C1, vec3(0.0)) / (DITHER_PQ_C2 - DITHER_PQ_C3 * t);
    return pow(nd, vec3(DITHER_PQ_RCP_N)) * DITHER_PQ_MAX_NITS;
}

vec3 ditherLinearToPq(vec3 linearNits)
{
    vec3 normalized = pow(max(linearNits, vec3(0.0)) / DITHER_PQ_MAX_NITS, vec3(DITHER_PQ_N));
    vec3 nd = (DITHER_PQ_C1 + DITHER_PQ_C2 * normalized) / (1.0 + DITHER_PQ_C3 * normalized);
    return pow(nd, vec3(DITHER_PQ_M));
}

vec3 applyEncodedDither(vec3 color, uvec2 pixel, float strength, uint bits, float useBlueNoise)
{
    if (strength <= 0.0 || bits < 2u) {
        return color;
    }
    float levels = float((1u << bits) - 1u);
    return color + sampleDitherNoise3(pixel, useBlueNoise) * (strength / levels);
}

vec3 applyPqAwareDither(vec3 pqColor, uvec2 pixel, float strength, uint bits, float useBlueNoise)
{
    if (strength <= 0.0 || bits < 2u) {
        return pqColor;
    }

    vec3 linearNits = ditherPqToLinear(pqColor);
    float levels = float((1u << bits) - 1u);
    vec3 pqStep = vec3(1.0 / levels);
    vec3 linearStep = max(abs(ditherPqToLinear(min(pqColor + pqStep, vec3(1.0))) - linearNits), vec3(1e-6));

    vec3 ditheredLinear = linearNits + sampleDitherNoise3(pixel, useBlueNoise) * linearStep * strength;
    return ditherLinearToPq(max(ditheredLinear, vec3(0.0)));
}

vec3 applyPresentDither(vec3 color, uvec2 pixel, float strength, uint bits, uint outputMode, float useBlueNoise)
{
    if (outputMode < 1u) {
        return applyPqAwareDither(color, pixel, strength, bits, useBlueNoise);
    }
    vec3 result = applyEncodedDither(color, pixel, strength, bits, useBlueNoise);
    if (outputMode < 2u) {
        return result;
    }
    return clamp(result, 0.0, 1.0);
}
