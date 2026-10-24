#pragma once

// #364 Cryptomatte: the worker-side reads behind the Properties layer picker and the Viewer
// eyedropper. Both compile the node's matte preview (a viewer-only plan), capture and probe its
// sources identity-verified like evaluation, and never touch project state.

#include <bloom/core/rational_time.hpp>
#include <bloom/document/document.hpp>
#include <bloom/document/ids.hpp>
#include <bloom/runtime/cancellation.hpp>
#include <bloom/runtime/channel_inspection.hpp>
#include <bloom/runtime/evaluation.hpp>

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace bloom::runtime {

class SnapshotCompiler;

struct CryptomatteLayerListing final {
    // The Cryptomatte types (`CryptoObject`, `CryptoMaterial`, ...) of the node's channel set.
    std::vector<std::string> layers;
    // Why the channel set could not be listed (empty on success).
    std::string diagnostic;
};

[[nodiscard]] CryptomatteLayerListing
listCryptomatteLayers(const SnapshotCompiler& compiler, const document::Snapshot& snapshot,
                      document::CompositionId composition, document::NodeId node,
                      const std::filesystem::path& assetBase, core::RationalTime time,
                      const EvaluationColorIntent& colorIntent,
                      const CancellationToken& cancellation);

struct CryptomattePick final {
    // The matte under the pixel, as the matte list stores it: the manifest name, or the raw ID
    // (`<1.23456e-05>`) when the manifest does not name it. Empty when no object covers the pixel.
    std::optional<std::string> matte;
    std::optional<std::uint32_t> id;
    float coverage = 0.0F;
    std::string diagnostic;
};

// The object with the largest coverage at pixel (`x`, `y`) of the node's selected layer, at the
// composition time `time` mapped through the layer time of the plan the Viewer shows (the
// composition, or the `inspection` it displays): a retimed or offset layer reads its own source
// frame, and absent content reads nothing. The coordinates are pixels of the channel set's frame,
// relative to its display window origin; the caller maps the Viewer point there through the
// layer's world transform.
[[nodiscard]] CryptomattePick
pickCryptomatte(const SnapshotCompiler& compiler, const document::Snapshot& snapshot,
                document::CompositionId composition, document::NodeId node,
                const std::filesystem::path& assetBase, core::RationalTime time,
                const EvaluationColorIntent& colorIntent,
                const std::optional<ChannelInspection>& inspection, std::int64_t x, std::int64_t y,
                const CancellationToken& cancellation);

} // namespace bloom::runtime
