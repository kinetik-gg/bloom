#include <bloom/document/layer_masks.hpp>
#include <bloom/document/project.hpp>

#include <bloom/document/persisted_text.hpp>
#include <bloom/document/render_reachability.hpp>

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <limits>
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
                                      const auto& path, const std::string_view typeName,
                                      std::unordered_map<Id, std::size_t>& declarationOwners,
                                      bloom::document::ValidationResult& result) {
    if (!id.isValid()) {
        return;
    }

    const auto [existing, inserted] = declarationOwners.try_emplace(id, compositionOrdinal);
    if (!inserted && existing->second != compositionOrdinal) {
        // #357: the diagnostic path is built only when there is a diagnostic.
        result.add(bloom::document::ValidationCode::DuplicateId, path(),
                   std::string(typeName) + " ID is declared by more than one composition");
    }
}

} // namespace

namespace bloom::document {

namespace {
std::atomic<std::uint64_t> draftContentDetaches{0};
std::atomic<std::uint64_t> editableContentCopies{0};
std::atomic<std::uint64_t> contentChargeWalks{0};
std::atomic<std::uint64_t> projectValidations{0};

void charge(std::size_t& total, const std::size_t bytes) noexcept {
    total = bytes > std::numeric_limits<std::size_t>::max() - total
                ? std::numeric_limits<std::size_t>::max()
                : total + bytes;
}

template <typename T> void chargeRecords(std::size_t& total, const std::size_t count) noexcept {
    charge(total, count > std::numeric_limits<std::size_t>::max() / sizeof(T)
                      ? std::numeric_limits<std::size_t>::max()
                      : count * sizeof(T));
}

void chargeValue(std::size_t& total, const ParameterValue& value) {
    std::visit(
        [&](const auto& item) {
            using Value = std::decay_t<decltype(item)>;
            if constexpr (std::is_same_v<Value, std::string>)
                charge(total, item.size());
            else if constexpr (std::is_same_v<Value, PathValue>)
                chargeRecords<PathAnchor>(total, item.anchors.size());
        },
        value);
}

void chargeLocator(std::size_t& total, const AssetLocator& locator) noexcept {
    charge(total, locator.kind.size());
    charge(total, locator.portability.size());
    charge(total, locator.path.size());
    charge(total, locator.relinkHint.size());
}

void chargeColumn(std::size_t& total, const DataBlockTableColumn& column) {
    charge(total, column.name.size());
    chargeRecords<ParameterValue>(total, column.values.size());
    for (const auto& value : column.values)
        chargeValue(total, value);
}
} // namespace

Composition::Composition(const CompositionId id, std::string name,
                         const core::RationalTime duration, CanonicalGraph graph,
                         const CompositionFormat format)
    : id_(id), name_(std::move(name)), duration_(duration), format_(format),
      editableContent_(std::make_unique<Content>(std::move(graph))) {}

Composition::Composition(const Composition& other)
    : workArea_(other.workArea_), timecode_(other.timecode_), folder_(other.folder_),
      workingColorSpaceId_(other.workingColorSpaceId_), safeAreas_(other.safeAreas_),
      backgroundColor_(other.backgroundColor_), id_(other.id_), name_(other.name_),
      duration_(other.duration_), format_(other.format_), frozenContent_(other.frozenContent_) {
    if (other.editableContent_) {
        editableContent_ = std::make_unique<Content>(*other.editableContent_);
        editableContentCopies.fetch_add(1, std::memory_order_relaxed);
    }
}

Composition& Composition::operator=(const Composition& other) {
    if (this != &other) {
        Composition copy(other);
        *this = std::move(copy);
    }
    return *this;
}

Composition::Content& Composition::mutableContent() {
    if (!editableContent_) {
        editableContent_ = std::make_unique<Content>(*frozenContent_);
        draftContentDetaches.fetch_add(1, std::memory_order_relaxed);
    }
    return *editableContent_;
}

void Composition::freezeContent() {
    if (editableContent_) {
        // Called on a COPY of the draft's Project. Moving this copy's buffer into immutable
        // storage cannot let a pointer retained from the original Draft mutate a Snapshot.
        auto frozen = std::make_shared<Content>(std::move(*editableContent_));
        frozen->frozenByteCharge = calculateContentByteCharge(*frozen);
        frozen->frozenLockIndex = buildLockIndex(*frozen);
        frozen->frozenDeclaredIds = buildDeclaredIdIndex(*frozen);
        frozen->frozenNestedReferences = buildNestedReferences(*frozen);
        // Publish first so the reachability walk reads the frozen payload through *this; nothing
        // else shares `frozen` until this function returns.
        frozenContent_ = frozen;
        editableContent_.reset();
        frozen->frozenReachability = std::make_shared<const RenderReachability>(
            RenderReachability::compute(*this, builtInNodeDefinitions()));
    }
}

std::shared_ptr<const RenderReachability> Composition::frozenRenderReachability() const {
    return !editableContent_ && frozenContent_ ? frozenContent_->frozenReachability : nullptr;
}

Composition::CopyWork Composition::copyWork() noexcept {
    return {.draftDetaches = draftContentDetaches.load(std::memory_order_relaxed),
            .editableCopies = editableContentCopies.load(std::memory_order_relaxed),
            .contentChargeWalks = contentChargeWalks.load(std::memory_order_relaxed)};
}

std::size_t Composition::contentByteCharge() const {
    return editableContent_ ? calculateContentByteCharge(*editableContent_)
                            : frozenContent_->frozenByteCharge;
}

std::size_t Composition::calculateContentByteCharge(const Content& authored) {
    contentChargeWalks.fetch_add(1, std::memory_order_relaxed);
    std::size_t bytes = sizeof(Content);
    chargeRecords<ParameterRecord>(bytes, authored.parameters.records().size());
    chargeRecords<AnimationCurveRecord>(bytes, authored.animationCurves.records().size());
    chargeRecords<NodeRecord>(bytes, authored.graph.nodes().size());
    chargeRecords<EdgeRecord>(bytes, authored.graph.edges().size());
    chargeRecords<LayerOutputBoundary>(bytes, authored.graph.layerOutputs().size());
    chargeRecords<LayerStack>(bytes, authored.graph.merges().size());
    chargeRecords<std::pair<const NodeId, NodeLayoutRecord>>(bytes, authored.nodeLayout.size());
    chargeRecords<std::pair<const NodeGroupId, NodeGroupRecord>>(bytes, authored.nodeGroups.size());
    for (const auto& node : authored.graph.nodes()) {
        charge(bytes, node.typeId.size());
        chargeRecords<ParameterBinding>(bytes, node.parameters.size());
        for (const auto& binding : node.parameters)
            charge(bytes, binding.role.size());
    }
    for (const auto& parameter : authored.parameters.records()) {
        charge(bytes, parameter.schemaKey.size());
        std::visit(
            [&](const auto& source) {
                using Source = std::decay_t<decltype(source)>;
                if constexpr (std::is_same_v<Source, ConstantValueSource>)
                    chargeValue(bytes, source.value);
                else if constexpr (std::is_same_v<Source, AnimationCurveSource>) {
                    if (source.defaultValue)
                        chargeValue(bytes, *source.defaultValue);
                } else
                    charge(bytes, source.outputPort.size());
            },
            parameter.source);
    }
    for (const auto& edge : authored.graph.edges()) {
        charge(bytes, edge.source.port.size());
        std::visit(
            [&](const auto& destination) {
                using Destination = std::decay_t<decltype(destination)>;
                if constexpr (std::is_same_v<Destination, NodeInputRef>)
                    charge(bytes, destination.port.size());
                else
                    charge(bytes, destination.role.size());
            },
            edge.destination);
    }
    for (const auto& boundary : authored.graph.layerOutputs()) {
        charge(bytes, boundary.name.size());
        charge(bytes, boundary.outputPort.size());
    }
    for (const auto& stack : authored.graph.merges())
        chargeRecords<LayerStackEntry>(bytes, stack.entries().size());
    for (const auto& [id, group] : authored.nodeGroups) {
        static_cast<void>(id);
        charge(bytes, group.name.size());
        chargeRecords<NodeId>(bytes, group.members.size());
    }
    for (const auto& curve : authored.animationCurves.records()) {
        std::visit(
            [&](const auto& record) {
                using Record = std::decay_t<decltype(record)>;
                if constexpr (std::is_same_v<Record, ScalarAnimationCurve>) {
                    chargeRecords<ScalarKeyframe>(bytes, record.keyframes.size());
                } else if constexpr (std::is_same_v<Record, PathAnimationCurve>) {
                    chargeRecords<PathKeyframe>(bytes, record.keyframes.size());
                    for (const auto& keyframe : record.keyframes)
                        chargeRecords<PathAnchor>(bytes, keyframe.value.anchors.size());
                } else {
                    for (const auto& component : record.components)
                        chargeRecords<ScalarKeyframe>(bytes, component.keyframes.size());
                }
            },
            curve);
    }
    return bytes;
}

void Project::freezeCompositionContent() {
    for (auto& composition : compositions_)
        composition.freezeContent();
    assets_.freeze();
    assetFolders_.freeze();
    extensionRecords_.freeze();
    dataBlocks_.freeze();
}

std::size_t Project::rootByteCharge() const {
    std::size_t bytes = sizeof(Project);
    charge(bytes, name_.size());
    chargeRecords<Composition>(bytes, compositions_.size());
    chargeRecords<AssetRecord>(bytes, assets_.get().size());
    chargeRecords<AssetFolder>(bytes, assetFolders_.get().size());
    chargeRecords<ExtensionRecord>(bytes, extensionRecords_.get().size());
    chargeRecords<DataBlockRecord>(bytes, dataBlocks_.get().size());
    for (const auto& composition : compositions_) {
        charge(bytes, composition.name().size());
        if (composition.workingColorSpaceId())
            charge(bytes, composition.workingColorSpaceId()->size());
    }
    for (const auto& asset : assets_.get()) {
        chargeLocator(bytes, asset.locator);
        charge(bytes, asset.interpretation.inputColorSpaceId.size());
        charge(bytes, asset.manifest.pattern.size());
        chargeRecords<AssetSequenceMember>(bytes, asset.manifest.members.size());
        chargeRecords<std::int64_t>(bytes, asset.manifest.gaps.size());
        for (const auto& member : asset.manifest.members)
            chargeLocator(bytes, member.locator);
        chargeRecords<AssetVideoStream>(bytes, asset.videoStreams.size());
        for (const auto& stream : asset.videoStreams) {
            charge(bytes, stream.codec.size());
            charge(bytes, stream.profile.size());
            charge(bytes, stream.pixelFormat.size());
            charge(bytes, stream.timecode.size());
            chargeRecords<std::string>(bytes, stream.channelLayout.size());
            for (const auto& channel : stream.channelLayout)
                charge(bytes, channel.size());
        }
        charge(bytes, asset.fontFamily.size());
        charge(bytes, asset.fontStyle.size());
        charge(bytes, asset.name.size());
        chargeRecords<std::string>(bytes, asset.tags.size());
        for (const auto& tag : asset.tags)
            charge(bytes, tag.size());
    }
    for (const auto& folder : assetFolders_.get())
        charge(bytes, folder.name.size());
    for (const auto& block : dataBlocks_.get()) {
        charge(bytes, block.typeId.size());
        charge(bytes, block.mediaType.size());
        charge(bytes, block.provenance.createdAt.size());
        std::visit(
            [&](const auto& source) {
                using Source = std::decay_t<decltype(source)>;
                if constexpr (std::is_same_v<Source, AssetLocator>)
                    chargeLocator(bytes, source);
                else {
                    charge(bytes, source.name.size());
                    charge(bytes, source.version.size());
                }
            },
            block.provenance.source);
        chargeRecords<std::string>(bytes, block.tags.size());
        for (const auto& tag : block.tags)
            charge(bytes, tag.size());
        std::visit(
            [&](const auto& payload) {
                using Payload = std::decay_t<decltype(payload)>;
                if constexpr (std::is_same_v<Payload, DataBlockCurve>) {
                    chargeRecords<double>(bytes, payload.samples.size());
                } else if constexpr (std::is_same_v<Payload, DataBlockRamp>) {
                    chargeRecords<DataBlockRampStop>(bytes, payload.stops.size());
                } else if constexpr (std::is_same_v<Payload, DataBlockTable>) {
                    chargeRecords<DataBlockTableColumn>(bytes, payload.columns.size());
                    for (const auto& column : payload.columns)
                        chargeColumn(bytes, column);
                } else if constexpr (std::is_same_v<Payload, DataBlockPointSet>) {
                    chargeRecords<Vec3d>(bytes, payload.points.size());
                    chargeRecords<DataBlockTableColumn>(bytes, payload.attributes.size());
                    for (const auto& column : payload.attributes)
                        chargeColumn(bytes, column);
                } else if constexpr (std::is_same_v<Payload, PathValue>)
                    chargeRecords<PathAnchor>(bytes, payload.anchors.size());
                // Opaque payload bytes have shared immutable storage. HistoryCharge accounts for
                // that storage separately, by identity, across all retained roots.
            },
            block.payload);
    }
    for (const auto& record : extensionRecords_.get()) {
        charge(bytes, record.ownerId.size());
        charge(bytes, record.typeId.size());
        charge(bytes, record.mediaType.size());
        std::visit(
            [&](const auto& policy) {
                using Policy = std::decay_t<decltype(policy)>;
                if constexpr (std::is_same_v<Policy, ExtensionHostReferenceTable>) {
                    chargeRecords<ExtensionHostReference>(bytes, policy.references.size());
                    for (const auto& reference : policy.references)
                        charge(bytes, reference.key.size());
                } else if constexpr (std::is_same_v<Policy, ExtensionOwnerRemapper>)
                    charge(bytes, policy.remapperId.size());
            },
            record.referencePolicy);
    }
    return bytes;
}

namespace {
std::atomic<std::uint64_t> lockIndexBuildCount{0};
} // namespace

Composition::LockIndex Composition::lockIndex() const {
    if (!editableContent_ && frozenContent_ && frozenContent_->frozenLockIndex)
        return *frozenContent_->frozenLockIndex;
    return buildLockIndex(content());
}

Composition::LockIndex Composition::buildLockIndex(const Content& authored) {
    lockIndexBuildCount.fetch_add(1, std::memory_order_relaxed);
    LockIndex index;
    std::vector<NodeId> pending;
    for (const auto& layer : authored.graph.layerOutputs())
        if (layer.locked)
            pending.push_back(layer.nodeId);
    if (pending.empty())
        return index;
    // One reverse BFS over node inputs from every locked boundary.
    std::unordered_map<NodeId, std::vector<NodeId>> sources;
    for (const auto& edge : authored.graph.edges())
        if (const auto* input = std::get_if<NodeInputRef>(&edge.destination))
            sources[input->nodeId].push_back(edge.source.nodeId);
    while (!pending.empty()) {
        const auto id = pending.back();
        pending.pop_back();
        if (!index.nodes.insert(id).second)
            continue;
        if (const auto found = sources.find(id); found != sources.end())
            pending.insert(pending.end(), found->second.begin(), found->second.end());
    }
    for (const auto& node : authored.graph.nodes())
        if (index.nodes.contains(node.id))
            for (const auto& binding : node.parameters)
                index.parameters.insert(binding.parameterId);
    // ADR 0028: a layer mask's parameters follow its layer's lock.
    for (const auto& layer : authored.graph.layerOutputs())
        if (layer.locked)
            for (const auto& mask : layer.masks)
                for (const auto& [owned, schemaKey] : layerMaskParameters(mask)) {
                    static_cast<void>(schemaKey);
                    index.parameters.insert(owned);
                }
    return index;
}

std::uint64_t Composition::lockIndexBuilds() noexcept {
    return lockIndexBuildCount.load(std::memory_order_relaxed);
}

bool Composition::nodeLocked(const NodeId node) const {
    if (!editableContent_ && frozenContent_ && frozenContent_->frozenLockIndex)
        return frozenContent_->frozenLockIndex->nodes.contains(node);
    return buildLockIndex(content()).nodes.contains(node);
}
bool Composition::parameterLocked(const ParameterId parameter) const {
    if (!editableContent_ && frozenContent_ && frozenContent_->frozenLockIndex)
        return frozenContent_->frozenLockIndex->parameters.contains(parameter);
    return buildLockIndex(content()).parameters.contains(parameter);
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
    if (!timecode_.isValid()) {
        result.add(ValidationCode::InvalidValue, "timecode.start",
                   "Composition start timecode must not be negative");
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
    for (const auto& layer : graph().layerOutputs()) {
        // ADR 0024: an explicit range is in placement time and may extend past either composition
        // bound (keeping a frame inside is a gesture rule). A stored zero outPoint stays the
        // full-duration sentinel, so an explicit end is never zero or negative.
        if (layer.outPoint < core::RationalTime{} || layer.inPoint >= layer.endPoint(duration_))
            result.add(ValidationCode::InvalidValue, "graph.layerOutputs.range",
                       "Invalid layer range");
        if (!layer.timing.isValid())
            result.add(ValidationCode::InvalidValue, "graph.layerOutputs.timing",
                       "Invalid layer timing");
    }
    result.append("parameters", parameters().validate());
    result.append("animationCurves", animationCurves().validate());
    result.append("", validateAnimationCurveReferences(parameters(), animationCurves()));
    result.append("graph", graph().validate(parameters(), builtInNodeDefinitions(), id_));
    result.append("", validateLayerMasks(graph(), parameters()));
    result.append("", validateNodeLayout(nodeLayout(), graph()));
    result.append("", validateNodeGroups(nodeGroups(), graph()));
    return result;
}

const AssetRecord* Project::findAsset(AssetId id) const noexcept {
    const auto found = std::ranges::find(assets_.get(), id, &AssetRecord::id);
    return found == assets_.get().end() ? nullptr : &*found;
}
AssetRecord* Project::findAsset(AssetId id) {
    // A miss must not detach the shared collection.
    if (std::as_const(*this).findAsset(id) == nullptr)
        return nullptr;
    auto& records = assets_.edit();
    return &*std::ranges::find(records, id, &AssetRecord::id);
}
bool Project::addAsset(AssetRecord asset) {
    if (asset.name.empty())
        asset.name = defaultAssetName(asset);
    if (findAsset(asset.id) || !asset.validate().ok())
        return false;
    auto& assets = assets_.edit();
    assets.push_back(std::move(asset));
    std::ranges::sort(assets, {}, &AssetRecord::id);
    return true;
}
bool Project::removeAsset(AssetId id) {
    if (std::as_const(*this).findAsset(id) == nullptr)
        return false;
    return std::erase_if(assets_.edit(), [id](const auto& asset) { return asset.id == id; }) != 0;
}

const AssetFolder* Project::findAssetFolder(AssetFolderId id) const noexcept {
    const auto found = std::ranges::find(assetFolders_.get(), id, &AssetFolder::id);
    return found == assetFolders_.get().end() ? nullptr : &*found;
}
AssetFolder* Project::findAssetFolder(AssetFolderId id) {
    if (std::as_const(*this).findAssetFolder(id) == nullptr)
        return nullptr;
    auto& records = assetFolders_.edit();
    return &*std::ranges::find(records, id, &AssetFolder::id);
}
bool Project::addAssetFolder(AssetFolder folder) {
    if (!folder.id.isValid() || !isValidHumanFacingName(folder.name) || findAssetFolder(folder.id))
        return false;
    auto& folders = assetFolders_.edit();
    folders.push_back(std::move(folder));
    std::ranges::sort(folders, {}, &AssetFolder::id);
    return true;
}
bool Project::removeAssetFolder(AssetFolderId id) {
    if (std::as_const(*this).findAssetFolder(id) == nullptr)
        return false;
    return std::erase_if(assetFolders_.edit(),
                         [id](const auto& folder) { return folder.id == id; }) != 0;
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
    const auto iterator =
        std::ranges::lower_bound(extensionRecords_.get(), id, {}, &ExtensionRecord::id);
    return iterator == extensionRecords_.get().end() || iterator->id != id ? nullptr : &*iterator;
}

ExtensionRecord* Project::findExtensionRecord(const ExtensionRecordId id) {
    if (std::as_const(*this).findExtensionRecord(id) == nullptr)
        return nullptr;
    auto& records = extensionRecords_.edit();
    return &*std::ranges::lower_bound(records, id, {}, &ExtensionRecord::id);
}

bool Project::addExtensionRecord(ExtensionRecord record) {
    if (!record.id.isValid()) {
        return false;
    }
    if (std::as_const(*this).findExtensionRecord(record.id) != nullptr)
        return false;
    auto& records = extensionRecords_.edit();
    const auto insertion = std::ranges::lower_bound(records, record.id, {}, &ExtensionRecord::id);
    records.insert(insertion, std::move(record));
    return true;
}

bool Project::removeExtensionRecord(const ExtensionRecordId id) {
    if (std::as_const(*this).findExtensionRecord(id) == nullptr)
        return false;
    auto& records = extensionRecords_.edit();
    records.erase(std::ranges::lower_bound(records, id, {}, &ExtensionRecord::id));
    return true;
}

const DataBlockRecord* Project::findDataBlock(const DataBlockRecordId id) const noexcept {
    const auto found = std::ranges::find_if(dataBlocks_.get(), [id](const auto& block) {
        const auto* blockId = std::get_if<DataBlockRecordId>(&block.id);
        return blockId != nullptr && *blockId == id;
    });
    return found == dataBlocks_.get().end() ? nullptr : &*found;
}

DataBlockRecord* Project::findDataBlock(const DataBlockRecordId id) {
    if (std::as_const(*this).findDataBlock(id) == nullptr)
        return nullptr;
    auto& records = dataBlocks_.edit();
    return &*std::ranges::find_if(records, [id](const auto& block) {
        const auto* blockId = std::get_if<DataBlockRecordId>(&block.id);
        return blockId != nullptr && *blockId == id;
    });
}

bool Project::addDataBlock(DataBlockRecord block) {
    const auto* id = std::get_if<DataBlockRecordId>(&block.id);
    if (id == nullptr || findDataBlock(*id) != nullptr || !block.validate(this).ok())
        return false;
    auto& blocks = dataBlocks_.edit();
    blocks.push_back(std::move(block));
    std::ranges::sort(blocks, {},
                      [](const auto& value) { return std::get<DataBlockRecordId>(value.id); });
    return true;
}

bool Project::removeDataBlock(const DataBlockRecordId id) {
    if (std::as_const(*this).findDataBlock(id) == nullptr)
        return false;
    return std::erase_if(dataBlocks_.edit(), [id](const auto& block) {
               return std::get<DataBlockRecordId>(block.id) == id;
           }) != 0;
}

std::vector<DataBlockRecord> Project::dataBlocks() const {
    std::vector<DataBlockRecord> blocks;
    blocks.reserve(assets_.get().size() + extensionRecords_.get().size() +
                   dataBlocks_.get().size());
    for (const auto& asset : assets_.get())
        blocks.push_back(DataBlockRecord::fromAsset(asset));
    for (const auto& record : extensionRecords_.get())
        blocks.push_back(DataBlockRecord::fromExtension(record));
    for (const auto& block : dataBlocks_.get())
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
        for (const auto target : composition.nestedReferences()) {
            // Missing compositions remain preservable; compilation diagnoses the reference.
            if (!incoming.contains(target))
                continue;
            references[composition.id()].push_back(target);
            ++incoming[target];
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

std::uint64_t Project::validationCount() noexcept {
    return projectValidations.load(std::memory_order_relaxed);
}

void Project::validateRootRecords(ValidationResult& result) const {
    std::unordered_set<AssetId> assetIds;
    for (const auto& asset : assets_.get()) {
        result.append("assets", asset.validate());
        validateHumanFacingName(asset.name, "assets.name", "Asset name", result);
        if (!assetIds.insert(asset.id).second)
            result.add(ValidationCode::DuplicateId, "assets.id", "Duplicate asset ID");
        if (asset.folder && !findAssetFolder(*asset.folder))
            result.add(ValidationCode::MissingReference, "assets.folder",
                       "Asset folder does not exist");
    }
    for (const auto& composition : compositions_)
        if (composition.folder() && !findAssetFolder(*composition.folder()))
            result.add(ValidationCode::MissingReference, "compositions.folder",
                       "Composition folder does not exist");
    std::unordered_map<AssetFolderId, const AssetFolder*> folders;
    std::set<std::pair<std::optional<AssetFolderId>, std::string>> siblingNames;
    for (const auto& folder : assetFolders_.get()) {
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
    for (const auto& folder : assetFolders_.get()) {
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
}

void Project::validateCrossRecords(ValidationResult& result) const {
    result.append("", validateExtensionRecords(*this));
    std::size_t aggregateDataBlockBytes = 0;
    std::unordered_set<DataBlockRecordId> dataBlockIds;
    for (const auto& block : dataBlocks_.get()) {
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
}

ValidationResult Project::validate() const {
    projectValidations.fetch_add(1, std::memory_order_relaxed);
    ValidationResult result;
    validateRootRecords(result);

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
            validateProjectUniqueDeclaration(
                node.id, compositionOrdinal,
                [&] { return path + ".graph.nodes[" + std::to_string(node.id.value()) + "].id"; },
                "Node", nodeDeclarations, result);
        }
        for (const auto& edge : composition.graph().edges()) {
            validateProjectUniqueDeclaration(
                edge.id, compositionOrdinal,
                [&] { return path + ".graph.edges[" + std::to_string(edge.id.value()) + "].id"; },
                "Edge", edgeDeclarations, result);
        }
        for (const auto& parameter : composition.parameters().records()) {
            validateProjectUniqueDeclaration(
                parameter.id, compositionOrdinal,
                [&] {
                    return path + ".parameters[" + std::to_string(parameter.id.value()) + "].id";
                },
                "Parameter", parameterDeclarations, result);
        }
        for (const auto& record : composition.animationCurves().records()) {
            const auto curveId = animationCurveId(record);
            const auto curvePath = [&] {
                return path + ".animationCurves[" + std::to_string(curveId.value()) + "]";
            };
            validateProjectUniqueDeclaration(
                curveId, compositionOrdinal, [&] { return curvePath() + ".id"; }, "Animation curve",
                animationCurveDeclarations, result);
            std::visit(
                [&](const auto& curve) {
                    using Curve = std::decay_t<decltype(curve)>;
                    const auto validateKey = [&](const auto& keyframe, const auto& keyPath) {
                        validateProjectUniqueDeclaration(
                            keyframe.id, compositionOrdinal, [&] { return keyPath() + ".id"; },
                            "Keyframe", keyframeDeclarations, result);
                    };
                    if constexpr (kWholeValueAnimationCurve<Curve>) {
                        for (const auto& keyframe : curve.keyframes)
                            validateKey(keyframe, [&] {
                                return curvePath() + ".keyframes[" +
                                       std::to_string(keyframe.id.value()) + "]";
                            });
                    } else {
                        for (std::size_t componentIndex = 0;
                             componentIndex < curve.components.size(); ++componentIndex)
                            for (const auto& keyframe : curve.components[componentIndex].keyframes)
                                validateKey(keyframe, [&] {
                                    return curvePath() + ".components[" +
                                           std::to_string(componentIndex) + "].keyframes[" +
                                           std::to_string(keyframe.id.value()) + "]";
                                });
                    }
                },
                record);
        }
        for (const auto& boundary : composition.graph().layerOutputs()) {
            validateProjectUniqueDeclaration(
                boundary.layerId, compositionOrdinal,
                [&] {
                    return path + ".graph.layerOutputs[" +
                           std::to_string(boundary.layerId.value()) + "].layerId";
                },
                "Layer", layerDeclarations, result);
        }
        for (const auto& [groupId, group] : composition.nodeGroups()) {
            validateProjectUniqueDeclaration(
                groupId, compositionOrdinal,
                [&] { return path + ".nodeGroups[" + std::to_string(groupId.value()) + "].id"; },
                "Node group", nodeGroupDeclarations, result);
        }
        for (const auto& stack : composition.graph().merges())
            for (const auto& entry : stack.entries()) {
                validateProjectUniqueDeclaration(
                    entry.slotId, compositionOrdinal,
                    [&] {
                        return path + ".graph.layerStack.entries[" +
                               std::to_string(entry.slotId.value()) + "].slotId";
                    },
                    "Layer Stack slot", layerSlotDeclarations, result);
            }
    }
    result.append("", validateCompositionNesting());
    validateCrossRecords(result);
    return result;
}

} // namespace bloom::document
