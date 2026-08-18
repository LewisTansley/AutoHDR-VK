#version 450
#extension GL_GOOGLE_include_directive : require

layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 outColor;

layout(set = 0, binding = 0) uniform sampler2D uInput;

layout(std140, set = 0, binding = 1) uniform ToneLut {
    vec4 toneCurveLutPacked[1024]; // 4096 floats
};

layout(std140, set = 0, binding = 2) uniform ToneParams {
    float blackPoint;
    float colorIntensity;
    float gamutExpansion;
    float referenceNits;
    float peakNits;
    float toneCurveInputSpan;
    float highlightSoftness;
    float perceptualColorEnabled;
    float outputMode; // 0=pq, 1=scrgb, 2=sdr preview
    float inputIsSrgb;
    float _pad0;
    float _pad1;
    vec4 pqBoostParams;
};

#include "tonemap_color.glsl"

float mapToneCurve(float inputNits, float inputSpan)
{
    float span = max(inputSpan, 1e-3);
    float u = clamp(inputNits / span, 0.0, 1.0);
    float idx = u * 4095.0;
    int i0 = int(floor(idx));
    int i1 = min(i0 + 1, 4095);
    float t = fract(idx);
    float y0 = toneCurveLutPacked[i0 / 4][i0 % 4];
    float y1 = toneCurveLutPacked[i1 / 4][i1 % 4];
    return mix(y0, y1, t);
}

vec3 toneMapPipeline(vec3 rgbNits, float ref, float displayPeak, float curveSpan)
{
    rgbNits = reconstructHighlights(rgbNits, ref);

    float rawLumaNits = max(luminanceYNits(rgbNits), 1e-6);
    float curveInputNits = applyUserBlackPoint(rawLumaNits / ref, blackPoint) * ref;
    float outputNits = mapToneCurve(curveInputNits, curveSpan);

    if (perceptualColorEnabled > 0.5) {
        rgbNits = applyPerceptualLuminanceMap(rgbNits, rawLumaNits, outputNits, colorIntensity, pqBoostParams);
        if (gamutExpansion > 0.0) {
            rgbNits = expandGamutSmart(rgbNits / ref, gamutExpansion) * ref;
        }
        vec3 xyz = autohdrRec709ToXYZ(rgbNits);
        float outY = max(xyz.y, 1e-6);
        float limitedY = applyHighlightPeakLimit(outY, displayPeak, highlightSoftness);
        rgbNits = autohdrXyzToRec709(xyz * (limitedY / outY));
    } else {
        float scale = outputNits / max(rawLumaNits, 1e-6);
        rgbNits *= scale;
        if (gamutExpansion > 0.0) {
            rgbNits = expandGamutSmart(rgbNits / ref, gamutExpansion) * ref;
        }
        float outLuma = dot(rgbNits, AUTOHDR_LUMA);
        float limitedLuma = applyHighlightPeakLimit(outLuma, displayPeak, highlightSoftness);
        rgbNits *= limitedLuma / max(outLuma, 1e-6);
    }
    return max(rgbNits, vec3(0.0));
}

void main()
{
    vec4 tex = texture(uInput, vUV);
    vec3 rgb = tex.rgb;
    if (inputIsSrgb > 0.5) {
        rgb = srgbToLinear(rgb);
    }

    float ref = max(referenceNits, 1.0);
    float displayPeak = max(peakNits, ref);
    float curveSpan = toneCurveInputSpan > 1.0 ? toneCurveInputSpan : ref;

    vec3 rgbNits = rgb * ref;
    rgbNits = toneMapPipeline(rgbNits, ref, displayPeak, curveSpan);

    vec3 outRgb;
    if (outputMode < 0.5) {
        outRgb = autohdrLinearToPq(rgbNits, 10000.0);
    } else if (outputMode < 1.5) {
        outRgb = rgbNits / 80.0;
    } else {
        vec3 rel = rgbNits / displayPeak;
        outRgb = linearToSrgb(clamp(rel, 0.0, 1.0));
    }

    outColor = vec4(outRgb, 1.0);
}
