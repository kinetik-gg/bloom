// #357. Incremental publication validation: a draft is checked against the published project it was
// drafted from, so an edit costs what it changed rather than what the project holds.
#include <bloom/document/persisted_text.hpp>
#include <bloom/document/project.hpp>

#include <algorithm>
#include <atomic>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace bloom::document {
namespace {

std::atomic<std::uint64_t> incrementalValidations{0};
std::atomic<std::uint64_t> compositionsValidated{0};
std::atomic<std::uint64_t> compositionsSkipped{0};
std::atomic<std::uint64_t> rootSectionValidations{0};
std::atomic<std::uint64_t> notProvenCount{0};

void sortUnique(std::vector<std::uint64_t>& ids) {
    std::ranges::sort(ids);
    ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
}

[[nodiscard]] bool contains(const std::vector<std::uint64_t>& sorted, const std::uint64_t id) {
    return std::ranges::binary_search(sorted, id);
}

bool notProven() {
    notProvenCount.fetch_add(1, std::memory_order_relaxed);
    return false;
}

} // namespace

Composition::DeclaredIdIndex Composition::buildDeclaredIdIndex(const Content& authored) {
    DeclaredIdIndex index;
    const auto add = [](std::vector<std::uint64_t>& ids, const auto id) {
        if (id.isValid())
            ids.push_back(id.value());
    };
    for (const auto& node : authored.graph.nodes())
        add(index.nodes, node.id);
    for (const auto& edge : authored.graph.edges())
        add(index.edges, edge.id);
    for (const auto& parameter : authored.parameters.records())
        add(index.parameters, parameter.id);
    for (const auto& record : authored.animationCurves.records()) {
        add(index.curves, animationCurveId(record));
        std::visit(
            [&](const auto& curve) {
                using Curve = std::decay_t<decltype(curve)>;
                if constexpr (kWholeValueAnimationCurve<Curve>) {
                    for (const auto& keyframe : curve.keyframes)
                        add(index.keyframes, keyframe.id);
                } else {
                    for (const auto& component : curve.components)
                        for (const auto& keyframe : component.keyframes)
                            add(index.keyframes, keyframe.id);
                }
            },
            record);
    }
    for (const auto& boundary : authored.graph.layerOutputs())
        add(index.layers, boundary.layerId);
    for (const auto& [groupId, group] : authored.nodeGroups)
        add(index.groups, groupId);
    for (const auto& stack : authored.graph.merges())
        for (const auto& entry : stack.entries())
            add(index.layerSlots, entry.slotId);
    for (auto* ids : {&index.nodes, &index.edges, &index.parameters, &index.curves,
                      &index.keyframes, &index.layers, &index.layerSlots, &index.groups})
        sortUnique(*ids);
    return index;
}

std::vector<CompositionId> Composition::buildNestedReferences(const Content& authored) {
    std::vector<CompositionId> references;
    for (const auto& node : authored.graph.nodes()) {
        if (node.typeId != kCompositionSourceNodeType)
            continue;
        for (const auto& binding : node.parameters) {
            if (binding.role != "composition")
                continue;
            const auto* parameter = authored.parameters.find(binding.parameterId);
            const auto* constant =
                parameter ? std::get_if<ConstantValueSource>(&parameter->source) : nullptr;
            const auto* value = constant ? std::get_if<std::int64_t>(&constant->value) : nullptr;
            if (!value || *value <= 0)
                continue;
            references.push_back(CompositionId::fromRaw(static_cast<std::uint64_t>(*value)));
        }
    }
    return references;
}

const Composition::DeclaredIdIndex* Composition::frozenDeclaredIds() const noexcept {
    return !editableContent_ && frozenContent_ && frozenContent_->frozenDeclaredIds
               ? &*frozenContent_->frozenDeclaredIds
               : nullptr;
}

std::vector<CompositionId> Composition::nestedReferences() const {
    if (!editableContent_ && frozenContent_ && frozenContent_->frozenNestedReferences)
        return *frozenContent_->frozenNestedReferences;
    return buildNestedReferences(content());
}

// A new shell member belongs here AND in sameEvaluationInputs(Composition) (#399).
bool Composition::sameShell(const Composition& other) const noexcept {
    return id_ == other.id_ && name_ == other.name_ && duration_ == other.duration_ &&
           format_ == other.format_ && workArea_ == other.workArea_ &&
           workingColorSpaceId_ == other.workingColorSpaceId_ && safeAreas_ == other.safeAreas_ &&
           backgroundColor_ == other.backgroundColor_ && timecode_ == other.timecode_ &&
           folder_ == other.folder_;
}

bool Project::sameRootRecordStorage(const Project& other) const noexcept {
    return assets_.identity() == other.assets_.identity() &&
           assetFolders_.identity() == other.assetFolders_.identity() &&
           extensionRecords_.identity() == other.extensionRecords_.identity() &&
           dataBlocks_.identity() == other.dataBlocks_.identity() &&
           exportJobs_.identity() == other.exportJobs_.identity();
}

Project::IncrementalValidationWork Project::incrementalValidationWork() noexcept {
    return {.incrementalValidations = incrementalValidations.load(std::memory_order_relaxed),
            .compositionsValidated = compositionsValidated.load(std::memory_order_relaxed),
            .compositionsSkipped = compositionsSkipped.load(std::memory_order_relaxed),
            .rootSectionValidations = rootSectionValidations.load(std::memory_order_relaxed),
            .notProven = notProvenCount.load(std::memory_order_relaxed)};
}

bool Project::validatesAgainst(const Project& base) const {
    incrementalValidations.fetch_add(1, std::memory_order_relaxed);
    if (id_ != base.id_)
        return notProven();

    // Root records and the project name: revalidated only when their storage changed.
    const bool rootChanged = !sameRootRecordStorage(base) || name_ != base.name_;

    // Which compositions changed. Unchanged ones still share their frozen content with `base`.
    std::unordered_map<CompositionId, const Composition*> before;
    before.reserve(base.compositions_.size());
    for (const auto& composition : base.compositions_)
        before.emplace(composition.id(), &composition);
    std::vector<std::size_t> changed;
    std::vector<std::size_t> unchanged;
    std::unordered_set<CompositionId> ids;
    for (std::size_t ordinal = 0; ordinal < compositions_.size(); ++ordinal) {
        const auto& composition = compositions_[ordinal];
        if (!composition.id().isValid() || !ids.insert(composition.id()).second)
            return notProven();
        const auto found = before.find(composition.id());
        if (found != before.end() &&
            found->second->contentIdentity() == composition.contentIdentity() &&
            found->second->sameShell(composition) && composition.frozenDeclaredIds() != nullptr)
            unchanged.push_back(ordinal);
        else
            changed.push_back(ordinal);
    }
    const bool compositionSetChanged =
        !changed.empty() || base.compositions_.size() != ids.size() ||
        std::ranges::any_of(base.compositions_,
                            [&](const auto& c) { return !ids.contains(c.id()); });
    compositionsSkipped.fetch_add(unchanged.size(), std::memory_order_relaxed);

    if (rootChanged) {
        rootSectionValidations.fetch_add(1, std::memory_order_relaxed);
        ValidationResult root;
        validateRootRecords(root);
        if (!root.ok())
            return notProven();
    }
    // ADR 0033: an export job's composition must exist, so a changed composition set re-checks the
    // queue even when the queue itself is unchanged.
    if (!rootChanged && compositionSetChanged && !exportJobs_.get().empty()) {
        ValidationResult queue;
        validateExportQueue(queue);
        if (!queue.ok())
            return notProven();
    }
    // #378: a composition's folder must exist (folders are root records; a composition shell
    // carries the reference).
    if (rootChanged || !changed.empty())
        for (const auto& composition : compositions_)
            if (composition.folder() && !findAssetFolder(*composition.folder()))
                return notProven();

    // Changed compositions: their own validation, then project-wide ID uniqueness against every
    // other changed composition and against the frozen indexes of the unchanged ones.
    std::vector<Composition::DeclaredIdIndex> fresh;
    fresh.reserve(changed.size());
    for (const auto ordinal : changed) {
        compositionsValidated.fetch_add(1, std::memory_order_relaxed);
        const auto& composition = compositions_[ordinal];
        if (!composition.validate().ok())
            return notProven();
        fresh.push_back(Composition::buildDeclaredIdIndex(composition.content()));
    }
    using Field = std::vector<std::uint64_t> Composition::DeclaredIdIndex::*;
    for (const Field field :
         {&Composition::DeclaredIdIndex::nodes, &Composition::DeclaredIdIndex::edges,
          &Composition::DeclaredIdIndex::parameters, &Composition::DeclaredIdIndex::curves,
          &Composition::DeclaredIdIndex::keyframes, &Composition::DeclaredIdIndex::layers,
          &Composition::DeclaredIdIndex::layerSlots, &Composition::DeclaredIdIndex::groups}) {
        std::unordered_set<std::uint64_t> seen;
        for (const auto& index : fresh)
            for (const auto id : index.*field) {
                if (!seen.insert(id).second)
                    return notProven();
                for (const auto ordinal : unchanged)
                    if (contains(compositions_[ordinal].frozenDeclaredIds()->*field, id))
                        return notProven();
            }
    }

    if (compositionSetChanged && !validateCompositionNesting().ok())
        return notProven();

    // Extension records and data blocks resolve targets through project truth. Their storage
    // change, or a composition change while any of them can name composition content, reruns
    // them whole.
    bool crossRecordsAffected = rootChanged;
    if (!crossRecordsAffected && compositionSetChanged) {
        // Any owner or subject may name a composition or its content.
        crossRecordsAffected = !extensionRecords_.get().empty() ||
                               std::ranges::any_of(dataBlocks_.get(), [](const auto& block) {
                                   return block.owner.has_value() || block.subject.has_value();
                               });
    }
    if (crossRecordsAffected) {
        rootSectionValidations.fetch_add(1, std::memory_order_relaxed);
        ValidationResult cross;
        validateCrossRecords(cross);
        if (!cross.ok())
            return notProven();
    }
    return true;
}

} // namespace bloom::document
