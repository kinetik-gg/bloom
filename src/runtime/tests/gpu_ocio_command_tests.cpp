// CPU-only proof for the runtime OCIO command lifecycle: reusable wrapper generation, canonical
// command identity, prepared-command validation, and the bounded preparation cache. No device is
// touched. The preparation-cache cases need the pinned glslangValidator/spirv-val and an OCIO
// config; when they are absent those cases print an explicit note and the identity/wrapper cases
// still run.

#include <bloom/color/ocio_builtin_registry.hpp>
#include <bloom/color/ocio_gpu_program.hpp>
#include <bloom/core/sha256.hpp>
#include <bloom/render/gpu_shader_artifact.hpp>
#include <bloom/render/ocio_gpu_program.hpp>
#include <bloom/runtime/gpu_ocio_command.hpp>
#include <bloom/runtime/gpu_ocio_preparation.hpp>
#include <bloom/runtime/gpu_ocio_wrapper.hpp>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <latch>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {

using bloom::core::Sha256Digest;
using bloom::core::Sha256Hasher;
using bloom::render::CompiledGpuShader;
using bloom::render::GpuShaderStage;
using bloom::render::OcioGpuProgramDesc;
using bloom::render::OcioGpuProgramStage;
using bloom::runtime::GpuOcioCommandError;
using bloom::runtime::GpuOcioCommandGeometry;
using bloom::runtime::GpuOcioCommandIdentityParts;
using bloom::runtime::GpuOcioCommandSourceBinding;
using bloom::runtime::GpuOcioCompileOptions;
using bloom::runtime::GpuOcioOutputEncoding;
using bloom::runtime::GpuOcioPreparationError;
using bloom::runtime::GpuOcioProgramPreparer;
using bloom::runtime::GpuOcioTransformKind;
using bloom::runtime::GpuOcioTransformSpec;
using bloom::runtime::GpuOcioWrapperError;
using bloom::runtime::PreparedGpuOcioCommand;
using bloom::runtime::ViewAdjust;

class Expectations final {
  public:
    void expect(const bool condition, const std::string_view message) {
        if (condition) {
            return;
        }
        ++failures_;
        std::cerr << "FAILED: " << message << '\n';
    }
    [[nodiscard]] int failures() const noexcept { return failures_; }

  private:
    int failures_ = 0;
};

[[nodiscard]] Sha256Digest digestOf(const std::string_view text) {
    const auto digest = Sha256Hasher::hash(std::as_bytes(std::span(text.data(), text.size())));
    return digest.has_value() ? *digest : Sha256Digest{};
}

[[nodiscard]] OcioGpuProgramDesc syntheticProgram(const OcioGpuProgramStage stage) {
    OcioGpuProgramDesc program;
    program.shaderText = "vec4 bloom_ocio_transform(vec4 inPixel) { return inPixel; }\n";
    program.functionName = "bloom_ocio_transform";
    program.resourcePrefix = "bloom_ocio_";
    program.stage = stage;
    program.semanticsId = "test.ocio-gpu-command.v1";
    program.shaderTextDigest = bloom::render::computeOcioGpuShaderTextDigest(program.shaderText);
    program.resourceDigest = bloom::render::computeOcioGpuResourceDigest(program);
    program.contentIdentity = program.shaderTextDigest;
    return program;
}

[[nodiscard]] CompiledGpuShader syntheticArtifact() {
    CompiledGpuShader artifact;
    artifact.spirv = {0x03, 0x02, 0x23, 0x07};
    artifact.entryPoint = "main";
    artifact.targetEnvironment = "vulkan1.2";
    artifact.stage = GpuShaderStage::Compute;
    artifact.spirvDigest = *Sha256Hasher::hash(std::as_bytes(std::span(artifact.spirv)));
    return artifact;
}

[[nodiscard]] GpuOcioCommandSourceBinding syntheticBinding(const ViewAdjust adjust = {}) {
    GpuOcioCommandSourceBinding binding;
    binding.wrapperVersion = "test-sampling-v1";
    binding.wrapperSourceDigest = digestOf("test-wrapper-source");
    binding.viewAdjust = adjust;
    return binding;
}

[[nodiscard]] bool contains(const std::string& text, const std::string_view needle) {
    return text.find(needle) != std::string::npos;
}

void testWrapper(Expectations& expectations) {
    const auto effect = bloom::runtime::buildGpuOcioWrapperGlsl(
        syntheticProgram(OcioGpuProgramStage::ProcessEffect));
    expectations.expect(effect.succeeded(), "the effect wrapper builds");
    expectations.expect(contains(effect.source, "layout(local_size_x = 64) in;"),
                        "the wrapper fixes the workgroup size");
    expectations.expect(contains(effect.source,
                                 "layout(set = 1, binding = 0, rgba32f) uniform readonly "
                                 "image2D bloom_ocio_input;"),
                        "the effect wrapper binds the resident input at set 1 binding 0");
    expectations.expect(contains(effect.source, "imageStore(bloom_ocio_output, c, vec4(t.rgb * a, "
                                                "a));"),
                        "the effect wrapper publishes premultiplied RGBA32F");
    expectations.expect(!contains(effect.source, "bloom_ocio_quantize"),
                        "the effect wrapper never quantizes");
    expectations.expect(contains(effect.source, "bloom_ocio_status.flags[0] = 1u;"),
                        "the wrapper raises the status word on a non-finite value");
    expectations.expect(contains(effect.source, "bloom_ocio_transform(vec4(s, 1.0))"),
                        "the wrapper calls the extracted OCIO function on straight RGB");
    expectations.expect(contains(effect.source, "if (a == 0.0) { imageStore(bloom_ocio_output, c, "
                                                "p); return; }"),
                        "the effect wrapper copies an alpha-zero pixel through unchanged");
    expectations.expect(effect.entryPoint == "main", "the entry point is main");
    expectations.expect(effect.samplingVersion == bloom::color::kOcioGpuPreciseSamplingVersion,
                        "the wrapper records the shared precise-sampling version");
    expectations.expect(effect.sourceDigest == digestOf(effect.source),
                        "the wrapper source digest is exact");

    const auto display = bloom::runtime::buildGpuOcioWrapperGlsl(
        syntheticProgram(OcioGpuProgramStage::DisplayPacking));
    expectations.expect(display.succeeded(), "the display wrapper builds");
    expectations.expect(contains(display.source, "bloom_ocio_quantize"),
                        "the display wrapper quantizes straight RGBA8");
    expectations.expect(contains(display.source, "words[index] = r | (g << 8) | (b << 16) | "
                                                 "(qa << 24);"),
                        "the display wrapper packs the RGBA8 word");
    expectations.expect(!contains(display.source, "imageStore(bloom_ocio_output, c, p)"),
                        "the display wrapper keeps its distinct alpha-zero display semantics");
    expectations.expect(!contains(display.source, "bloom_ocio_view_adjust"),
                        "a neutral display wrapper emits no adjustment code");

    const ViewAdjust adjust{.exposure = 1.5, .gamma = 0.9};
    const auto adjusted = bloom::runtime::buildGpuOcioWrapperGlsl(
        syntheticProgram(OcioGpuProgramStage::DisplayPacking), adjust);
    expectations.expect(adjusted.succeeded(), "an adjusted display wrapper builds");
    expectations.expect(contains(adjusted.source, "bloom_ocio_view_adjust(t.r)"),
                        "the adjusted display wrapper applies the post-display adjustment");
    expectations.expect(contains(adjusted.source, "exp2(exposure)"),
                        "the adjustment applies exposure on the linear display light");
    expectations.expect(adjusted.sourceDigest != display.sourceDigest,
                        "the adjustment changes the wrapper source digest");
    expectations.expect(bloom::runtime::buildGpuOcioWrapperGlsl(
                            syntheticProgram(OcioGpuProgramStage::ProcessEffect), adjust)
                                .error == GpuOcioWrapperError::UnsupportedViewAdjust,
                        "a non-neutral adjustment on the ProcessEffect wrapper is refused");
    expectations.expect(bloom::runtime::buildGpuOcioWrapperGlsl(
                            syntheticProgram(OcioGpuProgramStage::DisplayPacking),
                            ViewAdjust{.exposure = 0.0, .gamma = 0.0})
                                .error == GpuOcioWrapperError::InvalidViewAdjust,
                        "an out-of-domain adjustment is refused by the wrapper");

    auto badFunction = syntheticProgram(OcioGpuProgramStage::ProcessEffect);
    badFunction.functionName = "bad-name";
    expectations.expect(bloom::runtime::buildGpuOcioWrapperGlsl(badFunction).error ==
                            GpuOcioWrapperError::InvalidFunctionName,
                        "an invalid function name is refused");
    auto empty = syntheticProgram(OcioGpuProgramStage::ProcessEffect);
    empty.shaderText.clear();
    expectations.expect(bloom::runtime::buildGpuOcioWrapperGlsl(empty).error ==
                            GpuOcioWrapperError::InvalidProgram,
                        "an empty shader is refused");
    auto foreignSet = syntheticProgram(OcioGpuProgramStage::ProcessEffect);
    foreignSet.descriptorSetIndex = 1;
    expectations.expect(bloom::runtime::buildGpuOcioWrapperGlsl(foreignSet).error ==
                            GpuOcioWrapperError::UnsupportedDescriptorSet,
                        "a non-zero OCIO descriptor set is refused");
}

void testIdentity(Expectations& expectations) {
    const auto program = syntheticProgram(OcioGpuProgramStage::ProcessEffect);
    const auto artifact = syntheticArtifact();
    const std::vector<std::byte> uniforms{std::byte{1}, std::byte{2}, std::byte{3}, std::byte{4}};
    const GpuOcioCommandIdentityParts base{
        .encoding = GpuOcioOutputEncoding::FinalRgba32f,
        .geometry = GpuOcioCommandGeometry{4, 3},
        .programContentIdentity = program.contentIdentity,
        .programResourceDigest = program.resourceDigest,
        .programShaderTextDigest = program.shaderTextDigest,
        .uniformSnapshot = uniforms,
        .artifactDigest = artifact.spirvDigest,
        .wrapperVersion = "test-sampling-v1",
        .wrapperSourceDigest = digestOf("test-wrapper-source"),
        .viewAdjust = ViewAdjust{},
    };
    const auto identity = bloom::runtime::computeGpuOcioCommandIdentity(base);
    expectations.expect(identity != Sha256Digest{}, "the identity is non-empty");
    expectations.expect(bloom::runtime::computeGpuOcioCommandIdentity(base) == identity,
                        "the identity is deterministic");

    auto other = base;
    other.encoding = GpuOcioOutputEncoding::DisplayRgba8;
    expectations.expect(bloom::runtime::computeGpuOcioCommandIdentity(other) != identity,
                        "the output encoding changes the identity");
    other = base;
    other.geometry = GpuOcioCommandGeometry{4, 4};
    expectations.expect(bloom::runtime::computeGpuOcioCommandIdentity(other) != identity,
                        "the geometry changes the identity");
    other = base;
    const std::vector<std::byte> changedUniforms{std::byte{1}, std::byte{2}, std::byte{3},
                                                 std::byte{5}};
    other.uniformSnapshot = changedUniforms;
    expectations.expect(bloom::runtime::computeGpuOcioCommandIdentity(other) != identity,
                        "the uniform snapshot changes the identity");
    other = base;
    other.artifactDigest = program.resourceDigest;
    expectations.expect(bloom::runtime::computeGpuOcioCommandIdentity(other) != identity,
                        "the artifact digest changes the identity");
    other = base;
    other.wrapperSourceDigest = digestOf("other-wrapper-source");
    expectations.expect(bloom::runtime::computeGpuOcioCommandIdentity(other) != identity,
                        "the wrapper source digest changes the identity");
    other = base;
    other.viewAdjust = ViewAdjust{.exposure = 1.0, .gamma = 1.0};
    expectations.expect(bloom::runtime::computeGpuOcioCommandIdentity(other) != identity,
                        "the view adjustment changes the identity");
    other = base;
    other.viewAdjust = ViewAdjust{.exposure = 0.0, .gamma = 2.0};
    expectations.expect(bloom::runtime::computeGpuOcioCommandIdentity(other) != identity,
                        "the view gamma changes the identity");
}

void testPreparedCommand(Expectations& expectations) {
    const GpuOcioCommandGeometry geometry{4, 3};
    auto effect =
        PreparedGpuOcioCommand::prepare(syntheticProgram(OcioGpuProgramStage::ProcessEffect),
                                        syntheticArtifact(), geometry, syntheticBinding());
    expectations.expect(effect.hasValue(), "an effect command prepares");
    if (effect) {
        expectations.expect(effect.command->encoding() == GpuOcioOutputEncoding::FinalRgba32f,
                            "the effect command is FinalRgba32f");
        expectations.expect(effect.command->identity() != Sha256Digest{},
                            "the prepared command has an identity");
        expectations.expect(effect.command->spirvWords().size() == 1,
                            "the prepared command owns whole SPIR-V words");
        expectations.expect(effect.command->retainedBytes() >= 4,
                            "the prepared command accounts its retained bytes");
    }
    auto display =
        PreparedGpuOcioCommand::prepare(syntheticProgram(OcioGpuProgramStage::DisplayPacking),
                                        syntheticArtifact(), geometry, syntheticBinding());
    expectations.expect(display.hasValue(), "a display command prepares");
    if (display && effect) {
        expectations.expect(display.command->encoding() == GpuOcioOutputEncoding::DisplayRgba8,
                            "the display command is DisplayRgba8");
        expectations.expect(display.command->identity() != effect.command->identity(),
                            "FinalRgba32f and DisplayRgba8 identities stay distinct");
        expectations.expect(display.command->viewAdjust().neutral(),
                            "a neutral display command carries a neutral adjustment");
        expectations.expect(effect.command->viewAdjust().neutral(),
                            "a process command carries a neutral adjustment");
    }
    const ViewAdjust adjusted{.exposure = 1.25, .gamma = 0.8};
    const auto adjustedDisplay =
        PreparedGpuOcioCommand::prepare(syntheticProgram(OcioGpuProgramStage::DisplayPacking),
                                        syntheticArtifact(), geometry, syntheticBinding(adjusted));
    expectations.expect(adjustedDisplay.hasValue() &&
                            adjustedDisplay.command->viewAdjust() == adjusted,
                        "the display command exposes its exact bound adjustment");
    if (adjustedDisplay && display) {
        expectations.expect(adjustedDisplay.command->identity() != display.command->identity(),
                            "a changed adjustment changes the display command identity");
    }
    expectations.expect(
        PreparedGpuOcioCommand::prepare(syntheticProgram(OcioGpuProgramStage::ProcessEffect),
                                        syntheticArtifact(), geometry, syntheticBinding(adjusted))
                .error == GpuOcioCommandError::UnsupportedViewAdjust,
        "a non-neutral adjustment on the ProcessEffect arm is refused");
    expectations.expect(
        PreparedGpuOcioCommand::prepare(syntheticProgram(OcioGpuProgramStage::DisplayPacking),
                                        syntheticArtifact(), geometry,
                                        syntheticBinding(ViewAdjust{.exposure = 0.0, .gamma = 0.0}))
                .error == GpuOcioCommandError::InvalidViewAdjust,
        "an out-of-domain adjustment is refused");

    expectations.expect(PreparedGpuOcioCommand::prepare(
                            syntheticProgram(OcioGpuProgramStage::ProcessEffect),
                            syntheticArtifact(), GpuOcioCommandGeometry{0, 3}, syntheticBinding())
                                .error == GpuOcioCommandError::InvalidGeometry,
                        "an empty geometry is refused");
    auto badDigest = syntheticArtifact();
    badDigest.spirvDigest = digestOf("wrong");
    expectations.expect(
        PreparedGpuOcioCommand::prepare(syntheticProgram(OcioGpuProgramStage::ProcessEffect),
                                        badDigest, geometry, syntheticBinding())
                .error == GpuOcioCommandError::ArtifactDigestMismatch,
        "a mismatched artifact digest is refused");
    auto emptyArtifact = syntheticArtifact();
    emptyArtifact.spirv.clear();
    expectations.expect(
        PreparedGpuOcioCommand::prepare(syntheticProgram(OcioGpuProgramStage::ProcessEffect),
                                        emptyArtifact, geometry, syntheticBinding())
                .error == GpuOcioCommandError::InvalidArtifact,
        "an empty artifact is refused");
    auto invalidProgram = syntheticProgram(OcioGpuProgramStage::ProcessEffect);
    invalidProgram.semanticsId.clear();
    expectations.expect(PreparedGpuOcioCommand::prepare(invalidProgram, syntheticArtifact(),
                                                        geometry, syntheticBinding())
                                .error == GpuOcioCommandError::InvalidProgram,
                        "an invalid descriptor is refused");
    auto mismatchedUniforms = syntheticProgram(OcioGpuProgramStage::ProcessEffect);
    mismatchedUniforms.uniformBufferSize = 16;
    mismatchedUniforms.uniformBufferData.assign(4, std::byte{0});
    expectations.expect(PreparedGpuOcioCommand::prepare(mismatchedUniforms, syntheticArtifact(),
                                                        geometry, syntheticBinding())
                                .error == GpuOcioCommandError::UniformSnapshotMismatch,
                        "a mismatched uniform snapshot is refused");
}

#ifdef BLOOM_GPUSHADER_TOOLS_DIR
[[nodiscard]] GpuOcioCompileOptions compileOptions() {
    GpuOcioCompileOptions options;
    options.glslangValidatorPath = std::string(BLOOM_GPUSHADER_TOOLS_DIR) + "/glslangValidator";
    options.spirvValPath = std::string(BLOOM_GPUSHADER_TOOLS_DIR) + "/spirv-val";
    return options;
}

void testPreparer(Expectations& expectations,
                  const bloom::color::ResolvedBloomNeutralConfig& neutral,
                  const std::optional<bloom::color::ResolvedBloomNeutralConfig>& aces) {
    GpuOcioProgramPreparer preparer;
    const auto options = compileOptions();
    const bloom::color::ResolvedBloomNeutralConfig& cstConfig = aces.has_value() ? *aces : neutral;
    GpuOcioTransformSpec cst;
    cst.kind = GpuOcioTransformKind::Cst;
    if (aces.has_value()) {
        cst.fromId = "ACES2065-1";
        cst.toId = "ACEScg";
    } else {
        cst.fromId = std::string(neutral.processColorSpaceId());
        cst.toId = "srgb_rec709_display";
    }
    const GpuOcioCommandGeometry geometry{4, 3};
    const auto first = preparer.prepare(cstConfig, cst, geometry, options);
    expectations.expect(first.hasValue(), "the CST command prepares off-device");
    if (!first) {
        std::cerr << "prepare diagnostic: " << first.diagnostic << '\n';
        return;
    }
    expectations.expect(first.command->encoding() == GpuOcioOutputEncoding::FinalRgba32f,
                        "the CST command is FinalRgba32f");
    auto counters = preparer.counters();
    expectations.expect(counters.compiles == 1 && counters.extractions == 1,
                        "the first prepare extracts and compiles once");
    const auto second = preparer.prepare(cstConfig, cst, geometry, options);
    expectations.expect(second.hasValue() && second.command == first.command,
                        "the warm prepare returns the cached immutable command");
    counters = preparer.counters();
    expectations.expect(counters.cacheHits == 1 && counters.compiles == 1 &&
                            counters.extractions == 1,
                        "the warm prepare performs no extraction or compile");

    const auto changedGeometry =
        preparer.prepare(cstConfig, cst, GpuOcioCommandGeometry{8, 3}, options);
    expectations.expect(changedGeometry.hasValue(), "a changed geometry prepares");
    if (changedGeometry) {
        expectations.expect(changedGeometry.command->identity() != first.command->identity(),
                            "a changed geometry invalidates the command identity");
    }

    GpuOcioTransformSpec adjust;
    adjust.kind = GpuOcioTransformKind::ExposureContrast;
    adjust.fromId = aces.has_value() ? "ACEScg" : std::string(neutral.processColorSpaceId());
    adjust.exposure = 0.5;
    adjust.contrast = 1.0;
    const auto adjustA = preparer.prepare(cstConfig, adjust, geometry, options);
    adjust.exposure = 0.7;
    const auto adjustB = preparer.prepare(cstConfig, adjust, geometry, options);
    expectations.expect(adjustA.hasValue() && adjustB.hasValue(),
                        "the exposure/contrast view-adjust commands prepare");
    if (adjustA && adjustB) {
        expectations.expect(adjustA.command->identity() != adjustB.command->identity(),
                            "a changed uniform value invalidates the command identity");
    }

    GpuOcioCompileOptions noTools = options;
    noTools.glslangValidatorPath.clear();
    expectations.expect(preparer.prepare(cstConfig, cst, geometry, noTools).error ==
                            GpuOcioPreparationError::InvalidRequest,
                        "missing tool paths are refused");
    expectations.expect(
        preparer.prepare(cstConfig, cst, geometry, options, []() { return true; }).error ==
            GpuOcioPreparationError::CompileCancelled,
        "cancellation before extraction is honoured");

    GpuOcioProgramPreparer bounded({.maxEntries = 1, .maxBytes = 1ULL << 30});
    (void)bounded.prepare(cstConfig, cst, geometry, options);
    (void)bounded.prepare(cstConfig, cst, GpuOcioCommandGeometry{8, 3}, options);
    const auto boundedCounters = bounded.counters();
    expectations.expect(boundedCounters.cacheEntries == 1 && boundedCounters.evictions == 1,
                        "the preparation cache is bounded and evicts");

    // Zero/oversized limit and zero-deadline arguments are refused consistently, before any cache
    // hit can bypass them.
    auto zeroSource = options;
    zeroSource.maxSourceBytes = 0;
    expectations.expect(preparer.prepare(cstConfig, cst, geometry, zeroSource).error ==
                            GpuOcioPreparationError::InvalidRequest,
                        "a zero source-byte limit is refused");
    auto zeroSpirv = options;
    zeroSpirv.maxSpirvBytes = 0;
    expectations.expect(preparer.prepare(cstConfig, cst, geometry, zeroSpirv).error ==
                            GpuOcioPreparationError::InvalidRequest,
                        "a zero SPIR-V limit is refused");
    auto zeroDeadline = options;
    zeroDeadline.deadline = std::chrono::milliseconds::zero();
    expectations.expect(preparer.prepare(cstConfig, cst, geometry, zeroDeadline).error ==
                            GpuOcioPreparationError::InvalidRequest,
                        "a non-positive deadline is refused");
    auto raised = options;
    raised.maxSpirvBytes = 128ULL * 1024ULL * 1024ULL;
    expectations.expect(preparer.prepare(cstConfig, cst, geometry, raised).error ==
                            GpuOcioPreparationError::InvalidRequest,
                        "a limit above the hard ceiling is refused");

    // Warm stricter limit: a lower per-call SPIR-V ceiling is a distinct key, so it must NOT reuse
    // the cached artifact; the compile is re-run and fails its own ceiling.
    const std::size_t artifactBytes = first.command->artifact().spirv.size();
    const std::uint64_t bytesBeforeStrict = preparer.counters().cacheBytes;
    auto strict = options;
    strict.maxSpirvBytes = artifactBytes > 0 ? artifactBytes - 1 : 0;
    const auto strictResult = preparer.prepare(cstConfig, cst, geometry, strict);
    expectations.expect(strictResult.error == GpuOcioPreparationError::CompileFailed &&
                            strictResult.command == nullptr,
                        "a stricter SPIR-V limit never reuses the larger cached artifact");
    expectations.expect(preparer.counters().cacheBytes == bytesBeforeStrict,
                        "a refused stricter request retains no bytes");
    expectations.expect(preparer.prepare(cstConfig, cst, geometry, options).hasValue(),
                        "the original cached command survives a refused stricter request");

    // Cancellation before publication retains nothing.
    GpuOcioProgramPreparer cancelled;
    const GpuOcioCommandGeometry cancelGeometry{7, 4};
    expectations.expect(
        cancelled.prepare(cstConfig, cst, cancelGeometry, options, []() { return true; }).error ==
            GpuOcioPreparationError::CompileCancelled,
        "a cancelled prepare is refused");
    expectations.expect(cancelled.counters().cacheEntries == 0,
                        "a cancelled prepare retains no command");
    const auto afterCancel = cancelled.prepare(cstConfig, cst, cancelGeometry, options);
    expectations.expect(afterCancel.hasValue() && cancelled.counters().cacheEntries == 1,
                        "a later request still prepares after a cancelled one");

    // Deterministic concurrent same-key regression: all threads return the same canonical command,
    // the cache holds exactly one entry, and its byte charge equals that one command's retained
    // bytes (no double charge, no unnecessary eviction).
    GpuOcioProgramPreparer concurrent;
    const GpuOcioCommandGeometry concurrentGeometry{6, 5};
    constexpr int kThreads = 8;
    std::array<std::shared_ptr<const PreparedGpuOcioCommand>, kThreads> results{};
    std::latch start{1};
    std::vector<std::thread> workers;
    workers.reserve(kThreads);
    for (int index = 0; index < kThreads; ++index) {
        workers.emplace_back([&, index]() {
            start.wait();
            auto result = concurrent.prepare(cstConfig, cst, concurrentGeometry, options);
            results[static_cast<std::size_t>(index)] = std::move(result.command);
        });
    }
    start.count_down();
    for (auto& worker : workers) {
        worker.join();
    }
    std::size_t nullResults = 0;
    const auto* canonical = results[0].get();
    for (const auto& result : results) {
        if (result == nullptr) {
            ++nullResults;
        }
    }
    expectations.expect(nullResults == 0, "every concurrent same-key prepare succeeds");
    for (const auto& result : results) {
        expectations.expect(result.get() == canonical,
                            "every concurrent same-key prepare returns the canonical command");
    }
    const auto concurrentCounters = concurrent.counters();
    expectations.expect(concurrentCounters.cacheEntries == 1,
                        "the concurrent same-key cache holds exactly one entry");
    if (canonical != nullptr) {
        expectations.expect(concurrentCounters.cacheBytes == canonical->retainedBytes(),
                            "the concurrent same-key byte charge equals the one retained command");
    }
    expectations.expect(concurrentCounters.evictions == 0,
                        "a duplicate same-key miss never evicts");
}
#endif

} // namespace

int main() {
    Expectations expectations;
    testWrapper(expectations);
    testIdentity(expectations);
    testPreparedCommand(expectations);

#ifdef BLOOM_GPUSHADER_TOOLS_DIR
    auto neutralResolution = bloom::color::resolveBloomNeutralV1BuiltIn(
        bloom::color::OcioConfigLocatorKind::BloomBuiltIn, bloom::color::kBloomNeutralV1ConfigUri,
        bloom::color::kBloomNeutralV1ConfigDigest);
    auto neutral = std::move(neutralResolution).takeResolved();
    if (!neutral.has_value()) {
        std::cerr << "FAILED: the Bloom Neutral built-in does not resolve\n";
        return 1;
    }
    std::optional<bloom::color::ResolvedBloomNeutralConfig> aces;
    if (const auto revision = bloom::color::ocioBuiltInContentRevision(
            bloom::color::OcioConfigLocatorKind::BloomBuiltIn, bloom::color::kAcesCgV1ConfigUri);
        revision.has_value()) {
        auto resolution =
            bloom::color::resolveOcioBuiltIn(bloom::color::OcioConfigLocatorKind::BloomBuiltIn,
                                             bloom::color::kAcesCgV1ConfigUri, *revision, "ACEScg");
        aces = std::move(resolution).takeResolved();
    }
    testPreparer(expectations, *neutral, aces);
#else
    std::cout << "NOTE: BLOOM_GPUSHADER_TOOLS_DIR is not set; the preparation-cache cases are "
                 "skipped\n";
#endif

    if (expectations.failures() != 0) {
        std::cerr << expectations.failures() << " OCIO command expectation(s) failed\n";
        return 1;
    }
    std::cout << "PASS: runtime OCIO command and preparation\n";
    return 0;
}
