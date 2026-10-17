// Shared AutoHDR color helpers for the Vulkan layer (no KWin colormanagement).

#include "ictcp.glsl"

const vec3 AUTOHDR_LUMA = vec3(0.2126, 0.7152, 0.0722);

// Rec.709 linear → BT.2020 linear (via XYZ), for HDR10 PQ encode only.
const mat3 AUTOHDR_REC709_TO_BT2020 = mat3(
    0.62740466, 0.06909699, 0.01639143,
    0.32928679, 0.91953894, 0.08801336,
    0.04335164, 0.01134733, 0.89559566);

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

// Monotonic highlight-tail remap using histogram-precomputed contentWhite / stretchOut.
// Mids below the knee stay identity; only the top of the content-white band is lifted.
float applyStretchRemap(float yRel, float shadowFloor, float contentWhite, float stretchOut)
{
    yRel = clamp(yRel, 0.0, 1.0);
    if (stretchOut <= contentWhite + 1e-4) {
        return yRel;
    }

    // High knee for already-bright masters — only nudge the top tail.
    float kneeMix = smoothstep(0.60, 0.95, contentWhite);
    float knee = max(shadowFloor, mix(contentWhite * 0.70, contentWhite * 0.88, kneeMix));

    if (yRel <= knee) {
        return yRel;
    }

    if (yRel <= contentWhite) {
        float t = (yRel - knee) / max(contentWhite - knee, 1e-3);
        return mix(knee, stretchOut, clamp(t, 0.0, 1.0));
    }

    // Above content white: expand [contentWhite, 1] → [stretchOut, 1], never compress.
    float t = (yRel - contentWhite) / max(1.0 - contentWhite, 1e-3);
    return max(mix(stretchOut, 1.0, clamp(t, 0.0, 1.0)), yRel);
}

// Monotonic highlight separation via multiplicative log gain that only rises with yRel.
// Avoids range-compressing pow remaps (those posterize highlights after PQ quantize) and
// avoids peak-hold inversion. Soften still owns absolute panel peak.
float applySeparationBoost(float yRel, float mid, float hi, float amount)
{
    amount = clamp(amount, 0.0, 1.0);
    if (amount <= 1e-4) {
        return yRel;
    }

    yRel = max(yRel, 0.0);
    mid = clamp(mid, 1e-3, 0.95);
    hi = max(hi, mid + 1e-3);

    // 0 below mid → 1 at/above hi; never falls, so gain never inverts order.
    float recovery = smoothstep(mid, hi, yRel);
    recovery = pow(recovery, mix(1.0, 1.35, amount));

    // Mild mid flatten (~−0.05 stop) → gentle highlight punch (~0.85 stop ≈ 1.8×).
    float logBoost = mix(mix(0.0, -0.05, amount), mix(0.0, 0.85, amount), recovery);
    return yRel * exp2(logBoost * amount);
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

// Legacy XYZ/PQ applicator (unused by live tonemap.comp; kept for orphan tonemap.frag).
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

// Pull negative Rec.709 channels toward CIE Y until all channels are non-negative (preserve Y).
vec3 softPullPositiveRgb(vec3 rgbNits)
{
    float y = luminanceYNits(rgbNits);
    if (!(y > 1e-8) || any(isnan(rgbNits)) || any(isinf(rgbNits))) {
        return vec3(0.0);
    }
    float minC = min(rgbNits.r, min(rgbNits.g, rgbNits.b));
    if (minC >= 0.0) {
        return rgbNits;
    }
    // rgb = mix(gray, rgb, t) with gray = vec3(y); solve min channel >= 0.
    // minC' = mix(y, minC, t) >= 0 → t <= y / (y - minC) when minC < y.
    float tMax = y / max(y - minC, 1e-6);
    float t = clamp(tMax, 0.0, 1.0);
    return mix(vec3(y), rgbNits, t);
}

// ICtCp applicator: LUT still owns yOut; CIE-Y locked after chroma restore/vibrance.
// Slider 0–1 maps the former useful 0–0.4 band (Color=1 ≈ old Color=0.4).
vec3 applyICtCpLuminanceMap(vec3 rgbNits, float yIn, float yOut, float colorIntensity)
{
    float scale = yOut / max(yIn, 1e-6);
    if (abs(scale - 1.0) < 1e-4) {
        return rgbNits;
    }

    vec3 yOnlyRgb = rgbNits * scale;
    float c = clamp(colorIntensity, 0.0, 1.0);
    if (c <= 0.001) {
        return yOnlyRgb;
    }

    // Stretch old 0–0.4 response across the full slider; structure reaches full ICtCp at Color=1.
    float cEff = c * 0.4;
    float structureMix = c;

    vec3 ictcp = linearRgbToICtCp(rgbNits);
    float iLinear = ictcpPqEotf(ictcp.x) * 10000.0;
    float iLinearOut = max(iLinear * scale, 0.0);
    ictcp.x = ictcpPqInvEotf(iLinearOut / 10000.0);

    // Soft capped chroma restore — uncapped iOut/iIn explodes under strong Shape expand.
    float iRatio = iLinearOut / max(iLinear, 1e-6);
    float chromaRestore = mix(1.0, min(iRatio, 1.2), 0.35);
    float chromaMul = chromaRestore * (1.0 + 0.45 * cEff);
    ictcp.y *= chromaMul;
    ictcp.z *= chromaMul;

    vec3 mapped = softPullPositiveRgb(iCtCpToLinearRgb(ictcp));
    float yMapped = luminanceYNits(mapped);
    if (!(yMapped > 1e-6) || any(isnan(mapped)) || any(isinf(mapped))) {
        return yOnlyRgb;
    }
    mapped *= yOut / yMapped;

    return mix(yOnlyRgb, mapped, structureMix);
}

vec3 autohdrRec709ToBt2020(vec3 linearRec709Nits)
{
    return AUTOHDR_REC709_TO_BT2020 * linearRec709Nits;
}

vec3 reconstructHighlights(vec3 rgbNits, float refNits)
{
    vec3 rel = rgbNits / max(refNits, 1.0);
    float peak = max(max(rel.r, rel.g), rel.b);
    if (peak <= 0.90) {
        return rgbNits;
    }
    float minChannel = min(rel.r, min(rel.g, rel.b));
    float nearClip = smoothstep(0.90, 0.99, peak);
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
    if (softness <= 0.0) {
        return min(luma, displayPeak);
    }
    if (luma >= displayPeak) {
        return displayPeak;
    }
    float kneeStart = mix(displayPeak, displayPeak * 0.92, softness);
    if (luma <= kneeStart) {
        return luma;
    }
    float t = (luma - kneeStart) / max(displayPeak - kneeStart, 1e-6);
    return mix(kneeStart, displayPeak, 1.0 - pow(1.0 - clamp(t, 0.0, 1.0), 1.5));
}

float applyHighlightPeakLimit(float outLuma, float displayPeak, float softness)
{
    if (softness > 0.0) {
        return softHighlightShoulder(outLuma, displayPeak, softness);
    }
    return min(outLuma, displayPeak);
}

// RenoDX-style highlight blowout: luma-preserving ICtCp chroma reduction near display peak only.
vec3 applyHighlightBlowout(vec3 rgbNits, float displayPeak, float blowout)
{
    if (blowout <= 1e-4) {
        return rgbNits;
    }

    float yIn = max(luminanceYNits(rgbNits), 1e-6);
    float peak = max(displayPeak, 1e-6);

    // Knee mirrors softHighlightShoulder: only desaturate the top highlight tail.
    float kneeStart = mix(peak * 0.92, peak * 0.72, blowout);
    if (yIn <= kneeStart) {
        return rgbNits;
    }

    float t = (yIn - kneeStart) / max(peak - kneeStart, 1e-6);
    float desat = pow(clamp(t, 0.0, 1.0), mix(1.8, 0.65, blowout)) * blowout;
    float chromaMul = 1.0 - desat;
    if (chromaMul >= 1.0 - 1e-4) {
        return rgbNits;
    }

    vec3 ictcp = linearRgbToICtCp(rgbNits);
    ictcp.y *= chromaMul;
    ictcp.z *= chromaMul;

    vec3 mapped = softPullPositiveRgb(iCtCpToLinearRgb(ictcp));
    float yMapped = luminanceYNits(mapped);
    if (!(yMapped > 1e-6) || any(isnan(mapped)) || any(isinf(mapped))) {
        return rgbNits;
    }
    return mapped * (yIn / yMapped);
}
