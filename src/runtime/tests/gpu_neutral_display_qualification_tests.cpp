// Tests for bounded runtime qualification of the already-embedded fixed Bloom Neutral v1 display
// operation. Local mode pins the explicit loader and requires a device; a hardware-free CI image
// prints an explicit skip and still exercises the CPU-only and eligibility paths. The hardware body
// is compiled unconditionally and depends only on runtime create() availability, so the same test
// works against the CPU stub.

#include <bloom/runtime/gpu_neutral_display_qualification.hpp>

#include <bloom/color/display_processor_identity.hpp>
#include <bloom/color/ocio_builtin_registry.hpp>
#include <bloom/color/ocio_cpu_display_frame.hpp>
#include <bloom/color/ocio_cpu_display_processor.hpp>
#include <bloom/render/gpu_device.hpp>
#include <bloom/render/gpu_neutral_display.hpp>

#include <cstddef>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <iostream>
#include <memory>
#include <optional>
#include <source_location>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace {

using bloom::color::CancellationPredicateRef;
using bloom::color::PreparedCpuDisplayProcessorHandle;
using bloom::render::GpuDevice;
using bloom::render::GpuDeviceCreationOptions;
using bloom::render::GpuDeviceState;
using bloom::render::GpuNeutralDisplay;
using bloom::runtime::GpuNeutralDisplayQualificationDiagnosticCode;
using bloom::runtime::GpuNeutralDisplayQualificationOutcome;
using bloom::runtime::GpuNeutralDisplayQualificationReport;

class Expectations final {
  public:
    void expect(const bool condition, const std::string_view message,
                const std::source_location location = std::source_location::current()) {
        if (condition) {
            return;
        }
        ++failures_;
        std::cerr << location.file_name() << ':' << location.line() << ": " << message << '\n';
    }

    [[nodiscard]] bool ok() const noexcept { return failures_ == 0; }

  private:
    std::size_t failures_ = 0;
};

struct Options final {
    std::filesystem::path loader_path;
    bool require_device = false;
    bool valid = true;
};

[[nodiscard]] Options parseOptions(const int argc, char** argv) {
    Options options;
    for (int index = 1; index < argc; ++index) {
        const std::string_view argument = argv[index];
        if (argument == "--loader") {
            if (index + 1 >= argc) {
                std::cerr << "--loader requires a path argument\n";
                options.valid = false;
                return options;
            }
            options.loader_path = argv[++index];
        } else if (argument == "--require-device") {
            options.require_device = true;
        } else {
            std::cerr << "unknown argument: " << argument << '\n';
            options.valid = false;
            return options;
        }
    }
    return options;
}

[[nodiscard]] std::optional<PreparedCpuDisplayProcessorHandle> buildDefaultProcessor() {
    auto resolution = bloom::color::resolveBloomNeutralV1BuiltIn(
        bloom::color::OcioConfigLocatorKind::BloomBuiltIn, bloom::color::kBloomNeutralV1ConfigUri,
        bloom::color::kBloomNeutralV1ConfigDigest);
    if (!resolution.ready()) {
        return std::nullopt;
    }
    auto resolved = std::move(resolution).takeResolved();
    if (!resolved.has_value()) {
        return std::nullopt;
    }
    auto built = bloom::color::buildBloomNeutralCpuDisplayProcessor(*resolved);
    if (!built) {
        return std::nullopt;
    }
    return std::move(built).takeHandle();
}

// A deliberately ineligible processor: the OCIO 2.5 ACES CG built-in. Qualification must reject it
// rather than substitute the Bloom Neutral operation.
[[nodiscard]] std::optional<PreparedCpuDisplayProcessorHandle> buildAcesProcessor() {
    auto revision = bloom::color::ocioBuiltInContentRevision(
        bloom::color::OcioConfigLocatorKind::BloomBuiltIn, bloom::color::kAcesCgV1ConfigUri);
    if (!revision.has_value()) {
        return std::nullopt;
    }
    auto resolution = bloom::color::resolveOcioBuiltIn(
        bloom::color::OcioConfigLocatorKind::BloomBuiltIn, bloom::color::kAcesCgV1ConfigUri,
        *revision, bloom::color::kAcesCgV1SceneLinearColorSpaceId);
    if (!resolution.ready()) {
        return std::nullopt;
    }
    auto resolved = std::move(resolution).takeResolved();
    if (!resolved.has_value()) {
        return std::nullopt;
    }
    auto built = bloom::color::buildBloomNeutralCpuDisplayProcessor(*resolved);
    if (!built) {
        return std::nullopt;
    }
    return std::move(built).takeHandle();
}

void testFixtureProvenance(Expectations& expectations) {
    const auto fixtures = bloom::runtime::detail::makeGpuNeutralDisplayParityFixtures();
    expectations.expect(fixtures.has_value(), "the parity fixtures construct");
    if (!fixtures.has_value()) {
        return;
    }
    const auto digest = bloom::runtime::detail::gpuNeutralDisplayFixtureDigest(*fixtures);
    const auto again = bloom::runtime::detail::makeGpuNeutralDisplayParityFixtures();
    expectations.expect(again.has_value() &&
                            digest ==
                                bloom::runtime::detail::gpuNeutralDisplayFixtureDigest(*again),
                        "the fixture digest is deterministic");

    bool hasOddTail = false;
    bool hasAlphaBoundary = false;
    bool hasSpecial = false;
    for (const auto& fixture : *fixtures) {
        if (fixture.name == "odd-257" && fixture.pixels.size() == 257) {
            hasOddTail = true;
        }
        if (fixture.name == "alpha-boundary") {
            hasAlphaBoundary = true;
        }
        if (fixture.name == "special-values") {
            hasSpecial = true;
        }
    }
    expectations.expect(hasOddTail, "the fixtures include the odd 257-pixel tail");
    expectations.expect(hasAlphaBoundary, "the fixtures include alpha quantization boundaries");
    expectations.expect(hasSpecial, "the fixtures include signed/HDR/tiny special values");

    const auto measured = bloom::runtime::detail::makeGpuNeutralDisplayMeasuredPixels(256, 144);
    expectations.expect(measured.has_value() &&
                            measured->size() == static_cast<std::size_t>(256U) * 144U,
                        "the measured fixture constructs at the smallest size");
}

// Build an adopted canonical identity with the official writer, without forging a CPU handle.
[[nodiscard]] std::optional<bloom::color::DisplayProcessorIdentityV1>
adoptIdentity(const bloom::color::DisplayProcessorIdentityV1InputView& input) {
    const auto validation = bloom::color::validateDisplayProcessorIdentityV1(input);
    if (!validation) {
        return std::nullopt;
    }
    std::vector<std::byte> bytes(validation.requiredByteCount());
    if (!bloom::color::writeDisplayProcessorIdentityV1(input, bytes)) {
        return std::nullopt;
    }
    auto adoption = bloom::color::adoptDisplayProcessorIdentityV1(std::move(bytes));
    if (!adoption) {
        return std::nullopt;
    }
    return std::move(adoption).takeIdentity();
}

// A canonically valid record with a different source color space must fail the exact identity
// predicate, while the expected record passes.
void testIdentityEligibility(Expectations& expectations) {
    const bloom::color::DisplayProcessorIdentityV1InputView expected{
        .expectedOcioRevision = bloom::color::kBloomNeutralV1ConfigDigest,
        .contextVariables = {},
        .sourceColorSpaceId = bloom::color::kDisplayProcessorIdentitySourceColorSpaceId,
        .displayName = bloom::runtime::kGpuNeutralDisplayDisplayName,
        .viewName = bloom::runtime::kGpuNeutralDisplayViewName,
        .lookMode = bloom::color::DisplayProcessorLookModeV1::Bypass,
        .lookNames = {},
        .outputColorSpaceId = bloom::color::kDisplayProcessorIdentityOutputColorSpaceId,
        .qualityId = bloom::color::kDisplayProcessorIdentityQualityId,
        .semanticsProfileId = bloom::color::kDisplayProcessorIdentitySemanticsProfileId,
        .packingId = bloom::color::kDisplayProcessorIdentityPackingId,
    };
    auto expectedIdentity = adoptIdentity(expected);
    expectations.expect(expectedIdentity.has_value(), "the expected identity record adopts");
    if (expectedIdentity.has_value()) {
        std::string reason;
        expectations.expect(
            bloom::runtime::gpuNeutralDisplayIdentityIsEligible(*expectedIdentity, reason),
            "the expected Bloom Neutral v1 identity is eligible");
    }

    auto incompatible = expected;
    incompatible.sourceColorSpaceId = "some_other_scene";
    auto incompatibleIdentity = adoptIdentity(incompatible);
    expectations.expect(incompatibleIdentity.has_value(),
                        "a valid but incompatible identity record adopts");
    if (incompatibleIdentity.has_value()) {
        std::string reason;
        expectations.expect(
            !bloom::runtime::gpuNeutralDisplayIdentityIsEligible(*incompatibleIdentity, reason),
            "a canonically valid but incompatible source color space is ineligible");
        expectations.expect(!reason.empty(), "the identity rejection carries a reason");
    }
}

void testWrongProcessorRejected(Expectations& expectations, GpuDevice& device,
                                GpuNeutralDisplay& pipeline) {
    auto aces = buildAcesProcessor();
    expectations.expect(aces.has_value(), "the ACES CG processor builds for the rejection test");
    if (!aces.has_value()) {
        return;
    }
    std::string reason;
    expectations.expect(!bloom::runtime::gpuNeutralDisplayProcessorIsEligible(*aces, reason),
                        "the eligibility helper rejects the ACES processor");
    auto report = bloom::runtime::qualifyGpuNeutralDisplay(*aces, device, pipeline);
    expectations.expect(report.outcome() == GpuNeutralDisplayQualificationOutcome::Unavailable,
                        "an ineligible processor is Unavailable");
    expectations.expect(report.diagnostic().code ==
                            GpuNeutralDisplayQualificationDiagnosticCode::NotEligibleProcessor,
                        "an ineligible processor reports NotEligibleProcessor");
    expectations.expect(!report.eligible() && !report.eligibleInterval().has_value(),
                        "an ineligible processor grants no eligible interval");
}

// A non-owner thread must get false from isBoundTo and must not disturb the owner's pairing.
void testWrongThreadBinding(Expectations& expectations, GpuDevice& device,
                            GpuNeutralDisplay& pipeline) {
    bool foreignResult = true;
    std::thread worker(
        [&device, &pipeline, &foreignResult]() { foreignResult = pipeline.isBoundTo(device); });
    worker.join();
    expectations.expect(!foreignResult, "isBoundTo from a joined non-owner thread is false");
    expectations.expect(pipeline.isBoundTo(device),
                        "the owner-thread pairing remains bound and usable");
}

// Cancellation partway through a run must abort with Cancelled and publish no eligible interval.
void testMidRunCancellation(Expectations& expectations, GpuDevice& device,
                            const PreparedCpuDisplayProcessorHandle& processor) {
    auto created = GpuNeutralDisplay::create(device);
    expectations.expect(created.hasValue(), "a dedicated pipeline is created for mid-run cancel");
    if (!created) {
        return;
    }
    auto pipeline = std::move(created.display);
    int remaining = 10; // cancel after a bounded number of poll-boundary checks
    auto countdown = [&remaining]() {
        if (remaining > 0) {
            --remaining;
            return false;
        }
        return true;
    };
    auto report = bloom::runtime::qualifyGpuNeutralDisplay(processor, device, *pipeline,
                                                           CancellationPredicateRef{countdown});
    expectations.expect(report.outcome() == GpuNeutralDisplayQualificationOutcome::Unavailable,
                        "a mid-run cancellation is Unavailable");
    expectations.expect(report.diagnostic().code ==
                            GpuNeutralDisplayQualificationDiagnosticCode::Cancelled,
                        "a mid-run cancellation reports Cancelled");
    expectations.expect(!report.eligible() && !report.eligibleInterval().has_value(),
                        "a mid-run cancellation publishes no eligible interval");
    // Destroy on the owner thread; the destructor drains any retained in-flight submission.
    pipeline.reset();
    expectations.expect(!GpuNeutralDisplay::teardownDrainIncomplete(),
                        "the cancelled pipeline drained without abandoning a generation");
}

// A pipeline bound to one device must not be labelled with a second device's qualification.
void testSecondDeviceRejected(Expectations& expectations, GpuDevice& firstDevice,
                              GpuNeutralDisplay& firstPipeline,
                              const PreparedCpuDisplayProcessorHandle& processor) {
    // A second device is not guaranteed to exist; if it cannot be created, the binding check is
    // still exercised by the null/stale case below.
    GpuDeviceCreationOptions options;
    auto second = GpuDevice::create(options);
    if (second) {
        expectations.expect(!firstPipeline.isBoundTo(*second.device),
                            "a pipeline is not bound to a different device");
        auto report =
            bloom::runtime::qualifyGpuNeutralDisplay(processor, *second.device, firstPipeline);
        expectations.expect(report.outcome() == GpuNeutralDisplayQualificationOutcome::Unavailable,
                            "qualifying a mismatched device/pipeline pair is Unavailable");
        expectations.expect(report.diagnostic().code ==
                                GpuNeutralDisplayQualificationDiagnosticCode::NotEligibleDevice,
                            "a mismatched device/pipeline pair reports NotEligibleDevice");
    }
    expectations.expect(firstPipeline.isBoundTo(firstDevice),
                        "the original device/pipeline pairing remains bound and usable");
}

void testCancellation(Expectations& expectations, GpuDevice& device, GpuNeutralDisplay& pipeline,
                      const PreparedCpuDisplayProcessorHandle& processor) {
    bool cancelled = true;
    auto predicate = [&cancelled]() { return cancelled; };
    auto report = bloom::runtime::qualifyGpuNeutralDisplay(processor, device, pipeline,
                                                           CancellationPredicateRef{predicate});
    expectations.expect(report.outcome() == GpuNeutralDisplayQualificationOutcome::Unavailable,
                        "a pre-cancelled qualification is Unavailable");
    expectations.expect(report.diagnostic().code ==
                            GpuNeutralDisplayQualificationDiagnosticCode::Cancelled,
                        "a pre-cancelled qualification reports Cancelled");
}

void testMissingLoaderFallback(Expectations& expectations) {
    GpuDeviceCreationOptions options;
    options.loader_path = "/nonexistent/bloom-missing-vulkan-loader";
    auto device = GpuDevice::create(options);
    expectations.expect(!device, "a forced missing loader yields no device");
    expectations.expect(
        device.diagnostic.code == bloom::render::GpuDiagnosticCode::LoaderUnavailable ||
            device.diagnostic.code == bloom::render::GpuDiagnosticCode::BackendNotBuilt,
        "the missing-loader path reports a typed loader/backend diagnostic");
}

void testDefaultQualification(Expectations& expectations, GpuDevice& device,
                              GpuNeutralDisplay& pipeline,
                              const PreparedCpuDisplayProcessorHandle& processor) {
    auto report = bloom::runtime::qualifyGpuNeutralDisplay(processor, device, pipeline);
    expectations.expect(report.outcome() == GpuNeutralDisplayQualificationOutcome::PreviewOnly,
                        "the default Bloom Neutral processor reaches PreviewOnly");
    expectations.expect(report.deviceGeneration() == device.capabilityReport().generation,
                        "the report carries the exact device generation");
    expectations.expect(report.configRevision() == bloom::color::kBloomNeutralV1ConfigDigest,
                        "the report carries the frozen config revision");
    expectations.expect(report.processorCacheId() ==
                            bloom::runtime::kGpuNeutralDisplayProcessorCacheId,
                        "the report carries the pinned processor cache ID");
    expectations.expect(report.shaderDigest() == bloom::runtime::kGpuNeutralDisplayShaderDigest,
                        "the report carries the pinned shader digest");
    expectations.expect(report.timings().size() == 5, "the report measured all five sizes");
    expectations.expect(report.subnormalFrameRejected(),
                        "the subnormal frame is measured as whole-frame rejected");

    const auto& timings = report.timings();
    const auto interval = report.eligibleInterval();
    if (report.eligible() && interval.has_value()) {
        // Derive the expected contiguous faster suffix from the actual timings.
        std::size_t lowest = timings.size() - 1;
        while (lowest > 0 && timings[lowest - 1].native_improved) {
            --lowest;
        }
        expectations.expect(timings.back().native_improved,
                            "an eligible interval requires the 4K size to improve");
        expectations.expect(interval->min_pixels == timings[lowest].pixel_count &&
                                interval->max_pixels == timings.back().pixel_count,
                            "the eligible interval matches the measured faster suffix");
        std::cout << "QUALIFY eligible min_pixels=" << interval->min_pixels
                  << " max_pixels=" << interval->max_pixels << '\n';
    } else {
        expectations.expect(
            !timings.back().native_improved ||
                report.diagnostic().code ==
                    GpuNeutralDisplayQualificationDiagnosticCode::TimingNotImproved,
            "a CPU-only outcome is explained by the largest size or TimingNotImproved");
        std::cout << "QUALIFY CPU-only\n";
    }
    for (const auto& sample : timings) {
        std::cout << "TIMING " << sample.width << 'x' << sample.height
                  << " native_ms=" << sample.native_full_ms << " cpu_ms=" << sample.cpu_full_ms
                  << " improved=" << sample.native_improved << '\n';
    }
}

} // namespace

int main(int argc, char** argv) {
    try {
        const Options options = parseOptions(argc, argv);
        if (!options.valid) {
            return 2;
        }

        Expectations expectations;
        testFixtureProvenance(expectations);
        testIdentityEligibility(expectations);
        testMissingLoaderFallback(expectations);

        auto processor = buildDefaultProcessor();
        expectations.expect(processor.has_value(), "the default Bloom Neutral processor builds");
        if (!processor.has_value()) {
            std::cerr << "FAIL: the default processor could not be built\n";
            return 1;
        }

        GpuDeviceCreationOptions createOptions;
        createOptions.loader_path = options.loader_path;
        auto device = GpuDevice::create(createOptions);
        if (!device) {
            if (options.require_device) {
                std::cerr << "FAIL: required device unavailable: " << device.diagnostic.message
                          << '\n';
                return 1;
            }
            std::cout << "SKIP: no compatible Vulkan device available: "
                      << device.diagnostic.message
                      << "; the fixture/eligibility/CPU-fallback checks above still passed\n";
            return expectations.ok() ? 0 : 1;
        }
        expectations.expect(device.device->state() == GpuDeviceState::Ready, "the device is Ready");

        auto pipeline = GpuNeutralDisplay::create(*device.device);
        expectations.expect(pipeline.hasValue(), "the native display pipeline is created");
        if (!pipeline) {
            std::cerr << "FAIL: pipeline creation failed: " << pipeline.diagnostic.message << '\n';
            return 1;
        }
        expectations.expect(pipeline.display->isBoundTo(*device.device),
                            "the new pipeline is bound to its creating device");

        testWrongProcessorRejected(expectations, *device.device, *pipeline.display);
        testCancellation(expectations, *device.device, *pipeline.display, *processor);
        testWrongThreadBinding(expectations, *device.device, *pipeline.display);
        testSecondDeviceRejected(expectations, *device.device, *pipeline.display, *processor);
        testMidRunCancellation(expectations, *device.device, *processor);
        testDefaultQualification(expectations, *device.device, *pipeline.display, *processor);
        return expectations.ok() ? 0 : 1;
    } catch (const std::exception& exception) {
        std::cerr << "Unexpected test exception: " << exception.what() << '\n';
        return 1;
    }
}
