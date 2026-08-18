// Tonemap-pass bit-depth dither (IGN) when present compute is unavailable.

float tonemapIgn(vec2 p)
{
    return fract(52.9829189 * fract(dot(p, vec2(0.06711056, 0.00583715))));
}

vec3 applyTonemapFallbackDither(vec3 color, uvec2 pixel, float strength, uint bits, float outputMode)
{
    if (strength <= 0.0 || bits < 2u) {
        return color;
    }

    float levels = float((1u << bits) - 1u);
    vec2 p = vec2(pixel) + 0.5;
    float n0 = tonemapIgn(p);
    float n1 = tonemapIgn(p + vec2(17.0, 9.0));
    vec3 noise = vec3((n0 + n1 - 1.0) * (strength / levels));

    if (outputMode < 0.5) {
        vec3 linearNits = color * 10000.0;
        const float pqN = 0.1593017578125;
        const float pqRcpN = 6.2773946361256;
        const float pqM = 78.84375;
        const float pqRcpM = 0.0126833133693186;
        const float pqC1 = 0.8359375;
        const float pqC2 = 18.8515625;
        const float pqC3 = 18.6875;

        vec3 t = pow(max(color, vec3(0.0)), vec3(pqRcpM));
        vec3 nd = max(t - pqC1, vec3(0.0)) / (pqC2 - pqC3 * t);
        linearNits = pow(nd, vec3(pqRcpN)) * 10000.0;

        vec3 pqStep = vec3(1.0 / levels);
        vec3 tStep = pow(min(color + pqStep, vec3(1.0)), vec3(pqRcpM));
        vec3 ndStep = max(tStep - pqC1, vec3(0.0)) / (pqC2 - pqC3 * tStep);
        vec3 linearStep = max(abs(pow(ndStep, vec3(pqRcpN)) * 10000.0 - linearNits), vec3(1e-6));

        linearNits += noise * linearStep;
        vec3 normalized = pow(max(linearNits, vec3(0.0)) / 10000.0, vec3(pqN));
        vec3 ndOut = (pqC1 + pqC2 * normalized) / (1.0 + pqC3 * normalized);
        return clamp(pow(ndOut, vec3(pqM)), 0.0, 1.0);
    }

    vec3 result = color + noise;
    if (outputMode < 1.5) {
        return result;
    }
    return clamp(result, 0.0, 1.0);
}
