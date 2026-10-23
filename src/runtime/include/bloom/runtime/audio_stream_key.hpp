#pragma once
#include <bloom/document/ids.hpp>
#include <compare>
#include <cstdint>
namespace bloom::runtime {
// Identity of one decoded audio buffer: an asset and one of its audio streams. Preview and export
// buffer stores key on it so two Video nodes can play different streams of one file.
struct AudioStreamKey final {
    document::AssetId asset;
    std::uint32_t stream = 0;
    friend auto operator<=>(const AudioStreamKey& a, const AudioStreamKey& b) {
        if (const auto order = a.asset.value() <=> b.asset.value(); order != 0)
            return order;
        return a.stream <=> b.stream;
    }
    friend bool operator==(const AudioStreamKey&, const AudioStreamKey&) = default;
};
} // namespace bloom::runtime
