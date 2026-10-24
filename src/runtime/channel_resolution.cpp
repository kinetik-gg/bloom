#include "channel_resolution.hpp"

#include <algorithm>
#include <array>

namespace bloom::runtime::detail {
namespace {

[[nodiscard]] std::string_view trim(std::string_view text) noexcept {
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t'))
        text.remove_prefix(1);
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t'))
        text.remove_suffix(1);
    return text;
}

[[nodiscard]] bool glob(const std::string_view pattern, const std::string_view text) noexcept {
    // Iterative wildcard match with single-star backtracking.
    std::size_t p = 0;
    std::size_t t = 0;
    std::size_t star = std::string_view::npos;
    std::size_t mark = 0;
    while (t < text.size()) {
        if (p < pattern.size() && (pattern[p] == '?' || pattern[p] == text[t])) {
            ++p;
            ++t;
        } else if (p < pattern.size() && pattern[p] == '*') {
            star = p++;
            mark = t;
        } else if (star != std::string_view::npos) {
            p = star + 1;
            t = ++mark;
        } else {
            return false;
        }
    }
    while (p < pattern.size() && pattern[p] == '*')
        ++p;
    return p == pattern.size();
}

[[nodiscard]] bool roleMatches(const std::string_view name, const render::ChannelRole role) {
    using render::ChannelRole;
    if (name == "color" || name == "colour")
        return render::isColourRole(role);
    if (name == "alpha")
        return role == ChannelRole::Alpha;
    if (name == "luminance")
        return role == ChannelRole::Luminance;
    if (name == "depth")
        return role == ChannelRole::Depth;
    if (name == "normal")
        return role == ChannelRole::NormalX || role == ChannelRole::NormalY ||
               role == ChannelRole::NormalZ;
    if (name == "motion")
        return role == ChannelRole::MotionU || role == ChannelRole::MotionV;
    if (name == "position")
        return role == ChannelRole::PositionX || role == ChannelRole::PositionY ||
               role == ChannelRole::PositionZ;
    if (name == "uv")
        return role == ChannelRole::UvU || role == ChannelRole::UvV;
    if (name == "id")
        return role == ChannelRole::Id;
    if (name == "coverage")
        return role == ChannelRole::Coverage;
    if (name == "data")
        return role == ChannelRole::Data;
    return false;
}

[[nodiscard]] bool tokenMatches(const std::string_view token,
                                const render::ChannelDescriptor& channel) {
    if (token.empty())
        return false;
    if (token.front() == '@')
        return roleMatches(token.substr(1), channel.role);
    if (token.find_first_of("*?") != std::string_view::npos)
        return glob(token, channel.name);
    return token == channel.name || token == channel.layer;
}

} // namespace

render::ChannelSampleKind channelSampleKind(const media::ImageChannelSampleKind kind) {
    return kind;
}

bool matchesChannelPattern(const std::string_view pattern,
                           const render::ChannelDescriptor& channel) {
    std::size_t start = 0;
    while (start <= pattern.size()) {
        const auto comma = pattern.find(',', start);
        const auto token = trim(pattern.substr(
            start, comma == std::string_view::npos ? std::string_view::npos : comma - start));
        if (tokenMatches(token, channel))
            return true;
        if (comma == std::string_view::npos)
            break;
        start = comma + 1;
    }
    return false;
}

std::optional<ResolvedChannel> resolveChannelSlot(const std::span<const ResolvedChannel> set,
                                                  const std::string_view slot) {
    if (slot == "0" || slot == "1") {
        ResolvedChannel constant;
        constant.name = std::string(slot);
        constant.descriptor = render::describeChannel(slot, render::ChannelSampleKind::Float);
        constant.constant = slot == "1" ? 1.0F : 0.0F;
        return constant;
    }
    const auto found =
        std::ranges::find_if(set, [&](const ResolvedChannel& channel) { return channel.name == slot; });
    if (found == set.end())
        return std::nullopt;
    return *found;
}

ChannelResolution resolveChannelExpression(const CompiledChannelExpression& expression,
                                           const std::span<const media::ImagePartInfo* const> parts) {
    ChannelResolution result;
    if (expression.steps.empty()) {
        result.error = "The channel set has no source";
        return result;
    }
    std::vector<std::vector<ResolvedChannel>> values;
    values.reserve(expression.steps.size());
    for (std::size_t index = 0; index < expression.steps.size(); ++index) {
        const auto& step = expression.steps[index];
        const auto earlier = [&](const std::uint32_t at) { return at < index; };
        std::vector<ResolvedChannel> value;
        switch (step.kind) {
        case CompiledChannelStepKind::Source: {
            if (step.source >= expression.sources.size() || step.source >= parts.size()) {
                result.error = "A channel source is missing";
                return result;
            }
            const auto* part = parts[step.source];
            if (part == nullptr) {
                result.error = "A channel source is unavailable";
                return result;
            }
            for (const auto& channel : part->channels) {
                ResolvedChannel resolved;
                resolved.name = channel.name;
                resolved.descriptor =
                    render::describeChannel(channel.name, channelSampleKind(channel.sampleKind));
                resolved.source = step.source;
                resolved.sourceChannel = channel.name;
                value.push_back(std::move(resolved));
            }
            break;
        }
        case CompiledChannelStepKind::Remove:
            if (!earlier(step.input)) {
                result.error = "A channel step reads a later step";
                return result;
            }
            for (const auto& channel : values[step.input])
                if (matchesChannelPattern(step.select, channel.descriptor) == step.keep)
                    value.push_back(channel);
            break;
        case CompiledChannelStepKind::Copy:
            if (!earlier(step.input) || !earlier(step.from)) {
                result.error = "A channel step reads a later step";
                return result;
            }
            value = values[step.input];
            for (const auto& channel : values[step.from]) {
                if (!matchesChannelPattern(step.select, channel.descriptor))
                    continue;
                const auto existing = std::ranges::find_if(
                    value, [&](const ResolvedChannel& item) { return item.name == channel.name; });
                if (existing != value.end())
                    *existing = channel;
                else
                    value.push_back(channel);
            }
            break;
        case CompiledChannelStepKind::Rename: {
            if (!earlier(step.input)) {
                result.error = "A channel step reads a later step";
                return result;
            }
            constexpr std::array<std::string_view, 4> kSuffixes{"R", "G", "B", "A"};
            for (std::size_t slot = 0; slot < 4; ++slot) {
                auto mapped = resolveChannelSlot(values[step.input], step.mapping[slot]);
                if (!mapped) {
                    result.error = "Shuffle names a channel that is not in its input: " +
                                   step.mapping[slot];
                    return result;
                }
                mapped->name = step.layer.empty() ? std::string(kSuffixes[slot])
                                                  : step.layer + "." + std::string(kSuffixes[slot]);
                mapped->descriptor =
                    render::describeChannel(mapped->name, mapped->descriptor.sampleKind);
                value.push_back(std::move(*mapped));
            }
            break;
        }
        }
        values.push_back(std::move(value));
    }
    result.channels = std::move(values.back());
    return result;
}

} // namespace bloom::runtime::detail
