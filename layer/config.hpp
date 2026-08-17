#pragma once

#include "calibration.hpp"

#include <mutex>
#include <string>
#include <vector>

namespace AutoHdrVk {

enum class OutputEncoding {
    Auto,
    Pq,
    LinearScRgb,
    SdrPreview,
};

struct Profile {
    std::string exe;
    bool enabled = true;
    bool hasOverrides = false;
    AutoHdr::CalibrationSettings settings;
    OutputEncoding encoding = OutputEncoding::Auto;
};

struct LayerConfig {
    bool enabled = true;
    AutoHdr::CalibrationSettings global;
    OutputEncoding encoding = OutputEncoding::Auto;
    bool setHdrMetadata = true;
    bool preferHdrSwapchain = true;
    std::vector<Profile> profiles;
    std::string configPath;
    std::string exeName;
};

LayerConfig &config();
std::mutex &configMutex();

void reloadConfig();
bool isEffectActiveForCurrentProcess();
AutoHdr::CalibrationSettings activeSettings();
OutputEncoding activeEncoding();
bool wantHdrMetadata();
bool wantPreferHdrSwapchain();
bool saveOverlaySettings(float intensity, float colorIntensity, float expansionShape, float blackFloor,
                         float highlightStretch);

std::string currentExecutableName();

} // namespace AutoHdrVk
