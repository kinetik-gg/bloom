#pragma once

#include <bloom/core/rational_time.hpp>
#include <bloom/document/animation.hpp>
#include <bloom/document/asset.hpp>
#include <bloom/document/color_settings.hpp>
#include <bloom/document/composition_settings.hpp>
#include <bloom/document/data_block.hpp>
#include <bloom/document/extension_records.hpp>
#include <bloom/document/graph.hpp>
#include <bloom/document/ids.hpp>
#include <bloom/document/node_layout.hpp>
#include <bloom/document/parameter.hpp>
#include <bloom/document/validation.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

namespace bloom::document {

class RenderReachability;

namespace detail {
// #357. Copy-on-write storage for one project-root collection. Published snapshots share one
// immutable vector; a draft copies the collection only when it edits it, and publication freezes
// only the copies a draft actually made -- the same ownership rule Composition content follows.
template <typename T> class SharedRecords final {
  public:
    SharedRecords() : frozen_(std::make_shared<const std::vector<T>>()) {}
    SharedRecords(const SharedRecords& other) : frozen_(other.frozen_) {
        if (other.editable_)
            editable_ = std::make_unique<std::vector<T>>(*other.editable_);
    }
    SharedRecords& operator=(const SharedRecords& other) {
        if (this != &other) {
            auto copy = other;
            *this = std::move(copy);
        }
        return *this;
    }
    SharedRecords(SharedRecords&&) noexcept = default;
    SharedRecords& operator=(SharedRecords&&) noexcept = default;
    ~SharedRecords() = default;

    [[nodiscard]] const std::vector<T>& get() const noexcept {
        return editable_ ? *editable_ : *frozen_;
    }
    [[nodiscard]] std::vector<T>& edit() {
        if (!editable_)
            editable_ = std::make_unique<std::vector<T>>(*frozen_);
        return *editable_;
    }
    // Called only on a publication copy, so a pointer retained from the draft cannot reach it.
    void freeze() {
        if (editable_) {
            frozen_ = std::make_shared<const std::vector<T>>(std::move(*editable_));
            editable_.reset();
        }
    }
    // Two shells share storage exactly when neither has edited it since they diverged.
    [[nodiscard]] const void* identity() const noexcept { return &get(); }

  private:
    std::shared_ptr<const std::vector<T>> frozen_;
    std::unique_ptr<std::vector<T>> editable_;
};
} // namespace detail

struct WorkArea {
    core::RationalTime start{}, end{};
    friend bool operator==(const WorkArea&, const WorkArea&) = default;
};
class Composition final {
  public:
    Composition(CompositionId id, std::string name, core::RationalTime duration,
                CanonicalGraph graph, CompositionFormat format = {});
    Composition(const Composition& other);
    Composition& operator=(const Composition& other);
    Composition(Composition&&) noexcept = default;
    Composition& operator=(Composition&&) noexcept = default;

    [[nodiscard]] CompositionId id() const noexcept { return id_; }
    [[nodiscard]] const std::string& name() const noexcept { return name_; }
    [[nodiscard]] core::RationalTime duration() const noexcept { return duration_; }
    [[nodiscard]] CompositionFormat format() const noexcept { return format_; }
    [[nodiscard]] const ParameterStore& parameters() const noexcept { return content().parameters; }
    [[nodiscard]] ParameterStore& parameters() { return mutableContent().parameters; }
    [[nodiscard]] const AnimationCurveStore& animationCurves() const noexcept {
        return content().animationCurves;
    }
    [[nodiscard]] AnimationCurveStore& animationCurves() {
        return mutableContent().animationCurves;
    }
    [[nodiscard]] const CanonicalGraph& graph() const noexcept { return content().graph; }
    [[nodiscard]] CanonicalGraph& graph() { return mutableContent().graph; }

    [[nodiscard]] const NodeLayout& nodeLayout() const noexcept { return content().nodeLayout; }
    [[nodiscard]] NodeLayout& nodeLayout() { return mutableContent().nodeLayout; }

    // Node groups live beside the layout, not inside the graph: they frame cards and carry no
    // evaluation meaning whatsoever (see node_layout.hpp).
    [[nodiscard]] const NodeGroups& nodeGroups() const noexcept { return content().nodeGroups; }
    [[nodiscard]] NodeGroups& nodeGroups() { return mutableContent().nodeGroups; }

    // The identity/charge of the immutable authored collections, for deterministic undo
    // accounting. Draft-local mutable content has its own identity until publication freezes it.
    [[nodiscard]] const void* contentIdentity() const noexcept { return &content(); }
    [[nodiscard]] std::size_t contentByteCharge() const;
    struct CopyWork final {
        std::uint64_t draftDetaches = 0;
        std::uint64_t editableCopies = 0;
        std::uint64_t contentChargeWalks = 0;
    };
    [[nodiscard]] static CopyWork copyWork() noexcept;

    [[nodiscard]] const std::optional<WorkArea>& workArea() const noexcept { return workArea_; }
    void setWorkArea(std::optional<WorkArea> area) noexcept { workArea_ = area; }
    // Null means inherit the project's ColorSettings.processColorSpaceId. A present value is
    // proved against the selected OCIO config by the color boundary before an edit is accepted.
    [[nodiscard]] const std::optional<std::string>& workingColorSpaceId() const noexcept {
        return workingColorSpaceId_;
    }
    void setWorkingColorSpaceId(std::optional<std::string> id) {
        workingColorSpaceId_ = std::move(id);
    }
    [[nodiscard]] core::Color4d backgroundColor() const noexcept { return backgroundColor_; }
    void setBackgroundColor(core::Color4d color) noexcept { backgroundColor_ = color; }
    [[nodiscard]] SafeAreaSettings safeAreas() const noexcept { return safeAreas_; }
    void setSafeAreas(SafeAreaSettings settings) noexcept { safeAreas_ = settings; }
    // A node is locked when a locked layer boundary depends on it through node inputs. Published
    // (frozen) content answers from a lock index built once at freeze time by one reverse BFS;
    // draft content builds the index per call, still linear in the graph (#357).
    [[nodiscard]] bool nodeLocked(NodeId node) const;
    [[nodiscard]] bool parameterLocked(ParameterId parameter) const;
    struct LockIndex final {
        std::unordered_set<NodeId> nodes;
        std::unordered_set<ParameterId> parameters;
    };
    [[nodiscard]] LockIndex lockIndex() const;
    // Lock-index builds, for work-counter tests.
    [[nodiscard]] static std::uint64_t lockIndexBuilds() noexcept;
    // The render reachability published content computed at freeze time (built-in registry), or
    // null for draft content. Prefer RenderReachability::of(), which falls back to computing.
    [[nodiscard]] std::shared_ptr<const RenderReachability> frozenRenderReachability() const;
    void setName(std::string name) { name_ = std::move(name); }
    [[nodiscard]] bool setDuration(core::RationalTime duration) noexcept;
    void setFormat(CompositionFormat format) noexcept { format_ = format; }

    [[nodiscard]] ValidationResult validate() const;

  private:
    friend class Project;
    // #357. Every project-unique ID this content declares, by kind, sorted. Published content
    // carries one built at freeze time so cross-composition uniqueness checks never rescan an
    // unchanged composition.
    struct DeclaredIdIndex final {
        std::vector<std::uint64_t> nodes, edges, parameters, curves, keyframes, layers, layerSlots,
            groups;
    };
    struct Content;
    [[nodiscard]] static DeclaredIdIndex buildDeclaredIdIndex(const Content& authored);
    // Constant Composition Source references (raw ids > 0) in node order; existence is checked by
    // the caller, exactly as validateCompositionNesting() always did.
    [[nodiscard]] static std::vector<CompositionId> buildNestedReferences(const Content& authored);
    [[nodiscard]] const DeclaredIdIndex* frozenDeclaredIds() const noexcept;
    [[nodiscard]] std::vector<CompositionId> nestedReferences() const;
    [[nodiscard]] bool sameShell(const Composition& other) const noexcept;
    struct Content final {
        explicit Content(CanonicalGraph graphValue)
            : graph(std::move(graphValue)), nodeLayout(defaultNodeLayout(graph.nodes())) {}
        ParameterStore parameters;
        AnimationCurveStore animationCurves;
        CanonicalGraph graph;
        NodeLayout nodeLayout;
        NodeGroups nodeGroups;
        // Computed once before this payload is published through frozenContent_. Editable
        // draft copies ignore this value and recompute their live charge on demand.
        std::size_t frozenByteCharge = 0;
        // Built once at freeze time; editable copies ignore it and build their own on demand.
        std::optional<LockIndex> frozenLockIndex;
        std::optional<DeclaredIdIndex> frozenDeclaredIds;
        std::optional<std::vector<CompositionId>> frozenNestedReferences;
        // #355/#357. Built once at freeze time against the built-in registry; immutable thereafter.
        std::shared_ptr<const RenderReachability> frozenReachability;
    };
    [[nodiscard]] const Content& content() const noexcept {
        return editableContent_ ? *editableContent_ : *frozenContent_;
    }
    [[nodiscard]] Content& mutableContent();
    [[nodiscard]] static std::size_t calculateContentByteCharge(const Content& authored);
    [[nodiscard]] static LockIndex buildLockIndex(const Content& authored);
    void freezeContent();
    std::optional<WorkArea> workArea_{};
    std::optional<std::string> workingColorSpaceId_{};
    SafeAreaSettings safeAreas_{};
    core::Color4d backgroundColor_{0.0, 0.0, 0.0, 1.0};
    CompositionId id_;
    std::string name_;
    core::RationalTime duration_;
    CompositionFormat format_;
    std::shared_ptr<const Content> frozenContent_;
    std::unique_ptr<Content> editableContent_;
};

class Project final {
  public:
    Project(ProjectId id, std::string name) : id_(id), name_(std::move(name)) {}

    [[nodiscard]] ProjectId id() const noexcept { return id_; }
    [[nodiscard]] const std::string& name() const noexcept { return name_; }
    [[nodiscard]] std::span<const AssetFolder> assetFolders() const noexcept {
        return assetFolders_.get();
    }
    [[nodiscard]] const AssetFolder* findAssetFolder(AssetFolderId id) const noexcept;
    [[nodiscard]] AssetFolder* findAssetFolder(AssetFolderId id);
    [[nodiscard]] bool addAssetFolder(AssetFolder folder);
    [[nodiscard]] bool removeAssetFolder(AssetFolderId id);
    [[nodiscard]] std::span<const AssetRecord> assets() const noexcept { return assets_.get(); }
    [[nodiscard]] const AssetRecord* findAsset(AssetId id) const noexcept;
    [[nodiscard]] AssetRecord* findAsset(AssetId id);
    [[nodiscard]] bool addAsset(AssetRecord asset);
    [[nodiscard]] bool removeAsset(AssetId id);
    [[nodiscard]] std::span<const Composition> compositions() const noexcept {
        return compositions_;
    }
    [[nodiscard]] const Composition* findComposition(CompositionId id) const noexcept;
    [[nodiscard]] Composition* findComposition(CompositionId id) noexcept;
    [[nodiscard]] std::span<const ExtensionRecord> extensionRecords() const noexcept {
        return extensionRecords_.get();
    }
    [[nodiscard]] const ExtensionRecord* findExtensionRecord(ExtensionRecordId id) const noexcept;
    [[nodiscard]] ExtensionRecord* findExtensionRecord(ExtensionRecordId id);

    void setName(std::string name) { name_ = std::move(name); }
    [[nodiscard]] bool addComposition(Composition composition);
    [[nodiscard]] bool removeComposition(CompositionId id);
    [[nodiscard]] bool addExtensionRecord(ExtensionRecord record);
    [[nodiscard]] bool removeExtensionRecord(ExtensionRecordId id);
    [[nodiscard]] std::span<const DataBlockRecord> typedDataBlocks() const noexcept {
        return dataBlocks_.get();
    }
    [[nodiscard]] const DataBlockRecord* findDataBlock(DataBlockRecordId id) const noexcept;
    [[nodiscard]] DataBlockRecord* findDataBlock(DataBlockRecordId id);
    [[nodiscard]] bool addDataBlock(DataBlockRecord block);
    [[nodiscard]] bool removeDataBlock(DataBlockRecordId id);
    // Normalized views used by the Inspector. Media assets and extension records remain in their
    // owning collections; only authored non-media blocks are stored in typedDataBlocks().
    [[nodiscard]] std::vector<DataBlockRecord> dataBlocks() const;

    [[nodiscard]] ValidationResult validate() const;
    // #357. True only when this project is provably valid given that `base` -- the published
    // project it was drafted from -- is valid: only compositions whose content or settings changed
    // are validated, cross-composition ID uniqueness consults the frozen ID indexes of unchanged
    // compositions, nesting reads cached references, and root records are revalidated only when
    // their shared storage changed (or when a composition changed while records that can target
    // composition content exist). False means "not proven"; callers then run validate() for the
    // exact diagnostics.
    [[nodiscard]] bool validatesAgainst(const Project& base) const;
    struct IncrementalValidationWork final {
        std::uint64_t incrementalValidations = 0;
        std::uint64_t compositionsValidated = 0;
        std::uint64_t compositionsSkipped = 0;
        std::uint64_t rootSectionValidations = 0;
        std::uint64_t notProven = 0;
    };
    [[nodiscard]] static IncrementalValidationWork incrementalValidationWork() noexcept;
    // Whether two projects share the storage of every root collection (assets, folders, extension
    // records, data blocks): true exactly when neither edited them since they diverged.
    [[nodiscard]] bool sameRootRecordStorage(const Project& other) const noexcept;
    [[nodiscard]] ValidationResult validateCompositionNesting() const;
    // Diagnostic work counter for detecting accidental whole-project validation on UI paths.
    [[nodiscard]] static std::uint64_t validationCount() noexcept;
    [[nodiscard]] std::size_t rootByteCharge() const;

  private:
    friend class Document;
    // Only Document may freeze its independent publication shell. Exposing this on a Draft's
    // mutable Project would let a retained element pointer mutate a later Snapshot.
    void freezeCompositionContent();
    void validateRootRecords(ValidationResult& result) const;
    void validateCrossRecords(ValidationResult& result) const;
    ProjectId id_;
    std::string name_;
    std::vector<Composition> compositions_;
    detail::SharedRecords<AssetRecord> assets_;
    detail::SharedRecords<AssetFolder> assetFolders_;
    detail::SharedRecords<ExtensionRecord> extensionRecords_;
    detail::SharedRecords<DataBlockRecord> dataBlocks_;
};

} // namespace bloom::document
