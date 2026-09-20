// Native proof for bloom::render::GpuOcioProgram: real OCIO GPU programs executed on the real
// Vulkan device, compared against the actual CPU colour processors. The complete GLSL (Bloom
// wrapper + extracted OCIO shader text) is compiled once, off the device owner thread, with the
// pinned glslangValidator/spirv-val from the dependency prefix; no runtime compiler exists here.
//
// A missing device is an explicit SKIP (exit 77) unless --require-device is passed.

#include <bloom/color/ocio_cpu_color_space_processor.hpp>
#include <bloom/color/ocio_cpu_display_processor.hpp>
#include <bloom/color/ocio_gpu_program.hpp>
#include <bloom/core/color.hpp>
#include <bloom/render/gpu_device.hpp>
#include <bloom/render/gpu_image.hpp>
#include <bloom/render/gpu_image_upload.hpp>
#include <bloom/render/gpu_ocio_program.hpp>
#include <bloom/render/image.hpp>
#include <bloom/render/image_types.hpp>

#include <algorithm>
#include <array>
#include <atomic>
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
#include <vector>

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
using bloom::render::GpuOcioProgramDiagnosticCode;
using bloom::render::GpuOcioProgramPollResult;
using bloom::render::OcioGpuProgramDesc;
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
    if (uploader.begin({source}, kBudget).code !=
        bloom::render::GpuImageUploadDiagnosticCode::None) {
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
    out << "  uint index = gl_GlobalInvocationID.x;\n";
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
        std::cerr << "glslangValidator rejected the complete OCIO wrapper program\n";
        return std::nullopt;
    }
    const std::string validate =
        "\"" + spirvVal.string() + "\" --target-env vulkan1.2 \"" + spvPath + "\"";
    if (std::system(validate.c_str()) != 0) {
        std::cerr << "spirv-val rejected the compiled OCIO wrapper program\n";
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

[[nodiscard]] std::optional<std::shared_ptr<GpuOcioProgram>> makeProgram(GpuDevice& device,
                                                                         OcioGpuProgramDesc desc) {
    const bool display = desc.stage == bloom::render::OcioGpuProgramStage::DisplayPacking;
    auto spirv = compileGlsl(buildWrapperGlsl(desc, display));
    if (!spirv.has_value()) {
        return std::nullopt;
    }
    auto created = GpuOcioProgram::create(device, std::move(desc), *spirv, {});
    if (!created) {
        std::cerr << "GpuOcioProgram::create failed: " << created.diagnostic.message << '\n';
        return std::nullopt;
    }
    return std::shared_ptr<GpuOcioProgram>(std::move(created.program));
}

[[nodiscard]] std::uint8_t quantize(const double value) {
    return static_cast<std::uint8_t>(std::floor(std::clamp(value, 0.0, 1.0) * 255.0 + 0.5));
}

// Premultiplied fixture: HDR, a negative channel, translucent alpha, and an exact zero-alpha pixel.
[[nodiscard]] std::vector<Rgba32f> fixturePixels(const std::uint32_t width,
                                                 const std::uint32_t height) {
    std::vector<Rgba32f> pixels(static_cast<std::size_t>(width) * height, Rgba32f::transparent());
    for (std::uint32_t y = 0; y < height; ++y) {
        for (std::uint32_t x = 0; x < width; ++x) {
            const float fx = static_cast<float>(x) / static_cast<float>(width);
            const float fy = static_cast<float>(y) / static_cast<float>(height);
            const float alpha = ((x + y) % 3 == 0) ? 0.0F : 0.25F + 0.5F * fx;
            const float r = (1.6F + fx) * alpha;
            const float g = (-0.2F + fy) * alpha;
            const float b = (0.125F + 2.0F * fx) * alpha;
            const auto value = Rgba32f::fromPremultiplied(r, g, b, alpha);
            if (value) {
                pixels[static_cast<std::size_t>(y) * width + x] = *value.value();
            }
        }
    }
    return pixels;
}

void testEffectCst(Expectations& expectations, GpuDevice& device,
                   const bloom::color::ResolvedBloomNeutralConfig& resolved) {
    auto desc = bloom::color::buildOcioGpuProgramForCst(resolved, "ACES2065-1", "ACEScg");
    expectations.expect(desc.succeeded(), "the ACES CST extracts");
    if (!desc.succeeded()) {
        return;
    }
    auto program = makeProgram(device, *desc.program());
    auto uploader = GpuImageUpload::create(device);
    expectations.expect(program != nullptr && uploader.hasValue(), "the CST program hosts");
    if (program == nullptr || !uploader) {
        return;
    }
    constexpr std::uint32_t width = 4;
    constexpr std::uint32_t height = 3;
    const auto pixels = fixturePixels(width, height);
    const auto input = uploadImage(*uploader.upload, width, height, pixels);
    expectations.expect(input != nullptr, "the CST input uploads");
    if (input == nullptr) {
        return;
    }
    const auto accepted = (*program)->beginEffect(input, {}, kBudget);
    expectations.expect(accepted.code == GpuOcioProgramDiagnosticCode::None,
                        "beginEffect accepts the request");
    auto poll = (*program)->poll();
    while (poll == GpuOcioProgramPollResult::Pending) {
        poll = (*program)->poll();
    }
    expectations.expect(poll == GpuOcioProgramPollResult::Ready, "the CST dispatch completes");
    auto output = (*program)->takeEffectOutput();
    expectations.expect(output != nullptr, "the CST resident output is published");
    if (output == nullptr) {
        return;
    }
    const auto readback = bloom::render::readbackResidentImage(*output, kBudget);
    if (!readback.hasValue()) {
        std::cerr << "CST readback failed: " << readback.message << "\n";
    }
    expectations.expect(readback.hasValue(), "the CST output reads back for the oracle");
    if (!readback) {
        return;
    }
    // CPU oracle: the real ACES CPU processor, applied to straight RGB with the image-effect
    // un-premultiply/re-premultiply flow.
    const auto cpu =
        bloom::color::CpuColorSpaceProcessor::prepare(resolved, "ACES2065-1", "ACEScg");
    expectations.expect(cpu.succeeded(), "the CPU ACES processor prepares");
    if (!cpu) {
        return;
    }
    std::size_t mismatches = 0;
    for (std::size_t index = 0; index < pixels.size(); ++index) {
        const auto& p = pixels[index];
        std::array<float, 4> straight{p.red(), p.green(), p.blue(), 1.0F};
        if (p.alpha() != 0.0F) {
            straight[0] = p.red() / p.alpha();
            straight[1] = p.green() / p.alpha();
            straight[2] = p.blue() / p.alpha();
        } else {
            straight = {0.0F, 0.0F, 0.0F, 1.0F};
        }
        std::span<std::array<float, 4>> span(&straight, 1);
        if (!cpu.processor()->apply(span)) {
            ++mismatches;
            continue;
        }
        const float expectedR = straight[0] * p.alpha();
        const float expectedG = straight[1] * p.alpha();
        const float expectedB = straight[2] * p.alpha();
        const auto& gpu = readback.pixels[index];
        const auto close = [](const float a, const float b) {
            if (a == b) {
                return true;
            }
            const double absolute = std::fabs(static_cast<double>(a) - static_cast<double>(b));
            const double magnitude =
                std::max(std::fabs(static_cast<double>(a)), std::fabs(static_cast<double>(b)));
            return absolute <= 2e-6 || absolute <= 2e-6 * magnitude;
        };
        if (!close(gpu.red(), expectedR) || !close(gpu.green(), expectedG) ||
            !close(gpu.blue(), expectedB) || gpu.alpha() != p.alpha()) {
            ++mismatches;
        }
    }
    expectations.expect(mismatches == 0, "every ACES CST pixel is within 2e-6 of the CPU oracle");

    // Cold/warm: a second job on the same compiled program succeeds with no recompilation.
    const auto again = (*program)->beginEffect(input, {}, kBudget);
    expectations.expect(again.code == GpuOcioProgramDiagnosticCode::None,
                        "the warm CST job is accepted");
    auto warmPoll = (*program)->poll();
    while (warmPoll == GpuOcioProgramPollResult::Pending) {
        warmPoll = (*program)->poll();
    }
    expectations.expect(warmPoll == GpuOcioProgramPollResult::Ready &&
                            (*program)->takeEffectOutput() != nullptr,
                        "the warm CST job completes on the retained program");
}

void testDisplay(Expectations& expectations, GpuDevice& device,
                 const bloom::color::ResolvedBloomNeutralConfig& resolved) {
    auto desc = bloom::color::buildOcioGpuProgramForDisplay(resolved, resolved.displayName(),
                                                            resolved.viewName());
    expectations.expect(desc.succeeded(), "the Bloom Neutral display program extracts");
    if (!desc.succeeded()) {
        return;
    }
    auto program = makeProgram(device, *desc.program());
    auto uploader = GpuImageUpload::create(device);
    expectations.expect(program != nullptr && uploader.hasValue(), "the display program hosts");
    if (program == nullptr || !uploader) {
        return;
    }
    constexpr std::uint32_t width = 5;
    constexpr std::uint32_t height = 2;
    const auto pixels = fixturePixels(width, height);
    const auto input = uploadImage(*uploader.upload, width, height, pixels);
    expectations.expect(input != nullptr, "the display input uploads");
    if (input == nullptr) {
        return;
    }
    const auto accepted = (*program)->beginDisplay(input, {}, kBudget);
    expectations.expect(accepted.code == GpuOcioProgramDiagnosticCode::None,
                        "beginDisplay accepts the request");
    auto poll = (*program)->poll();
    while (poll == GpuOcioProgramPollResult::Pending) {
        poll = (*program)->poll();
    }
    expectations.expect(poll == GpuOcioProgramPollResult::Ready, "the display dispatch completes");
    GpuDisplayImage output = (*program)->takeDisplayOutput();
    expectations.expect(output.isValid(), "the resident RGBA8 display output is published");
    if (!output.isValid()) {
        return;
    }
    const auto readback = bloom::render::readbackResidentDisplayImage(output, kBudget);
    if (!readback.hasValue()) {
        std::cerr << "display readback failed: " << readback.message << "\n";
    }
    expectations.expect(readback.hasValue(), "the display output reads back for the oracle");
    if (!readback) {
        return;
    }
    const auto handle = bloom::color::buildBloomNeutralCpuDisplayProcessor(resolved);
    const auto* const processor = handle.handle();
    expectations.expect(processor != nullptr, "the CPU display processor prepares");
    if (processor == nullptr) {
        return;
    }
    std::size_t rgbMismatches = 0;
    std::size_t alphaMismatches = 0;
    for (std::size_t index = 0; index < pixels.size(); ++index) {
        const auto& p = pixels[index];
        std::array<float, 3> straight{0.0F, 0.0F, 0.0F};
        if (p.alpha() != 0.0F) {
            straight = {p.red() / p.alpha(), p.green() / p.alpha(), p.blue() / p.alpha()};
        }
        const auto display = processor->referenceToDisplayLinear(
            bloom::core::Color4d{static_cast<double>(straight[0]), static_cast<double>(straight[1]),
                                 static_cast<double>(straight[2]), 1.0});
        if (!display.has_value()) {
            ++rgbMismatches;
            continue;
        }
        const auto expectedR = quantize(display->red);
        const auto expectedG = quantize(display->green);
        const auto expectedB = quantize(display->blue);
        const auto expectedA = quantize(static_cast<double>(p.alpha()));
        const auto& gpu = readback.pixels[index];
        if (std::abs(static_cast<int>(gpu.red) - expectedR) > 1 ||
            std::abs(static_cast<int>(gpu.green) - expectedG) > 1 ||
            std::abs(static_cast<int>(gpu.blue) - expectedB) > 1) {
            ++rgbMismatches;
        }
        if (gpu.alpha != expectedA) {
            ++alphaMismatches;
        }
    }
    expectations.expect(rgbMismatches == 0,
                        "every display pixel is within one RGBA8 code of the CPU oracle");
    expectations.expect(alphaMismatches == 0, "display alpha is byte-exact against the CPU oracle");
}

} // namespace

int main(int argc, char** argv) {
    const bool requireDevice = parseRequireDevice(argc, argv);
    Expectations expectations;

    auto neutralResolution = bloom::color::resolveBloomNeutralV1BuiltIn(
        bloom::color::OcioConfigLocatorKind::BloomBuiltIn, bloom::color::kBloomNeutralV1ConfigUri,
        bloom::color::kBloomNeutralV1ConfigDigest);
    auto neutral = std::move(neutralResolution).takeResolved();
    if (!neutral.has_value()) {
        std::cerr << "FAILED: the Bloom Neutral built-in does not resolve\n";
        return 1;
    }
    const auto acesRevision = bloom::color::ocioBuiltInContentRevision(
        bloom::color::OcioConfigLocatorKind::BloomBuiltIn, bloom::color::kAcesCgV1ConfigUri);
    if (!acesRevision.has_value()) {
        std::cerr << "SKIP: the ACES built-in is unavailable\n";
        return kSkipExit;
    }
    auto acesResolution =
        bloom::color::resolveOcioBuiltIn(bloom::color::OcioConfigLocatorKind::BloomBuiltIn,
                                         bloom::color::kAcesCgV1ConfigUri, *acesRevision, "ACEScg");
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
    testEffectCst(expectations, *device.device, *aces);
    testDisplay(expectations, *device.device, *neutral);

    if (expectations.failures() != 0) {
        std::cerr << expectations.failures() << " OCIO GPU program native expectation(s) failed\n";
        return 1;
    }
    std::cout << "PASS: native OCIO GPU program parity\n";
    return 0;
}
