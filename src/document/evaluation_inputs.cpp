#include <bloom/document/evaluation_inputs.hpp>

#include <algorithm>
#include <functional>
#include <span>
#include <type_traits>
#include <utility>
#include <variant>

namespace bloom::document {

namespace {

// Storage shared by both sides is equal without looking at a single record.
template <typename Record, typename Equal>
[[nodiscard]] bool sameRecords(const std::span<const Record> left,
                               const std::span<const Record> right, Equal equal) {
    if (left.size() != right.size())
        return false;
    if (left.data() == right.data())
        return true;
    return std::ranges::equal(left, right, equal);
}

template <typename Record>
[[nodiscard]] bool sameRecords(const std::span<const Record> left,
                               const std::span<const Record> right) {
    return sameRecords(left, right, std::equal_to<>{});
}

// A full match first: an unchanged record never pays for the copy evaluationView() makes.
template <typename Record>
[[nodiscard]] bool sameEvaluationRecord(const Record& left, const Record& right) {
    return left == right || evaluationView(left) == evaluationView(right);
}

} // namespace

bool nodeMuted(const NodeLayout& layout, const NodeId node) noexcept {
    const auto found = layout.find(node);
    return found != layout.end() && found->second.muted;
}

LayerOutputBoundary evaluationView(LayerOutputBoundary boundary) {
    boundary.name.clear();
    boundary.labelColor.reset();
    boundary.locked = false;
    // Document 1.30: the layer table's shy filter and whether the name follows the source.
    boundary.shy = false;
    boundary.nameFollowsSource = false;
    for (auto& mask : boundary.masks)
        mask.name.clear();
    return boundary;
}

AssetRecord evaluationView(AssetRecord asset) {
    asset.name.clear();
    asset.folder.reset();
    asset.tags.clear();
    asset.order = 0;
    return asset;
}

ScalarKeyframe evaluationView(ScalarKeyframe keyframe) noexcept {
    keyframe.handleMode = KeyframeHandleMode::Free;
    return keyframe;
}

PathKeyframe evaluationView(PathKeyframe keyframe) noexcept {
    keyframe.handleMode = KeyframeHandleMode::Free;
    return keyframe;
}

AnimationCurveRecord evaluationView(AnimationCurveRecord record) {
    std::visit(
        [](auto& curve) {
            using Curve = std::decay_t<decltype(curve)>;
            if constexpr (kWholeValueAnimationCurve<Curve>) {
                for (auto& key : curve.keyframes)
                    key.handleMode = KeyframeHandleMode::Free;
            } else {
                for (auto& component : curve.components)
                    for (auto& key : component.keyframes)
                        key.handleMode = KeyframeHandleMode::Free;
            }
        },
        record);
    return record;
}

bool sameEvaluationGraph(const CanonicalGraph& left, const CanonicalGraph& right) {
    if (!sameRecords(left.nodes(), right.nodes()) || !sameRecords(left.edges(), right.edges()) ||
        left.compositionOutput() != right.compositionOutput() ||
        !sameRecords(left.layerOutputs(), right.layerOutputs(),
                     [](const LayerOutputBoundary& a, const LayerOutputBoundary& b) {
                         return sameEvaluationRecord(a, b);
                     }))
        return false;
    return sameRecords(left.merges(), right.merges(), [](const LayerStack& a, const LayerStack& b) {
        return a.nodeId() == b.nodeId() && a.enabled() == b.enabled() &&
               std::ranges::equal(a.entries(), b.entries());
    });
}

bool sameEvaluationLayout(const NodeLayout& left, const NodeLayout& right) {
    // Both maps are ordered by NodeId, so the muted ids come out in the same order.
    auto leftRecord = left.begin();
    auto rightRecord = right.begin();
    const auto nextMuted = [](auto& record, const auto end) {
        while (record != end && !record->second.muted)
            ++record;
    };
    while (true) {
        nextMuted(leftRecord, left.end());
        nextMuted(rightRecord, right.end());
        if (leftRecord == left.end() || rightRecord == right.end())
            return leftRecord == left.end() && rightRecord == right.end();
        if (leftRecord->first != rightRecord->first)
            return false;
        ++leftRecord;
        ++rightRecord;
    }
}

bool sameEvaluationInputs(const Composition& left, const Composition& right) {
    // Evaluation inputs of the shell. name, folder, workArea, safeAreas, timecode and
    // backgroundColor are presentation (see the header).
    if (left.id() != right.id() || left.duration() != right.duration() ||
        left.format() != right.format() ||
        left.workingColorSpaceId() != right.workingColorSpaceId())
        return false;
    if (left.contentIdentity() == right.contentIdentity())
        return true;
    // nodeGroups is presentation whole; nodeLayout only through mute.
    return sameRecords(left.parameters().records(), right.parameters().records()) &&
           sameRecords(left.animationCurves().records(), right.animationCurves().records(),
                       [](const AnimationCurveRecord& a, const AnimationCurveRecord& b) {
                           return sameEvaluationRecord(a, b);
                       }) &&
           sameEvaluationGraph(left.graph(), right.graph()) &&
           sameEvaluationLayout(left.nodeLayout(), right.nodeLayout());
}

bool sameEvaluationInputs(const Project& left, const Project& right) {
    // The project name, asset folders and the export queue are presentation (see the header).
    return left.id() == right.id() &&
           sameRecords(left.compositions(), right.compositions(),
                       [](const Composition& a, const Composition& b) {
                           return sameEvaluationInputs(a, b);
                       }) &&
           sameRecords(left.assets(), right.assets(),
                       [](const AssetRecord& a, const AssetRecord& b) {
                           return sameEvaluationRecord(a, b);
                       }) &&
           sameRecords(left.extensionRecords(), right.extensionRecords()) &&
           sameRecords(left.typedDataBlocks(), right.typedDataBlocks());
}

} // namespace bloom::document
