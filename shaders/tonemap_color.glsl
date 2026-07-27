// Shared AutoHDR color helpers for the Vulkan layer (no KWin colormanagement).

const vec3 AUTOHDR_LUMA = vec3(0.2126, 0.7152, 0.0722);

const float AUTOHDR_PQ_N = 0.1593017578125;
const float AUTOHDR_PQ_RCP_N = 6.2773946361256;
const float AUTOHDR_PQ_M = 78.84375;
const float AUTOHDR_PQ_RCP_M = 0.0126833133693186;
const float AUTOHDR_PQ_C1 = 0.8359375;
const float AUTOHDR_PQ_C2 = 18.8515625;
const float AUTOHDR_PQ_C3 = 18.6875;

float applyUserBlackPoint(float t, float offset)
{
    return max(t - offset, 0.0) / max(1.0 - offset, 1e-6);
}

vec3 srgbToLinear(vec3 c)
{
    bvec3 cutoff = lessThanEqual(c, vec3(0.04045));
    vec3 higher = pow((c + 0.055) / 1.055, vec3(2.4));
    vec3 lower = c / 12.92;
    return mix(higher, lower, vec3(cutoff));
}

vec3 linearToSrgb(vec3 c)
{
    bvec3 cutoff = lessThanEqual(c, vec3(0.0031308));
    vec3 higher = 1.055 * pow(max(c, vec3(0.0)), vec3(1.0 / 2.4)) - 0.055;
    vec3 lower = c * 12.92;
    return mix(higher, lower, vec3(cutoff));
}

vec3 autohdrRec709ToXYZ(vec3 linearRec709)
{
    return vec3(
        dot(linearRec709, vec3(0.412390798, 0.357584327, 0.180480793)),
        dot(linearRec709, vec3(0.212639004, 0.715168655, 0.072192319)),
        dot(linearRec709, vec3(0.019330818, 0.119194783, 0.950532138))
    );
}

vec3 autohdrXyzToRec709(vec3 xyz)
{
    return vec3(
        dot(xyz, vec3(3.240969896, -1.537383198, -0.498610765)),
        dot(xyz, vec3(-0.969243646, 1.875967503, 0.041555058)),
        dot(xyz, vec3(0.055630080, -0.203976959, 1.056971550))
    );
}

float luminanceYNits(vec3 rgbNits)
{
    return autohdrRec709ToXYZ(rgbNits).y;
}

float autohdrLinearToPq(float x, float maxPqValue)
{
    float normalized = pow(max(x, 0.0) / max(maxPqValue, 1e-6), AUTOHDR_PQ_N);
    float nd = (AUTOHDR_PQ_C1 + AUTOHDR_PQ_C2 * normalized) / (1.0 + AUTOHDR_PQ_C3 * normalized);
    return pow(nd, AUTOHDR_PQ_M);
}

vec3 autohdrLinearToPq(vec3 x, float maxPqValue)
{
    vec3 normalized = pow(max(x, vec3(0.0)) / max(maxPqValue, 1e-6), vec3(AUTOHDR_PQ_N));
    vec3 nd = (AUTOHDR_PQ_C1 + AUTOHDR_PQ_C2 * normalized) / (1.0 + AUTOHDR_PQ_C3 * normalized);
    return pow(nd, vec3(AUTOHDR_PQ_M));
}

float autohdrPqToLinear(float x, float maxPqValue)
{
    float pq = pow(max(x, 0.0), AUTOHDR_PQ_RCP_M);
    float nd = max(pq - AUTOHDR_PQ_C1, 0.0) / (AUTOHDR_PQ_C2 - AUTOHDR_PQ_C3 * pq);
    return pow(nd, AUTOHDR_PQ_RCP_N) * maxPqValue;
}

vec3 autohdrPqToLinear(vec3 x, float maxPqValue)
{
    vec3 pq = pow(max(x, vec3(0.0)), vec3(AUTOHDR_PQ_RCP_M));
    vec3 nd = max(pq - AUTOHDR_PQ_C1, vec3(0.0)) / (AUTOHDR_PQ_C2 - AUTOHDR_PQ_C3 * pq);
    return pow(nd, vec3(AUTOHDR_PQ_RCP_N)) * maxPqValue;
}

float autohdrComputePqMul(float yIn, float yOut, vec4 pqParams)
{
    float p0 = pqParams.x;
    float p1 = pqParams.y;
    float p3 = pqParams.z;
    float pqIn = autohdrLinearToPq(yIn, p0);
    float pqOut = autohdrLinearToPq(yOut * p3, p1);
    return pqOut / max(pqIn, 1e-6);
}

vec3 applyPerceptualLuminanceMap(vec3 rgbNits, float yIn, float yOut, float colorIntensity, vec4 pqParams)
{
    float p0 = pqParams.x;
    float p1 = pqParams.y;
    float p3 = pqParams.z;
    if (p0 <= 0.1) {
        return rgbNits;
    }

    float scale = yOut / max(yIn, 1e-6);
    if (abs(scale - 1.0) < 1e-4) {
        return rgbNits;
    }

    vec3 xyz = autohdrRec709ToXYZ(rgbNits);
    float fLuma = max(xyz.y, 0.0);
    if (fLuma <= 0.0) {
        return rgbNits;
    }

    float pqMul = autohdrComputePqMul(yIn, yOut, pqParams);
    vec3 xyzResult;
    if (colorIntensity <= 0.001) {
        float newLuma = autohdrPqToLinear(autohdrLinearToPq(fLuma, p0) * pqMul, p1) / p3;
        xyzResult = xyz * (newLuma / max(fLuma, 1e-6));
    } else {
        vec3 boosted = autohdrPqToLinear(autohdrLinearToPq(xyz, p0) * pqMul, p1) / p3;
        float yOnly = autohdrPqToLinear(autohdrLinearToPq(fLuma, p0) * pqMul, p1) / p3;
        vec3 yOnlyXyz = xyz * (yOnly / max(fLuma, 1e-6));
        xyzResult = mix(yOnlyXyz, boosted, clamp(colorIntensity, 0.0, 1.0));
    }
    return autohdrXyzToRec709(xyzResult);
}

vec3 reconstructHighlights(vec3 rgbNits, float refNits)
{
    vec3 rel = rgbNits / max(refNits, 1.0);
    float peak = max(max(rel.r, rel.g), rel.b);
    if (peak <= 0.94) {
        return rgbNits;
    }
    float minChannel = min(rel.r, min(rel.g, rel.b));
    float nearClip = smoothstep(0.94, 0.99, peak);
    float channelSpread = (peak - minChannel) / max(peak, 1e-4);
    float clipMask = nearClip * smoothstep(0.03, 0.08, channelSpread);
    if (clipMask <= 1e-4) {
        return rgbNits;
    }
    float luma = dot(rel, AUTOHDR_LUMA);
    vec3 unclipped = rel / max(peak, 1e-4);
    float unclippedLuma = dot(unclipped, AUTOHDR_LUMA);
    vec3 reconstructed = unclipped * (luma / max(unclippedLuma, 1e-4));
    return mix(rel, reconstructed, clipMask) * refNits;
}

vec3 expandGamutSmart(vec3 vHDRColor, float userBoost)
{
    if (userBoost <= 0.0) {
        return vHDRColor;
    }
    float fExpandGamut = userBoost;
    const mat3 sRGB_2_AP1_D65 = mat3(
        vec3(0.616850994, 0.069866394, 0.020549067),
        vec3(0.334062934, 0.917416679, 0.107642211),
        vec3(0.049086072, 0.012716927, 0.871808722)
    );
    const mat3 AP1_D65_2_sRGB = mat3(
        vec3(1.692679398, -0.128573980, -0.024022465),
        vec3(-0.606218057, 1.137933633, -0.126211718),
        vec3(-0.086461341, -0.009359653, 1.150234183)
    );
    const mat3 Wide_2_AP1_D65 = mat3(
        vec3(0.834516905, 0.025545194, 0.001925829),
        vec3(0.160259590, 0.973101532, 0.030372797),
        vec3(0.005223505, 0.001353275, 0.967701374)
    );
    const mat3 AP1_2_sRGB = mat3(
        vec3(1.70505, -0.13026, -0.02400),
        vec3(-0.62179, 1.14080, -0.12897),
        vec3(-0.08326, -0.01055, 1.15297)
    );
    const mat3 ExpandMat = Wide_2_AP1_D65 * AP1_D65_2_sRGB;
    vec3 ColorAP1 = sRGB_2_AP1_D65 * vHDRColor;
    const mat3 ap1D65ToXYZ = mat3(
        vec3(0.647507191, 0.266086400, -0.005448868),
        vec3(0.134379134, 0.675967813, 0.004072095),
        vec3(0.168569595, 0.057945795, 1.090434551)
    );
    float LumaAP1 = (ap1D65ToXYZ * ColorAP1).y;
    vec3 ChromaAP1 = ColorAP1 / max(LumaAP1, 1e-6);
    float ChromaDistSqr = max(abs(dot(ChromaAP1 - 1.0, ChromaAP1 - 1.0)), 0.000001);
    float ExpandAmount = (1.0 - exp2(-4.0 * ChromaDistSqr))
                       * (1.0 - exp2(-4.0 * fExpandGamut * LumaAP1 * LumaAP1));
    vec3 ColorExpand = ExpandMat * ColorAP1;
    ColorAP1 = mix(ColorAP1, ColorExpand, ExpandAmount);
    return AP1_2_sRGB * ColorAP1;
}

float softHighlightShoulder(float luma, float displayPeak, float softness)
{
    if (softness <= 0.0 || luma <= displayPeak) {
        return min(luma, displayPeak);
    }
    float kneeStart = mix(displayPeak, displayPeak * 0.85, softness);
    if (luma <= kneeStart) {
        return luma;
    }
    float range = max(displayPeak - kneeStart, 1e-6);
    float t = (luma - kneeStart) / range;
    return kneeStart + range * (1.0 - exp(-t));
}

float applyHighlightPeakLimit(float outLuma, float displayPeak, float softness)
{
    if (softness > 0.0) {
        return softHighlightShoulder(outLuma, displayPeak, softness);
    }
    return min(outLuma, displayPeak);
}
