#pragma once

// #364: a real multi-rank Cryptomatte OpenEXR generated in-test (Cryptomatte specification v1.2):
// a premultiplied RGBA beauty plus `CryptoObject00` and `CryptoObject01` rank layers (four
// (ID, coverage) pairs per pixel, sorted by coverage) and, optionally, the
// `cryptomatte/<key>/manifest` metadata naming every object.

#include <ImathBox.h>
#include <ImfChannelList.h>
#include <ImfChromaticitiesAttribute.h>
#include <ImfFrameBuffer.h>
#include <ImfHeader.h>
#include <ImfOutputFile.h>
#include <ImfStringAttribute.h>

#include <bloom/runtime/cryptomatte_matte_list.hpp>

#include <array>
#include <bit>
#include <cstdio>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

namespace bloom::cryptomatte_fixture {

inline constexpr int kWidth = 4;
inline constexpr int kHeight = 2;
inline const std::array<std::string, 4> kObjects{"sphere", "cube", "floor", "sky"};

// Per pixel: (object index, coverage) pairs, highest coverage first.
inline const std::array<std::vector<std::pair<int, float>>, kWidth * kHeight> kPixels{{
    {{0, 1.0F}},
    {{0, 0.6F}, {1, 0.4F}},
    {{1, 0.5F}, {2, 0.3F}, {3, 0.2F}},
    {{2, 0.25F}, {3, 0.25F}, {1, 0.25F}, {0, 0.25F}},
    {{3, 1.0F}},
    {},
    {{1, 1.0F}},
    {{2, 0.75F}, {0, 0.25F}},
}};

[[nodiscard]] inline float expectedCoverage(const std::size_t pixel,
                                            const std::vector<int>& selected) {
    float coverage = 0.0F;
    for (const auto& [object, value] : kPixels[pixel])
        for (const auto chosen : selected)
            if (chosen == object)
                coverage = std::min(1.0F, coverage + value);
    return coverage;
}

[[nodiscard]] inline std::string hex(const std::uint32_t value) {
    std::array<char, 9> buffer{};
    std::snprintf(buffer.data(), buffer.size(), "%08x", value);
    return {buffer.data(), 8};
}

// `manifestText`, when non-empty, is written verbatim as the manifest (a malformed-manifest case).
// `objectShift` rotates which object each coverage entry names (a second sequence frame), and
// `beauty = false` writes a sidecar-style file with only the Cryptomatte ranks (no RGB).
inline void writeCryptomatteExr(const std::filesystem::path& path, const bool manifest,
                                const std::string& layer = "CryptoObject",
                                const std::string& manifestText = {},
                                const std::size_t objectShift = 0, const bool beauty = true) {
    const Imath::Box2i window(Imath::V2i(0, 0), Imath::V2i(kWidth - 1, kHeight - 1));
    Imf::Header header(window, window);
    header.insert("chromaticities", Imf::ChromaticitiesAttribute(Imf::Chromaticities()));
    const std::string key = "cryptomatte/" + hex(runtime::cryptomatteNameId(layer)).substr(0, 7);
    header.insert(key + "/name", Imf::StringAttribute(layer));
    header.insert(key + "/hash", Imf::StringAttribute("MurmurHash3_32"));
    header.insert(key + "/conversion", Imf::StringAttribute("uint32_to_float32"));
    if (!manifestText.empty()) {
        header.insert(key + "/manifest", Imf::StringAttribute(manifestText));
    } else if (manifest) {
        std::string json = "{";
        for (std::size_t index = 0; index < kObjects.size(); ++index) {
            if (index != 0)
                json += ",";
            json += "\"" + kObjects[index] + "\":\"" +
                    hex(runtime::cryptomatteNameId(kObjects[index])) + "\"";
        }
        json += "}";
        header.insert(key + "/manifest", Imf::StringAttribute(json));
    }
    constexpr std::size_t samples = static_cast<std::size_t>(kWidth) * kHeight;
    std::vector<std::pair<std::string, std::vector<float>>> planes;
    planes.reserve(12); // plane() hands out references; they must survive later planes
    const auto plane = [&](std::string name) -> std::vector<float>& {
        planes.emplace_back(std::move(name), std::vector<float>(samples, 0.0F));
        return planes.back().second;
    };
    if (beauty) {
        plane("R");
        plane("G");
        plane("B");
        auto& alpha = plane("A");
        alpha.assign(samples, 1.0F);
        for (std::size_t index = 0; index < samples; ++index) {
            planes[0].second[index] = 0.1F * static_cast<float>(index + 1);
            planes[1].second[index] = 0.5F;
            planes[2].second[index] = 0.25F;
        }
    }
    for (int rankLayer = 0; rankLayer < 2; ++rankLayer) {
        const auto prefix = layer + (rankLayer == 0 ? "00." : "01.");
        auto& id0 = plane(prefix + "R");
        auto& coverage0 = plane(prefix + "G");
        auto& id1 = plane(prefix + "B");
        auto& coverage1 = plane(prefix + "A");
        for (std::size_t pixel = 0; pixel < samples; ++pixel) {
            const auto& entries = kPixels[pixel];
            const auto put = [&](const std::size_t rank, std::vector<float>& id,
                                 std::vector<float>& coverage) {
                if (rank < entries.size()) {
                    id[pixel] = std::bit_cast<float>(runtime::cryptomatteNameId(
                        kObjects[(static_cast<std::size_t>(entries[rank].first) + objectShift) %
                                 kObjects.size()]));
                    coverage[pixel] = entries[rank].second;
                }
            };
            put(static_cast<std::size_t>(rankLayer) * 2, id0, coverage0);
            put(static_cast<std::size_t>(rankLayer) * 2 + 1, id1, coverage1);
        }
    }
    for (const auto& [name, values] : planes)
        header.channels().insert(name, Imf::Channel(Imf::FLOAT));
    Imf::FrameBuffer frameBuffer;
    for (auto& [name, values] : planes)
        frameBuffer.insert(name, Imf::Slice(Imf::FLOAT, reinterpret_cast<char*>(values.data()),
                                            sizeof(float), kWidth * sizeof(float)));
    Imf::OutputFile file(path.string().c_str(), header);
    file.setFrameBuffer(frameBuffer);
    file.writePixels(kHeight);
}

} // namespace bloom::cryptomatte_fixture
