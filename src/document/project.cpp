#include <bloom/document/project.hpp>

#include <bloom/document/persisted_text.hpp>

#include <algorithm>
#include <cstddef>
#include <set>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace {

template <typename Id>
void validateProjectUniqueDeclaration(const Id id, const std::size_t compositionOrdinal,
                                      std::string path, const std::string_view typeName,
                                      std::unordered_map<Id, std::size_t>& declarationOwners,
                                      bloom::document::ValidationResult& result) {
    if (!id.isValid()) {
        return;
    }

    const auto [existing, inserted] = declarationOwners.try_emplace(id, compositionOrdinal);
    if (!inserted && existing->second != compositionOrdinal) {
        result.add(bloom::document::ValidationCode::DuplicateId, std::move(path),
                   std::string(typeName) + " ID is declared by more than one composition");
    }
}

} // namespace

namespace bloom::document {

bool Composition::nodeLocked(const NodeId node) const {
    std::vector<NodeId> pending;
    for (const auto& layer : graph_.layerOutputs())
        if (layer.locked)
            pending.push_back(layer.nodeId);
    std::unordered_set<NodeId> seen;
    while (!pending.empty()) {
        const auto id = pending.back();
        pending.pop_back();
        if (id == node)
            return true;
        if (!seen.insert(id).second)
            continue;
        for (const auto& edge : graph_.edges()) {
            const auto* input = std::get_if<NodeInputRef>(&edge.destination);
            if (input && input->nodeId == id)
                pending.push_back(edge.source.nodeId);
        }
    }
    return false;
}
bool Composition::parameterLocked(const ParameterId parameter) const {
    for (const auto& node : graph_.nodes())
        for (const auto& binding : node.parameters)
            if (binding.parameterId == parameter && nodeLocked(node.id))
                return true;
    return false;
}

bool Composition::setDuration(const core::RationalTime duration) noexcept {
    if (duration <= core::RationalTime{}) {
        return false;
    }
    duration_ = duration;
    return true;
}

ValidationResult Composition::validate() const {
    ValidationResult result;
    if (!id_.isValid()) {
        result.add(ValidationCode::InvalidId, "id", "Composition ID must not be zero");
    }
    validateHumanFacingName(name_, "name", "Composition name", result);
    if (duration_ <= core::RationalTime{}) {
        result.add(ValidationCode::InvalidValue, "duration",
                   "Composition duration must be greater than zero");
    }

    if (workArea_ && (workArea_->start < core::RationalTime{} ||
                      workArea_->start >= workArea_->end || workArea_->end > duration_))
        result.add(ValidationCode::InvalidValue, "workArea", "Invalid composition work area");
    if (workingColorSpaceId_.has_value())
        result.append("workingColorSpaceId", validateWorkingColorSpaceId(*workingColorSpaceId_));
    if (!backgroundColor_.isValid())
        result.add(ValidationCode::InvalidValue, "backgroundColor",
                   "Invalid composition background colour");
    if (!safeAreas_.isValid())
        result.add(ValidationCode::InvalidValue, "safeAreas", "Invalid composition safe areas");
    for (const auto& layer : graph_.layerOutputs()) {
        if (layer.inPoint < core::RationalTime{} || layer.inPoint >= layer.endPoint(duration_) ||
            layer.endPoint(duration_) > duration_)
            result.add(ValidationCode::InvalidValue, "graph.layerOutputs.range",
                       "Invalid layer range");
    }
    result.append("parameters", parameters_.validate());
    result.append("animationCurves", animationCurves_.validate());
    result.append("", validateAnimationCurveReferences(parameters_, animationCurves_));
    result.append("graph", graph_.validate(parameters_, builtInNodeDefinitions(), id_));
    result.append("", validateNodeLayout(nodeLayout_, graph_));
    result.append("", validateNodeGroups(nodeGroups_, graph_));
    return result;
}

const AssetRecord* Project::findAsset(AssetId id) const noexcept {
    const auto found = std::ranges::find(assets_, id, &AssetRecord::id);
    return found == assets_.end() ? nullptr : &*found;
}
AssetRecord* Project::findAsset(AssetId id) noexcept {
    return const_cast<AssetRecord*>(std::as_const(*this).findAsset(id));
}
bool Project::addAsset(AssetRecord asset) {
    if (asset.name.empty())
        asset.name = defaultAssetName(asset);
    if (findAsset(asset.id) || !asset.validate().ok())
        return false;
    assets_.push_back(std::move(asset));
    std::ranges::sort(assets_, {}, &AssetRecord::id);
    return true;
}
bool Project::removeAsset(AssetId id) {
    return std::erase_if(assets_, [id](const auto& asset) { return asset.id == id; }) != 0;
}

const AssetFolder* Project::findAssetFolder(AssetFolderId id) const noexcept {
    const auto found = std::ranges::find(assetFolders_, id, &AssetFolder::id);
    return found == assetFolders_.end() ? nullptr : &*found;
}
AssetFolder* Project::findAssetFolder(AssetFolderId id) noexcept {
    return const_cast<AssetFolder*>(std::as_const(*this).findAssetFolder(id));
}
bool Project::addAssetFolder(AssetFolder folder) {
    if (!folder.id.isValid() || !isValidHumanFacingName(folder.name) || findAssetFolder(folder.id))
        return false;
    assetFolders_.push_back(std::move(folder));
    std::ranges::sort(assetFolders_, {}, &AssetFolder::id);
    return true;
}
bool Project::removeAssetFolder(AssetFolderId id) {
    return std::erase_if(assetFolders_, [id](const auto& folder) { return folder.id == id; }) != 0;
}

const Composition* Project::findComposition(const CompositionId id) const noexcept {
    const auto iterator =
        std::find_if(compositions_.begin(), compositions_.end(),
                     [id](const auto& composition) { return composition.id() == id; });
    return iterator == compositions_.end() ? nullptr : &*iterator;
}

Composition* Project::findComposition(const CompositionId id) noexcept {
    return const_cast<Composition*>(std::as_const(*this).findComposition(id));
}

bool Project::addComposition(Composition composition) {
    if (!composition.id().isValid() || !isValidHumanFacingName(composition.name()) ||
        findComposition(composition.id()) != nullptr) {
        return false;
    }
    compositions_.push_back(std::move(composition));
    return true;
}

bool Project::removeComposition(const CompositionId id) {
    const auto iterator =
        std::find_if(compositions_.begin(), compositions_.end(),
                     [id](const auto& composition) { return composition.id() == id; });
    if (iterator == compositions_.end()) {
        return false;
    }
    compositions_.erase(iterator);
    return true;
}

const ExtensionRecord* Project::findExtensionRecord(const ExtensionRecordId id) const noexcept {
    const auto iterator = std::ranges::lower_bound(extensionRecords_, id, {}, &ExtensionRecord::id);
    return iterator == extensionRecords_.end() || iterator->id != id ? nullptr : &*iterator;
}

ExtensionRecord* Project::findExtensionRecord(const ExtensionRecordId id) noexcept {
    return const_cast<ExtensionRecord*>(std::as_const(*this).findExtensionRecord(id));
}

bool Project::addExtensionRecord(ExtensionRecord record) {
    if (!record.id.isValid()) {
        return false;
    }
    const auto insertion =
        std::ranges::lower_bound(extensionRecords_, record.id, {}, &ExtensionRecord::id);
    if (insertion != extensionRecords_.end() && insertion->id == record.id) {
        return false;
    }
    extensionRecords_.insert(insertion, std::move(record));
    return true;
}

bool Project::removeExtensionRecord(const ExtensionRecordId id) {
    const auto iterator = std::ranges::lower_bound(extensionRecords_, id, {}, &ExtensionRecord::id);
    if (iterator == extensionRecords_.end() || iterator->id != id) {
        return false;
    }
    extensionRecords_.erase(iterator);
    return true;
}

const DataBlockRecord* Project::findDataBlock(const DataBlockRecordId id) const noexcept {
    const auto found = std::ranges::find_if(dataBlocks_, [id](const auto& block) {
        const auto* blockId = std::get_if<DataBlockRecordId>(&block.id);
        return blockId != nullptr && *blockId == id;
    });
    return found == dataBlocks_.end() ? nullptr : &*found;
}

DataBlockRecord* Project::findDataBlock(const DataBlockRecordId id) noexcept {
    return const_cast<DataBlockRecord*>(std::as_const(*this).findDataBlock(id));
}

bool Project::addDataBlock(DataBlockRecord block) {
    const auto* id = std::get_if<DataBlockRecordId>(&block.id);
    if (id == nullptr || findDataBlock(*id) != nullptr || !block.validate(this).ok())
        return false;
    dataBlocks_.push_back(std::move(block));
    std::ranges::sort(dataBlocks_, {},
                      [](const auto& value) { return std::get<DataBlockRecordId>(value.id); });
    return true;
}

bool Project::removeDataBlock(const DataBlockRecordId id) {
    return std::erase_if(dataBlocks_, [id](const auto& block) {
               return std::get<DataBlockRecordId>(block.id) == id;
           }) != 0;
}

std::vector<DataBlockRecord> Project::dataBlocks() const {
    std::vector<DataBlockRecord> blocks;
    blocks.reserve(assets_.size() + extensionRecords_.size() + dataBlocks_.size());
    for (const auto& asset : assets_)
        blocks.push_back(DataBlockRecord::fromAsset(asset));
    for (const auto& record : extensionRecords_)
        blocks.push_back(DataBlockRecord::fromExtension(record));
    for (const auto& block : dataBlocks_)
        blocks.push_back(block);
    return blocks;
}

ValidationResult Project::validateCompositionNesting() const {
    // Kahn's algorithm bounds stack use even for deeply nested or untrusted projects.
    std::unordered_map<CompositionId, std::size_t> incoming;
    std::unordered_map<CompositionId, std::vector<CompositionId>> references;
    for (const auto& composition : compositions_)
        incoming.emplace(composition.id(), 0);
    for (const auto& composition : compositions_) {
        for (const auto& node : composition.graph().nodes()) {
            if (node.typeId != kCompositionSourceNodeType)
                continue;
            for (const auto& binding : node.parameters) {
                if (binding.role != "composition")
                    continue;
                const auto* parameter = composition.parameters().find(binding.parameterId);
                const auto* constant =
                    parameter ? std::get_if<ConstantValueSource>(&parameter->source) : nullptr;
                const auto* value =
                    constant ? std::get_if<std::int64_t>(&constant->value) : nullptr;
                if (!value || *value <= 0)
                    continue;
                const auto target = CompositionId::fromRaw(static_cast<std::uint64_t>(*value));
                // Missing compositions remain preservable; compilation diagnoses the reference.
                if (!incoming.contains(target))
                    continue;
                references[composition.id()].push_back(target);
                ++incoming[target];
            }
        }
    }
    std::vector<CompositionId> ready;
    for (const auto& [id, count] : incoming)
        if (count == 0)
            ready.push_back(id);
    std::size_t visited = 0;
    while (!ready.empty()) {
        const auto id = ready.back();
        ready.pop_back();
        ++visited;
        for (const auto target : references[id])
            if (--incoming[target] == 0)
                ready.push_back(target);
    }
    ValidationResult result;
    if (visited != incoming.size())
        result.add(ValidationCode::CompositionNestingCycle, "compositions",
                   "Composition source references form a nesting cycle");
    return result;
}

ValidationResult Project::validate() const {
    ValidationResult result;
    std::unordered_set<AssetId> assetIds;
    for (const auto& asset : assets_) {
        result.append("assets", asset.validate());
        validateHumanFacingName(asset.name, "assets.name", "Asset name", result);
        if (!assetIds.insert(asset.id).second)
            result.add(ValidationCode::DuplicateId, "assets.id", "Duplicate asset ID");
        if (asset.folder && !findAssetFolder(*asset.folder))
            result.add(ValidationCode::MissingReference, "assets.folder",
                       "Asset folder does not exist");
    }
    std::unordered_map<AssetFolderId, const AssetFolder*> folders;
    std::set<std::pair<std::optional<AssetFolderId>, std::string>> siblingNames;
    for (const auto& folder : assetFolders_) {
        if (!folder.id.isValid() || !folders.emplace(folder.id, &folder).second)
            result.add(ValidationCode::InvalidId, "assetFolders.id",
                       "Invalid or duplicate folder ID");
        validateHumanFacingName(folder.name, "assetFolders.name", "Folder name", result);
        if (!siblingNames.emplace(folder.parent, folder.name).second)
            result.add(ValidationCode::InvalidValue, "assetFolders.name",
                       "Folder names must be unique among siblings");
    }
    // Mark completed chains once, so deep hierarchies validate in linear time.
    std::unordered_set<AssetFolderId> complete;
    for (const auto& folder : assetFolders_) {
        std::unordered_set<AssetFolderId> chain;
        auto current = std::optional(folder.id);
        while (current && !complete.contains(*current)) {
            const auto found = folders.find(*current);
            if (found == folders.end()) {
                result.add(ValidationCode::MissingReference, "assetFolders.parent",
                           "Parent folder does not exist");
                break;
            }
            if (!chain.insert(*current).second) {
                result.add(ValidationCode::GraphCycle, "assetFolders.parent",
                           "Folder hierarchy contains a cycle");
                break;
            }
            current = found->second->parent;
        }
        complete.insert(chain.begin(), chain.end());
    }
    if (!id_.isValid()) {
        result.add(ValidationCode::InvalidId, "id", "Project ID must not be zero");
    }
    validateHumanFacingName(name_, "name", "Project name", result);

    std::unordered_set<CompositionId> compositionIds;
    std::unordered_map<NodeId, std::size_t> nodeDeclarations;
    std::unordered_map<EdgeId, std::size_t> edgeDeclarations;
    std::unordered_map<ParameterId, std::size_t> parameterDeclarations;
    std::unordered_map<AnimationCurveId, std::size_t> animationCurveDeclarations;
    std::unordered_map<KeyframeId, std::size_t> keyframeDeclarations;
    std::unordered_map<LayerId, std::size_t> layerDeclarations;
    std::unordered_map<LayerSlotId, std::size_t> layerSlotDeclarations;
    std::unordered_map<NodeGroupId, std::size_t> nodeGroupDeclarations;

    for (std::size_t compositionOrdinal = 0; compositionOrdinal < compositions_.size();
         ++compositionOrdinal) {
        const auto& composition = compositions_[compositionOrdinal];
        const auto path = "compositions[" + std::to_string(composition.id().value()) + "]";
        if (!composition.id().isValid()) {
            result.add(ValidationCode::InvalidId, path + ".id", "Composition ID must not be zero");
        } else if (!compositionIds.insert(composition.id()).second) {
            result.add(ValidationCode::DuplicateId, path + ".id", "Composition ID is duplicated");
        }
        result.append(path, composition.validate());

        for (const auto& node : composition.graph().nodes()) {
            validateProjectUniqueDeclaration(node.id, compositionOrdinal,
                                             path + ".graph.nodes[" +
                                                 std::to_string(node.id.value()) + "].id",
                                             "Node", nodeDeclarations, result);
        }
        for (const auto& edge : composition.graph().edges()) {
            validateProjectUniqueDeclaration(edge.id, compositionOrdinal,
                                             path + ".graph.edges[" +
                                                 std::to_string(edge.id.value()) + "].id",
                                             "Edge", edgeDeclarations, result);
        }
        for (const auto& parameter : composition.parameters().records()) {
            validateProjectUniqueDeclaration(parameter.id, compositionOrdinal,
                                             path + ".parameters[" +
                                                 std::to_string(parameter.id.value()) + "].id",
                                             "Parameter", parameterDeclarations, result);
        }
        for (const auto& record : composition.animationCurves().records()) {
            const auto curveId = animationCurveId(record);
            const auto curvePath =
                path + ".animationCurves[" + std::to_string(curveId.value()) + "]";
            validateProjectUniqueDeclaration(curveId, compositionOrdinal, curvePath + ".id",
                                             "Animation curve", animationCurveDeclarations, result);
            std::visit(
                [&](const auto& curve) {
                    using Curve = std::decay_t<decltype(curve)>;
                    const auto validateKey = [&](const auto& keyframe, const std::string& keyPath) {
                        validateProjectUniqueDeclaration(keyframe.id, compositionOrdinal,
                                                         keyPath + ".id", "Keyframe",
                                                         keyframeDeclarations, result);
                    };
                    if constexpr (std::is_same_v<Curve, ScalarAnimationCurve>) {
                        for (const auto& keyframe : curve.keyframes)
                            validateKey(keyframe, curvePath + ".keyframes[" +
                                                      std::to_string(keyframe.id.value()) + "]");
                    } else {
                        for (std::size_t componentIndex = 0;
                             componentIndex < curve.components.size(); ++componentIndex)
                            for (const auto& keyframe : curve.components[componentIndex].keyframes)
                                validateKey(keyframe,
                                            curvePath + ".components[" +
                                                std::to_string(componentIndex) + "].keyframes[" +
                                                std::to_string(keyframe.id.value()) + "]");
                    }
                },
                record);
        }
        for (const auto& boundary : composition.graph().layerOutputs()) {
            validateProjectUniqueDeclaration(boundary.layerId, compositionOrdinal,
                                             path + ".graph.layerOutputs[" +
                                                 std::to_string(boundary.layerId.value()) +
                                                 "].layerId",
                                             "Layer", layerDeclarations, result);
        }
        for (const auto& [groupId, group] : composition.nodeGroups()) {
            validateProjectUniqueDeclaration(groupId, compositionOrdinal,
                                             path + ".nodeGroups[" +
                                                 std::to_string(groupId.value()) + "].id",
                                             "Node group", nodeGroupDeclarations, result);
        }
        for (const auto& stack : composition.graph().merges())
            for (const auto& entry : stack.entries()) {
                validateProjectUniqueDeclaration(entry.slotId, compositionOrdinal,
                                                 path + ".graph.layerStack.entries[" +
                                                     std::to_string(entry.slotId.value()) +
                                                     "].slotId",
                                                 "Layer Stack slot", layerSlotDeclarations, result);
            }
    }
    result.append("", validateCompositionNesting());
    result.append("", validateExtensionRecords(*this));
    std::size_t aggregateDataBlockBytes = 0;
    std::unordered_set<DataBlockRecordId> dataBlockIds;
    for (const auto& block : dataBlocks_) {
        result.append("dataBlocks", block.validate(this));
        if (!dataBlockIds.insert(std::get<DataBlockRecordId>(block.id)).second)
            result.add(ValidationCode::DuplicateId, "dataBlocks.id", "Duplicate data block ID");
        const auto bytes = block.payloadBytes();
        if (bytes > kMaxAggregateDataBlockPayloadBytes -
                        std::min(aggregateDataBlockBytes, kMaxAggregateDataBlockPayloadBytes)) {
            result.add(ValidationCode::InvalidValue, "dataBlocks",
                       "Data block payloads exceed the 128 MiB project limit");
            break;
        }
        aggregateDataBlockBytes += bytes;
    }
    return result;
}

} // namespace bloom::document
