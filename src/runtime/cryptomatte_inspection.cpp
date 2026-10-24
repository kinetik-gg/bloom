#include <bloom/runtime/cryptomatte_inspection.hpp>

#include "cryptomatte.hpp"

#include <bloom/document/channel_nodes.hpp>
#include <bloom/runtime/channel_inspection.hpp>
#include <bloom/runtime/snapshot_compiler.hpp>

#include <bit>
#include <limits>
#include <variant>

namespace bloom::runtime {
namespace {

struct CompiledNode final {
    std::shared_ptr<const CompiledCompositionPlan> plan;
    const CompiledCryptomatte* cryptomatte = nullptr;
    std::string diagnostic;
};

// The node's matte preview plan: it lowers exactly the node's CompiledCryptomatte.
[[nodiscard]] CompiledNode compileNode(const SnapshotCompiler& compiler,
                                       const document::Snapshot& snapshot,
                                       const document::CompositionId composition,
                                       const document::NodeId node,
                                       const CancellationToken& cancellation) {
    CompiledNode result;
    const auto compiled = compiler.compile(
        {.snapshot = snapshot,
         .compositionId = composition,
         .parameterOverrides = {},
         .channelInspection =
             ChannelInspection{node, std::string(document::kCryptomatteMaskOutputPort), {}}},
        cancellation);
    if (!compiled.plan) {
        result.diagnostic = compiled.diagnostics.empty() ? "The Cryptomatte could not be compiled"
                                                         : compiled.diagnostics.front().summary;
        return result;
    }
    result.plan = compiled.plan;
    for (const auto& operation : result.plan->operations())
        if (const auto* candidate = std::get_if<CompiledCryptomatte>(&operation);
            candidate != nullptr && candidate->sourceNodeId == node)
            result.cryptomatte = candidate;
    if (result.cryptomatte == nullptr)
        result.diagnostic = "The node is not an active Cryptomatte";
    return result;
}

} // namespace

CryptomatteLayerListing
listCryptomatteLayers(const SnapshotCompiler& compiler, const document::Snapshot& snapshot,
                      const document::CompositionId composition, const document::NodeId node,
                      const std::filesystem::path& assetBase, const core::RationalTime time,
                      const EvaluationColorIntent& colorIntent,
                      const CancellationToken& cancellation) {
    CryptomatteLayerListing listing;
    const auto compiled = compileNode(compiler, snapshot, composition, node, cancellation);
    if (compiled.cryptomatte == nullptr) {
        listing.diagnostic = compiled.diagnostic;
        return listing;
    }
    const auto sources = detail::selectChannelSetSources(
        compiled.cryptomatte->channels, time, compiled.plan->format().frameRate(), assetBase,
        cancellation, colorIntent, nullptr, MediaSourceRevisionPolicy::Revalidate,
        std::numeric_limits<std::size_t>::max(), false);
    if (!sources.available()) {
        listing.diagnostic = sources.cancelled ? "Cancelled" : sources.warning;
        return listing;
    }
    listing.layers = detail::cryptomatteLayers(sources.resolution.channels);
    return listing;
}

CryptomattePick pickCryptomatte(const SnapshotCompiler& compiler,
                                const document::Snapshot& snapshot,
                                const document::CompositionId composition,
                                const document::NodeId node,
                                const std::filesystem::path& assetBase,
                                const core::RationalTime time,
                                const EvaluationColorIntent& colorIntent, const std::int64_t x,
                                const std::int64_t y, const CancellationToken& cancellation) {
    CryptomattePick pick;
    const auto compiled = compileNode(compiler, snapshot, composition, node, cancellation);
    if (compiled.cryptomatte == nullptr) {
        pick.diagnostic = compiled.diagnostic;
        return pick;
    }
    const auto selection = detail::selectCryptomatte(
        *compiled.cryptomatte, time, compiled.plan->format().frameRate(), assetBase, cancellation,
        colorIntent, nullptr, MediaSourceRevisionPolicy::Revalidate,
        std::numeric_limits<std::size_t>::max(), false);
    if (!selection.available) {
        pick.diagnostic = selection.cancelled ? "Cancelled" : selection.warning;
        return pick;
    }
    const auto gather = detail::gatherCryptomatte(
        selection, std::numeric_limits<std::size_t>::max(), cancellation);
    if (gather.ranks.empty()) {
        pick.diagnostic = gather.cancelled ? "Cancelled" : gather.diagnostic;
        return pick;
    }
    const auto* descriptor = gather.ranks.front()->descriptor();
    const auto display = descriptor->displayWindow();
    const auto sampleX = display.originX() + x;
    const auto sampleY = display.originY() + y;
    if (!descriptor->dataWindow().contains(sampleX, sampleY))
        return pick;
    for (const auto& rank : gather.ranks) {
        const auto pixel = rank->read(sampleX, sampleY);
        if (!pixel)
            continue;
        const auto coverage = pixel.value()->green();
        if (coverage > pick.coverage) {
            pick.coverage = coverage;
            pick.id = std::bit_cast<std::uint32_t>(pixel.value()->red());
        }
    }
    if (!pick.id)
        return pick;
    if (auto name = detail::cryptomatteManifestName(gather.manifest, *pick.id))
        pick.matte = std::move(*name);
    else
        pick.matte = formatCryptomatteRawId(*pick.id);
    return pick;
}

} // namespace bloom::runtime
