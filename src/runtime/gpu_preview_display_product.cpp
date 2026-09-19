#include <bloom/runtime/gpu_preview_display_product.hpp>

#include <bloom/render/display_buffer.hpp>
#include <bloom/render/image.hpp>
#include <bloom/runtime/compiled_plan.hpp>
#include <bloom/runtime/evaluation.hpp>

#include <cstddef>
#include <exception>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace bloom::runtime {

bool gpuNeutralDisplayStageIsEligible(const PreviewCpuStage& stage,
                                      const GpuNeutralDisplayQualificationReport& report,
                                      std::string& reason) noexcept {
    try {
        if (!report.eligible()) {
            reason = "the GPU Neutral display qualification report is not eligible";
            return false;
        }
        if (report.shaderDigest() != kGpuNeutralDisplayShaderDigest) {
            reason = "the report shader digest is not the pinned Bloom Neutral v1 shader";
            return false;
        }
        if (report.configRevision() != color::kBloomNeutralV1ConfigDigest) {
            reason = "the report config revision is not the pinned Bloom Neutral v1 revision";
            return false;
        }
        if (report.processorCacheId() != kGpuNeutralDisplayProcessorCacheId) {
            reason = "the report processor cache ID is not the pinned Bloom Neutral v1 cache ID";
            return false;
        }
        if (!stage.ocioQualified() || stage.displayProcessor() == nullptr) {
            reason = "the CPU stage selected no qualified display processor";
            return false;
        }
        // The pinned processor/config/OCIO-version/display-view check is the qualification module's
        // one authoritative predicate, reused here rather than duplicated.
        if (!gpuNeutralDisplayProcessorIsEligible(*stage.displayProcessor(), reason)) {
            return false;
        }

        const auto& identity = stage.desiredIdentity();
        if (identity.requestGeneration == 0) {
            reason = "the request generation is zero";
            return false;
        }
        if (identity.output != PreviewOutput::Composition) {
            reason = "only the composition preview output is supported by the fixed GPU operation";
            return false;
        }
        // Full color intent: the working space must be linear Rec709 scene; the revision must be
        // the empty compatibility revision OR the exact pinned Bloom Neutral v1 revision; the URI
        // must be empty OR the builtin neutral URI. A foreign config URI is never accepted. (The
        // selected CPU processor itself is pinned strictly by gpuNeutralDisplayProcessorIsEligible
        // below, so this only rejects a request whose intent names a different transform.)
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
        if (!(identity.viewAdjust == ViewAdjust{})) {
            reason = "a non-neutral view adjustment is not supported by the fixed GPU operation";
            return false;
        }
        if (!(identity.displayName.empty() ||
              identity.displayName == kGpuNeutralDisplayDisplayName)) {
            reason = "a non-default display name is not supported by the fixed GPU operation";
            return false;
        }
        if (!(identity.viewName.empty() || identity.viewName == kGpuNeutralDisplayViewName)) {
            reason = "a non-default view name is not supported by the fixed GPU operation";
            return false;
        }

        const auto& processFrame = stage.processFrame();
        if (processFrame == nullptr) {
            reason = "the CPU stage retained no process frame";
            return false;
        }
        const auto* descriptor = processFrame->processImage().descriptor();
        if (descriptor == nullptr) {
            reason = "the process image has no valid descriptor";
            return false;
        }
        if (!(descriptor->dataWindow() == descriptor->displayWindow())) {
            reason = "the fixed GPU operation requires dataWindow == displayWindow";
            return false;
        }
        const auto& processIdentity = processFrame->identity();
        const auto& plan = processIdentity.plan;
        if (plan == nullptr) {
            reason = "the process frame has no compiled plan";
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

        const auto pixelCount = static_cast<std::uint64_t>(descriptor->layout().pixelCount);
        if (pixelCount == 0 || pixelCount > kGpuNeutralDisplayMaxPixels) {
            reason = "the display area is empty or exceeds the supported 4K ceiling";
            return false;
        }
        const auto& interval = report.eligibleInterval();
        if (!interval.has_value() || pixelCount < interval->min_pixels ||
            pixelCount > interval->max_pixels) {
            reason = "the display area is outside the measured eligible interval";
            return false;
        }
        const std::size_t packedBytes =
            static_cast<std::size_t>(pixelCount) * sizeof(render::Rgba8);
        const std::size_t geometryBytes = processFrame->evaluatedBounds().size_bytes();
        if (packedBytes > stage.pixelStorageByteLimit() ||
            geometryBytes > stage.pixelStorageByteLimit() - packedBytes) {
            reason = "the adopted display pixels plus evaluated geometry exceed the request budget";
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

std::optional<PreparedPreviewFrame>
makeGpuNeutralDisplayPreview(const PreviewCpuStage& stage,
                             std::shared_ptr<const GpuNeutralDisplayQualificationReport> report,
                             render::GpuNeutralDisplayReadback&& readback) noexcept {
    // The function is noexcept, so every allocation that could throw is caught here and turns into
    // a clean rejection rather than a terminate.
    //
    // Byte ownership contract: every rejection up to and including the adopt() call happens BEFORE
    // the payload is moved, so the caller's readback keeps its bytes. adopt() itself only moves
    // once its own checks pass. After adoption, a later allocation failure (the metadata vectors or
    // the shared envelope) may consume the readback's bytes -- but the ProcessFrame and the whole
    // CPU stage are untouched, so the caller can still fall back to the CPU display path with no
    // re-evaluation. Metadata is therefore allocated before the payload is moved whenever that is
    // cheap and clear.
    try {
        if (report == nullptr) {
            return std::nullopt;
        }
        // A typed native failure publishes nothing and leaves the caller's readback owned.
        if (!readback.hasValue()) {
            return std::nullopt;
        }
        // The authoritative eligibility check runs BEFORE any stage dereference: PreviewCpuStage's
        // public constructor permits a null process frame, and eligibility rejects it (and every
        // other mismatch) without touching it. After this returns true the process frame, its
        // descriptor, and the budget relationship are all known good, so the dereferences below
        // cannot fault.
        std::string reason;
        if (!gpuNeutralDisplayStageIsEligible(stage, *report, reason)) {
            return std::nullopt;
        }
        const auto& processFrame = stage.processFrame();
        const auto* processDescriptor = processFrame->processImage().descriptor();
        if (processDescriptor == nullptr) {
            return std::nullopt;
        }
        const auto displayDescriptor = render::ReferenceDisplayBufferDescriptor::create(
            processDescriptor->displayWindow(), processDescriptor->pixelAspect());
        if (!displayDescriptor) {
            return std::nullopt;
        }
        // adopt() re-checks size and budget; this size check only orders the metadata allocation
        // before the payload move.
        if (readback.pixels.size() != displayDescriptor.value()->layout().pixelCount) {
            return std::nullopt;
        }
        const std::size_t geometryBytes = processFrame->evaluatedBounds().size_bytes();

        // Metadata is allocated before the payload move so a metadata allocation failure is still a
        // rejection-before-adoption that preserves the caller's bytes.
        std::vector<EvaluatedOperationBounds> bounds(processFrame->evaluatedBounds().begin(),
                                                     processFrame->evaluatedBounds().end());
        auto provenance = PreviewDisplayProvenance{.provider = PreviewDisplayProvider::GpuNeutral,
                                                   .gpuQualification = std::move(report)};

        // The payload is moved only inside adopt(), after its checks; a rejection returns before
        // the move and leaves readback.pixels owned by the caller.
        auto buffer = render::PreparedReferenceDisplayBuffer::adopt(
            *displayDescriptor.value(), std::move(readback.pixels),
            stage.pixelStorageByteLimit() - geometryBytes);
        if (!buffer) {
            return std::nullopt;
        }

        // Construct the private display-only storage directly: this finalizer is the only friend,
        // so no arbitrary-provenance factory exists. createDisplayOnly() copies the identity's
        // strings, so its throw is caught by the enclosing handler rather than terminating.
        PreviewDisplayOnlyFrame displayOnly(stage.desiredIdentity(), processFrame->identity(),
                                            std::move(*buffer.value()), std::move(provenance),
                                            std::move(bounds));
        auto shared = std::make_shared<const PreviewDisplayOnlyFrame>(std::move(displayOnly));
        return PreparedPreviewFrame::createDisplayOnly(stage.desiredIdentity().requestGeneration,
                                                       std::move(shared));
    } catch (...) {
        return std::nullopt;
    }
}

} // namespace bloom::runtime
