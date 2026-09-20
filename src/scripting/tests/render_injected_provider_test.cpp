// Proves that Render::run reuses an injected GpuExportProvider without retiring it, while a
// locally created provider is still retired with proof before the call returns. This is the
// contract the MCP server relies on to share ONE server-lifetime provider across still and video
// renders instead of re-bootstrapping (or prematurely retiring) a device per frame.

#include <bloom/commands/animation_operations.hpp>
#include <bloom/commands/operations.hpp>
#include <bloom/commands/transaction.hpp>
#include <bloom/core/color.hpp>
#include <bloom/document/node_definition_registry.hpp>
#include <bloom/host/gpu_export_provider.hpp>
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
#include <chrono>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <memory>
#include <optional>
#include <source_location>
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

// Real headless render integration over the production Render::run: at least two EXR frames through
// an injected provider, positive genuine GPU counters and device epoch, strict CPU/process and
// encoded-output parity against a disabled-provider CPU reference, and the repeated provider still
// live. Requires BLOOM_TEST_VULKAN_LOADER (an absolute loader path); skips honestly otherwise.
void testNativeHeadlessRenderProvenanceAndParity(Expectations& expectations,
                                                 const bool requireDevice) {
    const char* loader = std::getenv("BLOOM_TEST_VULKAN_LOADER");
    if (loader == nullptr || *loader == '\0') {
        if (requireDevice) {
            expectations.expect(false,
                                "native render: --require-device needs BLOOM_TEST_VULKAN_LOADER");
        } else {
            std::cout << "NOTE: BLOOM_TEST_VULKAN_LOADER unset; skipping native headless render\n";
        }
        return;
    }
    auto session = buildSolidSession(expectations);
    if (session == nullptr) {
        return;
    }
    TempDirectory directory;
    expectations.expect(directory.isValid(), "native render: temp directory is available");
    if (!directory.isValid()) {
        return;
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
        return;
    }
    auto cpuProvider = host::GpuExportProvider::create(disabledOptions());
    cpuProvider->prepare(scheduler);
    const auto compositionId = session->snapshot().project().compositions().front().id();
    constexpr float kTolerance = 2e-6F;
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
    }
    expectations.expect(!provider->retirementComplete() && provider->evaluator() != nullptr,
                        "native render: the repeated injected provider stays live");
    expectations.expect(provider->shutdownAndWait(std::chrono::seconds(10)),
                        "native render: the provider retires with completion proof");
}

} // namespace

int main(int argc, char** argv) {
    Expectations expectations;
    bool requireDevice = false;
    for (int index = 1; index < argc; ++index) {
        if (std::string_view(argv[index]) == "--require-device") {
            requireDevice = true;
        }
    }
    try {
        testInjectedProviderIsReusedAndNotRetired(expectations);
        testLocalProviderIsRetired(expectations);
        testNativeHeadlessRenderProvenanceAndParity(expectations, requireDevice);
    } catch (const std::exception& error) {
        std::cerr << "unexpected exception: " << error.what() << '\n';
        return EXIT_FAILURE;
    }
    return expectations.failures() == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
