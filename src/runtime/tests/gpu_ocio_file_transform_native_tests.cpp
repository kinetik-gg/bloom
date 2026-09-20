// Strict native proof for the production OCIO wrapper's FileTransform arm. A real .cube LUT is read
// through color::readLutFile (the exact bytes and content digest), extracted through the isolated
// bloom-color-worker with the requested interpolation/direction, wrapped by the production wrapper
// (which uses the shared precise-sampling adapter), compiled off-device, and executed on the device
// owner thread. The unchanged isolated CPU FileTransform processor is the strict 2e-6 oracle.
//
// The preparer warm key folds the LUT content digest, format, interpolation, direction, and the
// process/working space ids, so a changed LUT is a different command. No CPU colour-space processor
// is used here. A missing device is an explicit SKIP unless --require-device is passed.

#include <bloom/color/ocio_builtin_registry.hpp>
#include <bloom/color/ocio_cpu_file_transform_processor.hpp>
#include <bloom/color/ocio_gpu_program.hpp>
#include <bloom/render/gpu_device.hpp>
#include <bloom/render/gpu_image.hpp>
#include <bloom/render/gpu_image_upload.hpp>
#include <bloom/render/image.hpp>
#include <bloom/render/image_types.hpp>
#include <bloom/runtime/gpu_ocio_preparation.hpp>
#include <bloom/runtime/gpu_ocio_program_executor.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <optional>
#include <random>
#include <string>
#include <string_view>
#include <vector>

namespace {

using bloom::render::GpuDevice;
using bloom::render::GpuDeviceCreationOptions;
using bloom::render::GpuDeviceState;
using bloom::render::GpuImage;
using bloom::render::GpuImageUpload;
using bloom::render::GpuImageUploadDiagnosticCode;
using bloom::render::GpuImageUploadPollResult;
using bloom::render::Rgba32f;
using bloom::render::Rgba32fImage;
using bloom::render::Rgba32fImageBuilder;
using bloom::render::Rgba32fImageDescriptor;
using bloom::runtime::GpuOcioCommandGeometry;
using bloom::runtime::GpuOcioCompileOptions;
using bloom::runtime::GpuOcioExecutorDiagnosticCode;
using bloom::runtime::GpuOcioExecutorPollResult;
using bloom::runtime::GpuOcioOutputEncoding;
using bloom::runtime::GpuOcioProgramExecutor;
using bloom::runtime::GpuOcioProgramPreparer;
using bloom::runtime::GpuOcioTransformKind;
using bloom::runtime::GpuOcioTransformSpec;

constexpr std::uint64_t kBudget = std::uint64_t{1} << 32;
constexpr int kSkipExit = 77;

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

[[nodiscard]] bool parseRequireDevice(const int argc, char** argv) {
    for (int index = 1; index < argc; ++index) {
        if (std::string_view(argv[index]) == "--require-device") {
            return true;
        }
    }
    return false;
}

[[nodiscard]] GpuOcioCompileOptions compileOptions() {
    GpuOcioCompileOptions options;
    options.glslangValidatorPath = std::string(BLOOM_GPUSHADER_TOOLS_DIR) + "/glslangValidator";
    options.spirvValPath = std::string(BLOOM_GPUSHADER_TOOLS_DIR) + "/spirv-val";
    return options;
}

void writeSteep1dCube(std::ofstream& file) {
    file << "LUT_1D_SIZE 4\n";
    file << "0 0 0\n";
    file << "0.001 0 0\n";
    file << "1 1 0.5\n";
    file << "1 1 1\n";
}

void writeCurve1dCube(std::ofstream& file) {
    file << "LUT_1D_SIZE 8\n";
    for (int index = 0; index < 8; ++index) {
        const float value = static_cast<float>(index) / 7.0F;
        file << value << ' ' << value * value << ' ' << 0.5F * value << '\n';
    }
}

[[nodiscard]] std::vector<Rgba32f> fixturePixels(const std::uint32_t width,
                                                 const std::uint32_t height) {
    std::vector<Rgba32f> pixels(static_cast<std::size_t>(width) * height, Rgba32f::transparent());
    for (std::uint32_t y = 0; y < height; ++y) {
        for (std::uint32_t x = 0; x < width; ++x) {
            const float fx = static_cast<float>(x) / static_cast<float>(width);
            const float fy = static_cast<float>(y) / static_cast<float>(height);
            const float alpha = ((x + y) % 3 == 0) ? 0.0F : 0.25F + 0.5F * fx;
            const auto value = Rgba32f::fromPremultiplied((0.2F + fx) * alpha, (0.1F + fy) * alpha,
                                                          (0.3F + 0.4F * fx) * alpha, alpha);
            if (value) {
                pixels[static_cast<std::size_t>(y) * width + x] = *value.value();
            }
        }
    }
    return pixels;
}

[[nodiscard]] std::shared_ptr<const GpuImage> uploadImage(GpuImageUpload& uploader,
                                                          const std::uint32_t width,
                                                          const std::uint32_t height,
                                                          const std::vector<Rgba32f>& pixels) {
    const auto windowResult = bloom::render::ImageWindow::create(0, 0, width, height);
    if (!windowResult) {
        return nullptr;
    }
    const auto descriptor = Rgba32fImageDescriptor::create(
        *windowResult.value(), *windowResult.value(), bloom::core::PixelAspectRatio::square());
    if (!descriptor || descriptor.value()->layout().pixelCount != pixels.size()) {
        return nullptr;
    }
    auto builder = Rgba32fImageBuilder::create(*descriptor.value(), kBudget);
    if (!builder) {
        return nullptr;
    }
    for (std::uint32_t y = 0; y < height; ++y) {
        const auto row = builder.value()->row(y);
        if (!row) {
            return nullptr;
        }
        for (std::uint32_t x = 0; x < width; ++x) {
            (*row.value())[x] = pixels[static_cast<std::size_t>(y) * width + x];
        }
    }
    auto frozen = std::move(*builder.value()).freeze();
    if (!frozen) {
        return nullptr;
    }
    auto source = std::make_shared<const Rgba32fImage>(std::move(*frozen.value()));
    if (uploader.begin({source}, kBudget).code != GpuImageUploadDiagnosticCode::None) {
        return nullptr;
    }
    auto poll = GpuImageUploadPollResult::Pending;
    while (poll == GpuImageUploadPollResult::Pending) {
        poll = uploader.poll();
    }
    if (poll != GpuImageUploadPollResult::Ready) {
        return nullptr;
    }
    return std::make_shared<GpuImage>(uploader.takeImage());
}

[[nodiscard]] bool close(const float a, const float b) {
    if (a == b) {
        return true;
    }
    const double absolute = std::fabs(static_cast<double>(a) - static_cast<double>(b));
    const double magnitude =
        std::max(std::fabs(static_cast<double>(a)), std::fabs(static_cast<double>(b)));
    return absolute <= 2e-6 || absolute <= 2e-6 * magnitude;
}

[[nodiscard]] bool writeFixture(const std::filesystem::path& path, void (*writer)(std::ofstream&)) {
    std::ofstream file(path);
    if (!file) {
        return false;
    }
    writer(file);
    return static_cast<bool>(file);
}

void testFileTransform(Expectations& expectations, GpuDevice& device,
                       const bloom::color::ResolvedBloomNeutralConfig& resolved,
                       const GpuOcioCompileOptions& options) {
    const auto base = std::filesystem::temp_directory_path() /
                      ("bloom_runtime_ft_" + std::to_string(std::random_device{}()));
    std::error_code directoryError;
    std::filesystem::create_directories(base, directoryError);
    expectations.expect(!directoryError, "the FileTransform fixture directory is creatable");
    if (directoryError) {
        return;
    }
    const std::string process(resolved.processColorSpaceId());
    const auto steepPath = base / "steep1d.cube";
    expectations.expect(writeFixture(steepPath, &writeSteep1dCube),
                        "the steep 1D LUT fixture is written");
    const auto steep = bloom::color::readLutFile(steepPath);
    expectations.expect(steep.error == bloom::color::LutError::None,
                        "the steep 1D LUT passes preflight");
    if (steep.error != bloom::color::LutError::None) {
        return;
    }
    auto lutHandle = std::make_shared<const bloom::color::LutFile>(steep);

    GpuOcioProgramPreparer preparer;
    GpuOcioTransformSpec spec;
    spec.kind = GpuOcioTransformKind::FileTransform;
    spec.lutFile = lutHandle;
    spec.interpolation = bloom::color::LutInterpolation::Best;
    spec.direction = bloom::color::LutDirection::Forward;
    spec.processSpaceId = process;
    spec.workingSpaceId = process;
    constexpr GpuOcioCommandGeometry geometry{5, 3};
    const auto prepared = preparer.prepare(resolved, spec, geometry, options);
    expectations.expect(prepared.hasValue(), "the production FileTransform command prepares");
    if (!prepared) {
        std::cerr << "FileTransform prepare diagnostic: " << prepared.diagnostic << '\n';
        return;
    }
    expectations.expect(prepared.command->encoding() == GpuOcioOutputEncoding::FinalRgba32f,
                        "the FileTransform command is ProcessEffect");
    expectations.expect(prepared.command->wrapperVersion() ==
                            bloom::color::kOcioGpuPreciseSamplingVersion,
                        "the production wrapper records the shared precise-sampling version");
    expectations.expect(!prepared.command->program().textures.empty(),
                        "the FileTransform program carries a real LUT texture");

    // Warm identical request: cache hit, no recompile.
    const auto countersBeforeWarm = preparer.counters();
    const auto warm = preparer.prepare(resolved, spec, geometry, options);
    expectations.expect(warm.hasValue() && warm.command == prepared.command,
                        "the warm FileTransform request is a cache hit");
    expectations.expect(preparer.counters().compiles == countersBeforeWarm.compiles,
                        "the warm FileTransform request recompiles nothing");

    // A changed LUT content digest is a different command.
    const auto curvePath = base / "curve1d.cube";
    expectations.expect(writeFixture(curvePath, &writeCurve1dCube),
                        "the curve 1D LUT fixture is written");
    const auto curve = bloom::color::readLutFile(curvePath);
    expectations.expect(curve.error == bloom::color::LutError::None,
                        "the curve 1D LUT passes preflight");
    if (curve.error == bloom::color::LutError::None) {
        auto curveSpec = spec;
        curveSpec.lutFile = std::make_shared<const bloom::color::LutFile>(curve);
        const auto changed = preparer.prepare(resolved, curveSpec, geometry, options);
        expectations.expect(changed.hasValue(), "the changed-LUT command prepares");
        if (changed) {
            expectations.expect(changed.command->identity() != prepared.command->identity(),
                                "a changed LUT content digest changes the command identity");
        }
    }

    auto executor = GpuOcioProgramExecutor::create(device);
    auto uploader = GpuImageUpload::create(device);
    expectations.expect(executor.hasValue() && uploader.hasValue(),
                        "the FileTransform executor and uploader host");
    if (!executor || !uploader) {
        return;
    }
    const auto pixels = fixturePixels(geometry.width, geometry.height);
    const auto input = uploadImage(*uploader.upload, geometry.width, geometry.height, pixels);
    expectations.expect(input != nullptr, "the FileTransform input uploads");
    if (input == nullptr) {
        return;
    }
    const auto accepted = executor.executor->begin(prepared.command, input, {}, kBudget);
    expectations.expect(accepted.code == GpuOcioExecutorDiagnosticCode::None,
                        "begin accepts the FileTransform command");
    auto poll = executor.executor->poll();
    while (poll == GpuOcioExecutorPollResult::Pending) {
        poll = executor.executor->poll();
    }
    expectations.expect(poll == GpuOcioExecutorPollResult::Ready,
                        "the FileTransform dispatch completes");
    auto output = executor.executor->takeEffectOutput();
    expectations.expect(output != nullptr, "the FileTransform resident output publishes");
    if (output == nullptr) {
        return;
    }
    const auto readback = bloom::render::readbackResidentImage(*output, kBudget);
    expectations.expect(readback.hasValue(), "the FileTransform output reads back for the oracle");
    if (!readback) {
        return;
    }
    // Unchanged isolated CPU FileTransform oracle (not a CPU colour-space processor).
    auto cpu = bloom::color::CpuFileTransformProcessor::prepare(*lutHandle, spec.interpolation,
                                                                spec.direction);
    expectations.expect(cpu.processor != nullptr, "the CPU FileTransform oracle prepares");
    if (cpu.processor == nullptr) {
        return;
    }
    std::size_t mismatches = 0;
    double maxError = 0.0;
    for (std::size_t index = 0; index < pixels.size(); ++index) {
        const auto& source = pixels[index];
        std::array<float, 4> straight{0.0F, 0.0F, 0.0F, 0.0F};
        if (source.alpha() != 0.0F) {
            straight = {source.red() / source.alpha(), source.green() / source.alpha(),
                        source.blue() / source.alpha(), source.alpha()};
        }
        std::span<std::array<float, 4>> span(&straight, 1);
        if (cpu.processor->apply(span) != bloom::color::LutError::None) {
            ++mismatches;
            continue;
        }
        const auto& gpu = readback.pixels[index];
        const float expectedR = straight[0] * source.alpha();
        const float expectedG = straight[1] * source.alpha();
        const float expectedB = straight[2] * source.alpha();
        if (!close(gpu.red(), expectedR) || !close(gpu.green(), expectedG) ||
            !close(gpu.blue(), expectedB) || gpu.alpha() != source.alpha()) {
            ++mismatches;
        }
        maxError = std::max(
            maxError,
            std::max(
                {std::fabs(static_cast<double>(gpu.red()) - static_cast<double>(expectedR)),
                 std::fabs(static_cast<double>(gpu.green()) - static_cast<double>(expectedG)),
                 std::fabs(static_cast<double>(gpu.blue()) - static_cast<double>(expectedB))}));
    }
    if (mismatches != 0) {
        std::cerr << "FileTransform mismatches=" << mismatches << " maxAbsError=" << maxError
                  << '\n';
    }
    expectations.expect(mismatches == 0,
                        "every FileTransform pixel is within the strict 2e-6 CPU oracle gate");
    std::filesystem::remove_all(base, directoryError);
}

} // namespace

int main(int argc, char** argv) {
    const bool requireDevice = parseRequireDevice(argc, argv);
    Expectations expectations;
#ifndef BLOOM_GPUSHADER_TOOLS_DIR
    std::cout << "SKIP: BLOOM_GPUSHADER_TOOLS_DIR is not set\n";
    return kSkipExit;
#else
    auto neutralResolution = bloom::color::resolveBloomNeutralV1BuiltIn(
        bloom::color::OcioConfigLocatorKind::BloomBuiltIn, bloom::color::kBloomNeutralV1ConfigUri,
        bloom::color::kBloomNeutralV1ConfigDigest);
    auto neutral = std::move(neutralResolution).takeResolved();
    if (!neutral.has_value()) {
        std::cerr << "FAILED: the Bloom Neutral built-in does not resolve\n";
        return 1;
    }
    GpuDeviceCreationOptions options;
    auto device = GpuDevice::create(options);
    if (!device) {
        if (requireDevice) {
            std::cerr << "FAIL: required device unavailable: " << device.diagnostic.message << '\n';
            return 1;
        }
        std::cout << "SKIP: no compatible Vulkan device available: " << device.diagnostic.message
                  << '\n';
        return kSkipExit;
    }
    expectations.expect(device.device->state() == GpuDeviceState::Ready, "the device is Ready");
    testFileTransform(expectations, *device.device, *neutral, compileOptions());
    if (expectations.failures() != 0) {
        std::cerr << expectations.failures() << " OCIO FileTransform expectation(s) failed\n";
        return 1;
    }
    std::cout << "PASS: runtime OCIO FileTransform native proof\n";
    return 0;
#endif
}
