#pragma once

// ADR 0026 Shuffle: the CPU reference and the GPU host preparation share this selection and gather.
// Selection resolves every expression source like an Image source (identity-verified revision,
// input colour space), resolves the channel expression and the four slots against the probed
// parts, and builds the cache identities. The gather reads the mapped source channels into one
// RGBA image in the anchor source's frame -- a decode-layout step: every slot is a source sample, a
// constant, or zero outside its source's data window -- and applies the process-image contract.

#include "channel_resolution.hpp"
#include "image_source.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace bloom::runtime::detail {

struct ChannelShuffleSelection final {
    std::vector<ImageSourceSelection> sources;
    std::array<ResolvedChannel, 4> mapping{};
    // The source whose frame the output uses: the first slot that reads a source channel.
    std::uint32_t anchor = 0;
    // The source whose input colour space a Colour mapping converts from (the first RGB slot that
    // reads a source channel); nullopt for Raw or an all-constant RGB triple.
    std::optional<std::uint32_t> colourSource;
    bool associated = true;
    std::string cacheKey;
    // Raw-gather identity (no colour interpretation): the GPU upload cache key.
    std::string decodeKey;
    std::string warning;
    bool available = false;
    bool cancelled = false;
    bool budgetExceeded = false;
    // Whether the CPU reference converts the RGB triple (a Colour mapping whose source needs an
    // input processor). The GPU path converts with the OCIO program instead.
    [[nodiscard]] bool converts() const noexcept {
        return colourSource.has_value() && sources[*colourSource].inputProcessor != nullptr;
    }
};

// The expression's sources selected like Image sources (identity-verified revisions, input colour
// resolution) and the expression resolved against their probed parts. Shared by Shuffle and
// Cryptomatte.
struct ChannelSetSourceSelection final {
    std::vector<ImageSourceSelection> sources;
    std::vector<const media::ImagePartInfo*> parts;
    ChannelResolution resolution;
    std::string warning;
    bool cancelled = false;
    bool budgetExceeded = false;
    [[nodiscard]] bool available() const noexcept { return warning.empty() && !cancelled; }
};
[[nodiscard]] ChannelSetSourceSelection
selectChannelSetSources(const CompiledChannelExpression& expression, core::RationalTime time,
                        document::FrameRate rate, const std::filesystem::path& base,
                        const CancellationToken& cancel, const EvaluationColorIntent& colorIntent,
                        OperationCache* revisionCache, MediaSourceRevisionPolicy revisionPolicy,
                        std::size_t hostByteBudget, bool allowRevisionInsertion,
                        LinearPassRetention* linearPass = nullptr);

[[nodiscard]] ChannelShuffleSelection
selectChannelShuffle(const CompiledChannelShuffle& shuffle, core::RationalTime time,
                     document::FrameRate rate, const std::filesystem::path& base,
                     const CancellationToken& cancel, const EvaluationColorIntent& colorIntent,
                     OperationCache* revisionCache, MediaSourceRevisionPolicy revisionPolicy,
                     std::size_t hostByteBudget, bool allowRevisionInsertion,
                     LinearPassRetention* linearPass = nullptr);

// The full-resolution gathered image in the anchor source's frame. With `convert` the RGB triple is
// converted by the colour source's input processor (the CPU reference); without it the image stays
// in the source encoding (the GPU raw upload). `repairedPixels` counts the ingest-policy repairs.
[[nodiscard]] media::ImageResult<std::shared_ptr<const render::Rgba32fImage>>
gatherChannelShuffle(const ChannelShuffleSelection& selection, bool convert, std::size_t budget,
                     const CancellationToken& cancel);

// CPU reference: the converted gather resampled to the request geometry (PointResampleV1).
[[nodiscard]] media::ImageResult<render::Rgba32fImage>
evaluateChannelShuffle(const ChannelShuffleSelection& selection,
                       render::Rgba32fImageDescriptor composition, double horizontalScale,
                       double verticalScale, std::size_t budget, const CancellationToken& cancel);

// The anchor source's frame bounds (see imageSourceFrameBounds).
[[nodiscard]] std::optional<ContentBounds>
channelShuffleFrameBounds(const ChannelShuffleSelection& selection) noexcept;

} // namespace bloom::runtime::detail
