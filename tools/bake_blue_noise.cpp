// Build-time tool: bake a 64x64 R8 blue-noise tile for present-pass dither.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <random>
#include <string>
#include <vector>

namespace {

constexpr int kWidth = 64;
constexpr int kHeight = 64;

void generateBlueNoise(std::vector<uint8_t> &out)
{
    out.assign(static_cast<size_t>(kWidth * kHeight), 0);

    std::vector<std::pair<float, size_t>> ranked;
    ranked.reserve(static_cast<size_t>(kWidth * kHeight));

    std::mt19937 rng(0xA11DBEEFu);
    for (int y = 0; y < kHeight; ++y) {
        for (int x = 0; x < kWidth; ++x) {
            const float u = static_cast<float>(x) + 0.5f;
            const float v = static_cast<float>(y) + 0.5f;
            const float g1 = std::fmod(u * 0.754877666f + v * 0.569840296f, 1.0f);
            const float g2 = std::fmod(u * 0.569840296f + v * 0.754877666f, 1.0f);
            const float score = g1 + g2 * 0.37f + static_cast<float>(rng()) / static_cast<float>(UINT32_MAX) * 0.08f;
            ranked.emplace_back(score, static_cast<size_t>(y * kWidth + x));
        }
    }

    std::sort(ranked.begin(), ranked.end(),
              [](const auto &a, const auto &b) { return a.first < b.first; });

    for (size_t rank = 0; rank < ranked.size(); ++rank) {
        const float t = (static_cast<float>(rank) + 0.5f) / static_cast<float>(ranked.size());
        out[ranked[rank].second] = static_cast<uint8_t>(std::clamp(t * 255.0f, 0.0f, 255.0f));
    }
}

bool writeHeader(const std::string &path, const std::vector<uint8_t> &pixels)
{
    std::ofstream out(path);
    if (!out) {
        return false;
    }
    out << "#pragma once\n\n";
    out << "#include <cstdint>\n\n";
    out << "namespace AutoHdrVk {\n\n";
    out << "inline constexpr uint32_t kBlueNoiseAtlasWidth = " << kWidth << "u;\n";
    out << "inline constexpr uint32_t kBlueNoiseAtlasHeight = " << kHeight << "u;\n";
    out << "inline constexpr uint8_t kBlueNoiseAtlasR8[] = {\n";
    for (size_t i = 0; i < pixels.size(); ++i) {
        if (i % 16 == 0) {
            out << "    ";
        }
        out << static_cast<int>(pixels[i]);
        if (i + 1 < pixels.size()) {
            out << ", ";
        }
        if (i % 16 == 15) {
            out << "\n";
        }
    }
    if (pixels.size() % 16 != 0) {
        out << "\n";
    }
    out << "};\n\n";
    out << "} // namespace AutoHdrVk\n";
    return true;
}

} // namespace

int main(int argc, char **argv)
{
    if (argc < 2) {
        std::fprintf(stderr, "usage: %s <blue_noise_atlas.h>\n", argv[0]);
        return 1;
    }

    std::vector<uint8_t> pixels;
    generateBlueNoise(pixels);
    if (!writeHeader(argv[1], pixels)) {
        std::fprintf(stderr, "failed to write %s\n", argv[1]);
        return 1;
    }
    return 0;
}
