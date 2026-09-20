#pragma once

// Bounded GPU coverage contract (docs/architecture/gpu-backend.md).
//
// One exhaustive, compile-time-checked registry of every pixel operation the compiled plan can
// carry, plus the feature axes a single fixture must not be able to claim on its own. GPU
// production preparation is REQUIRED by default: a pixel operation is a hole until a genuine
// production fixture proves the prepared GPU command path, and a newly added operation fails to
// compile until it is classified here.
//
// The only permitted exceptions are narrow, typed, explicitly approved host-preparation steps --
// media container/sample I/O and decompression, font load/shaping, parameter/curve/geometry
// RESOLUTION, and one final readback -- which are not whole-frame pixel-rendering opt-outs. An
// exception must name a nonempty stable id, a nonempty rationale, and an owning document/decision
// reference. There are no wildcard or blanket opt-outs, and no approval identifier may be invented.
// Per-pixel coverage rasterization (CPU PathRaster coverageRow) is a pixel transformation, never
// host preparation; the generated coverage mask is Required GPU work
// (feature.geometry.vector_coverage).

#include <bloom/core/blend_mode.hpp>
#include <bloom/document/node_definition_registry.hpp>
#include <bloom/document/shape.hpp>
#include <bloom/runtime/compiled_plan.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace bloom::runtime {

// The disposition of one pixel operation or feature axis. `Required` is the default; a new
// alternative or axis is required the moment it is added, with no grandfathering.
enum class GpuCoverageDisposition : std::uint8_t {
    Required,
    // A typed, narrow, explicitly approved exception for an actual pixel operation, feature, or
    // route. It still requires a nonempty stable id, rationale, and owning approval reference, and
    // no such exception exists in this repository today; agents must not add one to turn a check
    // green. This is not a host-preparation declaration.
    ApprovedCpuPixelException,
    // Narrow host-preparation work that runs on the CPU by accepted design. This is not a
    // permission to skip whole-frame pixel rendering and is tracked separately from the pixel
    // operation/feature/route required set.
    ApprovedCpuHostPreparation,
};

struct GpuCoverageEntry final {
    // Owns its storage so generated ids (for example node.<typeId>) stay valid after the registry
    // returns.
    std::string id;
    // label, approvalReference, and rationale are always string literals, so a view is safe. Any
    // future generated text here must become an owned std::string like id.
    std::string_view label;
    GpuCoverageDisposition disposition = GpuCoverageDisposition::Required;
    // Nonempty exactly for an approved exception or host-preparation entry. A repository document
    // or accepted decision that owns the approval, never a fabricated ticket id.
    std::string_view approvalReference;
    // Nonempty exactly for an approved exception or host-preparation entry.
    std::string_view rationale;

    friend bool operator==(const GpuCoverageEntry&, const GpuCoverageEntry&) = default;
};

// Exhaustive per-alternative traits. The primary templates are only declared, so a
// CompiledOperation or ImageEffectKernel alternative without a specialization fails to compile when
// the collectors below instantiate it. That is the compile-time gate on new alternatives.
template <typename Alternative> struct GpuOperationCoverageTraits;
template <typename Alternative> struct GpuImageEffectCoverageTraits;

#define BLOOM_GPU_REQUIRED_OPERATION(Alternative, Label)                                           \
    template <> struct GpuOperationCoverageTraits<Alternative> final {                             \
        [[nodiscard]] static constexpr GpuCoverageEntry entry() noexcept {                         \
            return GpuCoverageEntry{                                                               \
                "operation." #Alternative, Label, GpuCoverageDisposition::Required, {}, {}};       \
        }                                                                                          \
    }

BLOOM_GPU_REQUIRED_OPERATION(CompiledSolid, "solid source");
BLOOM_GPU_REQUIRED_OPERATION(CompiledText, "text source");
BLOOM_GPU_REQUIRED_OPERATION(CompiledImageSource, "image source");
BLOOM_GPU_REQUIRED_OPERATION(CompiledVideoSource, "video source");
BLOOM_GPU_REQUIRED_OPERATION(CompiledLayerOutput, "layer output transform");
BLOOM_GPU_REQUIRED_OPERATION(CompiledMerge, "layer stack merge");
BLOOM_GPU_REQUIRED_OPERATION(CompiledCompositionOutput, "composition output");
BLOOM_GPU_REQUIRED_OPERATION(CompiledShape, "shape source");
BLOOM_GPU_REQUIRED_OPERATION(CompiledCompositionSource, "nested composition source");
BLOOM_GPU_REQUIRED_OPERATION(CompiledImageEffect, "image effect");

#define BLOOM_GPU_REQUIRED_EFFECT(Alternative, Label)                                              \
    template <> struct GpuImageEffectCoverageTraits<Alternative> final {                           \
        [[nodiscard]] static constexpr GpuCoverageEntry entry() noexcept {                         \
            return GpuCoverageEntry{                                                               \
                "effect." #Alternative, Label, GpuCoverageDisposition::Required, {}, {}};          \
        }                                                                                          \
    }

BLOOM_GPU_REQUIRED_EFFECT(IdentityImageKernel, "identity image effect kernel");
BLOOM_GPU_REQUIRED_EFFECT(CstKernel, "colour-space transform kernel");
BLOOM_GPU_REQUIRED_EFFECT(FileTransformKernel, "file transform kernel");

#undef BLOOM_GPU_REQUIRED_OPERATION
#undef BLOOM_GPU_REQUIRED_EFFECT

namespace detail {

template <typename Alternative>
void collectGpuOperationCoverage(std::vector<GpuCoverageEntry>& out) {
    out.push_back(GpuOperationCoverageTraits<Alternative>::entry());
}

template <typename Alternative> void collectGpuEffectCoverage(std::vector<GpuCoverageEntry>& out) {
    out.push_back(GpuImageEffectCoverageTraits<Alternative>::entry());
}

template <typename Variant, std::size_t... Index>
[[nodiscard]] std::vector<GpuCoverageEntry> collectVariant(std::index_sequence<Index...>) {
    std::vector<GpuCoverageEntry> out;
    out.reserve(sizeof...(Index));
    (collectGpuOperationCoverage<std::variant_alternative_t<Index, Variant>>(out), ...);
    return out;
}

template <typename Variant, std::size_t... Index>
[[nodiscard]] std::vector<GpuCoverageEntry> collectEffectVariant(std::index_sequence<Index...>) {
    std::vector<GpuCoverageEntry> out;
    out.reserve(sizeof...(Index));
    (collectGpuEffectCoverage<std::variant_alternative_t<Index, Variant>>(out), ...);
    return out;
}

} // namespace detail

// Every pixel operation the compiled plan can carry. Instantiating this visits each variant
// alternative, so a new alternative without a traits specialization is a hard compile error.
[[nodiscard]] inline std::vector<GpuCoverageEntry> gpuOperationCoverage() {
    return detail::collectVariant<CompiledOperation>(
        std::make_index_sequence<std::variant_size_v<CompiledOperation>>{});
}

// Every image-effect kernel the plan can carry, under the same exhaustive rule.
[[nodiscard]] inline std::vector<GpuCoverageEntry> gpuImageEffectCoverage() {
    return detail::collectEffectVariant<ImageEffectKernel>(
        std::make_index_sequence<std::variant_size_v<ImageEffectKernel>>{});
}

// A feature axis a single basic fixture must not be able to claim on its own. Both id and label are
// OWNED: a shape label is generated text, so a view into a temporary std::string would dangle once
// the temporary dies and the caller retains the registry. `fixtureOwner` is always a string
// literal.
struct GpuFeatureCoverageEntry final {
    std::string id;
    std::string label;
    GpuCoverageDisposition disposition = GpuCoverageDisposition::Required;
    // The production module or suite that owns the genuine fixture.
    std::string_view fixtureOwner;
};

// Exhaustive shape-kind label. No default case: building the gate with
// -Wswitch (from -Wall, promoted by -Werror) turns a newly added document::ShapeKind into a compile
// failure, so the feature list cannot silently omit it.
[[nodiscard]] constexpr std::string_view
gpuShapeKindFeatureLabel(const document::ShapeKind kind) noexcept {
    switch (kind) {
    case document::ShapeKind::Rectangle:
        return "rectangle";
    case document::ShapeKind::Ellipse:
        return "ellipse";
    case document::ShapeKind::Triangle:
        return "triangle";
    case document::ShapeKind::Polygon:
        return "polygon";
    case document::ShapeKind::Star:
        return "star";
    case document::ShapeKind::Line:
        return "line";
    case document::ShapeKind::Path:
        return "path";
    }
    return {};
}

[[nodiscard]] inline std::vector<GpuFeatureCoverageEntry> gpuFeatureCoverage() {
    std::vector<GpuFeatureCoverageEntry> out;
    for (const auto mode : core::kBlendModes) {
        out.push_back(GpuFeatureCoverageEntry{std::string{"feature.blend."} +
                                                  std::to_string(core::blendModeStoredValue(mode)),
                                              "blend mode axis", GpuCoverageDisposition::Required,
                                              "src/render GpuComposite + composite parity tests"});
    }
    constexpr std::array<document::ShapeKind, 7> kShapeKinds{
        document::ShapeKind::Rectangle, document::ShapeKind::Ellipse, document::ShapeKind::Triangle,
        document::ShapeKind::Polygon,   document::ShapeKind::Star,    document::ShapeKind::Line,
        document::ShapeKind::Path};
    for (const auto kind : kShapeKinds) {
        out.push_back(GpuFeatureCoverageEntry{
            std::string{"feature.shape."} + std::to_string(static_cast<std::int64_t>(kind)),
            std::string{"shape kind axis: "} + std::string{gpuShapeKindFeatureLabel(kind)},
            GpuCoverageDisposition::Required, "src/render PathRaster + shape source tests"});
    }
    // The vector coverage axis is a PIXEL operation, not host preparation: a GPU vector path must
    // rasterize its own coverage on the device and prove it with native provenance/counters. A CPU
    // PathRaster coverage mask consumed by CoveredSolidV1 (a fill from a host mask) does NOT
    // satisfy this axis. The production builder now emits immutable PathRasterCoverageGeometry and
    // the executor runs the native GpuPathCoverage producer; the genuine fixture lives in the
    // executor native coverage tests and must observe a positive cold coverage dispatch and zero
    // warm. The fixture owner below names the native gate that carries that evidence.
    out.push_back(GpuFeatureCoverageEntry{
        "feature.geometry.vector_coverage", "native GPU vector coverage rasterization",
        GpuCoverageDisposition::Required, "src/runtime GpuSceneExecutor native coverage tests"});
    out.push_back(GpuFeatureCoverageEntry{
        "feature.layer.affine.scale", "layer scale axis", GpuCoverageDisposition::Required,
        "src/render LayerTransform + gpu scene preparation tests"});
    out.push_back(GpuFeatureCoverageEntry{
        "feature.layer.affine.rotation", "layer rotation axis", GpuCoverageDisposition::Required,
        "src/render LayerTransform + gpu scene preparation tests"});
    out.push_back(GpuFeatureCoverageEntry{
        "feature.layer.affine.anchor", "layer anchor axis", GpuCoverageDisposition::Required,
        "src/render LayerTransform + gpu scene preparation tests"});
    out.push_back(GpuFeatureCoverageEntry{"feature.layer.parent", "parented layer transform axis",
                                          GpuCoverageDisposition::Required,
                                          "src/runtime layer_parent_transform tests"});
    out.push_back(GpuFeatureCoverageEntry{
        "feature.layer.generic_input", "layer fed by a non-source input (graph compositing)",
        GpuCoverageDisposition::Required, "src/runtime GpuSceneExecutor graph tests"});
    // A request ROI is a required route: the prepared GPU scene must clip its native output to the
    // requested data window exactly as the CPU evaluator does, keep every native source/output
    // window and pixel aspect, and never fall back to a whole-frame CPU render. The genuine fixture
    // and its negative missed-GPU detection live in the ROI suites and the coverage gate.
    out.push_back(GpuFeatureCoverageEntry{
        "feature.roi", "request region-of-interest GPU composition",
        GpuCoverageDisposition::Required, "src/runtime GpuSceneExecutor ROI native tests"});
    // Colour conversion into the working space is a pixel transformation, never host I/O. Decode
    // and decompression are preparation; the conversion itself must have a GPU implementation.
    out.push_back(GpuFeatureCoverageEntry{
        "feature.color.working_space_transform", "decode-to-working-space colour transform",
        GpuCoverageDisposition::Required, "src/color OCIO processor tests"});
    out.push_back(GpuFeatureCoverageEntry{
        "feature.display.view_adjust", "custom viewer exposure/gamma adjust",
        GpuCoverageDisposition::Required, "src/runtime gpu_neutral_display qualification tests"});
    out.push_back(GpuFeatureCoverageEntry{
        "feature.display.custom_view_transform", "custom display/view transform",
        GpuCoverageDisposition::Required, "src/runtime gpu_neutral_display qualification tests"});
    return out;
}

// The render routes every pixel operation must reach. Preview and final render are both required:
// there is no preview-only exemption, and a GPU implementation that is silently a CPU whole-frame
// render does not count. The final-render routes may read back the single composited image at the
// CPU codec/file boundary; per-node or per-operation full-frame roundtrips are not an
// implementation.
// All three fields are fixed string literals, so views into static storage are safe here.
struct GpuRouteCoverageEntry final {
    std::string_view id;
    std::string_view label;
    // The owning production module and test suite that must carry the genuine fixture.
    std::string_view owner;
};

[[nodiscard]] inline std::vector<GpuRouteCoverageEntry> gpuRenderRouteCoverage() {
    return {
        GpuRouteCoverageEntry{"route.preview.viewer", "interactive viewer preview",
                              "src/runtime GpuPreviewDisplayService tests"},
        GpuRouteCoverageEntry{"route.preview.ram_preview", "RAM preview fill/playback",
                              "src/ui RamPreviewController tests"},
        GpuRouteCoverageEntry{"route.export.still_frame", "still-frame export",
                              "src/output process-frame export tests"},
        GpuRouteCoverageEntry{"route.export.sequence_range", "sequence/range export",
                              "src/output frame-range export tests"},
        GpuRouteCoverageEntry{"route.export.video", "video export",
                              "src/output video export tests"},
        GpuRouteCoverageEntry{"route.export.headless_scripted", "headless/scripted render",
                              "apps/bloom-cli headless render tests"},
    };
}

enum class GpuCoverageContractIssue : std::uint8_t {
    None,
    DuplicateId,
    MissingApprovalReference,
    MissingRationale,
    WildcardOptOut,
    ApprovalReferenceOnRequired,
    EffectNotCovered,
    LoweringEscapesCoverage,
    // A route proof carried no route id or harness, or an empty/generic value a boolean would.
    RouteProofEmptyOrGeneric,
    // A route proof named a route that is not one of the required render routes.
    RouteProofUnknownRoute,
    // The proof's harness kind does not match the route it claims to prove.
    RouteProofHarnessMismatch,
    // The proof omitted the production process identity, device ownership epoch, or evidence
    // digest.
    RouteProofMissingProvenance,
    // The proof recorded zero actual native dispatches: a CPU whole-frame fallback, not a GPU pass.
    RouteProofNoNativeDispatch,
    // The proof recorded no verified frame, so it demonstrates no real per-frame run.
    RouteProofNoVerifiedFrame,
    // The proof recorded more final readback submissions/payloads than its route policy allows.
    RouteProofExcessReadback,
    // A final-render proof omitted the explicit transferred-byte evidence.
    RouteProofMissingTransferredBytes,
};

struct GpuCoverageContractDiagnostic final {
    GpuCoverageContractIssue issue = GpuCoverageContractIssue::None;
    std::string detail;
};

// The typed render-route proof contract (harness kinds, GpuRouteExecutionProof, validation, and the
// sink) lives in gpu_coverage_route_proof_contract.hpp, which includes this header. It is kept
// separate so this classifier header stays bounded and consumers that do not need route proofs do
// not pull them in.

// The narrow, already-approved host-preparation steps. These are not whole-frame pixel-rendering
// opt-outs; the rendering operation itself remains Required above. Each entry names the accepted
// owning document, never a fabricated approval id.
//
// Media decode is intentionally scoped to container/sample I/O and decompression only. Converting
// the decoded samples into the working colour space is a pixel transformation and remains Required
// (see feature.color.working_space_transform and the OCIO image-effect kernels). Likewise, font
// load/shaping and parameter/curve/geometry resolution are host preparation, but the per-pixel
// coverage mask a vector source rasterizes is a pixel transformation and is Required GPU work
// (feature.geometry.vector_coverage); there is no host-preparation exemption for it.
[[nodiscard]] inline std::vector<GpuCoverageEntry> gpuApprovedCpuPreparation() {
    return {
        GpuCoverageEntry{"prep.media.io_decompress",
                         "container/sample I/O and entropy decompression for image/video sources",
                         GpuCoverageDisposition::ApprovedCpuHostPreparation,
                         "docs/architecture/media-io.md",
                         "Decoding pipes a decompressed sample buffer to the working-space colour "
                         "transform; it does not itself produce working-space pixels."},
        GpuCoverageEntry{"prep.font.shaping", "font load, shaping, and glyph geometry for text",
                         GpuCoverageDisposition::ApprovedCpuHostPreparation,
                         "docs/architecture/gpu-backend.md",
                         "Text shaping is host preparation; the whole-frame text render operation "
                         "remains Required and is not exempted."},
        GpuCoverageEntry{
            "prep.parameter.resolution",
            "curve sampling, value-graph driver, and parameter preflight",
            GpuCoverageDisposition::ApprovedCpuHostPreparation, "docs/architecture/gpu-backend.md",
            "Animated and driven operands are resolved by the real CPU preflight before "
            "the GPU command is built."},
        GpuCoverageEntry{
            "prep.export.final_readback",
            "single final composited image readback at the codec/file boundary",
            GpuCoverageDisposition::ApprovedCpuHostPreparation, "docs/architecture/frame-output.md",
            "Final render may read the single composited image back once for CPU codec "
            "and file writing; per-node or per-operation full-frame roundtrips are not "
            "a GPU implementation."},
    };
}

// Whether an authoring lowering produces pixels and therefore needs a Required pixel-operation
// mapping. The switch covers every enumerator with no default, so building this contract with
// -Wswitch (from -Wall, promoted by -Werror) makes a newly added NodeLoweringKind
// a compile failure instead of a silent escape.
[[nodiscard]] constexpr bool
gpuLoweringIsPixelOperation(const document::NodeLoweringKind lowering) noexcept {
    using document::NodeLoweringKind;
    switch (lowering) {
    case NodeLoweringKind::Solid:
    case NodeLoweringKind::Shape:
    case NodeLoweringKind::Text:
    case NodeLoweringKind::ImageSource:
    case NodeLoweringKind::ImageEffect:
    case NodeLoweringKind::VideoSource:
    case NodeLoweringKind::CompositionSource:
    case NodeLoweringKind::LayerOutput:
    case NodeLoweringKind::LayerStack:
    case NodeLoweringKind::CompositionOutput:
        return true;
    case NodeLoweringKind::AudioSource:
    case NodeLoweringKind::Unsupported:
    case NodeLoweringKind::ValueConstant:
    case NodeLoweringKind::ValueTime:
    case NodeLoweringKind::ValueScalarMath:
    case NodeLoweringKind::ValueVectorMath:
    case NodeLoweringKind::ValueVectorReduce:
    case NodeLoweringKind::ValueMapRange:
    case NodeLoweringKind::ValueClamp:
    case NodeLoweringKind::ValueMix:
    case NodeLoweringKind::ValueColorMix:
    case NodeLoweringKind::ValueCompare:
    case NodeLoweringKind::ValueSwitch:
    case NodeLoweringKind::ValueSeparate:
    case NodeLoweringKind::ValueCombine:
    case NodeLoweringKind::ValueRandom:
    case NodeLoweringKind::ValueReroute:
    case NodeLoweringKind::ValueBoundsReadout:
    case NodeLoweringKind::ValueUtility:
        return false;
    }
    return false;
}

// The Required operation id an image-producing lowering must map to. Same exhaustive-switch rule.
[[nodiscard]] inline std::string_view
gpuLoweringOperationId(const document::NodeLoweringKind lowering) noexcept {
    using document::NodeLoweringKind;
    switch (lowering) {
    case NodeLoweringKind::Solid:
        return "operation.CompiledSolid";
    case NodeLoweringKind::Shape:
        return "operation.CompiledShape";
    case NodeLoweringKind::Text:
        return "operation.CompiledText";
    case NodeLoweringKind::ImageSource:
        return "operation.CompiledImageSource";
    case NodeLoweringKind::ImageEffect:
        return "operation.CompiledImageEffect";
    case NodeLoweringKind::VideoSource:
        return "operation.CompiledVideoSource";
    case NodeLoweringKind::CompositionSource:
        return "operation.CompiledCompositionSource";
    case NodeLoweringKind::LayerOutput:
        return "operation.CompiledLayerOutput";
    case NodeLoweringKind::LayerStack:
        return "operation.CompiledMerge";
    case NodeLoweringKind::CompositionOutput:
        return "operation.CompiledCompositionOutput";
    case NodeLoweringKind::AudioSource:
        return "scope.audio";
    case NodeLoweringKind::Unsupported:
        return "scope.unsupported";
    case NodeLoweringKind::ValueConstant:
    case NodeLoweringKind::ValueTime:
    case NodeLoweringKind::ValueScalarMath:
    case NodeLoweringKind::ValueVectorMath:
    case NodeLoweringKind::ValueVectorReduce:
    case NodeLoweringKind::ValueMapRange:
    case NodeLoweringKind::ValueClamp:
    case NodeLoweringKind::ValueMix:
    case NodeLoweringKind::ValueColorMix:
    case NodeLoweringKind::ValueCompare:
    case NodeLoweringKind::ValueSwitch:
    case NodeLoweringKind::ValueSeparate:
    case NodeLoweringKind::ValueCombine:
    case NodeLoweringKind::ValueRandom:
    case NodeLoweringKind::ValueReroute:
    case NodeLoweringKind::ValueBoundsReadout:
    case NodeLoweringKind::ValueUtility:
        return "scope.value";
    }
    return {};
}

// One Required node-type entry per built-in authoring node that produces pixels. A new node type
// registered against an existing lowering still gains a new id here, so it cannot reuse another
// node's fixture and escape fixture identity; it will be reported as a missing required fixture
// until its own fixture exists.
[[nodiscard]] inline std::vector<GpuCoverageEntry>
gpuNodeTypeCoverage(const document::NodeDefinitionRegistry& registry) {
    std::vector<GpuCoverageEntry> out;
    for (const auto& definition : registry.definitions()) {
        if (!gpuLoweringIsPixelOperation(definition.lowering)) {
            continue;
        }
        out.push_back(GpuCoverageEntry{"node." + std::string{definition.key.typeId},
                                       "authoring node type",
                                       GpuCoverageDisposition::Required,
                                       {},
                                       {}});
    }
    return out;
}

// Structural self-check of the registry: unique ids, well-formed narrow exceptions, and every
// image-producing authoring lowering mapped to a Required operation. Any issue is a contract
// failure the CPU gate reports by name.
[[nodiscard]] inline std::vector<GpuCoverageContractDiagnostic> validateGpuCoverageContract() {
    std::vector<GpuCoverageContractDiagnostic> issues;
    std::vector<GpuCoverageEntry> entries = gpuOperationCoverage();
    const auto effectEntries = gpuImageEffectCoverage();
    entries.insert(entries.end(), effectEntries.begin(), effectEntries.end());
    const auto preparation = gpuApprovedCpuPreparation();
    entries.insert(entries.end(), preparation.begin(), preparation.end());
    for (const auto& route : gpuRenderRouteCoverage()) {
        entries.push_back(GpuCoverageEntry{
            std::string{route.id}, route.label, GpuCoverageDisposition::Required, {}, {}});
    }

    const auto isWildcard = [](const std::string_view id) {
        return id.empty() || id == "*" || id == "all" || id == "any" ||
               id.find('*') != std::string_view::npos;
    };
    for (std::size_t i = 0; i < entries.size(); ++i) {
        const auto& entry = entries[i];
        if (entry.id.empty()) {
            issues.push_back(
                {GpuCoverageContractIssue::DuplicateId, "a coverage entry has an empty id"});
            continue;
        }
        for (std::size_t j = i + 1; j < entries.size(); ++j) {
            if (entries[j].id == entry.id) {
                issues.push_back({GpuCoverageContractIssue::DuplicateId,
                                  "duplicate coverage id '" + std::string{entry.id} + "'"});
            }
        }
        if (entry.disposition == GpuCoverageDisposition::ApprovedCpuHostPreparation ||
            entry.disposition == GpuCoverageDisposition::ApprovedCpuPixelException) {
            if (isWildcard(entry.id)) {
                issues.push_back({GpuCoverageContractIssue::WildcardOptOut,
                                  "broad or wildcard opt-out '" + std::string{entry.id} + "'"});
            }
            if (entry.approvalReference.empty()) {
                issues.push_back(
                    {GpuCoverageContractIssue::MissingApprovalReference,
                     "opt-out '" + std::string{entry.id} + "' has no owning approval reference"});
            }
            if (entry.rationale.empty()) {
                issues.push_back({GpuCoverageContractIssue::MissingRationale,
                                  "opt-out '" + std::string{entry.id} + "' has no rationale"});
            }
        } else if (!entry.approvalReference.empty()) {
            issues.push_back({GpuCoverageContractIssue::ApprovalReferenceOnRequired,
                              "required entry '" + std::string{entry.id} +
                                  "' carries an approval reference and cannot opt out"});
        }
    }

    const auto hasRequiredOperation = [&entries](const std::string_view id) {
        for (const auto& entry : entries) {
            if (std::string_view{entry.id} == id &&
                entry.disposition == GpuCoverageDisposition::Required) {
                return true;
            }
        }
        return false;
    };
    // Every image-producing lowering must map to a Required operation. The runtime loop covers the
    // current enum range; the no-default switches above make a newly added enumerator a compile
    // failure under -Wswitch/-Werror before it could ever slip past this loop.
    for (int raw = 0; raw <= static_cast<int>(document::NodeLoweringKind::Unsupported); ++raw) {
        const auto lowering = static_cast<document::NodeLoweringKind>(raw);
        if (!gpuLoweringIsPixelOperation(lowering)) {
            continue;
        }
        const auto id = gpuLoweringOperationId(lowering);
        if (id.empty() || !hasRequiredOperation(id)) {
            issues.push_back({GpuCoverageContractIssue::LoweringEscapesCoverage,
                              "authoring lowering " + std::to_string(raw) +
                                  " has no Required pixel-operation coverage"});
        }
    }
    return issues;
}

// What one genuine fixture must prove for its contract id. There is no "admitted but not
// prepared" criterion: a fixture passes only when the production builder returns a prepared GPU
// scene, or (native-only) when the native execution gate proves execution, parity, and cache state.
enum class GpuCoverageFixtureCriterion : std::uint8_t {
    Prepared,       // the production builder returns a prepared GPU scene
    NativeRequired, // the genuine fixture is native-only; the CPU gate reports it NotRun
};

} // namespace bloom::runtime
