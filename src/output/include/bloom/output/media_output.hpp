#pragma once
#include <array>
#include <bloom/media/provider/encode_session.hpp>
#include <bloom/output/display_output.hpp>
#include <bloom/output/output_analysis.hpp>
#include <bloom/render/image.hpp>
#include <filesystem>
#include <functional>
#include <span>
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
// file and probes no device, so a dialog may ask it on the UI thread. Nothing is ever installed:
// - macOS: VideoToolbox inside the AVFoundation provider, then the bundled FFmpeg worker.
// - Linux and Windows: the bundled OpenH264 inside the FFmpeg worker (Linux prefers VA-API
//   hardware when a render node encodes, and falls back to OpenH264 silently).
// `None` is no longer produced; it remains until the desktop removes its references.
enum class H264EncoderV1 : std::uint8_t { None, VideoToolbox, OpenH264 };
struct H264EncoderSupportV1 {
    // The encoder used without hardware.
    H264EncoderV1 software = H264EncoderV1::OpenH264;
    // The provider can encode on VA-API hardware when a render node exists.
    bool hardwareVaapi = false;
    // Always empty: every platform has a software H.264 encoder.
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
// OpenH264 ships inside the FFmpeg worker, so these three always report the bundled encoder as
// installed and touch no file. They only keep the desktop's former install flow compiling until
// the UI removes it; the engine never calls them.
[[nodiscard]] H264RuntimeAvailabilityV1 verifyH264RuntimeV1();
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
// Whether an export of `preset` with codec `profile` may reuse a composite-cache frame
// (premultiplied RGBA half in the working space) instead of evaluating: only outputs whose
// quantization is coarser than half precision's error. H.264, HEVC, ProRes 422 (proxy, LT,
// standard, HQ), 8-bit DNxHR (LB, SQ, HQ) and 8-bit PNG qualify; 16-bit TIFF, float EXR, ProRes
// 4444/4444 XQ, DNxHR HQX/444 and audio-only outputs always render fresh.
[[nodiscard]] bool compositeFrameReuseEligibleV1(OutputPresetV1 preset,
                                                 std::string_view profile) noexcept;
// The codec-boundary half of prepareMediaRgba16V1 for pixels the GPU already transformed to the
// display: `displayAssociated` is the display-referred RGBA32F the export's display/view
// ProcessEffect produced, still associated with the process alpha. Un-associates, clamps and
// quantizes exactly as the CPU reference does after its display transform; no colour math runs
// here.
[[nodiscard]] media::provider::Result<media::provider::FrameProduct>
prepareMediaRgba16FromDisplayV1(std::span<const render::Rgba32f> displayAssociated,
                                std::uint32_t width, std::uint32_t height,
                                media::provider::Rational pts,
                                const platform::ProcessCancellation& cancellation = {});
} // namespace bloom::output
