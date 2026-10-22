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
#include <utility>
#include <vector>

namespace bloom::document {

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
    [[nodiscard]] bool nodeLocked(NodeId node) const;
    [[nodiscard]] bool parameterLocked(ParameterId parameter) const;
    void setName(std::string name) { name_ = std::move(name); }
    [[nodiscard]] bool setDuration(core::RationalTime duration) noexcept;
    void setFormat(CompositionFormat format) noexcept { format_ = format; }

    [[nodiscard]] ValidationResult validate() const;

  private:
    friend class Project;
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
    };
    [[nodiscard]] const Content& content() const noexcept {
        return editableContent_ ? *editableContent_ : *frozenContent_;
    }
    [[nodiscard]] Content& mutableContent();
    [[nodiscard]] static std::size_t calculateContentByteCharge(const Content& authored);
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
        return assetFolders_;
    }
    [[nodiscard]] const AssetFolder* findAssetFolder(AssetFolderId id) const noexcept;
    [[nodiscard]] AssetFolder* findAssetFolder(AssetFolderId id) noexcept;
    [[nodiscard]] bool addAssetFolder(AssetFolder folder);
    [[nodiscard]] bool removeAssetFolder(AssetFolderId id);
    [[nodiscard]] std::span<const AssetRecord> assets() const noexcept { return assets_; }
    [[nodiscard]] const AssetRecord* findAsset(AssetId id) const noexcept;
    [[nodiscard]] AssetRecord* findAsset(AssetId id) noexcept;
    [[nodiscard]] bool addAsset(AssetRecord asset);
    [[nodiscard]] bool removeAsset(AssetId id);
    [[nodiscard]] std::span<const Composition> compositions() const noexcept {
        return compositions_;
    }
    [[nodiscard]] const Composition* findComposition(CompositionId id) const noexcept;
    [[nodiscard]] Composition* findComposition(CompositionId id) noexcept;
    [[nodiscard]] std::span<const ExtensionRecord> extensionRecords() const noexcept {
        return extensionRecords_;
    }
    [[nodiscard]] const ExtensionRecord* findExtensionRecord(ExtensionRecordId id) const noexcept;
    [[nodiscard]] ExtensionRecord* findExtensionRecord(ExtensionRecordId id) noexcept;

    void setName(std::string name) { name_ = std::move(name); }
    [[nodiscard]] bool addComposition(Composition composition);
    [[nodiscard]] bool removeComposition(CompositionId id);
    [[nodiscard]] bool addExtensionRecord(ExtensionRecord record);
    [[nodiscard]] bool removeExtensionRecord(ExtensionRecordId id);
    [[nodiscard]] std::span<const DataBlockRecord> typedDataBlocks() const noexcept {
        return dataBlocks_;
    }
    [[nodiscard]] const DataBlockRecord* findDataBlock(DataBlockRecordId id) const noexcept;
    [[nodiscard]] DataBlockRecord* findDataBlock(DataBlockRecordId id) noexcept;
    [[nodiscard]] bool addDataBlock(DataBlockRecord block);
    [[nodiscard]] bool removeDataBlock(DataBlockRecordId id);
    // Normalized views used by the Inspector. Media assets and extension records remain in their
    // owning collections; only authored non-media blocks are stored in typedDataBlocks().
    [[nodiscard]] std::vector<DataBlockRecord> dataBlocks() const;

    [[nodiscard]] ValidationResult validate() const;
    [[nodiscard]] ValidationResult validateCompositionNesting() const;
    // Diagnostic work counter for detecting accidental whole-project validation on UI paths.
    [[nodiscard]] static std::uint64_t validationCount() noexcept;
    [[nodiscard]] std::size_t rootByteCharge() const;

  private:
    friend class Document;
    // Only Document may freeze its independent publication shell. Exposing this on a Draft's
    // mutable Project would let a retained element pointer mutate a later Snapshot.
    void freezeCompositionContent();
    ProjectId id_;
    std::string name_;
    std::vector<Composition> compositions_;
    std::vector<AssetRecord> assets_;
    std::vector<AssetFolder> assetFolders_;
    std::vector<ExtensionRecord> extensionRecords_;
    std::vector<DataBlockRecord> dataBlocks_;
};

} // namespace bloom::document
