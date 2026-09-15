#include <algorithm>
#include <bloom/media/image.hpp>
#include <charconv>
#include <limits>
#include <string_view>
#include <system_error>
#include <utility>

namespace bloom::media {
namespace {
struct NumberedName {
    std::string prefix;
    std::string extension;
    std::uint32_t padding = 0;
    std::int64_t frame = 0;
};
std::optional<NumberedName> numberedName(const std::filesystem::path& path) {
    const auto stem = path.stem().string();
    auto begin = stem.size();
    while (begin > 0 && stem[begin - 1] >= '0' && stem[begin - 1] <= '9')
        --begin;
    // A stem that is ALL digits ("0000.png") is a numbered member with an empty prefix -- render
    // farms emit exactly that -- so only a stem with no trailing digits at all is not numbered
    // (owner, 2026-09-15: a pure-numeric sequence imported as individual images).
    if (begin == stem.size())
        return {};
    const auto digits = std::string_view(stem).substr(begin);
    std::int64_t frame = 0;
    const auto parsed = std::from_chars(digits.data(), digits.data() + digits.size(), frame);
    if (parsed.ec != std::errc{} || frame < 0)
        return {};
    auto extension = path.extension().string();
    if (extension != ".png" && extension != ".jpg" && extension != ".jpeg" && extension != ".PNG" &&
        extension != ".JPG" && extension != ".JPEG")
        return {};
    return NumberedName{stem.substr(0, begin), extension, static_cast<std::uint32_t>(digits.size()),
                        frame};
}
} // namespace
ImageResult<SequenceManifest> scanSequence(const std::filesystem::path& path,
                                           const CancelImageWork& cancel,
                                           const ImageProgress& progress) {
    try {
        SequenceManifest manifest;
        const auto name = numberedName(path);
        if (!name)
            return {std::move(manifest), {}};
        manifest.padding = name->padding;
        manifest.pattern = name->prefix + std::string(name->padding, '#') + name->extension;
        std::error_code error;
        auto directory = path.parent_path();
        if (directory.empty())
            directory = ".";
        std::filesystem::directory_iterator iterator(directory, error);
        if (error)
            return {{}, "Sequence directory is unreadable"};
        std::size_t count = 0;
        for (const auto end = std::filesystem::directory_iterator{}; iterator != end;
             iterator.increment(error)) {
            if (error)
                return {{}, "Sequence directory enumeration failed"};
            if (cancel && cancel())
                return {{}, {}, true};
            if (++count > kMaxSequenceEntries)
                return {{}, "Sequence directory exceeds 100000 entries"};
            const auto candidate = numberedName(iterator->path());
            if (!candidate || candidate->prefix != name->prefix ||
                candidate->extension != name->extension)
                continue;
            if (candidate->padding != name->padding) {
                manifest.diagnostics.emplace_back(
                    "Sequence contains a different frame-number padding");
                continue;
            }
            const auto probe = probeImage(iterator->path(), cancel);
            if (!probe.value.has_value())
                return {{}, probe.diagnostic, probe.cancelled};
            manifest.members.push_back(
                {candidate->frame, iterator->path(), probe.value->contentDigest});
            if (progress)
                progress(count, 0);
        }
        if (error)
            return {{}, "Sequence directory enumeration failed"};
        std::ranges::sort(manifest.members, {}, &SequenceMember::frame);
        if (manifest.members.empty())
            return {{}, "Selected sequence member is missing"};
        manifest.first = manifest.members.front().frame;
        manifest.last = manifest.members.back().frame;
        if (manifest.last - manifest.first >= static_cast<std::int64_t>(kMaxSequenceEntries))
            return {{}, "Sequence span exceeds 100000 frames"};
        for (std::size_t index = 1; index < manifest.members.size(); ++index) {
            for (auto frame = manifest.members[index - 1].frame + 1;
                 frame < manifest.members[index].frame; ++frame)
                manifest.gaps.push_back(frame);
        }
        if (!manifest.gaps.empty())
            manifest.diagnostics.emplace_back("Sequence gaps hold the preceding frame");
        return {std::move(manifest), {}};
    } catch (const std::exception&) {
        return {{}, "Sequence scan allocation or I/O failed"};
    }
}
} // namespace bloom::media
