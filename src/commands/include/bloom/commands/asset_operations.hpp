#pragma once
#include <bloom/commands/operation.hpp>
#include <bloom/core/color.hpp>
#include <filesystem>
#include <functional>
#include <vector>

namespace bloom::commands {
inline constexpr std::string_view kCreateAssetFolderOutput = "folder";
class CreateAssetFolder final : public Operation {
  public:
    explicit CreateAssetFolder(std::string name, std::optional<document::AssetFolderId> parent = {})
        : name_(std::move(name)), parent_(parent) {}
    [[nodiscard]] std::string_view typeId() const noexcept override {
        return "bloom.asset.create-folder";
    }
    [[nodiscard]] OperationResult apply(document::Draft& draft) const override;

  private:
    std::string name_;
    std::optional<document::AssetFolderId> parent_;
};
class RenameAssetFolder final : public Operation {
  public:
    RenameAssetFolder(document::AssetFolderId id, std::string name)
        : id_(id), name_(std::move(name)) {}
    [[nodiscard]] std::string_view typeId() const noexcept override {
        return "bloom.asset.rename-folder";
    }
    [[nodiscard]] OperationResult apply(document::Draft& draft) const override;

  private:
    document::AssetFolderId id_;
    std::string name_;
};
class RemoveAssetFolder final : public Operation {
  public:
    explicit RemoveAssetFolder(document::AssetFolderId id) : id_(id) {}
    [[nodiscard]] std::string_view typeId() const noexcept override {
        return "bloom.asset.remove-folder";
    }
    [[nodiscard]] OperationResult apply(document::Draft& draft) const override;

  private:
    document::AssetFolderId id_;
};
class RenameAsset final : public Operation {
  public:
    RenameAsset(document::AssetId id, std::string name) : id_(id), name_(std::move(name)) {}
    [[nodiscard]] std::string_view typeId() const noexcept override { return "bloom.asset.rename"; }
    [[nodiscard]] OperationResult apply(document::Draft& draft) const override;

  private:
    document::AssetId id_;
    std::string name_;
};
// index addresses the destination's asset list AFTER removing all moved IDs. The caller's
// ID order is retained; all affected folders receive contiguous positions. No graph is rewritten.
class MoveAssets final : public Operation {
  public:
    MoveAssets(std::vector<document::AssetId> assets, std::optional<document::AssetFolderId> folder,
               std::size_t index)
        : assets_(std::move(assets)), folder_(folder), index_(index) {}
    [[nodiscard]] std::string_view typeId() const noexcept override { return "bloom.asset.move"; }
    [[nodiscard]] OperationResult apply(document::Draft& draft) const override;

  private:
    std::vector<document::AssetId> assets_;
    std::optional<document::AssetFolderId> folder_;
    std::size_t index_;
};
class SetAssetTags final : public Operation {
  public:
    SetAssetTags(std::vector<document::AssetId> assets, std::vector<std::string> tags)
        : assets_(std::move(assets)), tags_(std::move(tags)) {}
    SetAssetTags(document::AssetId asset, std::vector<std::string> tags)
        : assets_{asset}, tags_(std::move(tags)) {}
    [[nodiscard]] std::string_view typeId() const noexcept override {
        return "bloom.asset.set-tags";
    }
    [[nodiscard]] OperationResult apply(document::Draft& draft) const override;

  private:
    std::vector<document::AssetId> assets_;
    std::vector<std::string> tags_;
};
// A complete permutation of the assets in one folder (nullopt denotes the project root).
class ReorderAssets final : public Operation {
  public:
    ReorderAssets(std::optional<document::AssetFolderId> folder,
                  std::vector<document::AssetId> order)
        : folder_(folder), order_(std::move(order)) {}
    [[nodiscard]] std::string_view typeId() const noexcept override {
        return "bloom.asset.reorder";
    }
    [[nodiscard]] OperationResult apply(document::Draft& draft) const override;

  private:
    std::optional<document::AssetFolderId> folder_;
    std::vector<document::AssetId> order_;
};

class EnsureFontAsset final : public Operation {
  public:
    explicit EnsureFontAsset(document::AssetRecord asset) : asset_(std::move(asset)) {}
    [[nodiscard]] std::string_view typeId() const noexcept override {
        return "bloom.asset.ensure-font";
    }
    [[nodiscard]] OperationResult apply(document::Draft& draft) const override;

  private:
    document::AssetRecord asset_;
};

class RelinkFontAsset final : public Operation {
  public:
    RelinkFontAsset(document::AssetId id, document::AssetRecord replacement)
        : id_(id), replacement_(std::move(replacement)) {}
    [[nodiscard]] std::string_view typeId() const noexcept override {
        return "bloom.asset.relink-font";
    }
    [[nodiscard]] OperationResult apply(document::Draft& draft) const override;

  private:
    document::AssetId id_;
    document::AssetRecord replacement_;
};

// Construct on an import worker: preparation probes, hashes and scans. apply only copies the
// prepared records into a draft, so publishing the command never performs media I/O.
class ImportAssets final : public Operation {
  public:
    ImportAssets(const std::vector<std::filesystem::path>& paths,
                 const std::filesystem::path& projectDirectory,
                 const std::function<bool()>& cancel = {},
                 const std::function<void(std::uint64_t, std::uint64_t)>& progress = {});
    [[nodiscard]] std::string_view typeId() const noexcept override { return "bloom.asset.import"; }
    [[nodiscard]] OperationResult apply(document::Draft& draft) const override;
    [[nodiscard]] const std::vector<document::AssetRecord>& prepared() const noexcept {
        return assets_;
    }
    [[nodiscard]] const std::string& diagnostic() const noexcept { return diagnostic_; }

  private:
    std::vector<document::AssetRecord> assets_;
    std::string diagnostic_;
};
class RelinkAsset final : public Operation {
  public:
    RelinkAsset(document::AssetId id, const std::filesystem::path& path,
                const std::filesystem::path& projectDirectory,
                const std::function<bool()>& cancel = {});
    [[nodiscard]] std::string_view typeId() const noexcept override { return "bloom.asset.relink"; }
    [[nodiscard]] OperationResult apply(document::Draft& draft) const override;

  private:
    document::AssetId id_;
    ImportAssets prepared_;
};
class RemoveAsset final : public Operation {
  public:
    explicit RemoveAsset(document::AssetId id) : id_(id) {}
    [[nodiscard]] std::string_view typeId() const noexcept override { return "bloom.asset.remove"; }
    [[nodiscard]] OperationResult apply(document::Draft& draft) const override;

  private:
    document::AssetId id_;
};
class SetCompositionBackgroundColor final : public Operation {
  public:
    SetCompositionBackgroundColor(document::CompositionId composition, core::Color4d color)
        : composition_(composition), color_(color) {}
    [[nodiscard]] std::string_view typeId() const noexcept override {
        return "bloom.composition.set-background-color";
    }
    [[nodiscard]] OperationResult apply(document::Draft& draft) const override;

  private:
    document::CompositionId composition_;
    core::Color4d color_;
};
class AddImageLayer final : public Operation {
  public:
    AddImageLayer(document::CompositionId composition, document::AssetId asset)
        : composition_(composition), asset_(asset) {}
    [[nodiscard]] std::string_view typeId() const noexcept override {
        return "bloom.layer.add-image";
    }
    [[nodiscard]] OperationResult apply(document::Draft& draft) const override;

  private:
    document::CompositionId composition_;
    document::AssetId asset_;
};
class AddAudioLayer final : public Operation {
  public:
    AddAudioLayer(document::CompositionId composition, document::AssetId asset)
        : composition_(composition), asset_(asset) {}
    [[nodiscard]] std::string_view typeId() const noexcept override {
        return "bloom.layer.add-audio";
    }
    [[nodiscard]] OperationResult apply(document::Draft& draft) const override;

  private:
    document::CompositionId composition_;
    document::AssetId asset_;
};
} // namespace bloom::commands
