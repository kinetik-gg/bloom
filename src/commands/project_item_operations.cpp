#include <bloom/commands/project_item_operations.hpp>

#include <bloom/commands/asset_operations.hpp>
#include <bloom/commands/composition_settings_operations.hpp>
#include <bloom/commands/operations.hpp>
#include <bloom/document/project.hpp>

#include <string>

namespace bloom::commands {
namespace {

void collect(const document::Project& project, const document::AssetFolderId folder,
             ProjectItems& contents) {
    for (const auto& child : project.assetFolders())
        if (child.parent == folder)
            collect(project, child.id, contents);
    for (const auto& asset : project.assets())
        if (asset.folder == folder)
            contents.assets.push_back(asset.id);
    for (const auto& composition : project.compositions())
        if (composition.folder() == folder)
            contents.compositions.push_back(composition.id());
    contents.folders.push_back(folder);
}

[[nodiscard]] std::optional<OperationResult> applyStep(document::Draft& draft,
                                                       const Operation& operation) {
    auto result = operation.apply(draft);
    if (result.status == OperationStatus::Rejected)
        return result;
    return std::nullopt;
}

} // namespace

ProjectItems folderContents(const document::Project& project,
                            const document::AssetFolderId folder) {
    ProjectItems contents;
    collect(project, folder, contents);
    return contents;
}

OperationResult DeleteProjectItems::apply(document::Draft& draft) const {
    if (items_.assets.empty() && items_.compositions.empty() && items_.folders.empty())
        return OperationResult::rejected(OperationIssueCode::InvalidValue, "Nothing to delete");
    for (const auto folder : items_.folders)
        if (draft.project().findAssetFolder(folder) == nullptr)
            return OperationResult::rejected(OperationIssueCode::InvalidTarget,
                                             "The folder does not exist");
    // Folder contents are read before anything is removed, as the panel reads them from the
    // snapshot it shows.
    ProjectItems contents;
    if (withContents_)
        for (const auto folder : items_.folders)
            collect(draft.project(), folder, contents);
    for (const auto asset : items_.assets)
        if (auto failure = applyStep(draft, RemoveAsset(asset)))
            return std::move(*failure);
    for (const auto composition : items_.compositions)
        if (auto failure = applyStep(draft, DeleteComposition(composition)))
            return std::move(*failure);
    if (withContents_) {
        for (const auto asset : contents.assets)
            if (auto failure = applyStep(draft, RemoveAsset(asset)))
                return std::move(*failure);
        for (const auto composition : contents.compositions)
            if (auto failure = applyStep(draft, DeleteComposition(composition)))
                return std::move(*failure);
        for (const auto folder : contents.folders)
            if (auto failure = applyStep(draft, RemoveAssetFolder(folder)))
                return std::move(*failure);
    } else {
        for (const auto folder : items_.folders)
            if (auto failure = applyStep(draft, RemoveAssetFolder(folder)))
                return std::move(*failure);
    }
    return OperationResult::applied();
}

OperationResult AddCompositionFromFootage::apply(document::Draft& draft) const {
    const auto& project = draft.project();
    std::vector<document::AssetId> footage;
    for (const auto id : assets_) {
        const auto* asset = project.findAsset(id);
        if (asset == nullptr)
            return OperationResult::rejected(OperationIssueCode::InvalidTarget,
                                             "Asset " + std::to_string(id.value()) +
                                                 " does not exist");
        if (asset->kind != document::AssetKind::Lut && asset->kind != document::AssetKind::Font)
            footage.push_back(id);
    }
    if (footage.empty())
        return OperationResult::rejected(OperationIssueCode::InvalidValue,
                                         "Name at least one image, sequence, video or audio asset");
    // Sized from the first visual footage item, like After Effects; the template composition (or
    // the defaults) supplies what the footage does not state.
    CompositionSettings base;
    base.name = fallbackName_.empty()
                    ? "Composition " + std::to_string(project.compositions().size() + 1)
                    : fallbackName_;
    base.duration = core::RationalTime::fromInteger(10);
    if (template_) {
        const auto* current = project.findComposition(*template_);
        if (current == nullptr)
            return OperationResult::rejected(OperationIssueCode::InvalidTarget,
                                             "The template composition does not exist");
        base = compositionSettingsOf(*current);
    }
    const auto* first = project.findAsset(footage.front());
    for (const auto id : footage)
        if (const auto* asset = project.findAsset(id); asset->kind != document::AssetKind::Audio) {
            first = asset;
            break;
        }
    auto settings = compositionSettingsForAsset(*first, base);
    settings.timecode = {};
    return AddCompositionFromAssets(std::move(settings), std::move(footage), folder_).apply(draft);
}

} // namespace bloom::commands
