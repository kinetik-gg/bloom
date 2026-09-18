#include <algorithm>
#include <bloom/host/frame_export_publication.hpp>
#include <limits>

namespace bloom::host {
namespace {
bool safeName(std::string_view text) {
    return !text.empty() && text != "." && text != ".." && text.size() <= 240 &&
           std::ranges::none_of(text,
                                [](unsigned char c) {
                                    return c < 32 || c == 127 || c == '/' || c == '\\' ||
                                           c == ':' || c == '*' || c == '?' || c == '"' ||
                                           c == '<' || c == '>' || c == '|';
                                }) &&
           text.back() != '.' && text.back() != ' ';
}
} // namespace
std::optional<std::filesystem::path> sequencePublicationPathV1(
    const std::filesystem::path& destination, std::string_view compositionName, std::uint64_t frame,
    std::uint64_t firstFrame, std::uint64_t lastFrame, const SequenceNamingV1& naming) {
    if (destination.empty() || firstFrame > lastFrame || frame < firstFrame || frame > lastFrame ||
        naming.framePadding == 0 || naming.framePadding > 20 || naming.namePattern.size() > 240)
        return {};
    const auto start = naming.startFrame.value_or(firstFrame);
    if (lastFrame - firstFrame > std::numeric_limits<std::uint64_t>::max() - start)
        return {};
    const auto end = start + lastFrame - firstFrame;
    const auto digits = std::max<std::size_t>(naming.framePadding, std::to_string(end).size());
    auto number = std::to_string(start + frame - firstFrame);
    number.insert(0, digits - number.size(), '0');
    auto base = destination.stem().string();
    auto extension = destination.extension().string();
    if (extension.starts_with('.'))
        extension.erase(0, 1);
    if (base.empty())
        base = "frame";
    std::string result;
    unsigned frames = 0;
    const auto& pattern = naming.namePattern;
    for (std::size_t i = 0; i < pattern.size();) {
        const auto rest = std::string_view(pattern).substr(i);
        if (rest.starts_with("####") || rest.starts_with("{frame}")) {
            if (++frames != 1)
                return {};
            result += number;
            i += rest.starts_with("####") ? 4 : 7;
        } else if (rest.starts_with("{name}")) {
            result += compositionName;
            i += 6;
        } else if (rest.starts_with("<base>")) {
            result += base;
            i += 6;
        } else if (rest.starts_with("<ext>")) {
            result += extension;
            i += 5;
        } else {
            if (pattern[i] == '#' || pattern[i] == '{' || pattern[i] == '}')
                return {};
            result += pattern[i++];
        }
    }
    // Exactly one fixed-width ascending number makes every name unique and lexically monotonic.
    // Reject path components and platform-reserved punctuation before any frame can publish.
    if (frames != 1 || !safeName(result) ||
        std::filesystem::path(result).extension() != destination.extension())
        return {};
    return destination.parent_path() / result;
}
} // namespace bloom::host
