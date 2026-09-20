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
#include "vulkan/gpu_ocio_program_dispatch_plan.hpp"

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

[[nodiscard]] std::shared_ptr<GpuOcioProgram>
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

[[nodiscard]] std::shared_ptr<GpuOcioProgram> makeProgram(GpuDevice& device,
                                                          OcioGpuProgramDesc desc) {
    return makeProgram(device, std::move(desc), {});
}

// Bounded, non-busy completion wait. Never loops without a finite attempt ceiling.
[[nodiscard]] bool pollOcio(Expectations& expectations, GpuOcioProgram& program,
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

[[nodiscard]] std::uint8_t quantize(const double value) {
    return static_cast<std::uint8_t>(std::floor(std::clamp(value, 0.0, 1.0) * 255.0 + 0.5));
}

[[nodiscard]] bool closeEnough(const double a, const double b) {
    if (a == b) {
        return true;
    }
    const double absolute = std::fabs(a - b);
    const double magnitude = std::max(std::fabs(a), std::fabs(b));
    return absolute <= 2e-6 || absolute <= 2e-6 * magnitude;
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

[[nodiscard]] OCIO::ConstCPUProcessorRcPtr exposureContrastCpu(const double exposure,
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

void testAcesHdrDisplayLut(Expectations& expectations, GpuDevice& device,
                           const bloom::color::ResolvedBloomNeutralConfig& aces) {
    const std::string_view display = "Rec.2100-PQ - Display";
    const std::string_view view = "ACES 1.1 - HDR Video (1000 nits & Rec.2020 lim)";
    auto desc = bloom::color::buildOcioGpuProgramForDisplay(aces, display, view);
    expectations.expect(desc.succeeded(), "the ACES HDR display LUT program extracts");
    if (!desc.succeeded()) {
        return;
    }
    bool hasLut = false;
    for (const auto& texture : desc.program()->textures) {
        hasLut = hasLut || texture.dimensions != OcioGpuTextureDimensions::ThreeD;
    }
    expectations.expect(hasLut, "the extracted display program carries a sampled LUT texture");
    auto program = makeProgram(device, *desc.program());
    auto uploader = GpuImageUpload::create(device);
    expectations.expect(program != nullptr && uploader.hasValue(), "the LUT display program hosts");
    if (program == nullptr || !uploader) {
        return;
    }
    const std::uint64_t retained = program->retainedAllocationBytes();
    const std::uint64_t declaredLutBytes =
        desc.program()->textures.front().samples.size() * sizeof(float);
    expectations.expect(retained >= declaredLutBytes,
                        "retainedAllocationBytes charges the actual persistent LUT allocation");
    constexpr std::uint32_t width = 4;
    constexpr std::uint32_t height = 3;
    const auto pixels = fixturePixels(width, height);
    const auto input = uploadImage(*uploader.upload, width, height, pixels);
    if (input == nullptr) {
        expectations.expect(false, "the LUT display input uploads");
        return;
    }
    const auto accepted = program->beginDisplay(input, {}, kBudget);
    expectations.expect(accepted.code == bloom::render::GpuOcioProgramDiagnosticCode::None,
                        "the LUT display begin is accepted");
    auto poll = program->poll();
    while (poll == bloom::render::GpuOcioProgramPollResult::Pending) {
        poll = program->poll();
    }
    expectations.expect(poll == bloom::render::GpuOcioProgramPollResult::Ready,
                        "the LUT display completes");
    GpuDisplayImage output = program->takeDisplayOutput();
    if (!output.isValid()) {
        expectations.expect(false, "the LUT display output is published");
        return;
    }
    const auto readback = bloom::render::readbackResidentDisplayImage(output, kBudget);
    if (!readback) {
        expectations.expect(false, "the LUT display output reads back");
        return;
    }
    const auto handle = bloom::color::buildCpuDisplayProcessorForView(aces, display, view);
    const auto* const processor = handle.handle();
    expectations.expect(processor != nullptr, "the ACES HDR CPU display processor prepares");
    if (processor == nullptr) {
        return;
    }
    std::size_t rgbMismatches = 0;
    std::size_t alphaMismatches = 0;
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
            ++rgbMismatches;
            continue;
        }
        const auto& gpu = readback.pixels[index];
        if (std::abs(static_cast<int>(gpu.red) - quantize(displayValue->red)) > 1 ||
            std::abs(static_cast<int>(gpu.green) - quantize(displayValue->green)) > 1 ||
            std::abs(static_cast<int>(gpu.blue) - quantize(displayValue->blue)) > 1) {
            ++rgbMismatches;
        }
        if (gpu.alpha != quantize(static_cast<double>(p.alpha()))) {
            ++alphaMismatches;
        }
    }
    expectations.expect(rgbMismatches == 0, "the LUT display is within one code of the CPU oracle");
    expectations.expect(alphaMismatches == 0, "the LUT display alpha is byte-exact");

    // Warm reuse: the retained texture/UBO/pipeline is reused with no re-upload.
    const auto warm = program->beginDisplay(input, {}, kBudget);
    expectations.expect(warm.code == bloom::render::GpuOcioProgramDiagnosticCode::None,
                        "the warm LUT display begin is accepted");
    poll = program->poll();
    while (poll == bloom::render::GpuOcioProgramPollResult::Pending) {
        poll = program->poll();
    }
    expectations.expect(poll == bloom::render::GpuOcioProgramPollResult::Ready &&
                            program->takeDisplayOutput().isValid(),
                        "the warm LUT display reuses the retained resources");
    expectations.expect(program->retainedAllocationBytes() == retained,
                        "retainedAllocationBytes is stable across warm jobs");
}

void testExposureContrastUniforms(Expectations& expectations, GpuDevice& device,
                                  const bloom::color::ResolvedBloomNeutralConfig& aces) {
    auto desc = bloom::color::buildOcioGpuProgramForExposureContrast(aces, "ACEScg", 0.5, 1.0);
    expectations.expect(desc.succeeded(), "the exposure/contrast program extracts");
    if (!desc.succeeded()) {
        expectations.expect(false, "the exposure/contrast program carries a non-zero UBO");
        return;
    }
    expectations.expect(desc.program()->uniformBufferSize > 0 &&
                            !desc.program()->uniformBufferData.empty() &&
                            desc.program()->uniforms.size() == 2,
                        "real OCIO uniform declarations and values are captured");
    auto program = makeProgram(device, *desc.program());
    auto uploader = GpuImageUpload::create(device);
    if (program == nullptr || !uploader) {
        expectations.expect(false, "the exposure/contrast program hosts");
        return;
    }
    constexpr std::uint32_t width = 4;
    constexpr std::uint32_t height = 2;
    const auto pixels = fixturePixels(width, height);
    const auto input = uploadImage(*uploader.upload, width, height, pixels);
    if (input == nullptr) {
        expectations.expect(false, "the exposure/contrast input uploads");
        return;
    }
    const auto run = [&](std::span<const std::byte> uniformBytes) -> std::vector<Rgba32f> {
        const auto accepted = program->beginEffect(input, uniformBytes, kBudget);
        expectations.expect(accepted.code == bloom::render::GpuOcioProgramDiagnosticCode::None,
                            "the exposure/contrast begin is accepted");
        auto poll = program->poll();
        while (poll == bloom::render::GpuOcioProgramPollResult::Pending) {
            poll = program->poll();
        }
        expectations.expect(poll == bloom::render::GpuOcioProgramPollResult::Ready,
                            "the exposure/contrast dispatch completes");
        auto output = program->takeEffectOutput();
        if (output == nullptr) {
            return {};
        }
        const auto readback = bloom::render::readbackResidentImage(*output, kBudget);
        return readback ? readback.pixels : std::vector<Rgba32f>{};
    };

    const auto snapshot = run({});
    const auto cpuHalf = exposureContrastCpu(0.5, 1.0);
    expectations.expect(cpuHalf != nullptr, "the exposure/contrast CPU oracle prepares");
    if (cpuHalf != nullptr) {
        compareEffect(
            expectations, pixels, snapshot,
            [&](std::array<float, 3>& rgb) { static_cast<void>(cpuHalf->applyRGB(rgb.data())); },
            "exposure 0.5 default snapshot");
    }

    std::array<std::byte, 8> overridden{};
    const float negativeExposure = -1.0F;
    const float contrast = 1.0F;
    std::memcpy(overridden.data(), &negativeExposure, sizeof(float));
    std::memcpy(overridden.data() + 4, &contrast, sizeof(float));
    const auto changed = run(overridden);
    const auto cpuNegative = exposureContrastCpu(-1.0, 1.0);
    if (cpuNegative != nullptr) {
        compareEffect(
            expectations, pixels, changed,
            [&](std::array<float, 3>& rgb) {
                static_cast<void>(cpuNegative->applyRGB(rgb.data()));
            },
            "exposure -1.0 uniform override");
    }
    bool differs = false;
    for (std::size_t index = 0; index < snapshot.size() && index < changed.size(); ++index) {
        differs = differs || snapshot[index].red() != changed[index].red() ||
                  snapshot[index].green() != changed[index].green() ||
                  snapshot[index].blue() != changed[index].blue();
    }
    expectations.expect(differs, "a changed uniform value changes the output without recompiling");
}

void testLut3d(Expectations& expectations, GpuDevice& device,
               const bloom::color::ResolvedBloomNeutralConfig& aces) {
    constexpr std::uint32_t edge = 3;
    std::vector<float> samples(edge * edge * edge * 3U);
    for (std::uint32_t r = 0; r < edge; ++r) {
        for (std::uint32_t g = 0; g < edge; ++g) {
            for (std::uint32_t b = 0; b < edge; ++b) {
                const std::size_t index =
                    ((static_cast<std::size_t>(r) * edge + g) * edge + b) * 3U;
                samples[index] = static_cast<float>(r) / (edge - 1);
                samples[index + 1] = static_cast<float>(g) / (edge - 1);
                samples[index + 2] = static_cast<float>(b) / (edge - 1);
            }
        }
    }
    auto desc = bloom::color::buildOcioGpuProgramForLut3d(
        aces, "ACEScg", edge, OcioGpuInterpolation::Tetrahedral, samples);
    expectations.expect(desc.succeeded(), "the in-memory 3D LUT program extracts");
    if (!desc.succeeded()) {
        return;
    }
    bool has3d = false;
    for (const auto& texture : desc.program()->textures) {
        has3d = has3d || (texture.dimensions == OcioGpuTextureDimensions::ThreeD &&
                          texture.edgeLength == edge &&
                          texture.interpolation != OcioGpuInterpolation::Unknown);
    }
    expectations.expect(has3d, "the extracted program carries the real 3D LUT and interpolation");
    auto program = makeProgram(device, *desc.program());
    auto uploader = GpuImageUpload::create(device);
    if (program == nullptr || !uploader) {
        expectations.expect(false, "the 3D LUT program hosts");
        return;
    }
    constexpr std::uint32_t width = 3;
    constexpr std::uint32_t height = 2;
    const auto pixels = fixturePixels(width, height);
    const auto input = uploadImage(*uploader.upload, width, height, pixels);
    if (input == nullptr) {
        expectations.expect(false, "the 3D LUT input uploads");
        return;
    }
    const auto accepted = program->beginEffect(input, {}, kBudget);
    expectations.expect(accepted.code == bloom::render::GpuOcioProgramDiagnosticCode::None,
                        "the 3D LUT begin is accepted");
    auto poll = program->poll();
    while (poll == bloom::render::GpuOcioProgramPollResult::Pending) {
        poll = program->poll();
    }
    expectations.expect(poll == bloom::render::GpuOcioProgramPollResult::Ready,
                        "the 3D LUT dispatch completes");
    auto output = program->takeEffectOutput();
    if (output == nullptr) {
        expectations.expect(false, "the 3D LUT output is published");
        return;
    }
    const auto readback = bloom::render::readbackResidentImage(*output, kBudget);
    if (!readback) {
        expectations.expect(false, "the 3D LUT output reads back");
        return;
    }
    auto config = OCIO::Config::CreateFromBuiltinConfig("cg-config-v1.0.0_aces-v1.3_ocio-v2.1");
    auto transform = OCIO::Lut3DTransform::Create();
    transform->setGridSize(edge);
    transform->setInterpolation(OCIO::INTERP_TETRAHEDRAL);
    for (std::uint32_t r = 0; r < edge; ++r) {
        for (std::uint32_t g = 0; g < edge; ++g) {
            for (std::uint32_t b = 0; b < edge; ++b) {
                const std::size_t index =
                    ((static_cast<std::size_t>(r) * edge + g) * edge + b) * 3U;
                transform->setValue(r, g, b, samples[index], samples[index + 1],
                                    samples[index + 2]);
            }
        }
    }
    auto cpu = config->getProcessor(OCIO::Context::Create(), transform, OCIO::TRANSFORM_DIR_FORWARD)
                   ->getDefaultCPUProcessor();
    compareEffect(
        expectations, pixels, readback.pixels,
        [&](std::array<float, 3>& rgb) { static_cast<void>(cpu->applyRGB(rgb.data())); }, "3D LUT");
}

// Pure capacity-planning test with injected device limits. No device is required, so it runs even
// when the native lane skips.
void testDispatchPlan(Expectations& expectations) {
    using bloom::render::ocio_program_detail::effectiveWorkGroupCount;
    using bloom::render::ocio_program_detail::OcioDispatchPlan;
    using bloom::render::ocio_program_detail::OcioDispatchPlanError;
    using bloom::render::ocio_program_detail::planOcioDispatch;
    constexpr std::uint32_t kWorkgroup = 64;

    const auto covers = [](const OcioDispatchPlan& plan, const std::uint64_t pixels,
                           const std::uint32_t maxX, const std::uint32_t maxY) {
        if (!plan.valid() || plan.groupsX == 0 || plan.groupsY == 0) {
            return false;
        }
        if (plan.groupsX > maxX || plan.groupsY > maxY) {
            return false;
        }
        const std::uint64_t stride =
            static_cast<std::uint64_t>(plan.groupsX) * kWorkgroup;
        if (stride > 0xFFFFFFFFULL) {
            return false;
        }
        const std::uint64_t total = stride * plan.groupsY;
        return total >= pixels && total <= 0x100000000ULL;
    };

    const auto single = planOcioDispatch(1, kWorkgroup, 65535, 65535);
    expectations.expect(single.valid() && single.groupsX == 1 && single.groupsY == 1,
                        "one pixel plans to a 1x1 dispatch");
    const auto tail = planOcioDispatch(65, kWorkgroup, 65535, 65535);
    expectations.expect(tail.valid() && tail.groupsX == 2 && tail.groupsY == 1,
                        "a 65-pixel tail plans to 2x1");
    // The conformant maxComputeWorkGroupCount[0] == 65535 boundary: 65536 groups must go 2D.
    const std::uint64_t boundary = static_cast<std::uint64_t>(65535) * kWorkgroup + 1ULL;
    const auto boundaryPlan = planOcioDispatch(boundary, kWorkgroup, 65535, 65535);
    expectations.expect(boundaryPlan.valid() && boundaryPlan.groupsX == 65535 &&
                            boundaryPlan.groupsY == 2 && covers(boundaryPlan, boundary, 65535, 65535),
                        "the >4M-pixel X boundary plans a bounded 2D grid");
    // Injected tiny limits force many rows.
    const auto tiny = planOcioDispatch(256, kWorkgroup, 3, 64);
    expectations.expect(tiny.valid() && tiny.groupsX == 3 && tiny.groupsY == 2 &&
                            covers(tiny, 256, 3, 64),
                        "tiny injected limits plan 3x2 for 256 pixels");
    const auto tooFewRows = planOcioDispatch(256, kWorkgroup, 3, 1);
    expectations.expect(!tooFewRows.valid() &&
                            tooFewRows.error == OcioDispatchPlanError::DeviceCapacity,
                        "a geometry the injected limits cannot cover is refused");
    expectations.expect(!planOcioDispatch(0, kWorkgroup, 65535, 65535).valid(),
                        "zero pixels are refused");
    expectations.expect(!planOcioDispatch(1, 0, 65535, 65535).valid(),
                        "a zero workgroup size is refused");
    expectations.expect(!planOcioDispatch(1, kWorkgroup, 0, 65535).valid(),
                        "a zero max-X is refused");
    // Enormous checked math: X is capped so the uint32 stride cannot wrap, then Y exceeds maxY.
    const auto hugeY = planOcioDispatch(0xFFFFFFFFULL, kWorkgroup, 1, 65535);
    expectations.expect(!hugeY.valid() && hugeY.error == OcioDispatchPlanError::DeviceCapacity,
                        "an enormous geometry is refused rather than wrapping");
    const auto hugeIndex = planOcioDispatch(0xFFFFFFFFULL, kWorkgroup, 0xFFFFFFFFu, 0xFFFFFFFFu);
    expectations.expect(!hugeIndex.valid() &&
                            hugeIndex.error == OcioDispatchPlanError::DeviceCapacity,
                        "an unrepresentable flattened index is refused");

    // The effective limit clamps a caller cap to the physical device limit: a nonzero request may
    // only lower it, and zero selects the physical limit. An oversized request can never authorize
    // a dispatch above maxComputeWorkGroupCount.
    constexpr std::uint32_t kPhysical = 65535;
    expectations.expect(effectiveWorkGroupCount(0, kPhysical) == kPhysical,
                        "zero selects the physical limit");
    expectations.expect(effectiveWorkGroupCount(13, kPhysical) == 13,
                        "a smaller requested cap lowers the limit");
    expectations.expect(effectiveWorkGroupCount(kPhysical, kPhysical) == kPhysical,
                        "a request equal to the physical limit is unchanged");
    expectations.expect(effectiveWorkGroupCount(0xFFFFFFFFu, kPhysical) == kPhysical,
                        "an oversized requested cap is clamped to the physical limit");
    const auto clampedPlan =
        planOcioDispatch(boundary, kWorkgroup, effectiveWorkGroupCount(0xFFFFFFFFu, kPhysical),
                         effectiveWorkGroupCount(0xFFFFFFFFu, kPhysical));
    expectations.expect(clampedPlan.valid() && clampedPlan.groupsX == kPhysical &&
                            clampedPlan.groupsY == 2 &&
                            covers(clampedPlan, boundary, kPhysical, kPhysical),
                        "an oversized requested cap still plans the physical 2D boundary");
}

// Builds the ACES CST effect oracle (ACES2065-1 -> ACEScg) as the CPU reference for the effect arm.
[[nodiscard]] OCIO::ConstCPUProcessorRcPtr acesCstCpu() {
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
void testForcedDispatchPlanning(Expectations& expectations, GpuDevice& device,
                                const bloom::color::ResolvedBloomNeutralConfig& aces) {
    auto desc = bloom::color::buildOcioGpuProgramForCst(aces, "ACES2065-1", "ACEScg");
    expectations.expect(desc.succeeded(), "the forced-planning CST extracts");
    if (!desc.succeeded()) {
        return;
    }
    bloom::render::GpuOcioProgramBudgets budgets;
    budgets.maxWorkGroupCountX = 13;
    budgets.maxWorkGroupCountY = 512;
    auto program = makeProgram(device, *desc.program(), budgets);
    auto uploader = GpuImageUpload::create(device);
    expectations.expect(program != nullptr && uploader.hasValue(),
                        "the forced-planning program hosts");
    if (program == nullptr || !uploader) {
        return;
    }
    const auto cpu = acesCstCpu();
    expectations.expect(cpu != nullptr, "the forced-planning CPU oracle prepares");
    if (cpu == nullptr) {
        return;
    }
    const auto run = [&](const std::uint32_t width, const std::uint32_t height,
                         const std::string_view label) {
        const auto pixels = fixturePixels(width, height);
        const auto input = uploadImage(*uploader.upload, width, height, pixels);
        expectations.expect(input != nullptr, "a forced-planning input uploads");
        if (input == nullptr) {
            return;
        }
        const auto accepted = program->beginEffect(input, {}, kBudget);
        expectations.expect(accepted.code == bloom::render::GpuOcioProgramDiagnosticCode::None,
                            "a forced-planning begin is accepted");
        if (accepted.code != bloom::render::GpuOcioProgramDiagnosticCode::None) {
            return;
        }
        if (!pollOcio(expectations, *program, label)) {
            return;
        }
        auto output = program->takeEffectOutput();
        expectations.expect(output != nullptr, "a forced-planning output is published");
        if (output == nullptr) {
            return;
        }
        const auto readback = bloom::render::readbackResidentImage(*output, kBudget);
        expectations.expect(readback.hasValue(), "a forced-planning output reads back");
        if (!readback) {
            return;
        }
        compareEffect(
            expectations, pixels, readback.pixels,
            [&](std::array<float, 3>& rgb) { static_cast<void>(cpu->applyRGB(rgb.data())); }, label);
    };
    run(5, 7, "small forced-planning geometry");
    run(401, 4, "tail forced-planning geometry");
    run(4096, 32, ">4K forced-planning geometry");
}

// A geometry the injected limits cannot cover must be refused before any submission.
void testDispatchRefusal(Expectations& expectations, GpuDevice& device,
                         const bloom::color::ResolvedBloomNeutralConfig& aces) {
    auto desc = bloom::color::buildOcioGpuProgramForCst(aces, "ACES2065-1", "ACEScg");
    expectations.expect(desc.succeeded(), "the refusal CTS extracts");
    if (!desc.succeeded()) {
        return;
    }
    bloom::render::GpuOcioProgramBudgets budgets;
    budgets.maxWorkGroupCountX = 1;
    budgets.maxWorkGroupCountY = 1;
    auto program = makeProgram(device, *desc.program(), budgets);
    auto uploader = GpuImageUpload::create(device);
    expectations.expect(program != nullptr && uploader.hasValue(), "the refusal program hosts");
    if (program == nullptr || !uploader) {
        return;
    }
    constexpr std::uint32_t width = 65;
    constexpr std::uint32_t height = 1;
    const auto pixels = fixturePixels(width, height);
    const auto input = uploadImage(*uploader.upload, width, height, pixels);
    expectations.expect(input != nullptr, "the refusal input uploads");
    if (input == nullptr) {
        return;
    }
    const auto refused = program->beginEffect(input, {}, kBudget);
    expectations.expect(refused.code == bloom::render::GpuOcioProgramDiagnosticCode::Unsupported,
                        "an unplannable geometry is refused typed before submit");
    expectations.expect(!program->hasUnretiredSubmission(),
                        "a refused geometry leaves no unretired submission");
    expectations.expect(program->lastJobAllocationBytes() == 0,
                        "a refused geometry publishes no job bytes");
    // A geometry that fits the injected limits still completes with parity.
    constexpr std::uint32_t okWidth = 32;
    constexpr std::uint32_t okHeight = 2;
    const auto okPixels = fixturePixels(okWidth, okHeight);
    const auto okInput = uploadImage(*uploader.upload, okWidth, okHeight, okPixels);
    expectations.expect(okInput != nullptr, "the recovery input uploads");
    if (okInput == nullptr) {
        return;
    }
    const auto recovered = program->beginEffect(okInput, {}, kBudget);
    expectations.expect(recovered.code == bloom::render::GpuOcioProgramDiagnosticCode::None,
                        "a fitting geometry recovers after the refusal");
    if (recovered.code != bloom::render::GpuOcioProgramDiagnosticCode::None) {
        return;
    }
    if (!pollOcio(expectations, *program, "refusal recovery")) {
        return;
    }
    auto output = program->takeEffectOutput();
    expectations.expect(output != nullptr, "the recovered output is published");
    if (output == nullptr) {
        return;
    }
    const auto readback = bloom::render::readbackResidentImage(*output, kBudget);
    expectations.expect(readback.hasValue(), "the recovered output reads back");
    if (!readback) {
        return;
    }
    const auto cpu = acesCstCpu();
    if (cpu != nullptr) {
        compareEffect(
            expectations, okPixels, readback.pixels,
            [&](std::array<float, 3>& rgb) { static_cast<void>(cpu->applyRGB(rgb.data())); },
            "refusal recovery");
    }
}

} // namespace

int main(int argc, char** argv) {
    const bool requireDevice = parseRequireDevice(argc, argv);
    Expectations expectations;
    testDispatchPlan(expectations);
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
    if (!aces.has_value()) {
        std::cerr << "FAILED: the ACES built-in does not resolve\n";
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
    testAcesHdrDisplayLut(expectations, *device.device, *aces);
    testExposureContrastUniforms(expectations, *device.device, *aces);
    testLut3d(expectations, *device.device, *aces);
    testForcedDispatchPlanning(expectations, *device.device, *aces);
    testDispatchRefusal(expectations, *device.device, *aces);

    if (expectations.failures() != 0) {
        std::cerr << expectations.failures() << " resource native expectation(s) failed\n";
        return 1;
    }
    std::cout << "PASS: native OCIO descriptor resources\n";
    return 0;
}
