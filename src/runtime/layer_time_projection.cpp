#include <bloom/runtime/layer_time_projection.hpp>

#include <algorithm>
#include <charconv>
#include <cstdint>
#include <limits>
#include <string>

namespace bloom::runtime {
namespace {

template <typename Value>
[[nodiscard]] const Value* constantOf(const document::Composition& composition,
                                      const document::NodeRecord& node,
                                      const std::string_view role) {
    const auto binding = std::ranges::find(node.parameters, role, &document::ParameterBinding::role);
    if (binding == node.parameters.end())
        return nullptr;
    const auto* record = composition.parameters().find(binding->parameterId);
    const auto* constant =
        record ? std::get_if<document::ConstantValueSource>(&record->source) : nullptr;
    return constant ? std::get_if<Value>(&constant->value) : nullptr;
}

[[nodiscard]] const document::AssetRecord* assetOf(const document::Project& project,
                                                   const document::Composition& composition,
                                                   const document::NodeRecord& node) {
    const auto* text = constantOf<std::string>(composition, node, "asset");
    if (text == nullptr)
        return nullptr;
    std::uint64_t raw = 0;
    const auto parsed = std::from_chars(text->data(), text->data() + text->size(), raw);
    if (parsed.ec != std::errc{} || parsed.ptr != text->data() + text->size())
        return nullptr;
    return project.findAsset(document::AssetId::fromRaw(raw));
}

[[nodiscard]] std::optional<core::RationalTime> framesAt(const std::uint64_t count,
                                                         const document::FrameRate rate) {
    if (count == 0 || count > static_cast<std::uint64_t>(std::numeric_limits<std::int32_t>::max()))
        return std::nullopt;
    return core::RationalTime::create(static_cast<std::int64_t>(count) * rate.denominator(),
                                      rate.numerator());
}

} // namespace

std::optional<TimeExtent> resolveVideoStreamExtent(const document::AssetRecord& asset) {
    if (asset.kind != document::AssetKind::Video)
        return std::nullopt;
    const auto stream = std::ranges::find(asset.videoStreams, 1U, &document::AssetVideoStream::kind);
    if (stream == asset.videoStreams.end() || stream->framePeriod.numerator() <= 0 ||
        stream->framePeriod.denominator() > std::numeric_limits<std::uint32_t>::max() ||
        stream->framePeriod.numerator() > std::numeric_limits<std::uint32_t>::max())
        return std::nullopt;
    const auto streamRate =
        document::FrameRate::create(static_cast<std::uint32_t>(stream->framePeriod.denominator()),
                                    static_cast<std::uint32_t>(stream->framePeriod.numerator()));
    const auto duration = streamRate ? framesAt(asset.frames, *streamRate) : std::nullopt;
    return duration ? std::optional{TimeExtent{*duration, *streamRate}} : std::nullopt;
}

std::optional<TimeExtent> resolveSourceExtent(const document::Project& project,
                                              const document::Composition& composition,
                                              const document::NodeId sourceNodeId) {
    const auto* node = composition.graph().findNode(sourceNodeId);
    if (node == nullptr)
        return std::nullopt;
    const auto rate = composition.format().frameRate();
    const auto identityStart = [&] {
        const auto* start = constantOf<std::int64_t>(composition, *node, "startFrame");
        return start == nullptr || *start == 0;
    };
    if (node->typeId == "bloom.image-source") {
        const auto* asset = assetOf(project, composition, *node);
        if (asset == nullptr || asset->kind != document::AssetKind::Sequence || !identityStart() ||
            asset->manifest.last < asset->manifest.first)
            return std::nullopt;
        const auto length = static_cast<std::uint64_t>(asset->manifest.last) -
                            static_cast<std::uint64_t>(asset->manifest.first) + 1;
        const auto duration = framesAt(length, rate);
        return duration ? std::optional{TimeExtent{*duration, rate}} : std::nullopt;
    }
    if (node->typeId == "bloom.video-source") {
        const auto* asset = assetOf(project, composition, *node);
        if (asset == nullptr || !identityStart())
            return std::nullopt;
        return resolveVideoStreamExtent(*asset);
    }
    if (node->typeId == "bloom.audio-source") {
        const auto* asset = assetOf(project, composition, *node);
        if (asset == nullptr || !identityStart() || asset->duration <= core::RationalTime{})
            return std::nullopt;
        return TimeExtent{asset->duration, rate};
    }
    if (node->typeId == document::kCompositionSourceNodeType) {
        const auto* id = constantOf<std::int64_t>(composition, *node, "composition");
        const auto* offset = constantOf<double>(composition, *node, "timeOffset");
        const auto* scale = constantOf<double>(composition, *node, "timeScale");
        if (id == nullptr || *id <= 0 || offset == nullptr || scale == nullptr || *offset != 0.0 ||
            *scale != 1.0)
            return std::nullopt;
        const auto* nested = project.findComposition(
            document::CompositionId::fromRaw(static_cast<std::uint64_t>(*id)));
        if (nested == nullptr)
            return std::nullopt;
        return TimeExtent{nested->duration(), nested->format().frameRate()};
    }
    return std::nullopt;
}

std::optional<TimeExtent> resolveLayerExtent(const document::Project& project,
                                             const document::Composition& composition,
                                             const document::LayerTiming& timing) {
    if (const auto* fixed = std::get_if<document::FixedLayerExtent>(&timing.contentExtent))
        return TimeExtent{fixed->duration, fixed->frameRate};
    if (const auto* source = std::get_if<document::SourceLayerExtent>(&timing.contentExtent))
        return resolveSourceExtent(project, composition, source->sourceNodeId);
    return std::nullopt;
}

std::optional<core::RationalTime> placementTimeOfLayerTime(const document::LayerTiming& timing,
                                                           const core::RationalTime layerTime) {
    // start + tau * den / num, with checked 64-bit products.
    const auto multiply = [](const std::int64_t a, const std::int64_t b) -> std::optional<std::int64_t> {
        constexpr auto limit = std::numeric_limits<std::int64_t>::max();
        if (a == std::numeric_limits<std::int64_t>::min() ||
            b == std::numeric_limits<std::int64_t>::min())
            return std::nullopt;
        const auto magnitude = a < 0 ? -a : a;
        if (magnitude != 0 && (b > limit / magnitude || b < -(limit / magnitude)))
            return std::nullopt;
        return a * b;
    };
    const auto& start = timing.startTime;
    const auto scaledNumerator = multiply(layerTime.numerator(), timing.speed.denominator());
    const auto scaledDenominator = multiply(layerTime.denominator(), timing.speed.numerator());
    if (!scaledNumerator || !scaledDenominator)
        return std::nullopt;
    const auto left = multiply(start.numerator(), *scaledDenominator);
    const auto right = multiply(*scaledNumerator, start.denominator());
    const auto denominator = multiply(start.denominator(), *scaledDenominator);
    if (!left || !right || !denominator ||
        (*right > 0 && *left > std::numeric_limits<std::int64_t>::max() - *right) ||
        (*right < 0 && *left < std::numeric_limits<std::int64_t>::min() - *right))
        return std::nullopt;
    return core::RationalTime::create(*left + *right, *denominator);
}

} // namespace bloom::runtime
