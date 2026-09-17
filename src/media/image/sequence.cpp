#include <algorithm>
#include <bloom/media/image.hpp>
#include <cctype>
#include <charconv>
#include <limits>
#include <string_view>
#include <system_error>
#include <utility>

namespace bloom::media {
namespace {
struct NumberedName {
    std::string prefix;
    std::string suffix;
    std::string extension;
    std::uint32_t padding = 0;
    std::int64_t frame = 0;
};
// The frame number is the LAST run of digits anywhere in the stem; whatever surrounds it is the
// member's identity. No naming convention is assumed (owner, 2026-09-15: "detect whatever the
// format of the name is, as long as it detects sequence numbering with whatever pad digits"):
// "shot.0001.png", "shot_0001.png", "shot0001.png", "0001.png" and "shot0001_left.png" are all
// members, and the padding may differ between members.
std::optional<NumberedName> numberedName(const std::filesystem::path& path) {
    auto extension = path.extension().string();
    std::string lowered = extension;
    std::ranges::transform(lowered, lowered.begin(), [](const unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    if (lowered != ".png" && lowered != ".jpg" && lowered != ".jpeg" && lowered != ".exr")
        return {};
    const auto stem = path.stem().string();
    const auto isDigit = [](const char ch) { return ch >= '0' && ch <= '9'; };
    auto end = stem.size();
    while (end > 0 && !isDigit(stem[end - 1]))
        --end;
    if (end == 0)
        return {};
    auto begin = end;
    while (begin > 0 && isDigit(stem[begin - 1]))
        --begin;
    const auto digits = std::string_view(stem).substr(begin, end - begin);
    std::int64_t frame = 0;
    const auto parsed = std::from_chars(digits.data(), digits.data() + digits.size(), frame);
    if (parsed.ec != std::errc{} || frame < 0)
        return {};
    return NumberedName{stem.substr(0, begin), stem.substr(end), lowered,
                        static_cast<std::uint32_t>(digits.size()), frame};
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
        manifest.pattern =
            name->prefix + std::string(name->padding, '#') + name->suffix + name->extension;
        std::error_code error;
        auto directory = path.parent_path();
        if (directory.empty())
            directory = ".";
        std::filesystem::directory_iterator iterator(directory, error);
        if (error)
            return {{}, "Sequence directory is unreadable"};
        std::size_t count = 0;
        bool mixedPadding = false;
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
                candidate->suffix != name->suffix || candidate->extension != name->extension)
                continue;
            // Mixed padding stays in the sequence: the number is what identifies a frame, the
            // padding is only how it was written. It is reported once so the artist knows.
            if (candidate->padding != name->padding && !mixedPadding) {
                mixedPadding = true;
                manifest.diagnostics.emplace_back("Sequence mixes frame-number padding");
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
