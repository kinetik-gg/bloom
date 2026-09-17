#include <algorithm>
#include <bloom/commands/asset_operations.hpp>
#include <bloom/document/persisted_text.hpp>
#include <set>
#include <tuple>
#include <unordered_set>

namespace bloom::commands {
namespace {
using Folder = std::optional<document::AssetFolderId>;
using Assets = std::vector<document::AssetId>;
bool folderExists(const document::Project& project, Folder folder) {
    return !folder || project.findAssetFolder(*folder);
}
bool siblingNameExists(const document::Project& project, Folder parent, const std::string& name,
                       document::AssetFolderId except = {}) {
    return std::ranges::any_of(project.assetFolders(), [&](const auto& folder) {
        return folder.id != except && folder.parent == parent && folder.name == name;
    });
}
Assets orderedAssets(const document::Project& project, Folder folder) {
    std::vector<const document::AssetRecord*> records;
    for (const auto& asset : project.assets())
        if (asset.folder == folder)
            records.push_back(&asset);
    std::ranges::sort(records, [](const auto* left, const auto* right) {
        return std::tie(left->order, left->id) < std::tie(right->order, right->id);
    });
    Assets result;
    for (const auto* asset : records)
        result.push_back(asset->id);
    return result;
}
bool assignOrder(document::Project& project, Folder folder, const Assets& ids) {
    bool changed = false;
    for (std::size_t index = 0; index < ids.size(); ++index) {
        auto* asset = project.findAsset(ids[index]);
        if (asset->folder != folder || asset->order != index)
            changed = true;
        asset->folder = folder;
        asset->order = index;
    }
    return changed;
}
bool validAssets(const document::Project& project, const Assets& ids) {
    std::unordered_set<document::AssetId> unique;
    return std::ranges::all_of(
        ids, [&](auto id) { return project.findAsset(id) && unique.insert(id).second; });
}
} // namespace
OperationResult CreateAssetFolder::apply(document::Draft& draft) const {
    if (!document::isValidHumanFacingName(name_) || !folderExists(draft.project(), parent_) ||
        siblingNameExists(draft.project(), parent_, name_))
        return OperationResult::rejected(
            OperationIssueCode::InvalidValue,
            "Use a non-empty, unique folder name and an existing parent");
    const auto id = draft.ids().allocateAssetFolder();
    if (!id)
        return OperationResult::rejected(OperationIssueCode::Unsupported,
                                         "Folder ID space is exhausted");
    if (!draft.project().addAssetFolder({*id, name_, parent_}))
        return OperationResult::rejected(OperationIssueCode::InvalidValue,
                                         "Folder could not be created");
    return OperationResult::applied({{std::string(kCreateAssetFolderOutput), *id}});
}
OperationResult RenameAssetFolder::apply(document::Draft& draft) const {
    auto* folder = draft.project().findAssetFolder(id_);
    if (!folder)
        return OperationResult::rejected(OperationIssueCode::InvalidTarget,
                                         "Folder does not exist");
    if (!document::isValidHumanFacingName(name_) ||
        siblingNameExists(draft.project(), folder->parent, name_, id_))
        return OperationResult::rejected(OperationIssueCode::InvalidValue,
                                         "Use a non-empty, unique folder name");
    if (folder->name == name_)
        return OperationResult::noChange();
    folder->name = name_;
    return OperationResult::applied();
}
OperationResult RemoveAssetFolder::apply(document::Draft& draft) const {
    auto& project = draft.project();
    const auto* folder = project.findAssetFolder(id_);
    if (!folder)
        return OperationResult::noChange();
    const auto parent = folder->parent;
    for (const auto& child : project.assetFolders())
        if (child.parent == id_ && siblingNameExists(project, parent, child.name, id_))
            return OperationResult::rejected(
                OperationIssueCode::InvalidValue,
                "Rename conflicting child folders before removing this folder");
    auto destination = orderedAssets(project, parent);
    const auto children = orderedAssets(project, id_);
    destination.insert(destination.end(), children.begin(), children.end());
    (void)assignOrder(project, parent, destination);
    for (const auto& child : project.assetFolders())
        if (child.parent == id_)
            project.findAssetFolder(child.id)->parent = parent;
    (void)project.removeAssetFolder(id_);
    return OperationResult::applied();
}
OperationResult RenameAsset::apply(document::Draft& draft) const {
    auto* asset = draft.project().findAsset(id_);
    if (!asset)
        return OperationResult::rejected(OperationIssueCode::InvalidTarget, "Asset does not exist");
    if (!document::isValidHumanFacingName(name_))
        return OperationResult::rejected(OperationIssueCode::InvalidValue,
                                         "Asset name must not be empty");
    if (asset->name == name_)
        return OperationResult::noChange();
    asset->name = name_;
    return OperationResult::applied();
}
OperationResult MoveAssets::apply(document::Draft& draft) const {
    auto& project = draft.project();
    if (!folderExists(project, folder_) || !validAssets(project, assets_))
        return OperationResult::rejected(OperationIssueCode::InvalidTarget,
                                         "Choose existing assets and a destination folder");
    std::set<Folder> affected;
    for (auto id : assets_)
        affected.insert(project.findAsset(id)->folder);
    auto destination = orderedAssets(project, folder_);
    const std::unordered_set<document::AssetId> moving(assets_.begin(), assets_.end());
    std::erase_if(destination, [&](auto id) { return moving.contains(id); });
    if (index_ > destination.size())
        return OperationResult::rejected(OperationIssueCode::InvalidOrder,
                                         "Asset destination index is outside the folder");
    if (assets_.empty())
        return OperationResult::noChange();
    destination.insert(destination.begin() + static_cast<std::ptrdiff_t>(index_), assets_.begin(),
                       assets_.end());
    bool changed = assignOrder(project, folder_, destination);
    for (const auto source : affected)
        if (source != folder_)
            changed = assignOrder(project, source, orderedAssets(project, source)) || changed;
    return changed ? OperationResult::applied() : OperationResult::noChange();
}
OperationResult SetAssetTags::apply(document::Draft& draft) const {
    if (!validAssets(draft.project(), assets_))
        return OperationResult::rejected(OperationIssueCode::InvalidTarget,
                                         "Asset does not exist or is repeated");
    auto tags = tags_;
    std::ranges::sort(tags);
    tags.erase(std::unique(tags.begin(), tags.end()), tags.end());
    if (tags.size() > 64 || !std::ranges::all_of(tags, document::isValidStructuralText))
        return OperationResult::rejected(
            OperationIssueCode::InvalidValue,
            "Use at most 64 non-empty tags, each at most 256 UTF-8 bytes");
    bool changed = false;
    for (auto id : assets_) {
        auto* asset = draft.project().findAsset(id);
        changed = changed || asset->tags != tags;
        asset->tags = tags;
    }
    return changed ? OperationResult::applied() : OperationResult::noChange();
}
OperationResult ReorderAssets::apply(document::Draft& draft) const {
    auto& project = draft.project();
    if (!folderExists(project, folder_) || !validAssets(project, order_) ||
        orderedAssets(project, folder_).size() != order_.size() ||
        !std::ranges::all_of(order_,
                             [&](auto id) { return project.findAsset(id)->folder == folder_; }))
        return OperationResult::rejected(
            OperationIssueCode::InvalidOrder,
            "Reorder must contain every asset in the folder exactly once");
    return assignOrder(project, folder_, order_) ? OperationResult::applied()
                                                 : OperationResult::noChange();
}
} // namespace bloom::commands
