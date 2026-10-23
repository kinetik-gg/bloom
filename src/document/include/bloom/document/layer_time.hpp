#pragma once

#include <bloom/core/rational_time.hpp>
#include <bloom/document/composition_settings.hpp>
#include <bloom/document/ids.hpp>

#include <cstdint>
#include <numeric>
#include <optional>
#include <string_view>
#include <variant>

// Layer-owned time (ADR 0024). A Layer Output converts its placement time t into
//
//   layer time    tau = (t - startTime) * speed
//   content time  c   = outOfExtent(timeRemap enabled ? timeRemap(tau) : tau)
//
// Every member defaults to identity, so a boundary decoded without them renders exactly as an
// absolute-composition-time layer did. The evaluation contract lives in runtime/layer_time.hpp and
// docs/architecture/animation-and-time.md ("Layer Time").
namespace bloom::document {

// What a layer shows where its content time falls outside its content extent. Stored values are
// the enumerator values; they are a closed vocabulary shared by Properties and the Timeline.
enum class LayerTimeMode : std::uint8_t { None = 0, Hold = 1, Loop = 2, PingPong = 3 };

[[nodiscard]] constexpr std::optional<LayerTimeMode>
layerTimeModeFromStoredValue(const std::int64_t value) noexcept {
    if (value < 0 || value > 3)
        return std::nullopt;
    return static_cast<LayerTimeMode>(value);
}

[[nodiscard]] constexpr std::string_view layerTimeModeName(const LayerTimeMode mode) noexcept {
    switch (mode) {
    case LayerTimeMode::None:
        return "none";
    case LayerTimeMode::Hold:
        return "hold";
    case LayerTimeMode::Loop:
        return "loop";
    case LayerTimeMode::PingPong:
        return "pingPong";
    }
    return {};
}

[[nodiscard]] constexpr std::optional<LayerTimeMode>
layerTimeModeFromName(const std::string_view name) noexcept {
    for (const auto mode : {LayerTimeMode::None, LayerTimeMode::Hold, LayerTimeMode::Loop,
                            LayerTimeMode::PingPong})
        if (layerTimeModeName(mode) == name)
            return mode;
    return std::nullopt;
}

// An exact positive speed ratio. Terms are reduced, each in [1, 2^31 - 1], and the value lies in
// [1/1000, 1000]. Reverse playback belongs to Time Remap, never to a negative speed.
class LayerSpeed final {
  public:
    static constexpr std::int64_t kMaximumTerm = 2147483647;
    static constexpr std::int64_t kMaximumRatio = 1000;

    constexpr LayerSpeed() noexcept = default;

    [[nodiscard]] static constexpr std::optional<LayerSpeed>
    create(const std::int64_t numerator, const std::int64_t denominator) noexcept {
        if (numerator <= 0 || denominator <= 0)
            return std::nullopt;
        const auto divisor = std::gcd(numerator, denominator);
        const auto n = numerator / divisor;
        const auto d = denominator / divisor;
        if (n > kMaximumTerm || d > kMaximumTerm || n > kMaximumRatio * d ||
            d > kMaximumRatio * n)
            return std::nullopt;
        return LayerSpeed(n, d);
    }

    [[nodiscard]] constexpr std::int64_t numerator() const noexcept { return numerator_; }
    [[nodiscard]] constexpr std::int64_t denominator() const noexcept { return denominator_; }
    [[nodiscard]] constexpr bool isIdentity() const noexcept {
        return numerator_ == 1 && denominator_ == 1;
    }

    friend constexpr bool operator==(const LayerSpeed&, const LayerSpeed&) noexcept = default;

  private:
    constexpr LayerSpeed(const std::int64_t numerator, const std::int64_t denominator) noexcept
        : numerator_(numerator), denominator_(denominator) {}

    std::int64_t numerator_ = 1;
    std::int64_t denominator_ = 1;
};

// The content interval [0, duration) the out-of-extent modes refer to, with the frame grid whose
// last frame start Hold and PingPong use.
struct FixedLayerExtent final {
    core::RationalTime duration;
    FrameRate frameRate = FrameRate::framesPerSecond24();
    friend bool operator==(const FixedLayerExtent&, const FixedLayerExtent&) = default;
};

// Resolved at compile time from a media or composition source node's natural length, so a relink
// or a nested duration change is followed without a document edit.
struct SourceLayerExtent final {
    NodeId sourceNodeId;
    friend bool operator==(const SourceLayerExtent&, const SourceLayerExtent&) = default;
};

// std::monostate is the Unbounded extent: content time passes through and the modes are inert.
using LayerContentExtent = std::variant<std::monostate, FixedLayerExtent, SourceLayerExtent>;

struct LayerTiming final {
    core::RationalTime startTime{};
    LayerSpeed speed{};
    LayerContentExtent contentExtent{};
    LayerTimeMode beforeMode = LayerTimeMode::None;
    LayerTimeMode afterMode = LayerTimeMode::None;

    // Identity timing creates no runtime time scope: the layer's upstream evaluates on its
    // placement clock, exactly as before layer-owned time existed. Time Remap is a parameter, not a
    // member, so callers also check that the boundary has no remap binding.
    [[nodiscard]] bool isIdentityMapping() const noexcept {
        return startTime == core::RationalTime{} && speed.isIdentity() &&
               std::holds_alternative<std::monostate>(contentExtent);
    }
    [[nodiscard]] bool isValid() const noexcept {
        if (const auto* fixed = std::get_if<FixedLayerExtent>(&contentExtent))
            return fixed->duration > core::RationalTime{};
        if (const auto* source = std::get_if<SourceLayerExtent>(&contentExtent))
            return source->sourceNodeId.isValid();
        return true;
    }

    friend bool operator==(const LayerTiming&, const LayerTiming&) = default;
};

} // namespace bloom::document
