#pragma once

// #364 Cryptomatte, following the Cryptomatte specification (Psyop, v1.2) as Nuke reads it:
//  - an object name's ID is MurmurHash3_x86_32(name, seed 0) whose bits are used as a float32;
//    when the exponent bits are 0 or 255 bit 23 is flipped so every ID is a finite normal float;
//  - a layer type `<name>` is stored as rank layers `<name>00`, `<name>01`, ..., each holding two
//    (ID, coverage) rank pairs in R/G and B/A;
//  - the part metadata `cryptomatte/<key>/name` names the type and `cryptomatte/<key>/manifest`
//    maps object names to their hex IDs.
// A matte's coverage is the sum, in rank order, of every rank's coverage whose ID bits equal a
// selected ID; it is clamped to [0, 1] after each rank. The CPU reference and the GPU kernel apply
// exactly that order in Float32, so they are bit-identical.

#include "channel_resolution.hpp"
#include "image_source.hpp"
#include <bloom/runtime/cryptomatte_matte_list.hpp>

#include <bloom/render/channel_set_image.hpp>
#include <bloom/render/image.hpp>

#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace bloom::runtime::detail {

using runtime::cryptomatteIdColour;
using runtime::cryptomatteNameId;
using runtime::formatCryptomatteRawId;
using runtime::joinCryptomatteMatteList;
using runtime::parseCryptomatteRawId;
using runtime::splitCryptomatteMatteList;

struct CryptomatteManifest final {
    // Manifest entries (name, ID) in file order; empty when the part has no manifest.
    std::vector<std::pair<std::string, std::uint32_t>> entries;
    bool present = false;
    // The verbatim manifest text, part of the cache identity.
    std::string text;
    std::string diagnostic;
};

// The manifest of the Cryptomatte type `layer` in a part's string metadata.
[[nodiscard]] CryptomatteManifest cryptomatteManifest(const render::ChannelMetadata& metadata,
                                                      std::string_view layer);
// The name a manifest gives `id`, if any.
[[nodiscard]] std::optional<std::string>
cryptomatteManifestName(const CryptomatteManifest& manifest, std::uint32_t id);

// Each matte entry's ID: a raw ID, a manifest name, or (absent from the manifest) the hashed name.
[[nodiscard]] std::vector<std::uint32_t> resolveCryptomatteIds(std::span<const std::string> mattes,
                                                               const CryptomatteManifest& manifest);

// One (ID, coverage) rank pair, by resolved channel name.
struct CryptomatteRank final {
    std::string id;
    std::string coverage;
};
// The rank pairs of type `layer` among `channels`, in rank order (00 R/G, 00 B/A, 01 R/G, ...).
// R/G/B/A, r/g/b/a and red/green/blue/alpha suffixes are accepted.
[[nodiscard]] std::vector<CryptomatteRank>
cryptomatteRanks(std::span<const ResolvedChannel> channels, std::string_view layer);
// The Cryptomatte type names present among `channels` (layers `<name>NN` with rank pairs).
[[nodiscard]] std::vector<std::string> cryptomatteLayers(std::span<const ResolvedChannel> channels);

// Coverage of one rank-pair image (R = ID bits, G = coverage) accumulated onto `previous`.
[[nodiscard]] float accumulateCryptomatteCoverage(float previous, float idBits, float coverage,
                                                  std::span<const std::uint32_t> ids) noexcept;

} // namespace bloom::runtime::detail

#include "channel_shuffle.hpp"

namespace bloom::runtime::detail {

struct CryptomatteSelection final {
    ChannelSetSourceSelection channels;
    std::vector<CryptomatteRank> ranks;
    // The resolved (ID, coverage) channels of each rank, two per rank.
    std::vector<ResolvedChannel> rankChannels;
    std::uint32_t anchor = 0;
    std::vector<std::string> mattes;
    std::string layer;
    std::string cacheKey;
    // Raw-gather identity (sources and rank channels, no selection): the GPU upload cache key.
    std::string gatherKey;
    std::string warning;
    bool available = false;
    bool cancelled = false;
    bool budgetExceeded = false;
    bool preview = false;
};

[[nodiscard]] CryptomatteSelection
selectCryptomatte(const CompiledCryptomatte& cryptomatte, core::RationalTime time,
                  document::FrameRate rate, const std::filesystem::path& base,
                  const CancellationToken& cancel, const EvaluationColorIntent& colorIntent,
                  OperationCache* revisionCache, MediaSourceRevisionPolicy revisionPolicy,
                  std::size_t hostByteBudget, bool allowRevisionInsertion,
                  LinearPassRetention* linearPass = nullptr);

// The host decode layout of every rank pair: one image per rank on the union window of the rank
// channels in the anchor source's frame, R = the ID's exact bits, G = coverage, B = 0, A = 1, with
// zero outside a channel's own window. A non-finite ID or coverage is repaired to 0 and counted.
struct CryptomatteGather final {
    std::vector<std::shared_ptr<const render::Rgba32fImage>> ranks;
    std::vector<std::uint32_t> ids;
    CryptomatteManifest manifest;
    std::uint64_t repairedPixels = 0;
    std::string diagnostic;
    bool cancelled = false;
    media::ImageDiagnosticCode code = media::ImageDiagnosticCode::None;
};
[[nodiscard]] CryptomatteGather gatherCryptomatte(const CryptomatteSelection& selection,
                                                  std::size_t budget,
                                                  const CancellationToken& cancel);

// GPU host preparation: the gathered ranks stacked vertically into ONE upload image (rank r
// occupies rows [origin + r * height, origin + (r + 1) * height)) on the gather's data window
// origin and display window, exactly the CryptomatteCoverageV1 source layout. Bits are copied
// verbatim.
[[nodiscard]] media::ImageResult<render::Rgba32fImage>
stackCryptomatteRanks(const CryptomatteGather& gather, std::size_t budget,
                      const CancellationToken& cancel);

// CPU reference: the full-resolution matte (premultiplied white x coverage) from the gather.
[[nodiscard]] media::ImageResult<render::Rgba32fImage>
cryptomatteCoverageImage(const CryptomatteGather& gather, std::size_t budget,
                         const CancellationToken& cancel);
// CPU reference of the Viewer's ID-colour preview (CryptomatteCoverageV1 mode 1): per rank, in
// rank order, colour += tint x coverage with tint = cryptomatteIdColour(id) (0.5 * tint + 0.5 for a
// selected ID) and alpha = clamp(alpha + coverage, 0, 1); colour is clamped to alpha.
[[nodiscard]] media::ImageResult<render::Rgba32fImage>
cryptomattePreviewImage(const CryptomatteGather& gather, std::size_t budget,
                        const CancellationToken& cancel);
// CPU reference resampled to the request geometry (PointResampleV1).
[[nodiscard]] media::ImageResult<render::Rgba32fImage>
evaluateCryptomatte(const CryptomatteSelection& selection,
                    render::Rgba32fImageDescriptor composition, double horizontalScale,
                    double verticalScale, std::size_t budget, const CancellationToken& cancel);

[[nodiscard]] std::optional<ContentBounds>
cryptomatteFrameBounds(const CryptomatteSelection& selection) noexcept;

} // namespace bloom::runtime::detail
