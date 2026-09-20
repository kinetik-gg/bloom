#pragma once

// GPU-resident preview display product: turns one real resident display result into the fourth,
// closed PreparedPreviewFrame arm, off the UI thread, with NO CPU pixels, NO native object and NO
// readback. The only resident storage is the opaque, owner-bound GpuResidentFrameLease the owner
// registry publishes.
//
// This is the product half of the GPU-resident display route (Solid / covered Solid / composite ->
// resident Bloom Neutral RGBA8), qualified by qualifyResidentPreview(). It only ever wraps a
// display image the native resident display already produced and the registry already owns; it
// performs no decoding, evaluation, rendering, upload, display or readback itself.
//
// The factory runs on the device OWNER thread, with the ACTUAL device, the ACTUAL registry bound to
// it, the ACTUAL selected CPU processor, and a GENUINE immutable qualification report from
// qualifyResidentPreview(). It validates the report against that exact device and processor, the
// request/process identity, the native dimensions/window/pixel aspect, and the measured eligible
// area and byte budget, then publishes the image into the registry. A successful lease plus the
// registry's own pin validation is what proves the token actually belongs to and is bound to the
// registry; a construction-time success in this factory is never accepted as a qualification and
// no report can be fabricated here.
//
// Provenance is explicit: a request built from a GPU scene carries
// ProcessFrameIdentity::provider == EvaluationProvider::GpuResident, one built from a CPU-evaluated
// stage carries CpuReference, and neither is inferred from the other. A GPU-evaluated process is
// never stamped CpuReference.

#include <bloom/core/sha256.hpp>
#include <bloom/render/gpu_resident_display.hpp>
#include <bloom/render/image.hpp>
#include <bloom/runtime/gpu_resident_frame_lease.hpp>
#include <bloom/runtime/gpu_resident_preview_qualification.hpp>
#include <bloom/runtime/prepared_preview_frame.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace bloom::runtime {

class PreviewCpuStage;
class PreparedGpuScene;

// The largest display area the fixed resident operation is admitted for. The report's measured
// eligible interval is always the real authority; this is only the 4K ceiling that interval can
// never exceed. Never a 720p floor: eligibility comes from the measured interval.
inline constexpr std::uint64_t kGpuResidentDisplayMaxPixels = 3840ULL * 2160ULL;

// One resident display product request. `bounds` is the evaluated operation geometry the retained
// frame keeps; `display` is the native resident RGBA8 image the real GpuResidentDisplay produced
// (non-null, valid, and bound to the factory's device). The process identity's own
// EvaluationProvider records honestly which execution produced the scene-linear process the
// resident display consumed.
struct GpuResidentDisplayProductRequest final {
    PreviewRequestIdentity identity;
    // A null plan is the honest "no process context yet" state; the factory rejects it. The default
    // exists only so the request is default-constructible for builders that fill it field by field.
    ProcessFrameIdentity processIdentity{.plan = nullptr,
                                         .time = core::RationalTime{},
                                         .output = OperationIndex::fromRaw(0),
                                         .resolution = CompositionFormatResolution{}};
    std::vector<EvaluatedOperationBounds> bounds;
    // The TRUSTED expected geometry, taken from the actual immutable prepared scene output
    // descriptor (GPU stage) or the actual CPU process image descriptor (CPU stage). The factory
    // validates the request resolution/ROI against THIS and the native display image against THIS;
    // it never fabricates the full composition format and never infers geometry from the returned
    // image. A null descriptor is rejected.
    std::optional<render::Rgba32fImageDescriptor> expectedDescriptor;
    std::shared_ptr<const render::GpuDisplayImage> display;
    std::size_t pixelStorageByteLimit = 0;
};

// One authoritative eligibility check for the resident display product. True only when: the report
// is genuinely eligible and names this exact device and processor; the selected processor is the
// pinned Bloom Neutral identity; the registry is the one bound to this device and the factory runs
// on the owner thread; the request identity is neutral and matches the process identity and plan;
// the trusted expected descriptor (from the actual prepared scene output / CPU process image) is
// consistent with the request's resolved resolution and ROI; the native display image matches that
// trusted descriptor exactly (dimensions, data/display window, pixel aspect); the area is inside
// the measured eligible interval and at or below 4K; and the actual native allocation fits the
// request byte budget. `reason` is set on failure. Used by the caller before dispatch and rechecked
// by the factory.
[[nodiscard]] bool gpuResidentDisplayProductIsEligible(
    render::GpuDevice& device, const GpuResidentFrameLeaseRegistry& registry,
    const color::PreparedCpuDisplayProcessorHandle& processor,
    const GpuResidentPreviewQualificationReport& report,
    const GpuResidentDisplayProductRequest& request, std::string& reason) noexcept;

// Wraps a successful resident display into the fourth PreparedPreviewFrame arm. Returns
// std::nullopt, publishing nothing, on any of: a null or ineligible report; a foreign device,
// registry or thread; a mismatch between the report, the request identity and the process
// identity/plan; a native dimension/window/pixel-aspect mismatch; an area outside the measured
// interval or over the byte budget; or a registry publish refusal (budget, foreign device,
// invalid image).
//
// On success the registry owns the native image, the returned frame retains only the opaque lease,
// the immutable report, the identities and the evaluated bounds, and the process frame (if the
// caller had one) is not retained.
[[nodiscard]] std::optional<PreparedPreviewFrame>
makeGpuResidentDisplayPreview(render::GpuDevice& device, GpuResidentFrameLeaseRegistry& registry,
                              const color::PreparedCpuDisplayProcessorHandle& processor,
                              std::shared_ptr<const GpuResidentPreviewQualificationReport> report,
                              GpuResidentDisplayProductRequest request) noexcept;

// Builds the honest CPU-evaluated request: the process identity, bounds and identity come from the
// already-evaluated stage, and the process provider is CpuReference. Nullopt-free; may allocate.
[[nodiscard]] GpuResidentDisplayProductRequest
makeGpuResidentDisplayProductRequest(const PreviewCpuStage& stage,
                                     std::shared_ptr<const render::GpuDisplayImage> display);

// Builds the honest GPU-evaluated request: the process identity and bounds come from the prepared
// GPU scene, and the process provider is set to GpuResident, never CpuReference. The request
// identity is supplied by the caller because a prepared scene carries the process identity but not
// the generation-bearing request identity.
[[nodiscard]] GpuResidentDisplayProductRequest
makeGpuResidentDisplayProductRequest(const PreparedGpuScene& scene, PreviewRequestIdentity identity,
                                     std::shared_ptr<const render::GpuDisplayImage> display);

// --- General display product -------------------------------------------------------------------
//
// The general display route produces a resident RGBA8 image through the per-request OCIO
// DisplayRgba8 program (runtime::GpuOcioDisplayArm), not the startup Neutral shader. This factory
// validates the actual native image against the trusted prepared-scene output descriptor and the
// pretend command geometry, then publishes the opaque lease. It does NOT fabricate a Neutral
// qualification report: the general display identity and its genuine program qualification are
// carried by the caller (the service stage), and the frame is built with an explicit general
// provenance. Eligibility is governed by the real device/geometry/budget relationship only.
struct GpuGeneralDisplayProductRequest final {
    PreviewRequestIdentity identity;
    ProcessFrameIdentity processIdentity{.plan = nullptr,
                                         .time = core::RationalTime{},
                                         .output = OperationIndex::fromRaw(0),
                                         .resolution = CompositionFormatResolution{}};
    std::vector<EvaluatedOperationBounds> bounds;
    std::optional<render::Rgba32fImageDescriptor> expectedDescriptor;
    std::shared_ptr<const render::GpuDisplayImage> display;
    std::size_t pixelStorageByteLimit = 0;
    // The exact command identity the display arm dispatched, so a consumer can correlate the frame
    // with the prepared program.
    core::Sha256Digest displayCommandIdentity{};
};

[[nodiscard]] bool gpuGeneralDisplayProductIsEligible(
    render::GpuDevice& device, const GpuResidentFrameLeaseRegistry& registry,
    const GpuGeneralDisplayProductRequest& request, std::string& reason) noexcept;

[[nodiscard]] std::optional<PreparedPreviewFrame>
makeGpuGeneralDisplayPreview(render::GpuDevice& device, GpuResidentFrameLeaseRegistry& registry,
                             GpuGeneralDisplayProductRequest request) noexcept;

} // namespace bloom::runtime
