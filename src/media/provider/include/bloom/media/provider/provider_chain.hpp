#pragma once
#include <bloom/media/provider/protocol.hpp>
#include <cstddef>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// Ordered media provider selection (ADR 0035). Each operating system lists its providers in
// preference order -- the platform media framework first, the bundled FFmpeg worker last -- and an
// operation (decode or encode of one codec) uses the first provider that is installed, declares
// the operation and does not fail it. Skipped and failed providers are recorded as diagnostics and
// provenance; they are never artist-facing text. Codec-free and process-free: the host binds the
// candidates to worker generations.
namespace bloom::media::provider {

enum class MediaDirectionV1 : std::uint8_t { Decode = 1, Encode = 2 };

struct MediaOperationV1 final {
    MediaDirectionV1 direction = MediaDirectionV1::Decode;
    // Provider-neutral codec family ("h264", "hevc", "prores", "dnxhd", "tiff", "pcm", "aac").
    std::string codec;
    // Container, or empty when the operation does not name one (decode).
    std::string container;
};

// The provider-neutral family of one provider's codec spelling: "prores_ks" -> "prores",
// "pcm_s24le" -> "pcm", "dnxhr_hq" -> "dnxhd". Unknown spellings map to themselves.
[[nodiscard]] std::string mediaCodecFamilyV1(std::string_view codec);

// True when the handshake declares the operation: a VideoDecode/AudioDecode capability of the
// codec family for Decode, or a VideoEncode/AudioEncode capability of the family plus a Mux of the
// container for Encode.
[[nodiscard]] bool declaresMediaOperationV1(const Handshake& handshake,
                                            const MediaOperationV1& operation);

enum class MediaFallbackCauseV1 : std::uint8_t {
    NotInstalled = 1, // the provider's worker or platform framework is absent on this computer
    NotDeclared = 2,  // the provider does not declare this codec/container
    Failed = 3,       // the provider refused or failed the operation before producing a product
};

struct MediaProviderFallbackV1 final {
    std::string provider;
    MediaFallbackCauseV1 cause = MediaFallbackCauseV1::NotInstalled;
    Error error = Error::Unavailable;
    std::string detail;
    friend bool operator==(const MediaProviderFallbackV1&,
                           const MediaProviderFallbackV1&) = default;
};

// Which provider served an operation and why the providers before it did not.
struct MediaProviderProvenanceV1 final {
    std::string provider; // empty when no provider served the operation
    std::vector<MediaProviderFallbackV1> fallbacks;
    // What the serving provider delivered differently from the request, e.g. an 8-bit HEVC Main
    // for a Main 10 request when no 10-bit encoder answered; empty when it delivered the request.
    std::string adjustment;
    [[nodiscard]] bool fellBack() const noexcept { return !fallbacks.empty(); }
    // One diagnostic line, e.g. "ffmpeg (after videotoolbox: failed [Crashed] ...)".
    [[nodiscard]] std::string diagnostic() const;
    friend bool operator==(const MediaProviderProvenanceV1&,
                           const MediaProviderProvenanceV1&) = default;
};

struct MediaProviderCandidateV1 final {
    // Stable provenance identity: "media-foundation", "videotoolbox", "ffmpeg-vaapi", "ffmpeg".
    std::string id;
    // A platform media framework rather than the bundled FFmpeg worker.
    bool native = false;
    std::function<bool()> installed;
    std::function<bool(const MediaOperationV1&)> declares;
};

// Errors that end an operation for every provider: the user cancelled, the owner is shutting
// down, or the source itself changed. Every other typed failure lets the next provider try.
[[nodiscard]] bool mediaFallbackPermittedV1(const Unavailable& error) noexcept;

class MediaProviderChainV1 final {
  public:
    MediaProviderChainV1() = default;
    explicit MediaProviderChainV1(std::vector<MediaProviderCandidateV1> candidates);
    [[nodiscard]] const std::vector<MediaProviderCandidateV1>& candidates() const noexcept {
        return candidates_;
    }
    // The first candidate at or after `from` that is installed and declares the operation.
    // Every candidate skipped on the way is appended to `provenance.fallbacks`.
    [[nodiscard]] std::optional<std::size_t> next(const MediaOperationV1& operation,
                                                  std::size_t from,
                                                  MediaProviderProvenanceV1& provenance) const;

    // Runs `attempt` on each eligible provider in order until one succeeds or fails with an error
    // that ends the operation. Each attempt is a complete, independent attempt: it must not have
    // published a product when it fails. Provenance records the serving provider and every
    // fallback. When every provider is exhausted the last provider's error is returned.
    template <typename T>
    [[nodiscard]] Result<T>
    run(const MediaOperationV1& operation,
        const std::function<Result<T>(const MediaProviderCandidateV1&)>& attempt,
        MediaProviderProvenanceV1& provenance) const {
        provenance = {};
        std::optional<Unavailable> last;
        for (auto index = next(operation, 0, provenance); index;
             index = next(operation, *index + 1, provenance)) {
            const auto& candidate = candidates_[*index];
            auto result = attempt(candidate);
            const auto* error = std::get_if<Unavailable>(&result);
            if (error == nullptr) {
                provenance.provider = candidate.id;
                return result;
            }
            if (!mediaFallbackPermittedV1(*error))
                return result;
            provenance.fallbacks.push_back(
                {candidate.id, MediaFallbackCauseV1::Failed, error->reason, error->detail});
            last = *error;
        }
        if (last)
            return *last;
        return Unavailable{Error::Unavailable, "No media provider declares " + describe(operation)};
    }
    [[nodiscard]] static std::string describe(const MediaOperationV1& operation);

  private:
    std::vector<MediaProviderCandidateV1> candidates_;
};

} // namespace bloom::media::provider
