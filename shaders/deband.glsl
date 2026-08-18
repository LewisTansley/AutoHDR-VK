// Split deband: classic mpv on large low-frequency flats; skip taps that hit hard geometry.

float debandIgn(vec2 p)
{
    return fract(52.9829189 * fract(dot(p, vec2(0.06711056, 0.00583715))));
}

vec2 debandHashDir(uvec2 gid, int iter)
{
    vec2 p = vec2(gid) + vec2(float(iter) * 19.17, float(iter) * 7.31);
    float a = debandIgn(p) * 6.28318530718;
    return vec2(cos(a), sin(a));
}

vec3 debandSampleInputNits(vec2 uv, vec2 texel, float ref, float inputIsSrgbFlag)
{
    uv = (floor(uv / texel) + 0.5) * texel;
    uv = clamp(uv, texel * 0.5, vec2(1.0) - texel * 0.5);
    vec3 rgb = texture(uInput, uv).rgb;
    if (inputIsSrgbFlag > 0.5) {
        rgb = srgbToLinear(rgb);
    }
    return max(rgb * ref, vec3(0.0));
}

float debandLocalRgbRange(vec3 centerSdrNits, vec2 uv, vec2 texel, float ref, float inputIsSrgbFlag)
{
    vec3 rgbMin = centerSdrNits;
    vec3 rgbMax = centerSdrNits;
    for (int dy = -1; dy <= 1; ++dy) {
        for (int dx = -1; dx <= 1; ++dx) {
            if (dx == 0 && dy == 0) {
                continue;
            }
            vec3 sampleNits = debandSampleInputNits(uv + vec2(float(dx), float(dy)) * texel, texel, ref,
                                                   inputIsSrgbFlag);
            rgbMin = min(rgbMin, sampleNits);
            rgbMax = max(rgbMax, sampleNits);
        }
    }
    vec3 range = rgbMax - rgbMin;
    return max(range.r, max(range.g, range.b));
}

bool debandIsFlat(float localRange, float ref)
{
    return localRange <= 4.0 * (ref / 255.0);
}

vec3 applyMpvDeband(vec3 color, vec2 uv, vec2 texel, uvec2 gid, float ref, float inputIsSrgbFlag,
                    float debandStrength, out float mixed)
{
    mixed = 0.0;
    float rel = clamp(luminanceYNits(color) / max(ref, 1e-6), 0.0, 1.0);
    float shadowW = smoothstep(0.08, 0.22, rel);
    if (shadowW <= 1e-4) {
        return color;
    }

    float rangePx = mix(8.0, 20.0, debandStrength) * shadowW;
    float thrNits = mix(1.5, 4.0, debandStrength) * (ref / 255.0) * shadowW;
    const int iters = 4;

    for (int i = 1; i <= iters; ++i) {
        float luma = max(luminanceYNits(color), 1e-6);
        float hardLsb = mix(4.0, 10.0, clamp(luma / ref, 0.0, 1.0));
        float hardLim = hardLsb * (ref / 255.0);
        vec3 hard3 = vec3(hardLim);

        float dist = rangePx * float(i) / float(iters);
        vec2 dir = debandHashDir(gid, i);
        vec2 offsetUv = dir * dist * texel;
        vec3 s1 = debandSampleInputNits(uv + offsetUv, texel, ref, inputIsSrgbFlag);
        vec3 s2 = debandSampleInputNits(uv - offsetUv, texel, ref, inputIsSrgbFlag);
        if (any(greaterThan(abs(s1 - color), hard3)) || any(greaterThan(abs(s2 - color), hard3))
            || any(greaterThan(abs(s1 - s2), hard3))) {
            continue;
        }
        vec3 avg = 0.5 * (s1 + s2);
        float lim = thrNits * float(i);
        bvec3 keep = greaterThan(abs(avg - color), vec3(lim));
        if (!all(keep)) {
            mixed = 1.0;
        }
        color = mix(avg, color, vec3(keep));
    }

    return color;
}

vec3 applyInputDeband(vec3 centerSdrNits, vec2 uv, uvec2 imageExtent, uvec2 gid, float ref,
                      float inputIsSrgbFlag, float debandStrength, out float flatMask)
{
    flatMask = 0.0;
    if (debandStrength <= 1e-4) {
        return centerSdrNits;
    }

    float rel = clamp(luminanceYNits(centerSdrNits) / max(ref, 1e-6), 0.0, 1.0);
    if (smoothstep(0.08, 0.22, rel) <= 1e-4) {
        return centerSdrNits;
    }

    vec2 texel = 1.0 / vec2(imageExtent);
    float localRange = debandLocalRgbRange(centerSdrNits, uv, texel, ref, inputIsSrgbFlag);
    if (!debandIsFlat(localRange, ref)) {
        return centerSdrNits;
    }

    return applyMpvDeband(centerSdrNits, uv, texel, gid, ref, inputIsSrgbFlag, debandStrength, flatMask);
}

vec3 applyDebandGrain(vec3 rgbNits, uvec2 pixel, float debandStrength, float flatMask)
{
    if (debandStrength <= 1e-4 || flatMask <= 1e-4) {
        return rgbNits;
    }

    float grainNits = mix(0.0, 0.8, debandStrength);
    vec2 p = vec2(pixel) + 0.5;
    vec3 noise = vec3(
        debandIgn(p) + debandIgn(p + vec2(5.2, 1.3)) - 1.0,
        debandIgn(p + vec2(17.0, 9.0)) + debandIgn(p + vec2(3.7, 8.1)) - 1.0,
        debandIgn(p + vec2(31.0, 23.0)) + debandIgn(p + vec2(11.4, 2.9)) - 1.0);
    return max(rgbNits + noise * grainNits, vec3(0.0));
}
