#include "composition_editor_support.hpp"

#include <bloom/ui/timeline_frame_math.hpp>

#include <bloom/document/graph.hpp>

#include <QChar>

#include <algorithm>
#include <string>

namespace bloom::ui {
namespace {

const document::LayerOutputBoundary* layerBoundary(const document::Composition& composition,
                                                   const document::LayerId layerId) {
    const auto boundaries = composition.graph().layerOutputs();
    const auto found = std::ranges::find_if(
        boundaries, [layerId](const auto& candidate) { return candidate.layerId == layerId; });
    return found == boundaries.end() ? nullptr : &*found;
}

} // namespace

QString layerName(const document::Composition& composition, const document::LayerId layerId) {
    const auto* boundary = layerBoundary(composition, layerId);
    if (boundary == nullptr || boundary->name.empty()) {
        return QStringLiteral("Layer %1").arg(layerId.value());
    }
    return QString::fromStdString(boundary->name);
}

const document::NodeRecord* directSourceNode(const CompositionSession& session,
                                             const document::LayerId layerId) {
    const auto* composition = session.composition();
    const auto sourceNodeId = session.directSourceNodeForLayer(layerId);
    return composition != nullptr && sourceNodeId.has_value()
               ? composition->graph().findNode(*sourceNodeId)
               : nullptr;
}

bool isKnownSource(const document::NodeRecord* node, const std::string_view typeId,
                   const std::uint32_t schemaVersion) {
    return node != nullptr && node->typeId == typeId && node->schemaVersion == schemaVersion;
}

std::optional<TimelineFrameContext> frameContextFor(const CompositionSession& session) {
    const auto* composition = session.composition();
    if (composition == nullptr) {
        return std::nullopt;
    }
    const auto frameRate = composition->format().frameRate();
    const auto duration = composition->duration();
    const auto maxIndex = maxFrameIndex(frameRate, duration);
    if (!maxIndex.has_value()) {
        return std::nullopt;
    }
    return TimelineFrameContext{frameRate, duration, *maxIndex};
}

QString formatExactSeconds(const core::RationalTime time) {
    constexpr int kDecimalPlaces = 3;
    constexpr unsigned __int128 kScale = 1000;
    const std::int64_t numerator = time.numerator();
    const auto denominator = static_cast<unsigned __int128>(time.denominator());
    const bool negative = numerator < 0;
    const auto magnitude = negative
                               ? static_cast<unsigned __int128>(-static_cast<__int128>(numerator))
                               : static_cast<unsigned __int128>(numerator);
    const auto wholeSeconds = static_cast<qulonglong>(magnitude / denominator);
    const auto remainder = magnitude % denominator;
    const auto scaledFraction = static_cast<qulonglong>((remainder * kScale) / denominator);
    return QStringLiteral("%1%2.%3s")
        .arg(negative ? QStringLiteral("-") : QString())
        .arg(wholeSeconds)
        .arg(scaledFraction, kDecimalPlaces, 10, QChar('0'));
}

} // namespace bloom::ui
