// Proves that Render::run reuses an injected GpuExportProvider without retiring it, while a
// locally created provider is still retired with proof before the call returns. This is the
// contract the MCP server relies on to share ONE server-lifetime provider across still and video
// renders instead of re-bootstrapping (or prematurely retiring) a device per frame.

#include "gpu_route_proof_export_support.hpp"

#include <bloom/commands/animation_operations.hpp>
#include <bloom/commands/operations.hpp>
#include <bloom/commands/transaction.hpp>
#include <bloom/core/color.hpp>
#include <bloom/document/node_definition_registry.hpp>
#include <bloom/host/gpu_export_provider.hpp>
#include <bloom/runtime/compiled_plan.hpp>
#include <bloom/runtime/snapshot_compiler.hpp>
#include <bloom/runtime/task_scheduler.hpp>
#include <bloom/scripting/render.hpp>
#include <bloom/scripting/session.hpp>

#include <OpenEXR/ImfChannelList.h>
#include <OpenEXR/ImfFrameBuffer.h>
#include <OpenEXR/ImfHeader.h>
#include <OpenEXR/ImfInputFile.h>
#include <OpenEXR/ImfPixelType.h>
#include <OpenEXR/ImfStringAttribute.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <memory>
#include <optional>
#include <source_location>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {

namespace document = bloom::document;
namespace commands = bloom::commands;
namespace core = bloom::core;
namespace host = bloom::host;
namespace runtime = bloom::runtime;
namespace scripting = bloom::scripting;
namespace routeproof = bloom::gpu_route_proof_export;

// A skip for the proof CTest is exit 77; --require-device turns it into a failure.
enum class GpuProofOutcome : std::uint8_t { NotRequested, Skipped, Ran };

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

class TempDirectory final {
  public:
    TempDirectory() {
        std::error_code error;
        const auto base = std::filesystem::temp_directory_path(error);
        if (error) {
            return;
        }
        const auto seed = std::chrono::steady_clock::now().time_since_epoch().count();
        for (int attempt = 0; attempt < 64 && path_.empty(); ++attempt) {
            auto candidate = base / ("bloom-render-injected-" + std::to_string(seed) + "-" +
                                     std::to_string(attempt));
            if (std::filesystem::create_directory(candidate, error) && !error) {
                path_ = std::move(candidate);
            }
        }
    }
    TempDirectory(const TempDirectory&) = delete;
    TempDirectory& operator=(const TempDirectory&) = delete;
    ~TempDirectory() {
        if (!path_.empty()) {
            std::error_code error;
            std::filesystem::remove_all(path_, error);
        }
    }
    [[nodiscard]] bool isValid() const noexcept { return !path_.empty(); }
    [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }

  private:
    std::filesystem::path path_;
};

[[nodiscard]] runtime::GpuProcessFrameEvaluatorOptions
optionsFor(const std::filesystem::path& loader) {
    runtime::GpuProcessFrameEvaluatorOptions options;
    // A deliberately absent loader keeps a real owner worker alive (because enabled is true) but
    // never publishes a device, so retirementComplete() is only true after we retire it.
    options.enabled = true;
    options.loaderPath = loader;
    return options;
}

void testInjectedProviderIsReusedAndNotRetired(Expectations& expectations) {
    auto created = scripting::Session::createNew("Injected Provider", "Composition");
    expectations.expect(static_cast<bool>(created), "session is created");
    if (!created) {
        return;
    }
    auto session = std::move(created).takeSession();
    const auto snapshot = session->snapshot();
    expectations.expect(!snapshot.project().compositions().empty(),
                        "the default project has a composition");
    if (snapshot.project().compositions().empty()) {
        return;
    }
    TempDirectory directory;
    expectations.expect(directory.isValid(), "temp directory is available");
    if (!directory.isValid()) {
        return;
    }

    runtime::TaskScheduler scheduler;
    runtime::SnapshotCompiler compiler(document::builtInNodeDefinitions());
    auto provider =
        host::GpuExportProvider::create(optionsFor("/nonexistent/bloom/test/vulkan/loader.so"));
    provider->prepare(scheduler);
    const auto bootstrapDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
    while (!provider->prepared() && std::chrono::steady_clock::now() < bootstrapDeadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    expectations.expect(provider->prepared(), "injected provider bootstraps");
    const auto evaluator = provider->evaluator();
    expectations.expect(evaluator != nullptr,
                        "injected provider publishes an (unavailable) evaluator");
    expectations.expect(!provider->retirementComplete(),
                        "injected provider is live before the render");

    const auto compositionId = snapshot.project().compositions().front().id();
    for (int pass = 0; pass < 2; ++pass) {
        const auto target = directory.path() / ("injected-" + std::to_string(pass) + ".exr");
        const auto result = scripting::Render::run(
            *session, scheduler, compiler,
            {.composition = compositionId,
             .frame = std::uint64_t{0},
             .range = std::nullopt,
             .preset = bloom::output::OutputPresetV1::FlatExrRgba32fLinRec709SceneV1,
             .destination = target},
            {}, provider);
        expectations.expect(result.succeeded, "the injected-provider render publishes");
        expectations.expect(!provider->retirementComplete(),
                            "Render::run never retires an injected provider between stills");
        expectations.expect(provider->evaluator() != nullptr,
                            "the injected evaluator is still published after a still");
    }

    expectations.expect(provider->shutdownAndWait(std::chrono::seconds(10)),
                        "the owner can still retire the injected provider explicitly");
}

void testLocalProviderIsRetired(Expectations& expectations) {
    auto created = scripting::Session::createNew("Local Provider", "Composition");
    expectations.expect(static_cast<bool>(created), "local session is created");
    if (!created) {
        return;
    }
    auto session = std::move(created).takeSession();
    const auto snapshot = session->snapshot();
    if (snapshot.project().compositions().empty()) {
        return;
    }
    TempDirectory directory;
    if (!directory.isValid()) {
        return;
    }
    runtime::TaskScheduler scheduler;
    runtime::SnapshotCompiler compiler(document::builtInNodeDefinitions());
    const auto result = scripting::Render::run(
        *session, scheduler, compiler,
        {.composition = snapshot.project().compositions().front().id(),
         .frame = std::uint64_t{0},
         .range = std::nullopt,
         .preset = bloom::output::OutputPresetV1::FlatExrRgba32fLinRec709SceneV1,
         .destination = directory.path() / "local.exr"});
    expectations.expect(result.succeeded,
                        "a locally owned provider still renders and retires with proof");
}

[[nodiscard]] runtime::GpuProcessFrameEvaluatorOptions disabledOptions() {
    runtime::GpuProcessFrameEvaluatorOptions options;
    options.enabled = false;
    return options;
}

// A real session whose composition is inside the currently prepared-GPU subset (a translated solid
// layer), so Render::run genuinely evaluates on the device when one is available.
[[nodiscard]] std::unique_ptr<scripting::Session> buildSolidSession(Expectations& expectations) {
    const auto format = document::CompositionFormat::create(32, 32);
    expectations.expect(format.has_value(), "native render: the composition format is valid");
    if (!format.has_value()) {
        return nullptr;
    }
    auto created = scripting::Session::createNew("Native GPU Render", "Main",
                                                 core::RationalTime::fromInteger(2), *format);
    expectations.expect(static_cast<bool>(created), "native render: the session is created");
    if (!created) {
        return nullptr;
    }
    auto session = std::move(created).takeSession();
    const auto compositionId = session->snapshot().project().compositions().front().id();
    commands::Transaction addSolid("Add a solid", session->snapshot().revision());
    addSolid.emplace<commands::AddSolidLayer>(
        compositionId, "Moving", core::Color4d{0.25, 0.5, 0.75, 1.0}, document::Vec2d{16.0, 16.0});
    auto result = session->execute(std::move(addSolid));
    expectations.expect(result.succeeded(), "native render: the solid layer is added");
    if (!result.succeeded()) {
        return nullptr;
    }
    return session;
}

struct ExrImage final {
    bool valid = false;
    int width = 0;
    int height = 0;
    std::vector<float> red, green, blue, alpha;
    int compression = 0;
    float pixelAspect = 1.0F;
    std::string colorInteropId;
    std::vector<std::string> channelNames;
};

// Reads a flat RGBA Float32 EXR straight from the published payload, with no per-node readback.
[[nodiscard]] ExrImage readExr(const std::filesystem::path& path) {
    ExrImage image;
    try {
        Imf::InputFile file(path.string().c_str());
        const auto& header = file.header();
        const auto window = header.dataWindow();
        image.width = window.max.x - window.min.x + 1;
        image.height = window.max.y - window.min.y + 1;
        if (image.width <= 0 || image.height <= 0) {
            return image;
        }
        const auto count =
            static_cast<std::size_t>(image.width) * static_cast<std::size_t>(image.height);
        image.red.assign(count, 0.0F);
        image.green.assign(count, 0.0F);
        image.blue.assign(count, 0.0F);
        image.alpha.assign(count, 0.0F);
        const auto xStride = static_cast<std::size_t>(sizeof(float));
        const auto yStride = static_cast<std::size_t>(image.width) * sizeof(float);
        const auto base = [&](std::vector<float>& plane) {
            return reinterpret_cast<char*>(plane.data()) -
                   static_cast<std::ptrdiff_t>(window.min.x) *
                       static_cast<std::ptrdiff_t>(xStride) -
                   static_cast<std::ptrdiff_t>(window.min.y) * static_cast<std::ptrdiff_t>(yStride);
        };
        Imf::FrameBuffer frameBuffer;
        frameBuffer.insert("R", Imf::Slice(Imf::FLOAT, base(image.red), xStride, yStride));
        frameBuffer.insert("G", Imf::Slice(Imf::FLOAT, base(image.green), xStride, yStride));
        frameBuffer.insert("B", Imf::Slice(Imf::FLOAT, base(image.blue), xStride, yStride));
        frameBuffer.insert("A", Imf::Slice(Imf::FLOAT, base(image.alpha), xStride, yStride));
        file.setFrameBuffer(frameBuffer);
        file.readPixels(window.min.y, window.max.y);
        image.compression = static_cast<int>(header.compression());
        image.pixelAspect = header.pixelAspectRatio();
        for (auto channel = header.channels().begin(); channel != header.channels().end();
             ++channel) {
            image.channelNames.emplace_back(channel.name());
        }
        if (const auto* attribute =
                header.findTypedAttribute<Imf::StringAttribute>("colorInteropID")) {
            image.colorInteropId = attribute->value();
        }
        image.valid = true;
    } catch (...) {
        image.valid = false;
    }
    return image;
}

[[nodiscard]] bool bitsEqual(const float lhs, const float rhs) {
    return std::memcmp(&lhs, &rhs, sizeof(float)) == 0;
}

[[nodiscard]] bool finiteClose(const float lhs, const float rhs, const float tolerance) {
    const auto difference = std::abs(lhs - rhs);
    return difference <= tolerance ||
           difference <= tolerance * std::max(std::abs(lhs), std::abs(rhs));
}

[[nodiscard]] bool descriptorsMatch(const ExrImage& lhs, const ExrImage& rhs) {
    return lhs.width == rhs.width && lhs.height == rhs.height &&
           lhs.compression == rhs.compression && bitsEqual(lhs.pixelAspect, rhs.pixelAspect) &&
           lhs.colorInteropId == rhs.colorInteropId && lhs.channelNames == rhs.channelNames;
}

[[nodiscard]] bool pixelsMatch(const ExrImage& lhs, const ExrImage& rhs, const float tolerance) {
    if (!lhs.valid || !rhs.valid || lhs.width != rhs.width || lhs.height != rhs.height) {
        return false;
    }
    for (std::size_t index = 0; index < lhs.red.size(); ++index) {
        if (!bitsEqual(lhs.alpha[index], rhs.alpha[index])) {
            return false; // alpha is exact
        }
        if (!finiteClose(lhs.red[index], rhs.red[index], tolerance) ||
            !finiteClose(lhs.green[index], rhs.green[index], tolerance) ||
            !finiteClose(lhs.blue[index], rhs.blue[index], tolerance)) {
            return false;
        }
    }
    return true;
}

// The per-frame production identity material the scripting facade can genuinely observe: the real
// compiled plan identity plus the exact frame request and preset. Render::run compiles the same
// plan, so this is the production plan/request identity, not a placeholder.
struct FrameIdentityFields final {
    std::string_view routeId;
    std::uint64_t projectId = 0;
    std::uint64_t compositionId = 0;
    std::uint64_t sourceRevision = 0;
    std::uint64_t outputIndex = 0;
    std::uint64_t operationCount = 0;
    std::uint32_t planSemantics = 0;
    std::uint32_t animationSamplingSemantics = 0;
    std::uint64_t frameIndex = 0;
    std::int64_t timeNumerator = 0;
    std::int64_t timeDenominator = 1;
    std::uint64_t preset = 0;
    std::string_view provider;
};

[[nodiscard]] std::string frameIdentityHex(const FrameIdentityFields& fields) {
    routeproof::CanonicalWriter writer;
    writer.text("bloom.gpu.route.export-frame-identity.v1");
    writer.text(fields.routeId);
    writer.u64(fields.projectId);
    writer.u64(fields.compositionId);
    writer.u64(fields.sourceRevision);
    writer.u64(fields.outputIndex);
    writer.u64(fields.operationCount);
    writer.u32(fields.planSemantics);
    writer.u32(fields.animationSamplingSemantics);
    writer.u64(fields.frameIndex);
    writer.i64(fields.timeNumerator);
    writer.i64(fields.timeDenominator);
    writer.u64(fields.preset);
    writer.text(fields.provider);
    return routeproof::sha256Hex(writer.bytes());
}

// SHA-256 over the exact float bytes the independently decoded EXR payload carries. Every plane is
// written in canonical big-endian bit order, so the digest is bound to the actual output values.
[[nodiscard]] std::string exrDigest(const ExrImage& image) {
    routeproof::CanonicalWriter writer;
    writer.u64(static_cast<std::uint64_t>(image.width));
    writer.u64(static_cast<std::uint64_t>(image.height));
    writer.u64(image.red.size());
    for (std::size_t index = 0; index < image.red.size(); ++index) {
        writer.f64(static_cast<double>(image.red[index]));
        writer.f64(static_cast<double>(image.green[index]));
        writer.f64(static_cast<double>(image.blue[index]));
        writer.f64(static_cast<double>(image.alpha[index]));
    }
    return routeproof::sha256Hex(writer.bytes());
}

// The actual paired GPU/CPU comparison of one independently decoded EXR frame.
[[nodiscard]] routeproof::FrameEvidence compareExrEvidence(const ExrImage& gpu, const ExrImage& cpu,
                                                           std::string identityHex,
                                                           const float tolerance) {
    routeproof::FrameEvidence evidence;
    evidence.identityHex = std::move(identityHex);
    evidence.comparedPixels = cpu.red.size();
    evidence.alphaExact = true;
    double maxDelta = 0.0;
    std::uint64_t mismatches = 0;
    if (gpu.valid && cpu.valid && gpu.red.size() == cpu.red.size()) {
        for (std::size_t index = 0; index < cpu.red.size(); ++index) {
            if (!bitsEqual(cpu.alpha[index], gpu.alpha[index])) {
                evidence.alphaExact = false;
                ++mismatches;
                continue;
            }
            bool pixelBad = false;
            const double red = std::abs(static_cast<double>(cpu.red[index] - gpu.red[index]));
            const double green = std::abs(static_cast<double>(cpu.green[index] - gpu.green[index]));
            const double blue = std::abs(static_cast<double>(cpu.blue[index] - gpu.blue[index]));
            maxDelta = std::max({maxDelta, red, green, blue});
            pixelBad = !finiteClose(cpu.red[index], gpu.red[index], tolerance) ||
                       !finiteClose(cpu.green[index], gpu.green[index], tolerance) ||
                       !finiteClose(cpu.blue[index], gpu.blue[index], tolerance);
            if (pixelBad) {
                ++mismatches;
            }
        }
    }
    evidence.maxFloatDelta = maxDelta;
    evidence.mismatchedPixels = mismatches;
    evidence.cpuDecodedDigest = exrDigest(cpu);
    evidence.gpuDecodedDigest = exrDigest(gpu);
    return evidence;
}

// Publishes one scripted export proof from the exact verified frames. The writer rejects a
// zero-dispatch / no-frame / over-policy proof, and the caller only reaches here after every
// assertion passed.
void publishScriptedProof(Expectations& expectations, const std::filesystem::path& proofDirectory,
                          const std::string_view routeId,
                          const bloom::runtime::GpuRouteHarnessKind harness,
                          const std::uint64_t nativeDispatches, const std::uint64_t epoch,
                          const std::uint64_t readbacks, const std::uint64_t frameWidth,
                          const std::uint64_t frameHeight,
                          const std::vector<std::string>& identityHex,
                          const std::vector<routeproof::FrameEvidence>& evidence) {
    if (proofDirectory.empty()) {
        return;
    }
    routeproof::ExportProofCounters counters;
    counters.deviceOwnershipEpoch = epoch;
    counters.nativeDispatches = nativeDispatches;
    counters.verifiedFrames = identityHex.size();
    counters.readbackSubmissions = readbacks;
    // The accepted final readback transfers one process payload; no separate production payload
    // counter exists yet, so the real submission count is the payload count, not a guess.
    counters.payloads = readbacks;
    counters.transferredBytes = routeproof::processPayloadBytes(readbacks, frameWidth, frameHeight);
    std::string nonce;
    if (!routeproof::readProofNonce(proofDirectory, nonce)) {
        expectations.expect(false, "scripted proof: a fresh run nonce is required");
        return;
    }
    const auto processDigest = routeproof::orderedIdentityDigest(identityHex);
    const auto capturedEvidenceDigest = routeproof::evidenceDigest(evidence);
    const auto written = routeproof::publishExportProof(
        proofDirectory, nonce, routeId, harness, counters, processDigest, capturedEvidenceDigest);
    if (!written.written) {
        expectations.expect(false, std::string{"scripted proof rejected: "} + written.detail);
        return;
    }
    std::cout << "PASS(route-proof) " << routeId << " frames=" << counters.verifiedFrames
              << " dispatches=" << counters.nativeDispatches
              << " readbacks=" << counters.readbackSubmissions
              << " bytes=" << counters.transferredBytes << '\n';
}

// Real headless render integration over the production Render::run: at least two EXR frames through
// an injected provider, positive genuine GPU counters and device epoch, strict CPU/process and
// encoded-output parity against a disabled-provider CPU reference, and the repeated provider still
// live. Requires BLOOM_TEST_VULKAN_LOADER (an absolute loader path); skips honestly otherwise.
GpuProofOutcome testNativeHeadlessRenderProvenanceAndParity(Expectations& expectations,
                                                            const bool requireDevice,
                                                            const std::filesystem::path& proofDir) {
    const auto skip = [&proofDir] {
        return proofDir.empty() ? GpuProofOutcome::NotRequested : GpuProofOutcome::Skipped;
    };
    const char* loader = std::getenv("BLOOM_TEST_VULKAN_LOADER");
    if (loader == nullptr || *loader == '\0') {
        if (requireDevice) {
            expectations.expect(false,
                                "native render: --require-device needs BLOOM_TEST_VULKAN_LOADER");
        } else {
            std::cout << "NOTE: BLOOM_TEST_VULKAN_LOADER unset; skipping native headless render\n";
        }
        return skip();
    }
    auto session = buildSolidSession(expectations);
    if (session == nullptr) {
        return GpuProofOutcome::Ran;
    }
    TempDirectory directory;
    expectations.expect(directory.isValid(), "native render: temp directory is available");
    if (!directory.isValid()) {
        return GpuProofOutcome::Ran;
    }
    runtime::TaskScheduler scheduler;
    runtime::SnapshotCompiler compiler(document::builtInNodeDefinitions());
    auto provider = host::GpuExportProvider::create(optionsFor(loader));
    provider->prepare(scheduler);
    const auto bootstrapDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    while (!provider->prepared() && std::chrono::steady_clock::now() < bootstrapDeadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    if (!provider->deviceAvailable()) {
        expectations.expect(!requireDevice, "native render: a device is required");
        std::cout << "NOTE: no compatible Vulkan device; skipping native headless render\n";
        return skip();
    }
    auto cpuProvider = host::GpuExportProvider::create(disabledOptions());
    cpuProvider->prepare(scheduler);
    const auto snapshot = session->snapshot();
    const auto compositionId = snapshot.project().compositions().front().id();
    const auto* composition = snapshot.project().findComposition(compositionId);
    const auto planResult =
        compiler.compile({.snapshot = snapshot, .compositionId = compositionId}, {});
    expectations.expect(planResult.plan != nullptr,
                        "native render: the plan compiles for identity");
    constexpr float kTolerance = 2e-6F;
    std::vector<std::string> identityHex;
    std::vector<routeproof::FrameEvidence> evidence;
    std::uint64_t proofDispatches = 0;
    std::uint64_t proofReadbacks = 0;
    std::uint64_t proofEpoch = 0;
    std::uint64_t frameWidth = 0;
    std::uint64_t frameHeight = 0;
    for (const std::uint64_t frame : {std::uint64_t{0}, std::uint64_t{1}}) {
        const auto gpuPath = directory.path() / ("gpu-" + std::to_string(frame) + ".exr");
        const auto cpuPath = directory.path() / ("cpu-" + std::to_string(frame) + ".exr");
        const auto gpu = scripting::Render::run(
            *session, scheduler, compiler,
            {.composition = compositionId,
             .frame = frame,
             .range = std::nullopt,
             .preset = bloom::output::OutputPresetV1::FlatExrRgba32fLinRec709SceneV1,
             .destination = gpuPath},
            {}, provider);
        expectations.expect(gpu.succeeded, "native render: the GPU frame publishes");
        expectations.expect(gpu.gpuEvaluatedFrames == 1 && gpu.gpuNativeDispatches > 0 &&
                                gpu.gpuReadbacks == 1,
                            "native render: genuine positive GPU counters for the frame");
        expectations.expect(gpu.gpuDeviceOwnershipEpoch > 0,
                            "native render: a genuine device ownership epoch is reported");
        const auto cpu = scripting::Render::run(
            *session, scheduler, compiler,
            {.composition = compositionId,
             .frame = frame,
             .range = std::nullopt,
             .preset = bloom::output::OutputPresetV1::FlatExrRgba32fLinRec709SceneV1,
             .destination = cpuPath},
            {}, cpuProvider);
        expectations.expect(cpu.succeeded && cpu.gpuEvaluatedFrames == 0 &&
                                cpu.gpuDeviceOwnershipEpoch == 0,
                            "native render: the disabled provider is an honest CPU reference");
        const auto gpuImage = readExr(gpuPath);
        const auto cpuImage = readExr(cpuPath);
        expectations.expect(gpuImage.valid && cpuImage.valid,
                            "native render: both published EXRs reopen");
        expectations.expect(descriptorsMatch(gpuImage, cpuImage),
                            "native render: GPU/CPU encoded descriptors match");
        expectations.expect(pixelsMatch(gpuImage, cpuImage, kTolerance),
                            "native render: GPU/CPU process pixels match within 2e-6 with exact "
                            "alpha");
        if (planResult.plan != nullptr && composition != nullptr) {
            const auto time = host::FrameRangeRunnerV1::timeForFrame(
                {.destination = gpuPath,
                 .firstFrame = frame,
                 .lastFrame = frame,
                 .frameRate = composition->format().frameRate(),
                 .duration = composition->duration()},
                frame);
            if (time.has_value()) {
                FrameIdentityFields fields;
                fields.routeId = "route.export.headless_scripted";
                fields.projectId = planResult.plan->projectId().value();
                fields.compositionId = planResult.plan->compositionId().value();
                fields.sourceRevision = planResult.plan->sourceRevision().value();
                fields.outputIndex = planResult.plan->output().value();
                fields.operationCount = planResult.plan->operations().size();
                fields.planSemantics = planResult.plan->planSemanticsVersion();
                fields.animationSamplingSemantics =
                    planResult.plan->animationSamplingSemanticsVersion();
                fields.frameIndex = frame;
                fields.timeNumerator = time->numerator();
                fields.timeDenominator = time->denominator();
                fields.preset = static_cast<std::uint64_t>(
                    bloom::output::OutputPresetV1::FlatExrRgba32fLinRec709SceneV1);
                fields.provider = "gpu-resident";
                const auto identity = frameIdentityHex(fields);
                identityHex.push_back(identity);
                evidence.push_back(compareExrEvidence(gpuImage, cpuImage, identity, kTolerance));
                frameWidth = static_cast<std::uint64_t>(gpuImage.width);
                frameHeight = static_cast<std::uint64_t>(gpuImage.height);
            }
        }
        proofDispatches += gpu.gpuNativeDispatches;
        proofReadbacks += gpu.gpuReadbacks;
        if (gpu.gpuDeviceOwnershipEpoch != 0) {
            proofEpoch = gpu.gpuDeviceOwnershipEpoch;
        }
    }
    expectations.expect(!provider->retirementComplete() && provider->evaluator() != nullptr,
                        "native render: the repeated injected provider stays live");
    expectations.expect(provider->shutdownAndWait(std::chrono::seconds(10)),
                        "native render: the provider retires with completion proof");
    if (expectations.failures() == 0 && identityHex.size() == 2) {
        publishScriptedProof(expectations, proofDir, "route.export.headless_scripted",
                             bloom::runtime::GpuRouteHarnessKind::HeadlessScripted, proofDispatches,
                             proofEpoch, proofReadbacks, frameWidth, frameHeight, identityHex,
                             evidence);
    }
    return GpuProofOutcome::Ran;
}

// Real sequence/range integration over the production Render::run range mode: the real
// FrameRangeRunner publishes numbered EXR frames through the same per-frame output attempt path,
// and every frame is verified against an independent disabled-provider CPU export. A device is
// required; without one this returns Skipped.
GpuProofOutcome testNativeSequenceRangeProvenanceAndParity(Expectations& expectations,
                                                           const bool requireDevice,
                                                           const std::filesystem::path& proofDir) {
    const auto skip = [&proofDir] {
        return proofDir.empty() ? GpuProofOutcome::NotRequested : GpuProofOutcome::Skipped;
    };
    const char* loader = std::getenv("BLOOM_TEST_VULKAN_LOADER");
    if (loader == nullptr || *loader == '\0') {
        if (requireDevice) {
            expectations.expect(
                false, "native sequence range: --require-device needs BLOOM_TEST_VULKAN_LOADER");
        }
        return skip();
    }
    auto session = buildSolidSession(expectations);
    if (session == nullptr) {
        return GpuProofOutcome::Ran;
    }
    TempDirectory directory;
    expectations.expect(directory.isValid(), "native sequence range: temp directory is available");
    if (!directory.isValid()) {
        return GpuProofOutcome::Ran;
    }
    runtime::TaskScheduler scheduler;
    runtime::SnapshotCompiler compiler(document::builtInNodeDefinitions());
    auto provider = host::GpuExportProvider::create(optionsFor(loader));
    provider->prepare(scheduler);
    const auto bootstrapDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    while (!provider->prepared() && std::chrono::steady_clock::now() < bootstrapDeadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    if (!provider->deviceAvailable()) {
        expectations.expect(!requireDevice, "native sequence range: a device is required");
        return skip();
    }
    auto cpuProvider = host::GpuExportProvider::create(disabledOptions());
    cpuProvider->prepare(scheduler);
    const auto snapshot = session->snapshot();
    const auto compositionId = snapshot.project().compositions().front().id();
    const auto* composition = snapshot.project().findComposition(compositionId);
    expectations.expect(composition != nullptr, "native sequence range: the composition exists");
    if (composition == nullptr) {
        return GpuProofOutcome::Ran;
    }
    const auto planResult =
        compiler.compile({.snapshot = snapshot, .compositionId = compositionId}, {});
    expectations.expect(planResult.plan != nullptr,
                        "native sequence range: the plan compiles for identity");
    constexpr std::uint64_t kFirstFrame = 0;
    constexpr std::uint64_t kLastFrame = 2;
    constexpr float kTolerance = 2e-6F;
    const auto gpuBase = directory.path() / "gpu-sequence.exr";
    const auto cpuBase = directory.path() / "cpu-sequence.exr";
    const auto gpuRange = scripting::Render::run(
        *session, scheduler, compiler,
        {.composition = compositionId,
         .frame = std::nullopt,
         .range = std::make_pair(kFirstFrame, kLastFrame),
         .preset = bloom::output::OutputPresetV1::FlatExrRgba32fLinRec709SceneV1,
         .destination = gpuBase},
        {}, provider);
    expectations.expect(gpuRange.succeeded && gpuRange.publishedFrames == 3,
                        "native sequence range: the GPU range publishes three frames");
    const auto cpuRange = scripting::Render::run(
        *session, scheduler, compiler,
        {.composition = compositionId,
         .frame = std::nullopt,
         .range = std::make_pair(kFirstFrame, kLastFrame),
         .preset = bloom::output::OutputPresetV1::FlatExrRgba32fLinRec709SceneV1,
         .destination = cpuBase},
        {}, cpuProvider);
    expectations.expect(cpuRange.succeeded && cpuRange.publishedFrames == 3 &&
                            cpuRange.gpuEvaluatedFrames == 0,
                        "native sequence range: the disabled provider is an honest CPU reference");
    expectations.expect(gpuRange.gpuEvaluatedFrames == 3 && gpuRange.gpuNativeDispatches > 0 &&
                            gpuRange.gpuReadbacks == 3 && gpuRange.gpuDeviceOwnershipEpoch > 0,
                        "native sequence range: genuine per-frame GPU counters");
    std::vector<std::string> identityHex;
    std::vector<routeproof::FrameEvidence> evidence;
    std::uint64_t frameWidth = 0;
    std::uint64_t frameHeight = 0;
    for (std::uint64_t frame = kFirstFrame; frame <= kLastFrame; ++frame) {
        const auto time =
            host::FrameRangeRunnerV1::timeForFrame({.destination = gpuBase,
                                                    .firstFrame = kFirstFrame,
                                                    .lastFrame = kLastFrame,
                                                    .frameRate = composition->format().frameRate(),
                                                    .duration = composition->duration()},
                                                   frame);
        const auto gpuPath =
            host::FrameRangeRunnerV1::sequenceFramePath(gpuBase, frame, kLastFrame);
        const auto cpuPath =
            host::FrameRangeRunnerV1::sequenceFramePath(cpuBase, frame, kLastFrame);
        const auto gpuImage = readExr(gpuPath);
        const auto cpuImage = readExr(cpuPath);
        expectations.expect(gpuImage.valid && cpuImage.valid &&
                                descriptorsMatch(gpuImage, cpuImage),
                            "native sequence range: both frame EXRs reopen with matching "
                            "descriptors");
        expectations.expect(pixelsMatch(gpuImage, cpuImage, kTolerance),
                            "native sequence range: GPU frame matches the CPU reference within "
                            "2e-6 with exact alpha");
        if (planResult.plan != nullptr && composition != nullptr && time.has_value()) {
            FrameIdentityFields fields;
            fields.routeId = "route.export.sequence_range";
            fields.projectId = planResult.plan->projectId().value();
            fields.compositionId = planResult.plan->compositionId().value();
            fields.sourceRevision = planResult.plan->sourceRevision().value();
            fields.outputIndex = planResult.plan->output().value();
            fields.operationCount = planResult.plan->operations().size();
            fields.planSemantics = planResult.plan->planSemanticsVersion();
            fields.animationSamplingSemantics =
                planResult.plan->animationSamplingSemanticsVersion();
            fields.frameIndex = frame;
            fields.timeNumerator = time->numerator();
            fields.timeDenominator = time->denominator();
            fields.preset = static_cast<std::uint64_t>(
                bloom::output::OutputPresetV1::FlatExrRgba32fLinRec709SceneV1);
            fields.provider = "gpu-resident";
            const auto identity = frameIdentityHex(fields);
            identityHex.push_back(identity);
            evidence.push_back(compareExrEvidence(gpuImage, cpuImage, identity, kTolerance));
            frameWidth = static_cast<std::uint64_t>(gpuImage.width);
            frameHeight = static_cast<std::uint64_t>(gpuImage.height);
        }
    }
    expectations.expect(provider->shutdownAndWait(std::chrono::seconds(10)),
                        "native sequence range: the provider retires with completion proof");
    if (expectations.failures() == 0 && identityHex.size() == 3) {
        publishScriptedProof(expectations, proofDir, "route.export.sequence_range",
                             bloom::runtime::GpuRouteHarnessKind::SequenceRangeExport,
                             gpuRange.gpuNativeDispatches, gpuRange.gpuDeviceOwnershipEpoch,
                             gpuRange.gpuReadbacks, frameWidth, frameHeight, identityHex, evidence);
    }
    return GpuProofOutcome::Ran;
}

} // namespace

int main(int argc, char** argv) {
    Expectations expectations;
    bool requireDevice = false;
    std::filesystem::path proofDir;
    for (int index = 1; index < argc; ++index) {
        const std::string_view argument{argv[index]};
        if (argument == "--require-device") {
            requireDevice = true;
        } else if (argument == "--route-proof-dir" && index + 1 < argc) {
            proofDir = argv[++index];
        }
    }
    GpuProofOutcome headlessOutcome = GpuProofOutcome::NotRequested;
    GpuProofOutcome sequenceOutcome = GpuProofOutcome::NotRequested;
    std::string proofRoute = "both";
    for (int index = 1; index < argc; ++index) {
        if (std::string_view(argv[index]) == "--route" && index + 1 < argc) {
            proofRoute = argv[++index];
        }
    }
    try {
        testInjectedProviderIsReusedAndNotRetired(expectations);
        testLocalProviderIsRetired(expectations);
        if (proofRoute != "sequence") {
            headlessOutcome =
                testNativeHeadlessRenderProvenanceAndParity(expectations, requireDevice, proofDir);
        }
        if (proofRoute != "headless") {
            sequenceOutcome =
                testNativeSequenceRangeProvenanceAndParity(expectations, requireDevice, proofDir);
        }
    } catch (const std::exception& error) {
        std::cerr << "unexpected exception: " << error.what() << '\n';
        return EXIT_FAILURE;
    }
    if (expectations.failures() != 0) {
        return EXIT_FAILURE;
    }
    if (headlessOutcome == GpuProofOutcome::Skipped ||
        sequenceOutcome == GpuProofOutcome::Skipped) {
        return 77;
    }
    return EXIT_SUCCESS;
}
