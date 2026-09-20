#ifndef BLOOM_COLOR_TESTS_OCIO_GPU_PROGRAM_RESOURCES_TEST_SUPPORT_HPP
#define BLOOM_COLOR_TESTS_OCIO_GPU_PROGRAM_RESOURCES_TEST_SUPPORT_HPP

// Shared harness for the native OCIO resource, dispatch-plan, and reflection tests. Header-only
// so all three translation units share one validating oracle and one Expectations type.

#include <bloom/color/ocio_cpu_display_processor.hpp>
#include <bloom/color/ocio_gpu_program.hpp>
#include <bloom/core/color.hpp>
#include <bloom/render/gpu_device.hpp>
#include <bloom/render/gpu_image.hpp>
#include <bloom/render/gpu_image_upload.hpp>
#include <bloom/render/gpu_ocio_program.hpp>
#include <bloom/render/image.hpp>
#include <bloom/render/image_types.hpp>

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
#include <memory>
#include <optional>
#include <random>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace OCIO = OCIO_NAMESPACE;

namespace bloom::color::ocio_resources_test {

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

[[nodiscard]] inline bool parseRequireDevice(const int argc, char** argv) {
    for (int index = 1; index < argc; ++index) {
        if (std::string_view(argv[index]) == "--require-device") {
            return true;
        }
    }
    return false;
}

[[nodiscard]] inline std::shared_ptr<const GpuImage>
uploadImage(GpuImageUpload& uploader, const std::uint32_t width, const std::uint32_t height,
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

[[nodiscard]] inline const char* quantizerGlsl() {
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

[[nodiscard]] inline std::string buildWrapperGlsl(const OcioGpuProgramDesc& desc,
                                                  const bool display) {
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

[[nodiscard]] inline std::optional<std::vector<std::uint32_t>>
compileGlsl(const std::string& glsl) {
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
    // NOLINTNEXTLINE(bugprone-command-processor) -- bounded test fixture: pinned build tool path.
    if (std::system(compile.c_str()) != 0) {
        std::cerr << "glslangValidator rejected the resource wrapper program\n";
        return std::nullopt;
    }
    const std::string validate =
        "\"" + spirvVal.string() + "\" --target-env vulkan1.2 \"" + spvPath + "\"";
    // NOLINTNEXTLINE(bugprone-command-processor) -- bounded test fixture: pinned build tool path.
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

[[nodiscard]] inline std::shared_ptr<GpuOcioProgram>
makeProgram(GpuDevice& device, OcioGpuProgramDesc desc,
            const bloom::render::GpuOcioProgramBudgets& budgets) {
    const bool display = desc.stage == bloom::render::OcioGpuProgramStage::DisplayPacking;
    auto spirv = compileGlsl(buildWrapperGlsl(desc, display));
    if (!spirv.has_value()) {
        return nullptr;
    }
    auto created = GpuOcioProgram::create(device, std::move(desc), *spirv, budgets);
    if (!created) {
        std::cerr << "GpuOcioProgram::create failed: " << created.diagnostic.message << '\n';
        return nullptr;
    }
    return std::shared_ptr<GpuOcioProgram>(std::move(created.program));
}

[[nodiscard]] inline std::shared_ptr<GpuOcioProgram> makeProgram(GpuDevice& device,
                                                                 OcioGpuProgramDesc desc) {
    return makeProgram(device, std::move(desc), {});
}

// Bounded, non-busy completion wait. Never loops without a finite attempt ceiling.
[[nodiscard]] inline bool pollOcio(Expectations& expectations, GpuOcioProgram& program,
                                   const std::string_view label) {
    constexpr int kMaxAttempts = 20000;
    for (int attempt = 0; attempt < kMaxAttempts; ++attempt) {
        const auto result = program.poll();
        if (result == bloom::render::GpuOcioProgramPollResult::Ready) {
            return true;
        }
        if (result != bloom::render::GpuOcioProgramPollResult::Pending) {
            expectations.expect(false, std::string(label) + ": the dispatch did not complete");
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    expectations.expect(false,
                        std::string(label) + ": the dispatch did not retire in the bounded window");
    return false;
}

[[nodiscard]] inline std::uint8_t quantize(const double value) {
    return static_cast<std::uint8_t>(std::floor(std::clamp(value, 0.0, 1.0) * 255.0 + 0.5));
}

[[nodiscard]] inline bool closeEnough(const double a, const double b) {
    if (a == b) {
        return true;
    }
    const double absolute = std::fabs(a - b);
    const double magnitude = std::max(std::fabs(a), std::fabs(b));
    return absolute <= 2e-6 || absolute <= 2e-6 * magnitude;
}

[[nodiscard]] inline std::vector<Rgba32f> fixturePixels(const std::uint32_t width,
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

[[nodiscard]] inline OCIO::ConstCPUProcessorRcPtr exposureContrastCpu(const double exposure,
                                                                      const double contrast) {
    auto config = OCIO::Config::CreateFromBuiltinConfig("cg-config-v1.0.0_aces-v1.3_ocio-v2.1");
    auto transform = OCIO::ExposureContrastTransform::Create();
    transform->setStyle(OCIO::EXPOSURE_CONTRAST_LINEAR);
    transform->setExposure(exposure);
    transform->setContrast(contrast);
    auto processor =
        config->getProcessor(OCIO::Context::Create(), transform, OCIO::TRANSFORM_DIR_FORWARD);
    return processor->getDefaultCPUProcessor();
}

template <typename Oracle>
std::size_t compareEffect(Expectations& expectations, const std::vector<Rgba32f>& pixels,
                          const std::vector<Rgba32f>& gpu, const Oracle& oracle,
                          const std::string_view label) {
    std::size_t mismatches = 0;
    for (std::size_t index = 0; index < pixels.size(); ++index) {
        const auto& p = pixels[index];
        std::array<float, 3> straight{0.0F, 0.0F, 0.0F};
        if (p.alpha() != 0.0F) {
            straight = {p.red() / p.alpha(), p.green() / p.alpha(), p.blue() / p.alpha()};
        }
        oracle(straight);
        const auto& g = gpu[index];
        const double alpha = static_cast<double>(p.alpha());
        if (!closeEnough(static_cast<double>(g.red()), static_cast<double>(straight[0]) * alpha) ||
            !closeEnough(static_cast<double>(g.green()),
                         static_cast<double>(straight[1]) * alpha) ||
            !closeEnough(static_cast<double>(g.blue()), static_cast<double>(straight[2]) * alpha) ||
            g.alpha() != p.alpha()) {
            ++mismatches;
        }
    }
    expectations.expect(mismatches == 0,
                        std::string(label) + " matches the CPU oracle within 2e-6");
    return mismatches;
}

[[nodiscard]] inline OCIO::ConstCPUProcessorRcPtr acesCstCpu() {
    auto config = OCIO::Config::CreateFromBuiltinConfig("cg-config-v1.0.0_aces-v1.3_ocio-v2.1");
    auto transform = OCIO::ColorSpaceTransform::Create();
    transform->setSrc("ACES2065-1");
    transform->setDst("ACEScg");
    auto processor =
        config->getProcessor(OCIO::Context::Create(), transform, OCIO::TRANSFORM_DIR_FORWARD);
    return processor->getDefaultCPUProcessor();
}

// Runs the real OCIO CST effect arm with a tiny injected maxComputeWorkGroupCount, so the bounded
// 2D dispatch and the wrapper flattening are exercised on any device (including one whose real X
// limit would hide the >4M-pixel boundary). Every pixel is compared to the CPU oracle, so a
// duplicate, a gap, or a wrong flattening fails.

// Test entry points defined across the resource, dispatch-plan, and reflection TUs.
void testAcesHdrDisplayLut(Expectations& expectations, bloom::render::GpuDevice& device,
                           const bloom::color::ResolvedBloomNeutralConfig& aces);
void testExposureContrastUniforms(Expectations& expectations, bloom::render::GpuDevice& device,
                                  const bloom::color::ResolvedBloomNeutralConfig& aces);
void testLut3d(Expectations& expectations, bloom::render::GpuDevice& device,
               const bloom::color::ResolvedBloomNeutralConfig& aces);
void testDispatchPlan(Expectations& expectations);
void testShaderInterfaceMalformed(Expectations& expectations);
void testForcedDispatchPlanning(Expectations& expectations, bloom::render::GpuDevice& device,
                                const bloom::color::ResolvedBloomNeutralConfig& aces);
void testDispatchRefusal(Expectations& expectations, bloom::render::GpuDevice& device,
                         const bloom::color::ResolvedBloomNeutralConfig& aces);
void testShaderInterfaceReflection(Expectations& expectations, bloom::render::GpuDevice& device,
                                   const bloom::color::ResolvedBloomNeutralConfig& aces);

} // namespace bloom::color::ocio_resources_test

#endif // BLOOM_COLOR_TESTS_OCIO_GPU_PROGRAM_RESOURCES_TEST_SUPPORT_HPP
