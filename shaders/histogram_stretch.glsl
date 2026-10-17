// Shared highlight ladder + stretch target math (histogram pass2 + tonemap).

// Pass1 bins: u = clamp((log2(y)+10)/10, 0, 1); bin = min(uint(u*64), 63).
// Mid-bin decode for percentiles (typical value inside the bin).
float histBinToY(uint i)
{
    float u = (float(i) + 0.5) / 64.0;
    return exp2(u * 10.0 - 10.0);
}

// Upper-edge decode for nearMax / peak (bin 63 → y = 1.0 exactly).
float histBinToYPeak(uint i)
{
    float u = (float(i) + 1.0) / 64.0;
    return exp2(u * 10.0 - 10.0);
}

// White slider: ref01 0=p100 near-max, 1=p98.
float sampleHighlightLadder(float ref01, float p98, float p100)
{
    return mix(p100, p98, clamp(ref01, 0.0, 1.0));
}

void computeStretchTargets(float yw, float nearMax, float geoMean, float stretchStrength,
                            out float stretchOut, out float highlightGap)
{
    yw = clamp(max(yw, 1e-3), 1e-3, 1.0);
    highlightGap = max(nearMax - yw, 0.0);
    stretchOut = yw;

    if (stretchStrength <= 1e-4 || yw >= 0.98) {
        return;
    }

    // Soft engage: square strength so low slider values barely move stretchOut.
    // Dark-scene raise (low yw) is gated by nearMax so frames with real highlights
    // are not expanded into peak (avoids blowing sparse/doorway speculars).
    // geoMean caps the ceiling: mid-luminance frames stay gain-limited; only truly
    // dark scenes may fill the rest of the way to 1.0.
    float s = clamp(stretchStrength, 0.0, 2.0);
    float ease = s * s;
    float darkRaise = 1.0 - smoothstep(0.55, 0.98, yw);
    float highlightHold = 1.0 - smoothstep(0.45, 0.90, nearMax);
    float head = darkRaise * highlightHold;
    float expand = head * min(ease, 1.0);
    // TOML values in (1, 2]: restore extra expand beyond the squared 0–1 band.
    if (s > 1.0) {
        float over = clamp(s - 1.0, 0.0, 1.0);
        expand = mix(expand, head * min(s, 2.0), over);
    }
    expand = clamp(expand, 0.0, 1.0);

    // Match applyStretchRemap knee so the ceiling cap uses the same pivot.
    float kneeMix = smoothstep(0.60, 0.95, yw);
    float knee = mix(yw * 0.70, yw * 0.88, kneeMix);
    float spanUpper = max(yw - knee, 1e-6);
    float muted = 1.0 - smoothstep(0.55, 0.98, yw);
    float maxGain = mix(1.25, 2.5, clamp(stretchStrength * 0.5, 0.0, 1.0));
    maxGain = mix(maxGain, maxGain + 1.5, muted);
    float gainCapped = min(1.0, knee + spanUpper * maxGain);
    gainCapped = max(gainCapped, yw);

    float raised = mix(yw, 1.0, expand);
    // Only truly dark geoMean may override the gain cap toward peak.
    float darkScene = 1.0 - smoothstep(0.045, 0.12, clamp(geoMean, 0.0, 1.0));
    stretchOut = min(mix(min(raised, gainCapped), raised, darkScene), 1.0);
}
