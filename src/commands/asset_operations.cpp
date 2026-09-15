#include <algorithm>
#include <bloom/commands/asset_operations.hpp>
#include <bloom/media/image.hpp>
#include <set>
#include <utility>

namespace bloom::commands {
namespace {
std::string utf8(const std::filesystem::path& path) {
    const auto bytes = path.generic_u8string();
    return {reinterpret_cast<const char*>(bytes.data()), bytes.size()};
}
std::string fileUri(const std::filesystem::path& path) {
    const auto text = utf8(path);
    std::string uri = text.starts_with("/") ? "file://" : "file:///";
    constexpr std::string_view hex = "0123456789ABCDEF";
    for (const char character : text) {
        const auto byte = static_cast<unsigned char>(character);
        if ((byte >= 'a' && byte <= 'z') || (byte >= 'A' && byte <= 'Z') ||
            (byte >= '0' && byte <= '9') || character == '/' || character == ':' ||
            character == '-' || character == '_' || character == '.' || character == '~')
            uri += character;
        else {
            uri += '%';
            uri += hex[byte >> 4U];
            uri += hex[byte & 15U];
        }
    }
    return uri;
}
document::AssetLocator locator(const std::filesystem::path& path,
                               const std::filesystem::path& base) {
    const auto absolute = std::filesystem::absolute(path).lexically_normal();
    const auto relative =
        absolute.lexically_relative(std::filesystem::absolute(base).lexically_normal());
    if (relative.empty())
        throw std::runtime_error("Image and project must share a filesystem root");
    return {"file", "project-relative", utf8(relative), fileUri(absolute)};
}
} // namespace
ImportAssets::ImportAssets(const std::vector<std::filesystem::path>& paths,
                           const std::filesystem::path& projectDirectory,
                           const std::function<bool()>& cancel,
                           const std::function<void(std::uint64_t, std::uint64_t)>& progress) {
    try {
        if (paths.size() > 100000) {
            diagnostic_ = "Too many files selected";
            return;
        }
        std::set<std::filesystem::path> admitted;
        for (const auto& path : paths) {
            if (admitted.contains(std::filesystem::absolute(path).lexically_normal()))
                continue;
            if (cancel && cancel()) {
                diagnostic_ = "Image import cancelled";
                assets_.clear();
                return;
            }
            const auto probe = media::probeImage(path, cancel);
            if (!probe.value.has_value()) {
                diagnostic_ = probe.cancelled ? "Image import cancelled" : probe.diagnostic;
                assets_.clear();
                return;
            }
            auto sequence = media::scanSequence(path, cancel, progress);
            if (!sequence.value.has_value()) {
                diagnostic_ = sequence.cancelled ? "Image import cancelled" : sequence.diagnostic;
                assets_.clear();
                return;
            }
            document::AssetRecord asset;
            asset.locator = locator(path, projectDirectory);
            asset.contentDigest = probe.value->contentDigest;
            asset.width = probe.value->width;
            asset.height = probe.value->height;
            if (sequence.value->members.size() > 1) {
                asset.kind = document::AssetKind::Sequence;
                const auto& scan = *sequence.value;
                asset.manifest.pattern = scan.pattern;
                asset.manifest.padding = scan.padding;
                asset.manifest.first = scan.first;
                asset.manifest.last = scan.last;
                asset.manifest.gaps = scan.gaps;
                core::Sha256Hasher hasher;
                for (const auto& member : scan.members) {
                    asset.manifest.members.push_back({member.frame,
                                                      locator(member.path, projectDirectory),
                                                      member.contentDigest});
                    const auto frame = std::to_string(member.frame) + ":";
                    const auto digest = member.contentDigest.toLowercaseHex();
                    if (!hasher.update(std::as_bytes(std::span(frame))) ||
                        !hasher.update(std::as_bytes(std::span(digest)))) {
                        diagnostic_ = "Sequence digest failed";
                        assets_.clear();
                        return;
                    }
                    admitted.insert(std::filesystem::absolute(member.path).lexically_normal());
                }
                asset.contentDigest = hasher.finalize();
                asset.locator = asset.manifest.members.front().locator;
            } else
                admitted.insert(std::filesystem::absolute(path).lexically_normal());
            assets_.push_back(std::move(asset));
            if (progress)
                progress(assets_.size(), paths.size());
        }
    } catch (const std::exception& error) {
        diagnostic_ = error.what();
        assets_.clear();
    }
}
OperationResult ImportAssets::apply(document::Draft& draft) const {
    if (!diagnostic_.empty())
        return OperationResult::rejected(OperationIssueCode::InvalidValue, diagnostic_);
    std::vector<OperationOutput> outputs;
    for (auto asset : assets_) {
        const auto existing =
            std::ranges::find_if(draft.project().assets(), [&](const auto& candidate) {
                return candidate.locator.path == asset.locator.path &&
                       candidate.contentDigest == asset.contentDigest;
            });
        if (existing != draft.project().assets().end())
            continue;
        const auto id = draft.ids().allocateAsset();
        if (!id)
            return OperationResult::rejected(OperationIssueCode::Unsupported,
                                             "Asset ID space is exhausted");
        asset.id = *id;
        if (!draft.project().addAsset(std::move(asset)))
            return OperationResult::rejected(OperationIssueCode::InvalidValue,
                                             "Invalid imported asset");
        outputs.push_back({"asset", *id});
    }
    return outputs.empty() ? OperationResult::noChange()
                           : OperationResult::applied(std::move(outputs));
}
RelinkAsset::RelinkAsset(document::AssetId id, std::filesystem::path path,
                         const std::filesystem::path& base, const std::function<bool()>& cancel)
    : id_(id), prepared_({std::move(path)}, base, cancel) {}
OperationResult RelinkAsset::apply(document::Draft& draft) const {
    auto* target = draft.project().findAsset(id_);
    if (!target)
        return OperationResult::rejected(OperationIssueCode::InvalidTarget, "Asset does not exist");
    if (!prepared_.diagnostic().empty() || prepared_.prepared().size() != 1)
        return OperationResult::rejected(OperationIssueCode::InvalidValue, prepared_.diagnostic());
    auto replacement = prepared_.prepared().front();
    replacement.id = id_;
    replacement.interpretation = target->interpretation;
    *target = std::move(replacement);
    return OperationResult::applied({{"asset", id_}});
}
OperationResult RemoveAsset::apply(document::Draft& draft) const {
    if (!draft.project().removeAsset(id_))
        return OperationResult::noChange();
    return OperationResult::applied();
}
OperationResult SetCompositionBackgroundColor::apply(document::Draft& draft) const {
    auto* composition = draft.project().findComposition(composition_);
    if (!composition)
        return OperationResult::rejected(OperationIssueCode::InvalidTarget,
                                         "Composition does not exist");
    if (!color_.isValid())
        return OperationResult::rejected(OperationIssueCode::InvalidValue,
                                         "Invalid background colour");
    if (composition->backgroundColor() == color_)
        return OperationResult::noChange();
    composition->setBackgroundColor(color_);
    return OperationResult::applied();
}
} // namespace bloom::commands
