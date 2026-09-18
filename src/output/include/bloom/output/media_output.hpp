#pragma once
#include <array>
#include <bloom/media/provider/encode_session.hpp>
#include <bloom/output/display_output.hpp>
#include <bloom/output/output_analysis.hpp>
#include <bloom/render/image.hpp>
#include <filesystem>
#include <functional>

namespace bloom::output {
struct MediaOutputFacetV1 {
    OutputFacetIdV1 facet = OutputFacetIdV1::Pixels;
    OutputPreservationStateV1 preservation = OutputPreservationStateV1::Exact;
    std::string description;
};
// The time-based layer binds codec/profile, audio, range and immutable tolerance identity.
// Per-frame process preservation is additionally approved through OutputAnalysisAttemptV1.
struct MediaOutputAnalysisV1 {
    OutputPresetV1 preset;
    media::provider::EncodeSettingsV1 settings;
    std::array<MediaOutputFacetV1, kOutputAnalysisFacetCountV1> facets;
    media::provider::MediaDeterminismV1 determinism;
    core::Sha256Digest toleranceProfile, digest;
    std::string implementationNote;
    std::shared_ptr<const PreparedOutputDisplayV1> display = {};
    bool appleAuthorized = false, deliveryQualified = false;
};
struct H264RuntimeAvailabilityV1 {
    bool installed = false;
    std::string version;
    std::string digest;
    std::filesystem::path directory;
    std::string detail;
};
[[nodiscard]] H264RuntimeAvailabilityV1 verifyH264RuntimeV1();
[[nodiscard]] H264RuntimeAvailabilityV1
installH264RuntimeV1(bool explicitConsent, const std::function<void(std::uint64_t)>& progress = {});
[[nodiscard]] media::provider::Result<MediaOutputAnalysisV1>
analyzeMediaOutputV1(OutputPresetV1 preset, media::provider::EncodeSettingsV1 settings,
                     std::shared_ptr<const PreparedOutputDisplayV1> display = {},
                     std::uint64_t lookEffects = 0);
[[nodiscard]] media::provider::Result<media::provider::FrameProduct>
prepareMediaRgba16V1(const render::Rgba32fImage& image, media::provider::Rational pts,
                     const platform::ProcessCancellation& cancellation = {},
                     const PreparedOutputDisplayV1* display = nullptr);
} // namespace bloom::output
