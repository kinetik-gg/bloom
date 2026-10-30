#pragma once
// Assets-panel edits that read the project when they apply: deleting a mixed selection (optionally
// with everything a folder holds) and New Composition from Selection. They are one operation each
// so a script or an MCP agent issues the same single undo step the panel does, without having to
// read the project first.
#include <bloom/commands/operation.hpp>
#include <bloom/document/ids.hpp>

#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace bloom::commands {

struct ProjectItems final {
    std::vector<document::AssetId> assets;
    std::vector<document::CompositionId> compositions;
    std::vector<document::AssetFolderId> folders;
};

// The Assets Delete (#378): removes the assets and compositions, then the folders. With
// `withContents`, every asset, composition and subfolder a folder holds goes too, deepest first;
// without it the folders are removed alone and what they held moves to their parent (the
// RemoveAssetFolder rule). Composition layers that nest a deleted composition and nodes that read
// a deleted asset are left referring to it, as in the panel.
class DeleteProjectItems final : public Operation {
  public:
    DeleteProjectItems(ProjectItems items, bool withContents)
        : items_(std::move(items)), withContents_(withContents) {}
    [[nodiscard]] std::string_view typeId() const noexcept override {
        return "bloom.asset.delete-items";
    }
    [[nodiscard]] OperationResult apply(document::Draft& draft) const override;

  private:
    ProjectItems items_;
    bool withContents_;
};

// Everything `folder` holds, deepest folders first, ending with `folder` itself.
[[nodiscard]] ProjectItems folderContents(const document::Project& project,
                                          document::AssetFolderId folder);

// New Composition from Selection (After Effects' rule): a composition sized, timed and named from
// the first visual footage asset (the first asset when all are audio), with what the footage does
// not state taken from `templateComposition` when given (the composition the artist is looking
// at) or the defaults with `fallbackName`, filed in `folder`, holding every footage asset as a
// layer. Fonts and LUTs are skipped. Outputs "composition" and one "layer" per asset.
class AddCompositionFromFootage final : public Operation {
  public:
    AddCompositionFromFootage(std::vector<document::AssetId> assets,
                              std::optional<document::CompositionId> templateComposition,
                              std::string fallbackName,
                              std::optional<document::AssetFolderId> folder = {})
        : assets_(std::move(assets)), template_(templateComposition),
          fallbackName_(std::move(fallbackName)), folder_(folder) {}
    [[nodiscard]] std::string_view typeId() const noexcept override {
        return "bloom.composition.add-from-assets";
    }
    [[nodiscard]] OperationResult apply(document::Draft& draft) const override;

  private:
    std::vector<document::AssetId> assets_;
    std::optional<document::CompositionId> template_;
    std::string fallbackName_;
    std::optional<document::AssetFolderId> folder_;
};

} // namespace bloom::commands
