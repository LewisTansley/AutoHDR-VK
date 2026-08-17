#include "config.hpp"

#include "tone_curve.hpp"
#include "tone_curve_presets.hpp"

#include <toml++/toml.hpp>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <unistd.h>

namespace AutoHdrVk {

namespace {

LayerConfig g_config;
std::mutex g_configMutex;
bool g_loaded = false;

std::string toLower(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return value;
}

OutputEncoding encodingFromString(const std::string &value)
{
    const std::string n = toLower(value);
    if (n == "pq" || n == "hdr10" || n == "st2084") {
        return OutputEncoding::Pq;
    }
    if (n == "scrgb" || n == "linear" || n == "extended_srgb") {
        return OutputEncoding::LinearScRgb;
    }
    if (n == "sdr" || n == "preview" || n == "sdr_preview") {
        return OutputEncoding::SdrPreview;
    }
    return OutputEncoding::Auto;
}

void applySettingsTable(const toml::table &table, AutoHdr::CalibrationSettings &settings)
{
    if (auto v = table["intensity"].value<double>()) {
        settings.intensity = AutoHdr::clampIntensity(static_cast<float>(*v));
    }
    if (auto v = table["reference_nits"].value<double>()) {
        settings.referenceNits = AutoHdr::clampReferenceNits(static_cast<float>(*v));
    }
    if (auto v = table["peak_nits"].value<double>()) {
        settings.maxNits = std::max(static_cast<float>(*v), settings.referenceNits);
    }
    if (auto v = table["black_point"].value<double>()) {
        settings.blackPoint = AutoHdr::clampBlackPoint(static_cast<float>(*v));
    }
    if (auto v = table["black_floor"].value<double>()) {
        settings.blackFloor = AutoHdr::clampBlackFloor(static_cast<float>(*v));
    } else if (auto v = table["black_point"].value<double>()) {
        settings.blackFloor = AutoHdr::clampBlackFloor(static_cast<float>(*v) / 0.05f);
    }
    if (auto v = table["color_intensity"].value<double>()) {
        settings.colorIntensity = AutoHdr::clampColorIntensity(static_cast<float>(*v));
    }
    if (auto v = table["expansion_shape"].value<double>()) {
        settings.expansionShape = AutoHdr::clampExpansionShape(static_cast<float>(*v));
    }
    if (auto v = table["highlight_stretch"].value<double>()) {
        settings.highlightStretch = AutoHdr::clampHighlightStretch(static_cast<float>(*v));
    }
    if (auto v = table["gamut_expansion"].value<double>()) {
        settings.gamutExpansion = AutoHdr::clampGamutExpansion(static_cast<float>(*v));
    }
    if (auto v = table["highlight_softness"].value<double>()) {
        settings.highlightSoftness = AutoHdr::clampHighlightSoftness(static_cast<float>(*v));
    }
    if (auto v = table["dither"].value<bool>()) {
        settings.dither = *v;
    }
    if (auto v = table["dither_strength"].value<double>()) {
        settings.ditherStrength = AutoHdr::clampDitherStrength(static_cast<float>(*v));
    }
    if (auto v = table["perceptual_color"].value<bool>()) {
        settings.perceptualColor = *v;
    }
    if (auto v = table["tone_curve_preset"].value<std::string>()) {
        settings.toneCurvePreset = AutoHdr::presetFromString(*v);
    }
    if (auto v = table["tone_curve_points"].value<std::string>()) {
        settings.toneCurvePoints = AutoHdr::parseToneCurvePoints(*v);
        settings.toneCurvePreset = AutoHdr::ToneCurvePreset::Custom;
    }
    if (auto v = table["sdr_max_point"].value<std::string>()) {
        settings.sdrMaxPoint = AutoHdr::parseSdrMaxPoint(*v, settings.sdrMaxPoint);
        settings.toneCurvePreset = AutoHdr::ToneCurvePreset::Custom;
    }
}

void finalizeSettings(AutoHdr::CalibrationSettings &settings)
{
    settings.intensity = AutoHdr::clampIntensity(settings.intensity);
    settings.referenceNits = AutoHdr::clampReferenceNits(settings.referenceNits);
    settings.maxNits = std::max(settings.maxNits, settings.referenceNits);
    settings.blackPoint = AutoHdr::clampBlackPoint(settings.blackPoint);
    settings.blackFloor = AutoHdr::clampBlackFloor(settings.blackFloor);
    settings.colorIntensity = AutoHdr::clampColorIntensity(settings.colorIntensity);
    settings.expansionShape = AutoHdr::clampExpansionShape(settings.expansionShape);
    settings.highlightStretch = AutoHdr::clampHighlightStretch(settings.highlightStretch);
    settings.gamutExpansion = AutoHdr::clampGamutExpansion(settings.gamutExpansion);
    settings.highlightSoftness = AutoHdr::clampHighlightSoftness(settings.highlightSoftness);
    settings.ditherStrength = AutoHdr::clampDitherStrength(settings.ditherStrength);

    if (settings.toneCurvePreset != AutoHdr::ToneCurvePreset::Custom) {
        AutoHdr::applyToneCurvePreset(settings);
    } else if (settings.sdrMaxPoint.x <= 0.0f || settings.sdrMaxPoint.y <= 0.0f) {
        settings.sdrMaxPoint = {settings.referenceNits, settings.maxNits};
    }
}

std::filesystem::path defaultConfigPath()
{
    if (const char *xdg = std::getenv("XDG_CONFIG_HOME"); xdg && *xdg) {
        return std::filesystem::path(xdg) / "autohdr-vk" / "conf.toml";
    }
    if (const char *home = std::getenv("HOME"); home && *home) {
        return std::filesystem::path(home) / ".config" / "autohdr-vk" / "conf.toml";
    }
    return {};
}

} // namespace

LayerConfig &config()
{
    return g_config;
}

std::mutex &configMutex()
{
    return g_configMutex;
}

std::string currentExecutableName()
{
    char buf[4096] = {};
    const ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (n <= 0) {
        return {};
    }
    buf[n] = '\0';
    return std::filesystem::path(buf).filename().string();
}

void reloadConfig()
{
    std::lock_guard lock(g_configMutex);
    g_config = LayerConfig{};
    g_config.exeName = currentExecutableName();
    g_config.global.sdrMaxPoint = {203.0f, 1000.0f};

    if (const char *disable = std::getenv("DISABLE_AUTOHDR"); disable && *disable && std::string(disable) != "0") {
        g_config.enabled = false;
        g_loaded = true;
        return;
    }

    const char *enable = std::getenv("ENABLE_AUTOHDR");
    const bool envEnabled = enable && *enable && std::string(enable) != "0";

    std::filesystem::path path;
    if (const char *overridePath = std::getenv("AUTOHDR_CONFIG"); overridePath && *overridePath) {
        path = overridePath;
    } else {
        path = defaultConfigPath();
    }
    g_config.configPath = path.string();

    if (!path.empty() && std::filesystem::exists(path)) {
        try {
            const toml::table root = toml::parse_file(path.string());
            if (const auto *global = root["global"].as_table()) {
                applySettingsTable(*global, g_config.global);
                if (auto v = (*global)["encoding"].value<std::string>()) {
                    g_config.encoding = encodingFromString(*v);
                }
                if (auto v = (*global)["set_hdr_metadata"].value<bool>()) {
                    g_config.setHdrMetadata = *v;
                }
                if (auto v = (*global)["prefer_hdr_swapchain"].value<bool>()) {
                    g_config.preferHdrSwapchain = *v;
                }
                if (auto v = (*global)["enabled"].value<bool>()) {
                    g_config.enabled = *v;
                }
            }

            if (const auto *arr = root["profile"].as_array()) {
                for (const auto &node : *arr) {
                    const auto *table = node.as_table();
                    if (!table) {
                        continue;
                    }
                    Profile profile;
                    profile.settings = g_config.global;
                    if (auto v = (*table)["exe"].value<std::string>()) {
                        profile.exe = *v;
                    }
                    if (auto v = (*table)["enabled"].value<bool>()) {
                        profile.enabled = *v;
                    }
                    if (auto v = (*table)["encoding"].value<std::string>()) {
                        profile.encoding = encodingFromString(*v);
                        profile.hasOverrides = true;
                    }
                    applySettingsTable(*table, profile.settings);
                    profile.hasOverrides = true;
                    finalizeSettings(profile.settings);
                    if (!profile.exe.empty()) {
                        g_config.profiles.push_back(std::move(profile));
                    }
                }
            }
        } catch (...) {
            // Keep defaults on parse failure.
        }
    }

    finalizeSettings(g_config.global);

    // Implicit layer already gates on ENABLE_AUTOHDR; also allow always-on if config says so
    // and env is unset (useful for VkConfig). If ENABLE_AUTOHDR is set to 0, stay off.
    if (enable && std::string(enable) == "0") {
        g_config.enabled = false;
    } else if (envEnabled) {
        g_config.enabled = true;
    }

    g_loaded = true;
}

namespace {

const Profile *matchingProfileLocked()
{
    if (g_config.exeName.empty()) {
        return nullptr;
    }
    const std::string exeLower = toLower(g_config.exeName);
    for (const Profile &profile : g_config.profiles) {
        if (toLower(profile.exe) == exeLower) {
            return &profile;
        }
    }
    // Also allow basename substring match for Wine (.exe path quirks).
    for (const Profile &profile : g_config.profiles) {
        const std::string want = toLower(profile.exe);
        if (!want.empty() && exeLower.find(want) != std::string::npos) {
            return &profile;
        }
    }
    return nullptr;
}

void ensureLoaded()
{
    if (!g_loaded) {
        reloadConfig();
    }
}

} // namespace

bool isEffectActiveForCurrentProcess()
{
    std::lock_guard lock(g_configMutex);
    ensureLoaded();
    if (!g_config.enabled) {
        return false;
    }
    if (const Profile *profile = matchingProfileLocked()) {
        return profile->enabled;
    }
    // No profile: active whenever ENABLE_AUTOHDR is set (layer enable_environment).
    return true;
}

AutoHdr::CalibrationSettings activeSettings()
{
    std::lock_guard lock(g_configMutex);
    ensureLoaded();
    if (const Profile *profile = matchingProfileLocked()) {
        return profile->settings;
    }
    return g_config.global;
}

OutputEncoding activeEncoding()
{
    std::lock_guard lock(g_configMutex);
    ensureLoaded();
    if (const Profile *profile = matchingProfileLocked(); profile && profile->encoding != OutputEncoding::Auto) {
        return profile->encoding;
    }
    return g_config.encoding;
}

bool wantHdrMetadata()
{
    std::lock_guard lock(g_configMutex);
    ensureLoaded();
    return g_config.setHdrMetadata;
}

bool wantPreferHdrSwapchain()
{
    std::lock_guard lock(g_configMutex);
    ensureLoaded();
    return g_config.preferHdrSwapchain;
}

bool saveOverlaySettings(float intensity, float colorIntensity, float expansionShape, float blackFloor,
                         float highlightStretch)
{
    std::lock_guard lock(g_configMutex);
    ensureLoaded();

    intensity = AutoHdr::clampIntensity(intensity);
    colorIntensity = AutoHdr::clampColorIntensity(colorIntensity);
    expansionShape = AutoHdr::clampExpansionShape(expansionShape);
    blackFloor = AutoHdr::clampBlackFloor(blackFloor);
    highlightStretch = AutoHdr::clampHighlightStretch(highlightStretch);

    // Update in-memory first so subsequent activeSettings() sees the new values.
    if (Profile *profile = const_cast<Profile *>(matchingProfileLocked())) {
        profile->settings.intensity = intensity;
        profile->settings.colorIntensity = colorIntensity;
        profile->settings.expansionShape = expansionShape;
        profile->settings.blackFloor = blackFloor;
        profile->settings.highlightStretch = highlightStretch;
    } else {
        g_config.global.intensity = intensity;
        g_config.global.colorIntensity = colorIntensity;
        g_config.global.expansionShape = expansionShape;
        g_config.global.blackFloor = blackFloor;
        g_config.global.highlightStretch = highlightStretch;
    }

    if (g_config.configPath.empty()) {
        return false;
    }

    const std::filesystem::path path = g_config.configPath;
    try {
        toml::table root;
        if (std::filesystem::exists(path)) {
            root = toml::parse_file(path.string());
        }

        const Profile *matched = matchingProfileLocked();
        if (matched) {
            auto *arr = root["profile"].as_array();
            if (!arr) {
                root.insert_or_assign("profile", toml::array{});
                arr = root["profile"].as_array();
            }
            bool updated = false;
            const std::string want = toLower(matched->exe);
            for (auto &node : *arr) {
                auto *table = node.as_table();
                if (!table) {
                    continue;
                }
                auto exe = (*table)["exe"].value<std::string>();
                if (!exe || toLower(*exe) != want) {
                    continue;
                }
                table->insert_or_assign("intensity", static_cast<double>(intensity));
                table->insert_or_assign("color_intensity", static_cast<double>(colorIntensity));
                table->insert_or_assign("expansion_shape", static_cast<double>(expansionShape));
                table->insert_or_assign("black_floor", static_cast<double>(blackFloor));
                table->insert_or_assign("highlight_stretch", static_cast<double>(highlightStretch));
                updated = true;
                break;
            }
            if (!updated) {
                toml::table table;
                table.insert("exe", matched->exe);
                table.insert("intensity", static_cast<double>(intensity));
                table.insert("color_intensity", static_cast<double>(colorIntensity));
                table.insert("expansion_shape", static_cast<double>(expansionShape));
                table.insert("black_floor", static_cast<double>(blackFloor));
                table.insert("highlight_stretch", static_cast<double>(highlightStretch));
                arr->push_back(std::move(table));
            }
        } else {
            auto *global = root["global"].as_table();
            if (!global) {
                root.insert_or_assign("global", toml::table{});
                global = root["global"].as_table();
            }
            global->insert_or_assign("intensity", static_cast<double>(intensity));
            global->insert_or_assign("color_intensity", static_cast<double>(colorIntensity));
            global->insert_or_assign("expansion_shape", static_cast<double>(expansionShape));
            global->insert_or_assign("black_floor", static_cast<double>(blackFloor));
            global->insert_or_assign("highlight_stretch", static_cast<double>(highlightStretch));
        }

        const std::filesystem::path tmp = path.string() + ".tmp";
        {
            std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
            if (!out) {
                return false;
            }
            out << root;
        }
        std::filesystem::rename(tmp, path);
        return true;
    } catch (...) {
        return false;
    }
}

} // namespace AutoHdrVk
