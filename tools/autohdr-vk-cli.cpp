#include "config.hpp"
#include "tone_curve.hpp"
#include "tone_curve_presets.hpp"

#include <cstdio>
#include <iostream>
#include <string>

int main(int argc, char **argv)
{
    AutoHdrVk::reloadConfig();
    const auto &cfg = AutoHdrVk::config();

    if (argc > 1 && std::string(argv[1]) == "--help") {
        std::puts("autohdr-vk-cli — dump active AutoHDR-VK configuration\n"
                  "\n"
                  "Environment:\n"
                  "  ENABLE_AUTOHDR=1       enable implicit layer\n"
                  "  DISABLE_AUTOHDR=1      force off\n"
                  "  AUTOHDR_CONFIG=path    override config file\n"
                  "  AUTOHDR_LOG=1          layer stderr logging\n"
                  "\n"
                  "Config: ~/.config/autohdr-vk/conf.toml");
        return 0;
    }

    std::cout << "exe:            " << AutoHdrVk::currentExecutableName() << '\n';
    std::cout << "config:         " << (cfg.configPath.empty() ? "(none)" : cfg.configPath) << '\n';
    std::cout << "enabled:        " << (AutoHdrVk::isEffectActiveForCurrentProcess() ? "yes" : "no") << '\n';

    const auto settings = AutoHdrVk::activeSettings();
    std::cout << "intensity:      " << settings.intensity << '\n';
    std::cout << "reference_nits: " << settings.referenceNits << '\n';
    std::cout << "peak_nits:      " << settings.maxNits << '\n';
    std::cout << "black_point:    " << settings.blackPoint << '\n';
    std::cout << "black_floor:    " << settings.blackFloor << '\n';
    std::cout << "highlight_stretch:" << settings.highlightStretch << '\n';
    std::cout << "color_intensity:" << settings.colorIntensity << '\n';
    std::cout << "expansion_shape:" << settings.expansionShape << '\n';
    std::cout << "gamut_expansion:" << settings.gamutExpansion << '\n';
    std::cout << "dither:         " << (settings.dither ? "yes" : "no") << '\n';
    std::cout << "dither_strength:" << settings.ditherStrength << '\n';
    std::cout << "tone_curve:     " << AutoHdr::presetToString(settings.toneCurvePreset) << '\n';
    std::cout << "curve_points:   " << AutoHdr::formatToneCurvePoints(settings.toneCurvePoints) << '\n';
    std::cout << "sdr_max_point:  " << AutoHdr::formatSdrMaxPoint(settings.sdrMaxPoint) << '\n';
    std::cout << "profiles:       " << cfg.profiles.size() << '\n';
    for (const auto &p : cfg.profiles) {
        std::cout << "  - exe=" << p.exe << " enabled=" << (p.enabled ? "yes" : "no")
                  << " intensity=" << p.settings.intensity << '\n';
    }
    return 0;
}
