// ITU-R BT.2446 Method A style inverse tonemap on ICtCp I channel with dynamic k1

#ifndef BT2446_GLSL
#define BT2446_GLSL

#include "ictcp.glsl"

float bt2446MethodA_I(float iSdr, float k1, float sdrPeak, float hdrPeak)
{
    // iSdr ~ PQ-domain I for SDR content; map into expanded HDR I
    float n = clamp(iSdr / max(sdrPeak, 1e-4), 0.0, 1.0);
    float peakRatio = hdrPeak / max(sdrPeak, 1e-4);
    // Equal peaks: identity (no mid-lift reshape of SDR).
    if (peakRatio <= 1.001) {
        return clamp(n * sdrPeak, 0.0, hdrPeak);
    }
    float knee = clamp(k1, 0.55, 0.95);
    float kneeOut = knee * sdrPeak; // continuous with midtones
    float y;
    if (n < knee) {
        // SDR midtones unchanged — only the shoulder expands into HDR headroom.
        y = n * sdrPeak;
    } else {
        float u = (n - knee) / max(1.0 - knee, 1e-4);
        y = mix(kneeOut, hdrPeak, 1.0 - pow(1.0 - u, 1.6));
    }
    return clamp(y, 0.0, hdrPeak);
}

vec3 bt2446ICtCpTonemap(vec3 rgbNits, float refNits, float hdrPeakNits, float k1, float intensity,
                        float colorIntensity)
{
    float sdrI = ictcpPqInvEotf(refNits / 10000.0);
    float hdrI = ictcpPqInvEotf(hdrPeakNits / 10000.0);
    float t = clamp(intensity, 0.0, 1.0);

    vec3 ictcp = linearRgbToICtCp(rgbNits);
    float iIn = ictcp.x;
    float iMapped = bt2446MethodA_I(iIn, k1, sdrI, hdrI);
    float iOut = mix(iIn, iMapped, t);

    // Perceptual chroma restore: scale Ct/Cp by I ratio only (no extra highlight boost).
    // colorIntensity (0..1) blends luma-only remap vs full chroma restore.
    float chromaScale = 1.0;
    if (iIn > 1e-5) {
        chromaScale = iOut / iIn;
    }
    float cMix = clamp(colorIntensity, 0.0, 1.0);
    ictcp.x = iOut;
    ictcp.y *= mix(1.0, chromaScale, cMix);
    ictcp.z *= mix(1.0, chromaScale, cMix);
    return iCtCpToLinearRgb(ictcp);
}

#endif
