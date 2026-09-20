#include <bloom/runtime/gpu_resident_preview_product.hpp>

#include <bloom/render/display_buffer.hpp>
#include <bloom/render/gpu_device.hpp>
#include <bloom/render/image.hpp>
#include <bloom/runtime/compiled_plan.hpp>
#include <bloom/runtime/evaluation.hpp>
#include <bloom/runtime/gpu_neutral_display_qualification.hpp>
#include <bloom/runtime/prepared_gpu_scene.hpp>
#include <bloom/runtime/preview_cpu_stage.hpp>

#include <cstddef>
#include <cstdint>
#include <exception>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace bloom::runtime {

namespace {

// The report interval and the fixed neutral request window are the same authority used by the
// canonical qualification; the request must reproduce the pinned neutral display/view names.
[[nodiscard]] bool neutralNamesAreAcceptable(const PreviewRequestIdentity& identity) noexcept {
    return (identity.displayName.empty() ||
            identity.displayName == kGpuNeutralDisplayDisplayName) &&
           (identity.viewName.empty() || identity.viewName == kGpuNeutralDisplayViewName);
}

// Resolve the request's EvaluationResolution to the display window it implies, using the owning
// render helper. A CompositionFormatResolution means the full composition format; a ProxyResolution
// means exactly its resolved extent. This is the ONLY place the full format is used, and only when
// the request actually asked for it -- a reduced resolution is never silently forced back to the
// full format.
[[nodiscard]] std::optional<render::ImageWindow>
resolvedDisplayWindow(const document::CompositionFormat& format,
                      const EvaluationResolution& resolution) noexcept {
    if (std::holds_alternative<CompositionFormatResolution>(resolution)) {
        const auto result = render::ImageWindow::create(0, 0, format.width(), format.height());
        return result.hasValue() ? std::optional<render::ImageWindow>(*result.value())
                                 : std::nullopt;
    }
    const auto* proxy = std::get_if<ProxyResolution>(&resolution);
    if (proxy == nullptr) {
        return std::nullopt;
    }
    const auto result =
        render::ImageWindow::create(0, 0, proxy->extent.width(), proxy->extent.height());
    return result.hasValue() ? std::optional<render::ImageWindow>(*result.value()) : std::nullopt;
}

} // namespace

GpuResidentDisplayProductRequest
makeGpuResidentDisplayProductRequest(const PreviewCpuStage& stage,
                                     std::shared_ptr<const render::GpuDisplayImage> display) {
    GpuResidentDisplayProductRequest request;
    request.identity = stage.desiredIdentity();
    request.display = std::move(display);
    request.pixelStorageByteLimit = stage.pixelStorageByteLimit();
    if (const auto& processFrame = stage.processFrame(); processFrame != nullptr) {
        request.processIdentity = processFrame->identity();
        // The stage evaluated on the CPU; the process origin is recorded honestly as such.
        request.processIdentity.provider = EvaluationProvider::CpuReference;
        request.bounds.assign(processFrame->evaluatedBounds().begin(),
                              processFrame->evaluatedBounds().end());
        // Trusted expected geometry: the ACTUAL process image descriptor the CPU evaluated.
        if (const auto* descriptor = processFrame->processImage().descriptor();
            descriptor != nullptr) {
            request.expectedDescriptor = *descriptor;
        }
    }
    return request;
}

GpuResidentDisplayProductRequest
makeGpuResidentDisplayProductRequest(const PreparedGpuScene& scene, PreviewRequestIdentity identity,
                                     std::shared_ptr<const render::GpuDisplayImage> display) {
    GpuResidentDisplayProductRequest request;
    request.identity = std::move(identity);
    request.processIdentity = scene.processIdentity();
    // A prepared GPU scene is the GPU-evaluated process origin; it is never stamped CpuReference.
    request.processIdentity.provider = EvaluationProvider::GpuResident;
    request.bounds = scene.bounds();
    // Trusted expected geometry: the ACTUAL immutable prepared-scene output descriptor.
    request.expectedDescriptor = scene.outputDescriptor();
    request.display = std::move(display);
    request.pixelStorageByteLimit = 0;
    return request;
}

bool gpuResidentDisplayProductIsEligible(render::GpuDevice& device,
                                         const GpuResidentFrameLeaseRegistry& registry,
                                         const color::PreparedCpuDisplayProcessorHandle& processor,
                                         const GpuResidentPreviewQualificationReport& report,
                                         const GpuResidentDisplayProductRequest& request,
                                         std::string& reason) noexcept {
    try {
        if (!report.eligible()) {
            reason = "the resident preview qualification report is not eligible";
            return false;
        }
        if (!report.eligibleFor(device, processor)) {
            reason = "the report does not qualify this exact device and processor";
            return false;
        }
        if (!registry.isBoundTo(device)) {
            reason = "the lease registry is not the one bound to this device";
            return false;
        }
        if (!device.isOwnerThread()) {
            reason = "the resident display product must run on the device owner thread";
            return false;
        }
        if (!gpuResidentPreviewProcessorIsEligible(processor, reason)) {
            return false;
        }

        const auto& identity = request.identity;
        if (identity.requestGeneration == 0) {
            reason = "the request generation is zero";
            return false;
        }
        if (identity.output != PreviewOutput::Composition) {
            reason = "only the composition preview output is supported by the resident operation";
            return false;
        }
        if (!(identity.viewAdjust == ViewAdjust{})) {
            reason = "a non-neutral view adjustment is not supported by the resident operation";
            return false;
        }
        if (!neutralNamesAreAcceptable(identity)) {
            reason =
                "a non-default display or view name is not supported by the resident operation";
            return false;
        }
        const auto& intent = identity.colorIntent;
        if (intent.workingColorSpaceId != kLinearRec709SceneColorSpaceId) {
            reason = "the working color space is not lin_rec709_scene";
            return false;
        }
        if (!(intent.ocioConfigRevision == core::Sha256Digest{} ||
              intent.ocioConfigRevision == color::kBloomNeutralV1ConfigDigest)) {
            reason =
                "the color intent revision is neither the compatibility zero nor Bloom Neutral";
            return false;
        }
        if (!(intent.ocioConfigUri.empty() || intent.ocioConfigUri == kBloomNeutralOcioConfigUri)) {
            reason = "the color intent names a foreign config URI";
            return false;
        }

        const auto& processIdentity = request.processIdentity;
        const auto& plan = processIdentity.plan;
        if (plan == nullptr) {
            reason = "the process identity has no compiled plan";
            return false;
        }
        if (identity.projectId != plan->projectId() ||
            identity.compositionId != plan->compositionId() ||
            identity.sourceRevision != plan->sourceRevision() ||
            processIdentity.output != plan->output()) {
            reason = "the request identity does not match the process frame's plan";
            return false;
        }
        if (identity.time != processIdentity.time ||
            identity.resolution != processIdentity.resolution ||
            identity.quality != processIdentity.quality ||
            identity.colorIntent != processIdentity.colorIntent ||
            identity.roi != processIdentity.roi ||
            identity.showLook == processIdentity.bypassLookNodes) {
            reason = "the request identity does not match the process frame identity";
            return false;
        }

        const auto* display = request.display.get();
        if (display == nullptr || !display->isValid()) {
            reason = "the resident display image is null or invalid";
            return false;
        }
        if (!display->isBoundTo(device)) {
            reason = "the resident display image is not bound to this device";
            return false;
        }

        // The trusted expected geometry comes from the ACTUAL scene/process descriptor the helper
        // carried, never from the full composition format unconditionally and never from the
        // returned image. Validate it against the request's resolved resolution and ROI first.
        if (!request.expectedDescriptor.has_value()) {
            reason = "the request carries no trusted expected process descriptor";
            return false;
        }
        const auto& expected = *request.expectedDescriptor;
        const auto resolvedWindow =
            resolvedDisplayWindow(plan->format(), processIdentity.resolution);
        if (!resolvedWindow.has_value()) {
            reason = "the request resolution has no representable display window";
            return false;
        }
        if (!(expected.displayWindow() == *resolvedWindow)) {
            reason = "the trusted process display window does not match the resolved resolution";
            return false;
        }
        const auto expectedDataWindow = identity.roi.has_value() ? *identity.roi : *resolvedWindow;
        if (!(expected.dataWindow() == expectedDataWindow)) {
            reason = "the trusted process data window does not match the request ROI";
            return false;
        }
        if (identity.roi.has_value()) {
            const auto& roi = *identity.roi;
            if (roi.originX() < 0 || roi.originY() < 0 ||
                roi.maxXExclusive() > resolvedWindow->maxXExclusive() ||
                roi.maxYExclusive() > resolvedWindow->maxYExclusive()) {
                reason = "the request ROI is outside the resolved resolution window";
                return false;
            }
        }

        // The ACTUAL native display image must match that trusted descriptor exactly.
        const auto dataExtent = expected.dataWindow().extent();
        if (display->width() != dataExtent.width() || display->height() != dataExtent.height()) {
            reason = "the resident display dimensions do not match the trusted process descriptor";
            return false;
        }
        if (!(display->dataWindow() == expected.dataWindow()) ||
            !(display->displayWindow() == expected.displayWindow())) {
            reason = "the resident display window does not match the trusted process descriptor";
            return false;
        }
        if (!(display->pixelAspect() == expected.pixelAspect())) {
            reason =
                "the resident display pixel aspect does not match the trusted process descriptor";
            return false;
        }

        const auto pixelCount =
            static_cast<std::uint64_t>(dataExtent.width()) * dataExtent.height();
        if (pixelCount == 0 || pixelCount > kGpuResidentDisplayMaxPixels) {
            reason = "the display area is empty or exceeds the supported 4K ceiling";
            return false;
        }
        const auto& interval = report.eligibleInterval();
        if (!interval.has_value() || pixelCount < interval->min_pixels ||
            pixelCount > interval->max_pixels) {
            reason = "the display area is outside the measured eligible interval";
            return false;
        }
        const auto allocationBytes = display->allocationBytes();
        if (allocationBytes == 0) {
            reason = "the resident display reports no native allocation bytes";
            return false;
        }
        const std::size_t boundsBytes = std::span(request.bounds).size_bytes();
        if (allocationBytes > request.pixelStorageByteLimit ||
            boundsBytes > request.pixelStorageByteLimit - allocationBytes) {
            reason = "the resident allocation plus evaluated geometry exceed the request budget";
            return false;
        }
        return true;
    } catch (...) {
        // noexcept: never assign a string here (that could itself throw and terminate). Clearing
        // is non-allocating; the caller treats an empty reason as a generic ineligibility.
        reason.clear();
        return false;
    }
}

namespace detail {

std::optional<PreparedPreviewFrame> buildResidentPreviewFrame(
    PreviewRequestIdentity identity, ProcessFrameIdentity processIdentity,
    GpuResidentFrameLease lease,
    std::shared_ptr<const GpuResidentPreviewQualificationReport> qualification,
    std::vector<EvaluatedOperationBounds> bounds) noexcept {
    try {
        // This is the only place a PreviewResidentDisplayFrame private storage is built. The
        // factory has already verified the report, device, registry, request/process identity and
        // budget and has already published the lease, so this is a final assembly rather than a
        // validation pass.
        // Direct new (rather than std::make_shared) so the private constructor call is made in this
        // friend function's own context; the deleter only needs the public destructor.
        auto resident =
            std::shared_ptr<const PreviewResidentDisplayFrame>(new PreviewResidentDisplayFrame(
                std::move(identity), std::move(processIdentity), std::move(lease),
                std::move(qualification), std::move(bounds)));
        if (!resident->isDisplayValid()) {
            return std::nullopt;
        }
        const std::uint64_t requestGeneration = resident->desiredIdentity().requestGeneration;
        return PreparedPreviewFrame::createResident(requestGeneration, std::move(resident));
    } catch (...) {
        return std::nullopt;
    }
}

} // namespace detail

std::optional<PreparedPreviewFrame>
makeGpuResidentDisplayPreview(render::GpuDevice& device, GpuResidentFrameLeaseRegistry& registry,
                              const color::PreparedCpuDisplayProcessorHandle& processor,
                              std::shared_ptr<const GpuResidentPreviewQualificationReport> report,
                              GpuResidentDisplayProductRequest request) noexcept {
    try {
        if (report == nullptr) {
            return std::nullopt;
        }
        // The authoritative eligibility check runs first; after it returns true the report, device,
        // registry, processor, display image, identity and budget relationship are all known good.
        std::string reason;
        if (!gpuResidentDisplayProductIsEligible(device, registry, processor, *report, request,
                                                 reason)) {
            return std::nullopt;
        }
        if (request.display == nullptr) {
            return std::nullopt;
        }

        // Publishing is the proof the lease genuinely belongs to this registry and device: the
        // registry validates its own instance, the device binding, the thread and the byte budget.
        auto published = registry.publish(request.display);
        if (!published.hasValue()) {
            return std::nullopt;
        }

        return detail::buildResidentPreviewFrame(
            std::move(request.identity), std::move(request.processIdentity),
            std::move(published.lease), std::move(report), std::move(request.bounds));
    } catch (...) {
        return std::nullopt;
    }
}

} // namespace bloom::runtime
