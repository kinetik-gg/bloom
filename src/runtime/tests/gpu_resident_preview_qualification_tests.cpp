// Frozen native proof for the resident-preview runtime qualification.
//
// On a real Vulkan device it builds the actual device + typed pipelines + canonical CPU OCIO
// processor, runs qualifyResidentPreview() (whose parity pass compares every pixel of every
// resident operation against the existing CPU primitives and OCIO oracle), and then exercises the
// identity/negative gates. The embedded SPIR-V arrays are re-hashed here against the pins the
// report carries, so a substituted shader fails even if the pipeline still builds.
//
// Everything native is compiled from the frozen snapshot against ONLY its private headers. Readback
// happens here and inside the qualification; it is never a normal preview path. Local mode uses the
// explicit prefix loader and --require-device fails closed.

#include <bloom/color/ocio_builtin_registry.hpp>
#include <bloom/color/ocio_cpu_display_processor.hpp>
#include <bloom/core/sha256.hpp>
#include <bloom/render/gpu_composite.hpp>
#include <bloom/render/gpu_device.hpp>
#include <bloom/render/gpu_image_upload.hpp>
#include <bloom/render/gpu_resident_display.hpp>
#include <bloom/render/gpu_solid.hpp>
#include <bloom/runtime/gpu_resident_preview_qualification.hpp>

#include "shaders/neutral_display_spirv.inc"
#include "shaders/solid_covered_spirv.inc"
#include "shaders/solid_spirv.inc"
#include "shaders/source_over_spirv.inc"
#include "shaders/translation_opacity_spirv.inc"

#include <cstddef>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <iostream>
#include <limits>
#include <memory>
#include <optional>
#include <source_location>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <array>

namespace {

using bloom::color::PreparedCpuDisplayProcessorHandle;
using bloom::core::Sha256Digest;
using bloom::core::Sha256Hasher;
using bloom::render::GpuDevice;
using bloom::render::GpuDeviceCreationOptions;
using bloom::render::GpuDeviceState;
using bloom::runtime::GpuResidentPreviewBudgets;
using bloom::runtime::GpuResidentPreviewDiagnosticCode;
using bloom::runtime::GpuResidentPreviewOutcome;
using bloom::runtime::GpuResidentPreviewPipelines;
using bloom::runtime::GpuResidentPreviewQualificationReport;

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
    [[nodiscard]] int failures() const noexcept { return failures_; }

  private:
    int failures_ = 0;
};

struct Options final {
    std::filesystem::path loader_path;
    // --require-device asserts only that a compatible device exists.
    bool require_device = false;
    // Strict local performance gate. Device presence is not acceleration: this additionally
    // requires a measured faster resident interval. Default CI must stay valid on a correct GPU
    // that is slower than the CPU oracle, so this is opt-in only and is never implied by
    // --require-device.
    bool require_acceleration = false;
    bool valid = true;
};

[[nodiscard]] Options parseOptions(const int argc, char** argv) {
    Options options;
    for (int index = 1; index < argc; ++index) {
        const std::string_view argument = argv[index];
        if (argument == "--loader") {
            if (index + 1 >= argc) {
                options.valid = false;
                return options;
            }
            options.loader_path = argv[++index];
        } else if (argument == "--require-device") {
            options.require_device = true;
        } else if (argument == "--require-acceleration") {
            options.require_acceleration = true;
        } else {
            options.valid = false;
            return options;
        }
    }
    return options;
}

[[nodiscard]] std::optional<PreparedCpuDisplayProcessorHandle> buildCanonicalProcessor() {
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

// A genuinely non-default processor: a display/view processor built from the pinned ACES 1.3 CG
// built-in, which the Bloom Neutral identity predicate must reject outright.
[[nodiscard]] std::optional<PreparedCpuDisplayProcessorHandle> buildNonDefaultProcessor() {
    const auto revision = bloom::color::ocioBuiltInContentRevision(
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
    for (const bloom::color::DisplayViewEntry& entry : resolved->displays()) {
        auto built =
            bloom::color::buildCpuDisplayProcessorForView(*resolved, entry.display, entry.view);
        if (built) {
            return std::move(built).takeHandle();
        }
    }
    return std::nullopt;
}

[[nodiscard]] std::optional<std::string> hashEmbeddedSpirv(const std::uint32_t* words,
                                                           const std::uint32_t wordCount,
                                                           const std::uint32_t byteCount) {
    if (words == nullptr || wordCount * 4U != byteCount) {
        return std::nullopt;
    }
    const auto* raw = reinterpret_cast<const std::byte*>(words);
    const auto digest = Sha256Hasher::hash(std::span<const std::byte>(raw, byteCount));
    if (!digest.has_value()) {
        return std::nullopt;
    }
    const auto hex = digest->toLowercaseHex();
    return std::string(hex.data(), hex.size());
}

void verifyEmbeddedShaderPins(Expectations& expectations) {
    const auto solid = hashEmbeddedSpirv(bloom::render::vulkan_detail::kSolidSpirvCode,
                                         bloom::render::vulkan_detail::kSolidSpirvWordCount,
                                         bloom::render::vulkan_detail::kSolidSpirvByteCount);
    const auto covered =
        hashEmbeddedSpirv(bloom::render::vulkan_detail::kSolidCoveredSpirvCode,
                          bloom::render::vulkan_detail::kSolidCoveredSpirvWordCount,
                          bloom::render::vulkan_detail::kSolidCoveredSpirvByteCount);
    const auto translation =
        hashEmbeddedSpirv(bloom::render::vulkan_detail::kTranslationOpacitySpirvCode,
                          bloom::render::vulkan_detail::kTranslationOpacitySpirvWordCount,
                          bloom::render::vulkan_detail::kTranslationOpacitySpirvByteCount);
    const auto sourceOver =
        hashEmbeddedSpirv(bloom::render::vulkan_detail::kSourceOverSpirvCode,
                          bloom::render::vulkan_detail::kSourceOverSpirvWordCount,
                          bloom::render::vulkan_detail::kSourceOverSpirvByteCount);
    const auto display =
        hashEmbeddedSpirv(bloom::render::vulkan_detail::kNeutralDisplaySpirvCode,
                          bloom::render::vulkan_detail::kNeutralDisplaySpirvWordCount,
                          bloom::render::vulkan_detail::kNeutralDisplaySpirvByteCount);
    const std::array<std::optional<std::string>, 5> measured{solid, covered, translation,
                                                             sourceOver, display};
    const auto& pins = bloom::runtime::kGpuResidentPreviewShaderPins;
    for (std::size_t index = 0; index < measured.size(); ++index) {
        expectations.expect(measured[index].has_value() &&
                                *measured[index] == std::string(pins[index].spv_sha256),
                            "the embedded " + std::string(pins[index].name) +
                                " SPIR-V array hashes to its pinned digest");
    }
}

void printReport(const GpuResidentPreviewQualificationReport& report) {
    std::cout << "outcome="
              << (report.outcome() == GpuResidentPreviewOutcome::PreviewOnly ? "PreviewOnly"
                                                                             : "Unavailable")
              << " diagnostic=" << static_cast<int>(report.diagnostic().code)
              << " message=" << report.diagnostic().message
              << " eligible=" << (report.eligible() ? "true" : "false")
              << " generation=" << report.deviceGeneration()
              << " ownershipEpoch=" << report.ownershipEpoch()
              << " primitiveSemantics=" << report.primitiveSemanticsVersion()
              << " covered=" << report.coveredSemantics()
              << " dispatch=" << report.dispatchSemantics() << '\n';
    const auto fixtureHex = report.fixtureDigest().toLowercaseHex();
    std::cout << "processorCacheId=" << report.processorCacheId()
              << " fixtureDigest=" << std::string(fixtureHex.data(), fixtureHex.size())
              << " subnormalRejected=" << (report.subnormalFrameRejected() ? "true" : "false")
              << '\n';
    for (const auto& sample : report.timings()) {
        std::cout << "  " << sample.width << 'x' << sample.height << ": native "
                  << sample.native_full_ms << " ms, cpu " << sample.cpu_full_ms
                  << " ms, improved=" << (sample.native_improved ? "true" : "false") << '\n';
    }
    if (report.eligibleInterval().has_value()) {
        std::cout << "eligibleInterval=[" << report.eligibleInterval()->min_pixels << ", "
                  << report.eligibleInterval()->max_pixels << "] pixels\n";
    } else {
        std::cout << "eligibleInterval=none (CPU-only at the measured sizes)\n";
    }
}

struct AlwaysCancelled final {
    [[nodiscard]] bool operator()() const noexcept { return true; }
};

struct PipelineSet final {
    std::unique_ptr<bloom::render::GpuSolid> solid;
    std::unique_ptr<bloom::render::GpuImageUpload> upload;
    std::unique_ptr<bloom::render::GpuComposite> composite;
    std::unique_ptr<bloom::render::GpuResidentDisplay> display;

    [[nodiscard]] bool valid() const noexcept {
        return solid != nullptr && upload != nullptr && composite != nullptr && display != nullptr;
    }
    [[nodiscard]] GpuResidentPreviewPipelines view() noexcept {
        return GpuResidentPreviewPipelines{solid.get(), upload.get(), composite.get(),
                                           display.get()};
    }
};

[[nodiscard]] PipelineSet createPipelines(GpuDevice& device) {
    PipelineSet set;
    set.solid = std::move(bloom::render::GpuSolid::create(device).solid);
    set.upload = std::move(bloom::render::GpuImageUpload::create(device).upload);
    set.composite = std::move(bloom::render::GpuComposite::create(device).composite);
    set.display = std::move(bloom::render::GpuResidentDisplay::create(device).display);
    return set;
}

} // namespace

int main(int argc, char** argv) {
    try {
        const Options options = parseOptions(argc, argv);
        if (!options.valid) {
            std::cerr << "usage: --loader <path> [--require-device] [--require-acceleration]\n";
            return 2;
        }
        Expectations expectations;
        verifyEmbeddedShaderPins(expectations);

        GpuDeviceCreationOptions createOptions;
        createOptions.loader_path = options.loader_path;
        auto device = GpuDevice::create(createOptions);
        if (!device) {
            if (options.require_device || options.require_acceleration) {
                std::cerr << "FAIL: required device unavailable: " << device.diagnostic.message
                          << '\n';
                return 1;
            }
            std::cout << "SKIP: no compatible Vulkan device available: "
                      << device.diagnostic.message << '\n';
            return expectations.failures() == 0 ? 0 : 1;
        }
        expectations.expect(device.device->state() == GpuDeviceState::Ready, "the device is Ready");
        expectations.expect(device.device->ownershipEpoch() != 0,
                            "a real device has a nonzero ownership epoch");

        PipelineSet pipelines = createPipelines(*device.device);
        expectations.expect(pipelines.valid(), "the typed pipelines are created");
        const auto processor = buildCanonicalProcessor();
        expectations.expect(processor.has_value(), "the canonical CPU OCIO processor builds");
        if (!pipelines.valid() || !processor) {
            return 1;
        }

        const GpuResidentPreviewQualificationReport report =
            bloom::runtime::qualifyResidentPreview(*processor, *device.device, pipelines.view());
        printReport(report);
        expectations.expect(report.outcome() == GpuResidentPreviewOutcome::PreviewOnly,
                            "the aggregate resident profile passed parity");
        expectations.expect(report.deviceGeneration() == 1 && report.ownershipEpoch() != 0,
                            "the report pins the actual generation and ownership epoch");
        expectations.expect(report.subnormalFrameRejected(),
                            "a nonzero subnormal resident frame was rejected whole-frame");

        // The eligible interval and the measured timings must be mutually consistent. A correct GPU
        // that is slower than the CPU oracle stays PreviewOnly (all-pixel parity held) but measures
        // no faster interval: that is a truthful CPU fallback, not a test failure. Only the opt-in
        // --require-acceleration gate turns "no faster interval" into a failure.
        const auto& timings = report.timings();
        const auto interval = report.eligibleInterval();
        if (timings.empty()) {
            // A truthful report always measures at least one interval. Guard the size()-1/back()
            // derivation below so an empty report fails clearly instead of reading out of bounds;
            // expect() records and does not abort, so keep every later access behind this check.
            expectations.expect(false, "the report measured the resident interval");
        } else {
            expectations.expect(report.eligible() == interval.has_value(),
                                "eligibility is consistent with the measured eligible interval");
            if (interval.has_value()) {
                // Derive the expected contiguous faster suffix from the actual timings, exactly as
                // the qualification does, and require the report to match it.
                std::size_t lowest = timings.size() - 1;
                while (lowest > 0 && timings[lowest - 1].native_improved) {
                    --lowest;
                }
                expectations.expect(
                    timings.back().native_improved,
                    "an eligible interval requires the largest measured size to improve");
                expectations.expect(interval->min_pixels == timings[lowest].pixel_count &&
                                        interval->max_pixels == timings.back().pixel_count,
                                    "the eligible interval matches the measured faster suffix");
                expectations.expect(report.diagnostic().code ==
                                        GpuResidentPreviewDiagnosticCode::None,
                                    "an eligible report carries no timing diagnostic");
                expectations.expect(report.eligibleFor(*device.device, *processor),
                                    "an eligible report qualifies the actual device and processor");
                std::cout << "ELIGIBLE interval min_pixels=" << interval->min_pixels
                          << " max_pixels=" << interval->max_pixels << '\n';
            } else {
                expectations.expect(
                    !timings.back().native_improved,
                    "a CPU-only outcome requires the largest measured size not to improve");
                expectations.expect(
                    report.diagnostic().code == GpuResidentPreviewDiagnosticCode::TimingNotImproved,
                    "a CPU-only parity-preserving outcome reports TimingNotImproved");
                expectations.expect(!report.eligibleFor(*device.device, *processor),
                                    "an ineligible report grants no device/processor eligibility");
                std::cout << "CPU-ONLY outcome: parity held but no faster resident interval\n";
            }
        }
        if (options.require_acceleration) {
            expectations.expect(report.eligible() && interval.has_value(),
                                "--require-acceleration: a faster resident interval was measured");
            expectations.expect(
                report.eligibleFor(*device.device, *processor),
                "--require-acceleration: the report qualifies this device/processor");
        }

        // A second device on the same physical GPU must NOT be qualified by this report, even
        // though its capability-report generation is also 1.
        auto second = GpuDevice::create(createOptions);
        if (second) {
            expectations.expect(second.device->ownershipEpoch() != device.device->ownershipEpoch(),
                                "two devices on one physical GPU have distinct ownership epochs");
            expectations.expect(!report.eligibleFor(*second.device, *processor),
                                "the report rejects a second same-physical-GPU device");
            PipelineSet secondPipelines = createPipelines(*second.device);
            expectations.expect(secondPipelines.valid(),
                                "the second device's typed pipelines are created");
            if (secondPipelines.valid()) {
                const auto foreign = bloom::runtime::qualifyResidentPreview(
                    *processor, *device.device, secondPipelines.view(), {},
                    bloom::color::CancellationPredicateRef{});
                expectations.expect(foreign.outcome() == GpuResidentPreviewOutcome::Unavailable &&
                                        foreign.diagnostic().code ==
                                            GpuResidentPreviewDiagnosticCode::NotEligibleDevice,
                                    "a foreign-device pipeline set is refused before any dispatch");
            }
        } else {
            std::cout
                << "NOTE: a second device is unavailable; foreign-device gate not exercised\n";
        }

        // A non-default processor must be rejected by the predicate and by eligibleFor().
        if (const auto nonDefault = buildNonDefaultProcessor(); nonDefault.has_value()) {
            std::string reason;
            expectations.expect(
                !bloom::runtime::gpuResidentPreviewProcessorIsEligible(*nonDefault, reason),
                "the non-default processor is not eligible");
            expectations.expect(!report.eligibleFor(*device.device, *nonDefault),
                                "the report rejects the non-default processor");
        } else {
            std::cout << "NOTE: no non-default display/view processor was buildable\n";
        }

        // Cancellation must not publish an eligible report.
        AlwaysCancelled cancelled;
        const auto cancelledReport = bloom::runtime::qualifyResidentPreview(
            *processor, *device.device, pipelines.view(), {},
            bloom::color::CancellationPredicateRef{cancelled});
        expectations.expect(cancelledReport.outcome() == GpuResidentPreviewOutcome::Unavailable &&
                                cancelledReport.diagnostic().code ==
                                    GpuResidentPreviewDiagnosticCode::Cancelled,
                            "a cancelled qualification publishes no eligible report");

        // Budget refusal must not publish an eligible report.
        GpuResidentPreviewBudgets tight;
        tight.maxImageBytes = 4096;
        const auto tightReport = bloom::runtime::qualifyResidentPreview(*processor, *device.device,
                                                                        pipelines.view(), tight);
        expectations.expect(tightReport.outcome() == GpuResidentPreviewOutcome::Unavailable &&
                                tightReport.diagnostic().code ==
                                    GpuResidentPreviewDiagnosticCode::OverBudget,
                            "a budget-refused qualification publishes no eligible report");

        // Wrong thread must not touch the driver and must publish no eligible report.
        std::optional<GpuResidentPreviewQualificationReport> foreignThreadReport;
        std::thread worker([&]() {
            foreignThreadReport.emplace(bloom::runtime::qualifyResidentPreview(
                *processor, *device.device, pipelines.view()));
        });
        worker.join();
        expectations.expect(foreignThreadReport.has_value() &&
                                foreignThreadReport->outcome() ==
                                    GpuResidentPreviewOutcome::Unavailable &&
                                foreignThreadReport->diagnostic().code ==
                                    GpuResidentPreviewDiagnosticCode::WrongThread,
                            "a foreign-thread qualification publishes no eligible report");

        if (expectations.failures() != 0) {
            std::cerr << expectations.failures() << " resident-preview expectation(s) failed\n";
            return 1;
        }
        std::cout << "PASS: frozen GPU resident-preview qualification\n";
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << "Unexpected test exception: " << exception.what() << '\n';
        return 1;
    }
}
