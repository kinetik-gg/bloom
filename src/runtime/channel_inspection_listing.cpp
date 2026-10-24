#include <bloom/runtime/channel_inspection_listing.hpp>

#include "channel_resolution.hpp"
#include "image_source.hpp"

#include <bloom/runtime/snapshot_compiler.hpp>

#include <algorithm>
#include <array>
#include <string_view>

namespace bloom::runtime {
namespace {

// A layer whose suffixes cover one of these triples is offered as one RGB choice.
constexpr std::array<std::array<std::string_view, 3>, 4> kTriples{{{"R", "G", "B"},
                                                                   {"X", "Y", "Z"},
                                                                   {"r", "g", "b"},
                                                                   {"x", "y", "z"}}};

} // namespace

ChannelInspectionListing
listChannelInspectionChoices(const SnapshotCompiler& compiler, const document::Snapshot& snapshot,
                             const document::CompositionId composition, const document::NodeId node,
                             const std::string& port, const std::filesystem::path& assetBase,
                             const core::RationalTime time,
                             const EvaluationColorIntent& colorIntent,
                             const CancellationToken& cancellation) {
    ChannelInspectionListing listing;
    const auto compiled = compiler.compile(
        {.snapshot = snapshot,
         .compositionId = composition,
         .parameterOverrides = {},
         .channelInspection = ChannelInspection{node, port, {"0"}}},
        cancellation);
    if (!compiled.plan) {
        listing.diagnostic = compiled.diagnostics.empty() ? "The channel set could not be compiled"
                                                          : compiled.diagnostics.front().summary;
        return listing;
    }
    const CompiledChannelShuffle* shuffle = nullptr;
    for (const auto& operation : compiled.plan->operations())
        if (const auto* candidate = std::get_if<CompiledChannelShuffle>(&operation);
            candidate != nullptr && candidate->sourceNodeId == node)
            shuffle = candidate;
    if (shuffle == nullptr) {
        listing.diagnostic = "The node has no channel set";
        return listing;
    }
    std::vector<detail::ImageSourceSelection> selections;
    std::vector<const media::ImagePartInfo*> parts;
    for (const auto& source : shuffle->channels.sources) {
        const CompiledImageSource image{source.sourceNodeId, source.asset,      source.startFrame,
                                        source.loopMode,     source.colorSpace, source.inputColorSpaceId,
                                        false,               source.part,       {}};
        auto selected = detail::selectImageSource(image, time, compiled.plan->format().frameRate(),
                                                  assetBase, cancellation, colorIntent);
        if (selected.cancelled) {
            listing.diagnostic = "Cancelled";
            return listing;
        }
        const auto* part = selected.available && selected.sourceRevision
                               ? selected.sourceRevision->probe().findPart(source.part)
                               : nullptr;
        if (part == nullptr && listing.diagnostic.empty())
            listing.diagnostic = selected.warning.empty() ? "A channel source is unavailable"
                                                          : selected.warning;
        parts.push_back(part);
        selections.push_back(std::move(selected));
    }
    const auto resolution = detail::resolveChannelExpression(shuffle->channels, parts);
    if (!resolution.ok()) {
        if (listing.diagnostic.empty())
            listing.diagnostic = resolution.error;
        return listing;
    }
    listing.diagnostic.clear();
    std::vector<std::string> layers;
    for (const auto& channel : resolution.channels)
        if (std::ranges::find(layers, channel.descriptor.layer) == layers.end())
            layers.push_back(channel.descriptor.layer);
    const auto has = [&](const std::string& name) {
        return std::ranges::any_of(resolution.channels,
                                   [&](const auto& channel) { return channel.name == name; });
    };
    for (const auto& layer : layers) {
        const auto prefix = layer.empty() ? std::string{} : layer + ".";
        for (const auto& triple : kTriples) {
            std::vector<std::string> names;
            for (const auto suffix : triple)
                names.push_back(prefix + std::string(suffix));
            if (std::ranges::all_of(names, has)) {
                listing.choices.push_back(
                    {(layer.empty() ? std::string("RGB") : layer) + " (RGB)", std::move(names)});
                break;
            }
        }
    }
    for (const auto& channel : resolution.channels)
        listing.choices.push_back({channel.name, {channel.name}});
    return listing;
}

} // namespace bloom::runtime
