#include <bloom/output/output_analysis_attempt.hpp>

#include <bloom/output/output_analysis_digest.hpp>
#include <bloom/output/output_limits.hpp>
#include <bloom/render/image.hpp>

#include <limits>
#include <utility>

namespace bloom::output {

OutputAnalysisAttemptBuildResultV1 OutputAnalysisAttemptBuildResultV1::success(
    std::shared_ptr<const OutputAnalysisAttemptV1> attempt) noexcept {
    OutputAnalysisAttemptBuildResultV1 result;
    result.attempt_ = std::move(attempt);
    result.error_ = OutputAnalysisAttemptErrorCodeV1::None;
    return result;
}

OutputAnalysisAttemptBuildResultV1
OutputAnalysisAttemptBuildResultV1::failure(const OutputAnalysisAttemptErrorCodeV1 error) noexcept {
    OutputAnalysisAttemptBuildResultV1 result;
    result.error_ = error == OutputAnalysisAttemptErrorCodeV1::None
                        ? OutputAnalysisAttemptErrorCodeV1::InternalInvariant
                        : error;
    return result;
}

OutputAnalysisAttemptV1::OutputAnalysisAttemptV1(
    std::shared_ptr<const runtime::ProcessFrame> frame,
    std::shared_ptr<const ProcessFrameSemanticIdentityV1> processIdentity,
    std::shared_ptr<const OutputAnalysisReportV1> report,
    const std::optional<core::Sha256Digest> digest, const OutputPresetV1 preset,
    OutputAnalysisAttemptTargetV1 target, OutputAnalysisAttemptDisplayProductsV1 display,
    OutputAnalysisAttemptGpuDisplayV1 gpuDisplay,
    std::shared_ptr<ExportResourceReservationV1> reservation) noexcept
    : frame_(std::move(frame)), processIdentity_(std::move(processIdentity)),
      report_(std::move(report)), digest_(digest), preset_(preset), target_(std::move(target)),
      display_(std::move(display)), gpuDisplay_(std::move(gpuDisplay)),
      reservation_(std::move(reservation)) {}

namespace {

// Checked sum of the retained-product bytes the completed attempt charges through the approval
// decision: the retained process-pixel bytes, the report's bounded descriptor storage, and a fixed
// bound for the digest's own bounded streaming preimage (never allocated in full --
// output_analysis_digest.hpp streams it -- but charged conservatively at its closed cap anyway, so
// this reservation is never an under-count of what Analyzing/digesting can touch).
[[nodiscard]] std::optional<std::uint64_t>
checkedAttemptRetainedBytes(const render::Rgba32fImageDescriptor& descriptor,
                            const OutputAnalysisReportV1& report,
                            const OutputAnalysisAttemptDisplayProductsV1& display,
                            const OutputAnalysisAttemptGpuDisplayV1& gpuDisplay) noexcept {
    constexpr auto maximum = std::numeric_limits<std::uint64_t>::max();
    std::uint64_t total = descriptor.layout().pixelStorageBytes;
    const auto descriptorBytes = static_cast<std::uint64_t>(report.descriptorByteCount());
    if (descriptorBytes > maximum - total) {
        return std::nullopt;
    }
    total += descriptorBytes;
    constexpr auto digestPreimageBound =
        static_cast<std::uint64_t>(kOutputAnalysisDigestMaximumPreimageBytesV1);
    if (digestPreimageBound > maximum - total) {
        return std::nullopt;
    }
    total += digestPreimageBound;
    // The PNG display product the completed attempt additionally retains ("including the process
    // frame, semantic-identity product, report, target state, and PNG processor product"). Only
    // the canonical identity record has a byte count Bloom can measure: the qualified OCIO CPU
    // processor behind PreparedCpuDisplayProcessorHandle exposes no allocation size through its
    // public surface, so its own resident bytes are not charged here (a disclosed under-count of
    // that one library-owned allocation, not of anything Bloom allocates -- see the implementor's
    // report).
    if (display.identity != nullptr) {
        const auto identityBytes =
            static_cast<std::uint64_t>(display.identity->canonicalBytes().size());
        if (identityBytes > maximum - total) {
            return std::nullopt;
        }
        total += identityBytes;
    }
    if (report.display()) {
        const auto bytes = report.display()->processor().identity().canonicalBytes().size() +
                           report.display()->description().size();
        if (bytes > maximum - total)
            return std::nullopt;
        total += bytes;
    }
    // The retained GPU-encoded display payload (PNG only), charged at its exact checked RGBA8
    // bytes.
    const auto gpuDisplayBytes =
        static_cast<std::uint64_t>(gpuDisplay.pixels.size()) * sizeof(render::Rgba8);
    if (gpuDisplayBytes > maximum - total) {
        return std::nullopt;
    }
    total += gpuDisplayBytes;
    return total;
}

// The report's own preset decides which display products a completed attempt must retain; nothing
// else may supply or omit them.
[[nodiscard]] bool displayProductsMatchPreset(const OutputAnalysisReportV1& report,
                                              const OutputAnalysisAttemptDisplayProductsV1& display,
                                              const OutputPresetV1 preset) noexcept {
    if (preset != OutputPresetV1::PngRgba8SrgbV1) {
        return display.isAbsent();
    }
    if (report.approvable()) {
        return display.isPresent();
    }
    return display.isPresent() || display.isAbsent();
}

} // namespace

OutputAnalysisAttemptBuildResultV1
buildOutputAnalysisAttemptV1(OutputAnalysisAttemptBuildInputsV1 inputs,
                             ExportResourceLedgerV1& ledger) noexcept {
    if (!inputs.target.isValid()) {
        return OutputAnalysisAttemptBuildResultV1::failure(
            OutputAnalysisAttemptErrorCodeV1::InvalidTarget);
    }
    if (inputs.frame == nullptr || !inputs.frame->processImage().isValid() ||
        inputs.frame->processImage().descriptor() == nullptr) {
        return OutputAnalysisAttemptBuildResultV1::failure(
            OutputAnalysisAttemptErrorCodeV1::InvalidFrame);
    }
    if (inputs.processIdentity == nullptr) {
        return OutputAnalysisAttemptBuildResultV1::failure(
            OutputAnalysisAttemptErrorCodeV1::InvalidIdentity);
    }
    if (inputs.processIdentity->processFrame() != inputs.frame) {
        return OutputAnalysisAttemptBuildResultV1::failure(
            OutputAnalysisAttemptErrorCodeV1::InvalidIdentity);
    }
    if (inputs.report == nullptr) {
        return OutputAnalysisAttemptBuildResultV1::failure(
            OutputAnalysisAttemptErrorCodeV1::InvalidReport);
    }
    if (inputs.report->exr() &&
        !inputs.report->exr()->matches(inputs.frame->identity().colorIntent)) {
        return OutputAnalysisAttemptBuildResultV1::failure(
            OutputAnalysisAttemptErrorCodeV1::InvalidReport);
    }
    if (inputs.report->display()) {
        const auto displayIdentity =
            inputs.report->display()->processor().identity().borrowedView();
        const auto revision = inputs.frame->identity().colorIntent.ocioConfigRevision;
        if (!displayIdentity ||
            displayIdentity->expectedOcioRevision() != (revision == core::Sha256Digest{}
                                                            ? color::kBloomNeutralV1ConfigDigest
                                                            : revision)) {
            return OutputAnalysisAttemptBuildResultV1::failure(
                OutputAnalysisAttemptErrorCodeV1::InvalidReport);
        }
    }

    const auto preset = inputs.report->view().preset;
    if (!displayProductsMatchPreset(*inputs.report, inputs.display, preset)) {
        return OutputAnalysisAttemptBuildResultV1::failure(
            OutputAnalysisAttemptErrorCodeV1::InvalidDisplayProducts);
    }
    // Validate the optional GPU display payload's binding BEFORE it can be retained: PNG only,
    // exact process data-window geometry, the exact retained canonical display identity (same
    // immutable object, never a substitute), the exact process-pixel identity of the frame it was
    // transferred with, a present command identity, and a checked pixel count. A stale or invalid
    // binding is a typed refusal, never a silent use or a faked report.
    if (inputs.gpuDisplay.isPresent()) {
        const auto& gpuDisplay = inputs.gpuDisplay;
        const auto extent = inputs.frame->processImage().descriptor()->dataWindow().extent();
        if (preset != OutputPresetV1::PngRgba8SrgbV1 || inputs.display.identity == nullptr ||
            gpuDisplay.displayIdentity != inputs.display.identity ||
            inputs.processIdentity == nullptr ||
            gpuDisplay.processPixelDigest != inputs.processIdentity->processPixelDigest() ||
            gpuDisplay.width != extent.width() || gpuDisplay.height != extent.height() ||
            gpuDisplay.commandIdentity == core::Sha256Digest{} ||
            gpuDisplay.pixels.size() != static_cast<std::size_t>(extent.width()) *
                                            static_cast<std::size_t>(extent.height())) {
            return OutputAnalysisAttemptBuildResultV1::failure(
                OutputAnalysisAttemptErrorCodeV1::InvalidDisplayProducts);
        }
    }

    // A nonapprovable report is still a successful, completed attempt (frame-output.md: "A
    // nonapprovable but valid report is a successful analysis attempt"); only an approvable report
    // gets a digest at all. The PNG dependencies below are the exact retained products
    // (displayProductsMatchPreset() above already proved they are present for an approvable PNG
    // report and absent for EXR); computeOutputAnalysisDigestV1 itself re-checks that the expected
    // revision equals both the identity's embedded revision and the report's own target
    // external-dependency descriptor.
    std::optional<core::Sha256Digest> digest;
    if (inputs.report->approvable()) {
        const OutputAnalysisDigestDependenciesV1 dependencies{
            .expectedOcioRevision = inputs.display.expectedOcioRevision,
            .displayProcessorIdentity = inputs.display.identity.get()};
        const auto digestResult = computeOutputAnalysisDigestV1(
            *inputs.processIdentity, inputs.report->view(), dependencies);
        if (!digestResult) {
            return OutputAnalysisAttemptBuildResultV1::failure(
                OutputAnalysisAttemptErrorCodeV1::DigestFailed);
        }
        digest = *digestResult.digest();
    }

    const auto* descriptor = inputs.frame->processImage().descriptor();
    const auto retainedBytes =
        checkedAttemptRetainedBytes(*descriptor, *inputs.report, inputs.display, inputs.gpuDisplay);
    if (!retainedBytes.has_value()) {
        return OutputAnalysisAttemptBuildResultV1::failure(
            OutputAnalysisAttemptErrorCodeV1::ResourceReservationFailed);
    }

    ExportResourceAdmissionStatusV1 admissionStatus = ExportResourceAdmissionStatusV1::Reserved;
    auto reservation = ledger.reserve(*retainedBytes, admissionStatus);
    if (reservation == nullptr) {
        return OutputAnalysisAttemptBuildResultV1::failure(
            OutputAnalysisAttemptErrorCodeV1::ResourceReservationFailed);
    }

    try {
        auto attempt = std::shared_ptr<OutputAnalysisAttemptV1>(new OutputAnalysisAttemptV1(
            std::move(inputs.frame), std::move(inputs.processIdentity), std::move(inputs.report),
            digest, preset, std::move(inputs.target), std::move(inputs.display),
            std::move(inputs.gpuDisplay), std::move(reservation)));
        return OutputAnalysisAttemptBuildResultV1::success(std::move(attempt));
    } catch (const std::bad_alloc&) {
        return OutputAnalysisAttemptBuildResultV1::failure(
            OutputAnalysisAttemptErrorCodeV1::ResourceReservationFailed);
    }
}

OutputAnalysisAttemptBuildResultV1
prepareFlatExrAttemptV1(const OutputAnalysisAttemptV1& source, FlatExrRgba32fOptionsV1 options,
                        ExportResourceLedgerV1& ledger) noexcept {
    try {
        if (source.preset() != OutputPresetV1::FlatExrRgba32fLinRec709SceneV1 || !source.frame())
            return OutputAnalysisAttemptBuildResultV1::failure(
                OutputAnalysisAttemptErrorCodeV1::InvalidReport);
        const auto prepared = PreparedFlatExrOutputV1::prepare(
            source.frame()->identity().colorIntent, std::move(options));
        if (!prepared)
            return OutputAnalysisAttemptBuildResultV1::failure(
                OutputAnalysisAttemptErrorCodeV1::InvalidReport);
        const auto report = analyzeFlatExrRgba32fLinRec709SceneV1(
            {.process = {.readyIdentity = source.processIdentity(), .missingDescriptor = {}},
             .exr = prepared});
        if (!report)
            return OutputAnalysisAttemptBuildResultV1::failure(
                OutputAnalysisAttemptErrorCodeV1::InvalidReport);
        return buildOutputAnalysisAttemptV1({.frame = source.frame(),
                                             .processIdentity = source.processIdentity(),
                                             .report = report.report(),
                                             .target = source.target(),
                                             .display = {},
                                             .gpuDisplay = {}},
                                            ledger);
    } catch (...) {
        return OutputAnalysisAttemptBuildResultV1::failure(
            OutputAnalysisAttemptErrorCodeV1::InternalInvariant);
    }
}

} // namespace bloom::output
