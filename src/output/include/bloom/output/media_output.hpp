#pragma once
#include <array>
#include <bloom/media/provider/encode_session.hpp>
#include <bloom/output/display_output.hpp>
#include <bloom/output/output_analysis.hpp>
#include <bloom/render/image.hpp>
#include <filesystem>
#include <functional>
#include <string_view>

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
// Which H.264 encoder this platform's export provider uses. A pure platform answer: it opens no
// file and probes no device, so a dialog may ask it on the UI thread.
// - macOS: VideoToolbox, inside the AVFoundation provider; nothing to install or consent to.
// - Linux x86-64: Cisco OpenH264 (installed after consent, or located), or VA-API hardware.
// - Linux on other processors: VA-API hardware only; Cisco publishes no binary there.
// - Windows: none yet (the supervised FFmpeg worker is not available there).
enum class H264EncoderV1 : std::uint8_t { None, VideoToolbox, OpenH264 };
struct H264EncoderSupportV1 {
    // The encoder used without hardware.
    H264EncoderV1 software = H264EncoderV1::None;
    // The provider can encode on VA-API hardware when a render node exists.
    bool hardwareVaapi = false;
    // Why software is None.
    std::string_view softwareReason;
};
[[nodiscard]] H264EncoderSupportV1 h264EncoderSupportV1() noexcept;
// Whether a VA-API render node (/dev/dri/renderD*) exists. A filesystem probe: call it on a
// worker, never on the UI thread.
[[nodiscard]] bool vaapiRenderNodeAvailableV1();
// VA-API render-node discovery (#369): the render node whose driver opens the H.264 encode entry
// point (preferring one that also opens HEVC), probed through the supervised media worker so no
// driver code loads into the application. Blocking: worker threads only. Empty on other
// platforms or when no node encodes.
struct VaapiEncodeProbeV1 {
    std::string renderNode;
    bool h264 = false;
    bool hevc = false;
};
[[nodiscard]] VaapiEncodeProbeV1 probeVaapiEncodeV1(std::string workerExecutable = {});
// Whether the supervised media worker executable exists for this build. Worker threads only.
[[nodiscard]] bool mediaWorkerAvailableV1();
// Verifies the installed OpenH264 binary (a SHA-256 of the library): worker only.
[[nodiscard]] H264RuntimeAvailabilityV1 verifyH264RuntimeV1();
// Verifies and installs a downloaded OpenH264 binary (hash and copy): worker only, cancellable.
[[nodiscard]] H264RuntimeAvailabilityV1
locateH264RuntimeV1(const std::filesystem::path& file, const std::function<bool()>& cancelled = {});
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
