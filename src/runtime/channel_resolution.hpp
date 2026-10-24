#pragma once

// ADR 0026 host parameter resolution: a compiled channel expression resolved against the probed
// channel lists of its sources. Shared by the CPU evaluator and GPU scene preparation so both
// paths gather exactly the same channels.

#include <bloom/media/image.hpp>
#include <bloom/render/channel_set_image.hpp>
#include <bloom/runtime/channel_expression.hpp>

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace bloom::runtime::detail {

struct ResolvedChannel final {
    // The channel's name in the resolved set.
    std::string name;
    render::ChannelDescriptor descriptor;
    // The contributing source and its channel name, or nullopt for a constant channel.
    std::optional<std::uint32_t> source;
    std::string sourceChannel;
    float constant = 0.0F;
    friend bool operator==(const ResolvedChannel&, const ResolvedChannel&) = default;
};

struct ChannelResolution final {
    std::vector<ResolvedChannel> channels;
    std::string error;
    [[nodiscard]] bool ok() const noexcept { return error.empty(); }
};

// `parts[i]` is the probed part of `expression.sources[i]` (nullptr when unavailable).
[[nodiscard]] ChannelResolution
resolveChannelExpression(const CompiledChannelExpression& expression,
                         std::span<const media::ImagePartInfo* const> parts);

// A Shuffle slot: a channel of `set` by name, or the constants "0" and "1". Nullopt when the named
// channel is absent.
[[nodiscard]] std::optional<ResolvedChannel> resolveChannelSlot(std::span<const ResolvedChannel> set,
                                                                std::string_view slot);

// Comma-separated channel patterns: an exact channel name, a layer name (every channel of the
// layer), a glob over full names with `*` and `?`, or `@role` (color, alpha, luminance, depth,
// normal, motion, position, uv, id, coverage, data). An empty pattern matches nothing.
[[nodiscard]] bool matchesChannelPattern(std::string_view pattern,
                                         const render::ChannelDescriptor& channel);

[[nodiscard]] render::ChannelSampleKind channelSampleKind(media::ImageChannelSampleKind kind);

} // namespace bloom::runtime::detail
