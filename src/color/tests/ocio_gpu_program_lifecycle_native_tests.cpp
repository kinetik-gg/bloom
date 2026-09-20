// Native proof for descriptor-bearing OCIO GPU programs: real LUT textures (1D/2D/3D) and real
// OCIO uniform declarations/values, executed on the Vulkan device and compared against the real CPU
// colour processors. The complete wrapper + OCIO GLSL is compiled once, off the owner thread, with
// the pinned glslangValidator/spirv-val; no runtime compiler exists here.

#include <bloom/color/ocio_cpu_display_processor.hpp>
#include <bloom/color/ocio_gpu_program.hpp>
#include <bloom/core/color.hpp>
#include <bloom/render/gpu_device.hpp>
#include <bloom/render/gpu_image.hpp>
#include <bloom/render/gpu_image_upload.hpp>
#include <bloom/render/gpu_ocio_program.hpp>
#include <bloom/render/image.hpp>
#include <bloom/render/image_types.hpp>

#include "ocio_gpu_program_fault.hpp"

#include <OpenColorIO/OpenColorIO.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>
#include <memory>
#include <optional>
#include <random>
#include <span>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace OCIO = OCIO_NAMESPACE;

namespace {

using bloom::render::GpuDevice;
using bloom::render::GpuDeviceCreationOptions;
using bloom::render::GpuDeviceState;
using bloom::render::GpuDisplayImage;
using bloom::render::GpuImage;
using bloom::render::GpuImageUpload;
using bloom::render::GpuImageUploadDiagnosticCode;
using bloom::render::GpuImageUploadPollResult;
using bloom::render::GpuOcioProgram;
using bloom::render::OcioGpuInterpolation;
using bloom::render::OcioGpuProgramDesc;
using bloom::render::OcioGpuTextureDimensions;
using bloom::render::Rgba32f;
using bloom::render::Rgba32fImage;
using bloom::render::Rgba32fImageBuilder;
using bloom::render::Rgba32fImageDescriptor;

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

[[nodiscard]] const char* quantizerGlsl() {
    return R"(
uint bloom_ocio_quantize(float value)
{
    if (!(value > 0.0)) { return 0u; }
    if (value >= 1.0) { return 255u; }
    if (value < 0.001) { return 0u; }
    int exponent = 0;
    float significand = frexp(value, exponent);
    uint mantissa = uint(significand * 16777216.0);
    uint scaled = mantissa * 255u;
    int shift = 24 - exponent;
    if (shift >= 33) { return 0u; }
    if (shift == 32) { return (scaled >= 0x80000000u) ? 1u : 0u; }
    uint quotient = scaled >> uint(shift);
    uint remainder = scaled & ((1u << uint(shift)) - 1u);
    uint roundBit = 1u << uint(shift - 1);
    return quotient + ((remainder >= roundBit) ? 1u : 0u);
}
)";
}

[[nodiscard]] std::string buildWrapperGlsl(const OcioGpuProgramDesc& desc, const bool display) {
    std::ostringstream out;
    out << "#version 460\n";
    out << "layout(local_size_x = 64) in;\n";
    out << "layout(set = 1, binding = 0, rgba32f) uniform readonly image2D bloom_ocio_input;\n";
    if (display) {
        out << "layout(set = 1, binding = 1, std430) buffer BloomOcioOutput { uint words[]; } "
               "bloom_ocio_output;\n";
    } else {
        out << "layout(set = 1, binding = 1, rgba32f) uniform writeonly image2D "
               "bloom_ocio_output;\n";
    }
    out << "layout(set = 1, binding = 2, std430) buffer BloomOcioStatus { uint flags[]; } "
           "bloom_ocio_status;\n";
    out << "layout(push_constant) uniform BloomOcioPush { uint pixelCount; uint width; uint "
           "height; "
           "} bloom_ocio_push;\n";
    out << desc.shaderText << "\n";
    if (display) {
        out << quantizerGlsl();
    }
    out << "void main() {\n";
    out << "  const uint bloom_ocio_stride_x = gl_NumWorkGroups.x * gl_WorkGroupSize.x;\n";
    out << "  uint index = gl_GlobalInvocationID.y * bloom_ocio_stride_x + "
           "gl_GlobalInvocationID.x;\n";
    out << "  if (index >= bloom_ocio_push.pixelCount) { return; }\n";
    out << "  ivec2 c = ivec2(int(index % bloom_ocio_push.width), int(index / "
           "bloom_ocio_push.width));\n";
    out << "  vec4 p = imageLoad(bloom_ocio_input, c);\n";
    out << "  float a = p.a;\n";
    out << "  vec3 s = (a != 0.0) ? p.rgb / a : vec3(0.0);\n";
    out << "  vec4 t = " << desc.functionName << "(vec4(s, 1.0));\n";
    if (display) {
        out << "  uint r = bloom_ocio_quantize(t.r); uint g = bloom_ocio_quantize(t.g); uint b = "
               "bloom_ocio_quantize(t.b); uint aa = bloom_ocio_quantize(a);\n";
        out << "  bloom_ocio_output.words[index] = r | (g << 8) | (b << 16) | (aa << 24);\n";
    } else {
        out << "  imageStore(bloom_ocio_output, c, vec4(t.rgb * a, a));\n";
    }
    out << "}\n";
    return out.str();
}

[[nodiscard]] std::optional<std::vector<std::uint32_t>> compileGlsl(const std::string& glsl) {
#ifdef BLOOM_GPUSHADER_TOOLS_DIR
    const std::filesystem::path tools{BLOOM_GPUSHADER_TOOLS_DIR};
    const std::filesystem::path glslang = tools / "glslangValidator";
    const std::filesystem::path spirvVal = tools / "spirv-val";
    if (!std::filesystem::exists(glslang) || !std::filesystem::exists(spirvVal)) {
        return std::nullopt;
    }
    static std::atomic<std::uint64_t> fixtureCounter{0};
    const auto base = std::filesystem::temp_directory_path() /
                      ("bloom_ocio_fixture_" + std::to_string(std::random_device{}()) + "_" +
                       std::to_string(fixtureCounter.fetch_add(1)));
    std::error_code directoryError;
    std::filesystem::create_directories(base, directoryError);
    const auto sourcePath = (base / "wrapper.comp").string();
    const auto spvPath = (base / "wrapper.spv").string();
    {
        std::ofstream source(sourcePath, std::ios::binary | std::ios::trunc);
        source << glsl;
        if (!source) {
            return std::nullopt;
        }
    }
    const std::string compile = "\"" + glslang.string() + "\" --target-env vulkan1.2 -V \"" +
                                sourcePath + "\" -o \"" + spvPath + "\"";
    if (std::system(compile.c_str()) != 0) {
        std::cerr << "glslangValidator rejected the resource wrapper program\n";
        return std::nullopt;
    }
    const std::string validate =
        "\"" + spirvVal.string() + "\" --target-env vulkan1.2 \"" + spvPath + "\"";
    if (std::system(validate.c_str()) != 0) {
        std::cerr << "spirv-val rejected the resource wrapper program\n";
        return std::nullopt;
    }
    std::ifstream module(spvPath, std::ios::binary);
    if (!module) {
        return std::nullopt;
    }
    const std::vector<char> bytes((std::istreambuf_iterator<char>(module)),
                                  std::istreambuf_iterator<char>());
    if (bytes.size() % sizeof(std::uint32_t) != 0) {
        return std::nullopt;
    }
    std::vector<std::uint32_t> words(bytes.size() / sizeof(std::uint32_t));
    std::memcpy(words.data(), bytes.data(), bytes.size());
    std::error_code ignored;
    std::filesystem::remove_all(base, ignored);
    return words;
#else
    static_cast<void>(glsl);
    return std::nullopt;
#endif
}

[[nodiscard]] std::shared_ptr<GpuOcioProgram> makeProgram(GpuDevice& device,
                                                          OcioGpuProgramDesc desc) {
    const bool display = desc.stage == bloom::render::OcioGpuProgramStage::DisplayPacking;
    auto spirv = compileGlsl(buildWrapperGlsl(desc, display));
    if (!spirv.has_value()) {
        return nullptr;
    }
    auto created = GpuOcioProgram::create(device, std::move(desc), *spirv, {});
    if (!created) {
        std::cerr << "GpuOcioProgram::create failed: " << created.diagnostic.message << '\n';
        return nullptr;
    }
    return std::shared_ptr<GpuOcioProgram>(std::move(created.program));
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

void testCreateTimeBudgets(Expectations& expectations, GpuDevice& device,
                           const bloom::color::ResolvedBloomNeutralConfig& aces) {
    const std::string_view display = "Rec.2100-PQ - Display";
    const std::string_view view = "ACES 1.1 - HDR Video (1000 nits & Rec.2020 lim)";
    auto lutDesc = bloom::color::buildOcioGpuProgramForDisplay(aces, display, view);
    if (!lutDesc.succeeded()) {
        expectations.expect(false, "the create-time budget fixture extracts");
        return;
    }
    auto lutSpirv = compileGlsl(buildWrapperGlsl(*lutDesc.program(), true));
    if (!lutSpirv.has_value()) {
        expectations.expect(false, "the create-time budget fixture compiles");
        return;
    }
    const auto expectOverBudget = [&](const std::string_view label,
                                      const bloom::render::GpuOcioProgramBudgets& budgets) {
        auto created = GpuOcioProgram::create(device, *lutDesc.program(), *lutSpirv, budgets);
        expectations.expect(!created && created.diagnostic.code ==
                                            bloom::render::GpuOcioProgramDiagnosticCode::OverBudget,
                            label);
    };
    expectOverBudget("maxLutBytes=0 is refused before any upload",
                     bloom::render::GpuOcioProgramBudgets{.maxLutBytes = 0});
    expectOverBudget("maxLutBytes=1 is refused before any upload",
                     bloom::render::GpuOcioProgramBudgets{.maxLutBytes = 1});
    expectOverBudget("maxOwnedBytes=0 is refused",
                     bloom::render::GpuOcioProgramBudgets{.maxOwnedBytes = 0});
    expectOverBudget("maxOwnedBytes=1 is refused",
                     bloom::render::GpuOcioProgramBudgets{.maxOwnedBytes = 1});
    expectOverBudget("maxOwnedBytes below the staging peak is refused",
                     bloom::render::GpuOcioProgramBudgets{.maxOwnedBytes = 1024});
    // No fault fuse was consumed: a genuinely budgeted create still succeeds.
    auto valid = GpuOcioProgram::create(device, *lutDesc.program(), *lutSpirv, {});
    expectations.expect(valid.hasValue(), "a valid budget create still succeeds after refusals");

    // 3D LUT boundary: the declared LUT bytes are checked before allocation, and a UINT64 ceiling
    // must not wrap.
    constexpr std::uint32_t edge = 3;
    std::vector<float> samples(edge * edge * edge * 3U, 0.25F);
    auto lut3d = bloom::color::buildOcioGpuProgramForLut3d(aces, "ACEScg", edge,
                                                           OcioGpuInterpolation::Linear, samples);
    if (!lut3d.succeeded()) {
        expectations.expect(false, "the 3D budget fixture extracts");
        return;
    }
    auto lut3dSpirv = compileGlsl(buildWrapperGlsl(*lut3d.program(), false));
    if (!lut3dSpirv.has_value()) {
        expectations.expect(false, "the 3D budget fixture compiles");
        return;
    }
    const std::uint64_t lutBytes = samples.size() * sizeof(float);
    auto tooSmall =
        GpuOcioProgram::create(device, *lut3d.program(), *lut3dSpirv,
                               bloom::render::GpuOcioProgramBudgets{.maxLutBytes = lutBytes - 1});
    expectations.expect(!tooSmall && tooSmall.diagnostic.code ==
                                         bloom::render::GpuOcioProgramDiagnosticCode::OverBudget,
                        "a 3D LUT one byte over maxLutBytes is refused before upload");
    auto huge =
        GpuOcioProgram::create(device, *lut3d.program(), *lut3dSpirv,
                               bloom::render::GpuOcioProgramBudgets{
                                   .maxOwnedBytes = std::numeric_limits<std::uint64_t>::max(),
                                   .maxLutBytes = std::numeric_limits<std::uint64_t>::max()});
    expectations.expect(huge.hasValue(),
                        "a UINT64 budget ceiling does not wrap and admits a valid 3D LUT");
}

void testCancellationDuringResourceInit(Expectations& expectations, GpuDevice& device,
                                        const bloom::color::ResolvedBloomNeutralConfig& aces) {
    const std::string_view display = "Rec.2100-PQ - Display";
    const std::string_view view = "ACES 1.1 - HDR Video (1000 nits & Rec.2020 lim)";
    auto desc = bloom::color::buildOcioGpuProgramForDisplay(aces, display, view);
    if (!desc.succeeded()) {
        expectations.expect(false, "the cancellation fixture extracts");
        return;
    }
    auto spirv = compileGlsl(buildWrapperGlsl(*desc.program(), true));
    if (!spirv.has_value()) {
        expectations.expect(false, "the cancellation fixture compiles");
        return;
    }
    bloom::render::ocio_program_detail::setUploadFenceOverrideForTest(
        bloom::render::ocio_program_detail::UploadFenceOverride::CancelAfterSubmit);
    auto created = GpuOcioProgram::create(device, *desc.program(), *spirv, {});
    expectations.expect(!created && created.diagnostic.code ==
                                        bloom::render::GpuOcioProgramDiagnosticCode::Cancelled,
                        "cancellation during resource init publishes no program");
    expectations.expect(bloom::render::ocio_program_detail::uploadQuarantineOccupiedForTest(),
                        "a post-submit cancellation retains resources until fence proof");
    bloom::render::ocio_program_detail::setUploadFenceOverrideForTest(
        bloom::render::ocio_program_detail::UploadFenceOverride::None);
    for (int attempt = 0;
         attempt < 200 && bloom::render::ocio_program_detail::uploadQuarantineOccupiedForTest();
         ++attempt) {
        bloom::render::ocio_program_detail::retireUploadQuarantineForTest();
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    expectations.expect(!bloom::render::ocio_program_detail::uploadQuarantineOccupiedForTest(),
                        "the cancelled upload retires after fence proof");
    auto valid = GpuOcioProgram::create(device, *desc.program(), *spirv, {});
    expectations.expect(valid.hasValue(),
                        "a valid create succeeds after the cancelled upload retires");
}

[[nodiscard]] std::uint8_t lifecycleQuantize(const double value) {
    return static_cast<std::uint8_t>(std::floor(std::clamp(value, 0.0, 1.0) * 255.0 + 0.5));
}

void testCancelRetryWithoutHelper(Expectations& expectations, GpuDevice& device,
                                  const bloom::color::ResolvedBloomNeutralConfig& aces) {
    const std::string_view display = "Rec.2100-PQ - Display";
    const std::string_view view = "ACES 1.1 - HDR Video (1000 nits & Rec.2020 lim)";
    auto desc = bloom::color::buildOcioGpuProgramForDisplay(aces, display, view);
    if (!desc.succeeded()) {
        expectations.expect(false, "the cancel/retry fixture extracts");
        return;
    }
    auto spirv = compileGlsl(buildWrapperGlsl(*desc.program(), true));
    if (!spirv.has_value()) {
        expectations.expect(false, "the cancel/retry fixture compiles");
        return;
    }
    // (a) Deterministic post-submit cancellation; then retry WITHOUT the test retirement helper, so
    // production's same-owner reservation must retire the quarantine once the fence proves it.
    bloom::render::ocio_program_detail::setUploadFenceOverrideForTest(
        bloom::render::ocio_program_detail::UploadFenceOverride::CancelAfterSubmit);
    auto cancelled = GpuOcioProgram::create(device, *desc.program(), *spirv, {});
    expectations.expect(!cancelled && cancelled.diagnostic.code ==
                                          bloom::render::GpuOcioProgramDiagnosticCode::Cancelled,
                        "post-submit cancellation publishes no program");
    expectations.expect(bloom::render::ocio_program_detail::uploadQuarantineOccupiedForTest(),
                        "the cancelled upload retains its resources");
    bloom::render::ocio_program_detail::setUploadFenceOverrideForTest(
        bloom::render::ocio_program_detail::UploadFenceOverride::None);
    // (b) A foreign thread must not retire or destroy the quarantine.
    std::thread foreign(
        [&]() { bloom::render::ocio_program_detail::retireUploadQuarantineForTest(); });
    foreign.join();
    expectations.expect(bloom::render::ocio_program_detail::uploadQuarantineOccupiedForTest(),
                        "a foreign-thread retire does not touch the quarantine");

    std::shared_ptr<GpuOcioProgram> program;
    for (int attempt = 0; attempt < 200 && program == nullptr; ++attempt) {
        auto created = GpuOcioProgram::create(device, *desc.program(), *spirv, {});
        if (created) {
            program = std::shared_ptr<GpuOcioProgram>(std::move(created.program));
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    expectations.expect(program != nullptr,
                        "a retry create succeeds after production retires the cancelled upload");
    if (program != nullptr) {
        auto uploader = GpuImageUpload::create(device);
        const auto pixels = fixturePixels(4, 3);
        const auto input = uploader ? uploadImage(*uploader.upload, 4, 3, pixels) : nullptr;
        if (input != nullptr) {
            const auto accepted = program->beginDisplay(input, {}, kBudget);
            auto poll = program->poll();
            while (poll == bloom::render::GpuOcioProgramPollResult::Pending) {
                poll = program->poll();
            }
            auto output = program->takeDisplayOutput();
            const auto readback = output.isValid()
                                      ? bloom::render::readbackResidentDisplayImage(output, kBudget)
                                      : bloom::render::GpuDisplayImageReadback{};
            const auto handle = bloom::color::buildCpuDisplayProcessorForView(aces, display, view);
            const auto* const processor = handle.handle();
            expectations.expect(accepted.code ==
                                        bloom::render::GpuOcioProgramDiagnosticCode::None &&
                                    poll == bloom::render::GpuOcioProgramPollResult::Ready &&
                                    readback.hasValue() && processor != nullptr,
                                "the retried program executes end to end");
            if (readback.hasValue() && processor != nullptr) {
                std::size_t mismatches = 0;
                for (std::size_t index = 0; index < pixels.size(); ++index) {
                    const auto& p = pixels[index];
                    std::array<double, 3> straight{0.0, 0.0, 0.0};
                    if (p.alpha() != 0.0F) {
                        const double alpha = static_cast<double>(p.alpha());
                        straight = {static_cast<double>(p.red()) / alpha,
                                    static_cast<double>(p.green()) / alpha,
                                    static_cast<double>(p.blue()) / alpha};
                    }
                    const auto displayValue = processor->referenceToDisplayLinear(
                        bloom::core::Color4d{straight[0], straight[1], straight[2], 1.0});
                    if (!displayValue.has_value()) {
                        ++mismatches;
                        continue;
                    }
                    const auto& gpu = readback.pixels[index];
                    if (std::abs(static_cast<int>(gpu.red) - lifecycleQuantize(displayValue->red)) >
                            1 ||
                        std::abs(static_cast<int>(gpu.green) -
                                 lifecycleQuantize(displayValue->green)) > 1 ||
                        std::abs(static_cast<int>(gpu.blue) -
                                 lifecycleQuantize(displayValue->blue)) > 1 ||
                        gpu.alpha != lifecycleQuantize(static_cast<double>(p.alpha()))) {
                        ++mismatches;
                    }
                }
                expectations.expect(mismatches == 0, "the retried program matches the CPU oracle");
            }
        } else {
            expectations.expect(false, "the retry parity input uploads");
        }
    }

    // (c) Throwing cancellation callback AFTER submission: no unwind may destroy live resources.
    int calls = 0;
    bloom::render::GpuOcioProgramCancellation throwing = [&calls]() -> bool {
        ++calls;
        if (calls >= 5) {
            throw std::runtime_error("cancellation callback threw");
        }
        return false;
    };
    auto threw = GpuOcioProgram::create(device, *desc.program(), *spirv, {}, throwing);
    expectations.expect(!threw && threw.diagnostic.code ==
                                      bloom::render::GpuOcioProgramDiagnosticCode::Cancelled,
                        "a throwing post-submit callback is treated as cancellation");
    expectations.expect(bloom::render::ocio_program_detail::uploadQuarantineOccupiedForTest(),
                        "a throwing callback still retains resources");
    std::shared_ptr<GpuOcioProgram> retried;
    for (int attempt = 0; attempt < 200 && retried == nullptr; ++attempt) {
        auto created = GpuOcioProgram::create(device, *desc.program(), *spirv, {});
        if (created) {
            retried = std::shared_ptr<GpuOcioProgram>(std::move(created.program));
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    expectations.expect(retried != nullptr,
                        "a retry create succeeds after a throwing-callback cancellation");
}

void testUnprovenUploadQuarantine(Expectations& expectations, GpuDevice& device,
                                  const bloom::color::ResolvedBloomNeutralConfig& aces) {
    const std::string_view display = "Rec.2100-PQ - Display";
    const std::string_view view = "ACES 1.1 - HDR Video (1000 nits & Rec.2020 lim)";
    auto desc = bloom::color::buildOcioGpuProgramForDisplay(aces, display, view);
    if (!desc.succeeded()) {
        expectations.expect(false, "the quarantine fixture extracts");
        return;
    }
    const bool displayArm =
        desc.program()->stage == bloom::render::OcioGpuProgramStage::DisplayPacking;
    auto spirv = compileGlsl(buildWrapperGlsl(*desc.program(), displayArm));
    expectations.expect(spirv.has_value(), "the quarantine fixture compiles");
    if (!spirv.has_value()) {
        return;
    }
    bloom::render::ocio_program_detail::setUploadFenceOverrideForTest(
        bloom::render::ocio_program_detail::UploadFenceOverride::NotReady);
    auto created = GpuOcioProgram::create(device, *desc.program(), *spirv, {});
    expectations.expect(!created &&
                            created.diagnostic.code ==
                                bloom::render::GpuOcioProgramDiagnosticCode::DeviceUnavailable,
                        "an unproven LUT upload is a typed DeviceUnavailable failure");
    expectations.expect(bloom::render::ocio_program_detail::uploadQuarantineOccupiedForTest(),
                        "the unproven upload retains every live resource in the quarantine");
    // Resources the GPU may still read are NOT destroyed. Give the copy time to complete, then
    // retire the quarantine; if any resource had been freed early this path would fault.
    bloom::render::ocio_program_detail::setUploadFenceOverrideForTest(
        bloom::render::ocio_program_detail::UploadFenceOverride::None);
    for (int attempt = 0;
         attempt < 100 && bloom::render::ocio_program_detail::uploadQuarantineOccupiedForTest();
         ++attempt) {
        bloom::render::ocio_program_detail::retireUploadQuarantineForTest();
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    expectations.expect(!bloom::render::ocio_program_detail::uploadQuarantineOccupiedForTest(),
                        "the quarantine retires only after the fence proves completion");
}

void testBudgetsAndOwnership(Expectations& expectations, GpuDevice& device,
                             const bloom::color::ResolvedBloomNeutralConfig& neutral) {
    auto desc = bloom::color::buildOcioGpuProgramForDisplay(neutral, neutral.displayName(),
                                                            neutral.viewName());
    if (!desc.succeeded()) {
        expectations.expect(false, "the neutral display program extracts for budget coverage");
        return;
    }
    auto program = makeProgram(device, *desc.program());
    auto uploader = GpuImageUpload::create(device);
    if (program == nullptr || !uploader) {
        expectations.expect(false, "the budget host program is created");
        return;
    }
    const auto pixels = fixturePixels(2, 2);
    const auto input = uploadImage(*uploader.upload, 2, 2, pixels);
    if (input == nullptr) {
        expectations.expect(false, "the budget input uploads");
        return;
    }
    const auto overBudget = program->beginDisplay(input, {}, 1);
    expectations.expect(overBudget.code == bloom::render::GpuOcioProgramDiagnosticCode::OverBudget,
                        "a too-small byte budget is refused");
    std::thread wrongThread([&]() {
        const auto result = program->beginDisplay(input, {}, kBudget);
        expectations.expect(result.code == bloom::render::GpuOcioProgramDiagnosticCode::WrongThread,
                            "a foreign-thread begin is refused as WrongThread");
    });
    wrongThread.join();
    const auto accepted = program->beginDisplay(input, {}, kBudget);
    expectations.expect(accepted.code == bloom::render::GpuOcioProgramDiagnosticCode::None,
                        "the cancel-target job is accepted");
    program->cancel();
    auto poll = program->poll();
    while (poll == bloom::render::GpuOcioProgramPollResult::Pending) {
        poll = program->poll();
    }
    expectations.expect(poll == bloom::render::GpuOcioProgramPollResult::Failure &&
                            program->diagnostic().code ==
                                bloom::render::GpuOcioProgramDiagnosticCode::Cancelled,
                        "a cancelled job publishes no output");
}

} // namespace

int main(int argc, char** argv) {
    const bool requireDevice = parseRequireDevice(argc, argv);
    Expectations expectations;
    const auto revision = bloom::color::ocioBuiltInContentRevision(
        bloom::color::OcioConfigLocatorKind::BloomBuiltIn, bloom::color::kAcesCgV1ConfigUri);
    if (!revision.has_value()) {
        std::cout << "SKIP: the ACES built-in is unavailable\n";
        return kSkipExit;
    }
    auto acesResolution =
        bloom::color::resolveOcioBuiltIn(bloom::color::OcioConfigLocatorKind::BloomBuiltIn,
                                         bloom::color::kAcesCgV1ConfigUri, *revision, "ACEScg");
    auto aces = std::move(acesResolution).takeResolved();
    auto neutralResolution = bloom::color::resolveBloomNeutralV1BuiltIn(
        bloom::color::OcioConfigLocatorKind::BloomBuiltIn, bloom::color::kBloomNeutralV1ConfigUri,
        bloom::color::kBloomNeutralV1ConfigDigest);
    auto neutral = std::move(neutralResolution).takeResolved();
    if (!aces.has_value() || !neutral.has_value()) {
        std::cerr << "FAILED: the built-in configs do not resolve\n";
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
    testBudgetsAndOwnership(expectations, *device.device, *neutral);
    testCreateTimeBudgets(expectations, *device.device, *aces);
    testCancellationDuringResourceInit(expectations, *device.device, *aces);
    testCancelRetryWithoutHelper(expectations, *device.device, *aces);
    testUnprovenUploadQuarantine(expectations, *device.device, *aces);
    if (expectations.failures() != 0) {
        std::cerr << expectations.failures() << " lifecycle native expectation(s) failed\n";
        return 1;
    }
    std::cout << "PASS: native OCIO resource lifecycle\n";
    return 0;
}
