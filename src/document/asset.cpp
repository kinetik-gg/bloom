#include <algorithm>
#include <bloom/core/utf8.hpp>
#include <bloom/document/asset.hpp>
#include <bloom/document/persisted_text.hpp>

namespace bloom::document {
namespace {
bool validLocator(const AssetLocator& locator) {
    if (locator.kind == "file") {
        return locator.portability == "project-relative" && !locator.path.empty() &&
               locator.path.size() <= 4096 && locator.path.front() != '/' &&
               locator.path.find('\\') == std::string::npos &&
               locator.path.find(':') == std::string::npos &&
               locator.path.find('\0') == std::string::npos && core::isValidUtf8(locator.path) &&
               locator.relinkHint.starts_with("file:") && locator.relinkHint.size() <= 16384 &&
               locator.relinkHint.find('\0') == std::string::npos &&
               core::isValidUtf8(locator.relinkHint);
    }
    if (locator.kind == "font" && locator.portability == "builtin") {
        return locator.path.size() <= 4096 && locator.relinkHint.starts_with("font:") &&
               locator.relinkHint.size() <= 16384 && core::isValidUtf8(locator.path) &&
               core::isValidUtf8(locator.relinkHint);
    }
    if (locator.kind == "font" && locator.portability == "system") {
        return !locator.path.empty() && locator.path.size() <= 4096 &&
               std::filesystem::path(locator.path).is_absolute() &&
               core::isValidUtf8(locator.path) && locator.relinkHint.starts_with("file:") &&
               locator.relinkHint.size() <= 16384 && core::isValidUtf8(locator.relinkHint);
    }
    return false;
}
} // namespace
std::string defaultAssetName(const AssetRecord& asset) {
    // Builtin font locators name a captured face, not a file (for example embedded/0).
    if (asset.kind == AssetKind::Font && asset.locator.portability == "builtin" &&
        !asset.fontFamily.empty())
        return asset.fontFamily + (asset.fontStyle.empty() ? "" : " " + asset.fontStyle);
    auto name = asset.locator.path.substr(asset.locator.path.find_last_of("/\\") + 1);
    const auto dot = name.find_last_of('.');
    if (dot != std::string::npos && dot != 0 && name != "..")
        name.resize(dot);
    if (name.empty())
        name = asset.fontFamily.empty() ? "Asset" : asset.fontFamily;
    return name;
}
ValidationResult AssetRecord::validate() const {
    ValidationResult result;
    // Prepared imports may omit the name; Project::addAsset supplies the lexical default.
    if (!name.empty())
        validateHumanFacingName(name, "name", "Asset name", result);
    if (folder && !folder->isValid())
        result.add(ValidationCode::InvalidId, "folder", "Folder ID must not be zero");
    if (tags.size() > 64 || !std::ranges::is_sorted(tags) ||
        std::ranges::adjacent_find(tags) != tags.end())
        result.add(ValidationCode::InvalidValue, "tags",
                   "Asset tags must be sorted, unique and at most 64");
    for (const auto& tag : tags)
        validateStructuralText(tag, "tags", "Asset tag", result);
    if (!id.isValid())
        result.add(ValidationCode::InvalidId, "id", "Asset ID must not be zero");
    if (!validLocator(locator))
        result.add(ValidationCode::InvalidValue, "locator", "Invalid asset locator");
    if (kind == AssetKind::Font) {
        if (fontFamily.empty() || fontStyle.empty() || fontFamily.size() > 1024 ||
            fontStyle.size() > 1024 || !core::isValidUtf8(fontFamily) ||
            !core::isValidUtf8(fontStyle))
            result.add(ValidationCode::InvalidValue, "font", "Invalid font face metadata");
        if (!manifest.members.empty() || !manifest.gaps.empty() || !manifest.pattern.empty())
            result.add(ValidationCode::InvalidValue, "manifest", "A font asset has no manifest");
    } else if (kind == AssetKind::Audio) {
        if (rate < 8000 || rate > 384000 || channels == 0 || channels > 32 || frames == 0 ||
            duration <= core::RationalTime{})
            result.add(ValidationCode::InvalidValue, "audio", "Invalid audio descriptor");
    } else if (width == 0 || height == 0 || width > 16384 || height > 16384 ||
               static_cast<std::uint64_t>(width) * height > 16777216)
        result.add(ValidationCode::InvalidValue, "dimensions", "Invalid image dimensions");
    if (interpretation.colorSpace > AssetColorSpace::Raw ||
        interpretation.alphaAssociation > AssetAlphaAssociation::Premultiplied)
        result.add(ValidationCode::InvalidValue, "interpretation", "Invalid image interpretation");
    if (kind == AssetKind::Font) {
        if (interpretation.colorSpace != AssetColorSpace::Auto ||
            interpretation.alphaAssociation != AssetAlphaAssociation::Straight)
            result.add(ValidationCode::InvalidValue, "interpretation",
                       "A font asset has no image interpretation");
    } else if (kind == AssetKind::Image) {
        if (!manifest.members.empty() || !manifest.gaps.empty() || !manifest.pattern.empty())
            result.add(ValidationCode::InvalidValue, "manifest",
                       "A still image has no sequence manifest");
    } else if (kind == AssetKind::Sequence) {
        if (manifest.members.size() < 2 || manifest.members.size() > 100000 || manifest.first < 0 ||
            manifest.last < manifest.first || manifest.last - manifest.first >= 100000 ||
            manifest.padding == 0 || manifest.padding > 255 || manifest.pattern.size() > 4096 ||
            manifest.pattern.empty()) {
            result.add(ValidationCode::InvalidValue, "manifest", "Invalid sequence range");
            return result;
        }
        std::int64_t previous = manifest.first - 1;
        std::vector<std::int64_t> gaps;
        for (const auto& member : manifest.members) {
            if (member.frame <= previous || member.frame > manifest.last ||
                !validLocator(member.locator)) {
                result.add(ValidationCode::InvalidValue, "manifest.members",
                           "Invalid sequence member order or locator");
                return result;
            }
            for (auto frame = previous + 1; frame < member.frame; ++frame)
                gaps.push_back(frame);
            previous = member.frame;
        }
        if (manifest.members.front().frame != manifest.first || previous != manifest.last ||
            gaps != manifest.gaps)
            result.add(ValidationCode::InvalidValue, "manifest.gaps",
                       "Sequence gaps disagree with members");
    } else if (kind == AssetKind::Audio) {
        if (!manifest.members.empty() || !manifest.gaps.empty() || !manifest.pattern.empty())
            result.add(ValidationCode::InvalidValue, "manifest",
                       "An audio asset has no sequence manifest");
    } else
        result.add(ValidationCode::InvalidValue, "kind", "Invalid asset kind");
    return result;
}
} // namespace bloom::document
