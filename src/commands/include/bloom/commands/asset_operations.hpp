#pragma once
#include <bloom/commands/operation.hpp>
#include <bloom/core/color.hpp>
#include <bloom/media/provider/contract.hpp>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
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
// Moves folders under another folder (nullopt is the project root). A folder cannot move into
// itself or a descendant, and the destination must not already hold a folder of the same name.
class MoveAssetFolders final : public Operation {
  public:
    MoveAssetFolders(std::vector<document::AssetFolderId> folders,
                     std::optional<document::AssetFolderId> parent)
        : folders_(std::move(folders)), parent_(parent) {}
    [[nodiscard]] std::string_view typeId() const noexcept override {
        return "bloom.asset.move-folders";
    }
    [[nodiscard]] OperationResult apply(document::Draft& draft) const override;

  private:
    std::vector<document::AssetFolderId> folders_;
    std::optional<document::AssetFolderId> parent_;
};
// Moves compositions into an Assets folder (nullopt is the project root). Compositions keep their
// project order; the Assets panel sorts them by its columns.
class MoveCompositions final : public Operation {
  public:
    MoveCompositions(std::vector<document::CompositionId> compositions,
                     std::optional<document::AssetFolderId> folder)
        : compositions_(std::move(compositions)), folder_(folder) {}
    [[nodiscard]] std::string_view typeId() const noexcept override {
        return "bloom.asset.move-compositions";
    }
    [[nodiscard]] OperationResult apply(document::Draft& draft) const override;

  private:
    std::vector<document::CompositionId> compositions_;
    std::optional<document::AssetFolderId> folder_;
};
// Removes one folder. Its assets, compositions and child folders move up to its parent.
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
// One undoable edit of an asset's input colour choice. Exact OCIO ids use Auto in the legacy
// field; Auto/Raw/legacy choices clear the id, so the two controls cannot contradict each other.
class SetAssetInputColor final : public Operation {
  public:
    SetAssetInputColor(document::AssetId asset, document::AssetColorSpace legacy,
                       std::string inputColorSpaceId)
        : asset_(asset), legacy_(legacy), inputColorSpaceId_(std::move(inputColorSpaceId)) {}
    [[nodiscard]] std::string_view typeId() const noexcept override {
        return "bloom.asset.set-input-color";
    }
    [[nodiscard]] OperationResult apply(document::Draft& draft) const override;

  private:
    document::AssetId asset_;
    document::AssetColorSpace legacy_;
    std::string inputColorSpaceId_;
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

// One file a batch import could not admit, with its own reason. The rest of the batch imports.
struct ImportFailure final {
    std::filesystem::path path;
    std::string diagnostic;
    std::optional<media::provider::Unavailable> media;
};
// Where a batch import stands, reported on the import worker (#377). An item is one chosen file,
// or one image sequence however many of its members were chosen; a chosen folder contributes the
// items found inside it. Progress is measured in work: one unit per sequence member (each is read
// and hashed, the slow part) and one per other item, so a long sequence moves the bar too.
struct ImportProgress final {
    enum class Phase : std::uint8_t {
        // Searching chosen folders and listing sequence members; the work total is not known.
        Finding,
        // Probing, reading and hashing items against a known work total.
        Reading,
    };
    Phase phase = Phase::Finding;
    std::size_t completed = 0;
    // Items in the batch; zero while finding.
    std::size_t total = 0;
    std::uint64_t workDone = 0;
    // Zero while finding.
    std::uint64_t workTotal = 0;
    // The folder or sequence being searched, or the item being read; empty once finished.
    std::filesystem::path current;
    // Finding: files (in a folder) or frames (of a sequence) found so far in `current`.
    std::uint64_t found = 0;
    bool findingFrames = false;
    // Reading a sequence: members read so far, of `frames`. Both zero for any other item.
    std::uint64_t framesRead = 0;
    std::uint64_t frames = 0;
};
using ImportProgressCallback = std::function<void(const ImportProgress&)>;
// The file types a chosen folder contributes: images, image-sequence members, video, audio and
// LUTs, by extension and ignoring case. A file chosen on its own is always tried.
[[nodiscard]] bool isImportableMediaFile(const std::filesystem::path& path);
// Construct on an import worker: preparation searches folders, probes, hashes and scans. apply only
// copies the prepared records into a draft, so publishing the command never performs media I/O.
//
// Cancellation (#377) stops at the next safe point: the item in progress is abandoned and every
// item already prepared is kept, so a cancelled batch still applies what finished. Only a batch
// with nothing prepared is refused.
class ImportAssets final : public Operation {
  public:
    ImportAssets(const std::vector<std::filesystem::path>& paths,
                 const std::filesystem::path& projectDirectory,
                 const std::function<bool()>& cancel = {},
                 const ImportProgressCallback& progress = {});
    [[nodiscard]] std::string_view typeId() const noexcept override { return "bloom.asset.import"; }
    [[nodiscard]] OperationResult apply(document::Draft& draft) const override;
    // The Assets folder the new assets land in, after its existing assets (#378). A folder removed
    // while the import was being prepared sends them to the root rather than losing the import.
    void setFolder(std::optional<document::AssetFolderId> folder) noexcept { folder_ = folder; }
    [[nodiscard]] const std::optional<document::AssetFolderId>& folder() const noexcept {
        return folder_;
    }
    [[nodiscard]] const std::vector<document::AssetRecord>& prepared() const noexcept {
        return assets_;
    }
    // Non-empty only when the whole batch is refused: cancellation before anything was prepared,
    // an unusable request, or no importable file (then it names the first per-file failure).
    [[nodiscard]] const std::string& diagnostic() const noexcept { return diagnostic_; }
    [[nodiscard]] const std::vector<ImportFailure>& failures() const noexcept { return failures_; }
    // True when cancellation stopped the batch before its last item.
    [[nodiscard]] bool cancelled() const noexcept { return cancelled_; }
    // Items a cancelled batch never finished, including the one it abandoned.
    [[nodiscard]] std::size_t notImported() const noexcept { return notImported_; }
    [[nodiscard]] const std::optional<media::provider::Unavailable>&
    mediaDiagnostic() const noexcept {
        return mediaDiagnostic_;
    }

  private:
    std::vector<document::AssetRecord> assets_;
    std::optional<document::AssetFolderId> folder_;
    std::vector<ImportFailure> failures_;
    std::string diagnostic_;
    std::optional<media::provider::Unavailable> mediaDiagnostic_;
    std::size_t notImported_ = 0;
    bool cancelled_ = false;
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
// The composition background is viewer presentation metadata: no compiled or evaluated input reads
// it and it never fills export alpha (docs/architecture/project-format.md), so changing it cannot
// change a rendered pixel. The Viewer re-presents the frame it already has with the new colour.
class SetCompositionBackgroundColor final : public Operation {
  public:
    SetCompositionBackgroundColor(document::CompositionId composition, core::Color4d color)
        : composition_(composition), color_(color) {}
    [[nodiscard]] std::string_view typeId() const noexcept override {
        return "bloom.composition.set-background-color";
    }
    [[nodiscard]] OperationResult apply(document::Draft& draft) const override;
    [[nodiscard]] bool renderAffecting() const noexcept override { return false; }

  private:
    document::CompositionId composition_;
    core::Color4d color_;
};

class SetCompositionWorkingColorSpace final : public Operation {
  public:
    SetCompositionWorkingColorSpace(document::CompositionId composition,
                                    std::optional<std::string> colorSpaceId)
        : composition_(composition), colorSpaceId_(std::move(colorSpaceId)) {}
    [[nodiscard]] std::string_view typeId() const noexcept override {
        return "bloom.composition.set-working-color-space";
    }
    [[nodiscard]] OperationResult apply(document::Draft& draft) const override;

  private:
    document::CompositionId composition_;
    std::optional<std::string> colorSpaceId_;
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
