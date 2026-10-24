#pragma once

// ADR 0026: the compiled form of a ChannelSet subgraph. Channel algebra (Remove, Channel Copy and
// Shuffle's channel output) touches no pixels, so it compiles to this symbolic expression. It is
// resolved against the probed source channel lists at evaluation (host parameter resolution), and
// the only pixel work is the Shuffle that maps resolved channels into a process image.

#include <bloom/document/asset.hpp>
#include <bloom/document/ids.hpp>

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace bloom::runtime {

// One Image source's `channels` output: every channel of the selected part.
struct CompiledChannelSource final {
    document::NodeId sourceNodeId;
    std::optional<document::AssetRecord> asset;
    std::int64_t startFrame = 0;
    std::int64_t loopMode = 0;
    std::int64_t colorSpace = 0;
    std::string inputColorSpaceId{};
    std::string part{};
    friend bool operator==(const CompiledChannelSource&, const CompiledChannelSource&) = default;
};

enum class CompiledChannelStepKind : std::uint8_t {
    // The channels of `sources[source]`.
    Source,
    // `steps[input]` without (or, with `keep`, only) the channels matching `select`.
    Remove,
    // `steps[input]` plus the channels of `steps[from]` matching `select`; a copied channel
    // replaces a same-named one.
    Copy,
    // Four channels named `<layer>.R/G/B/A` mapped from `steps[input]` by `mapping` (Shuffle's
    // `channels` output).
    Rename,
};

struct CompiledChannelStep final {
    CompiledChannelStepKind kind = CompiledChannelStepKind::Source;
    document::NodeId nodeId;
    std::uint32_t source = 0;
    std::uint32_t input = 0;
    std::uint32_t from = 0;
    std::string select{};
    bool keep = false;
    std::array<std::string, 4> mapping{};
    std::string layer{};
    friend bool operator==(const CompiledChannelStep&, const CompiledChannelStep&) = default;
};

// Steps are topologically ordered; every step names only earlier steps, and the last is the root.
struct CompiledChannelExpression final {
    std::vector<CompiledChannelSource> sources{};
    std::vector<CompiledChannelStep> steps{};
    friend bool operator==(const CompiledChannelExpression&,
                           const CompiledChannelExpression&) = default;
};

} // namespace bloom::runtime
