#include "ocio_gpu_program_native_helpers.hpp"

#include <bloom/color/ocio_gpu_program.hpp>
#include <bloom/core/color.hpp>
#include <bloom/render/gpu_image.hpp>
#include <bloom/render/gpu_image_upload.hpp>
#include <bloom/render/image.hpp>
#include <bloom/render/image_types.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <random>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace bloom::color::ocio_gpu_native_test {
namespace {

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

} // namespace

bool parseRequireDevice(const int argc, char** argv) {
    for (int index = 1; index < argc; ++index) {
        if (std::string_view(argv[index]) == "--require-device") {
            return true;
        }
    }
    return false;
}

std::vector<bloom::render::Rgba32f> fixturePixels(const std::uint32_t width,
                                                  const std::uint32_t height) {
    using bloom::render::Rgba32f;
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

std::shared_ptr<const bloom::render::GpuImage>
uploadImage(bloom::render::GpuImageUpload& uploader, const std::uint32_t width,
            const std::uint32_t height, const std::vector<bloom::render::Rgba32f>& pixels) {
    using bloom::render::GpuImage;
    using bloom::render::Rgba32fImage;
    using bloom::render::Rgba32fImageBuilder;
    using bloom::render::Rgba32fImageDescriptor;
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
    auto poll = bloom::render::GpuImageUploadPollResult::Pending;
    while (poll == bloom::render::GpuImageUploadPollResult::Pending) {
        poll = uploader.poll();
    }
    if (poll != bloom::render::GpuImageUploadPollResult::Ready) {
        return nullptr;
    }
    return std::make_shared<GpuImage>(uploader.takeImage());
}

std::optional<std::vector<std::uint32_t>> compileGlsl(const std::string& glsl) {
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

std::string buildWrapperGlsl(const bloom::render::OcioGpuProgramDesc& desc, const bool display) {
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
    // Shared per-sampler precise-sampling adapter: OCIO's generated body samples LUT resources with
    // the built-in texture(), whose hardware linear filtering is not full precision. The same
    // versioned helper the production wrapper must use emits one function per reflected sampler and
    // rewrites the body's texture() calls to it, keeping the generated transform body verbatim.
    const auto sampling = bloom::color::ocioGpuSamplingGlslFor(desc);
    out << sampling.preamble;
    out << desc.shaderText << "\n";
    out << sampling.definitions;
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

std::optional<std::shared_ptr<bloom::render::GpuOcioProgram>>
makeProgram(bloom::render::GpuDevice& device, bloom::render::OcioGpuProgramDesc desc) {
    using bloom::render::GpuOcioProgram;
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

bloom::render::GpuOcioProgramPollResult
awaitOcioCompletion(bloom::render::GpuOcioProgram& program, Expectations& expectations) {
    // Finite ceiling: ~20 s of 1 ms ticks. A job that has not retired by then is a failure, never a
    // hang. This is a bounded wait, not a naked busy loop.
    constexpr int kMaxAttempts = 20000;
    for (int attempt = 0; attempt < kMaxAttempts; ++attempt) {
        const auto result = program.poll();
        if (result != bloom::render::GpuOcioProgramPollResult::Pending) {
            return result;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    expectations.expect(false, "the native OCIO job retires within the bounded poll window");
    return bloom::render::GpuOcioProgramPollResult::Pending;
}

std::uint8_t quantize(const double value) {
    return static_cast<std::uint8_t>(std::floor(std::clamp(value, 0.0, 1.0) * 255.0 + 0.5));
}

void writeCurve1dCube(std::ofstream& file) {
    file << "LUT_1D_SIZE 8\n";
    for (int index = 0; index < 8; ++index) {
        const float value = static_cast<float>(index) / 7.0F;
        file << value << ' ' << value * value << ' ' << 0.5F * value << '\n';
    }
}

void writeVolume3dCube(std::ofstream& file) {
    file << "LUT_3D_SIZE 3\n";
    for (int b = 0; b < 3; ++b) {
        for (int g = 0; g < 3; ++g) {
            for (int r = 0; r < 3; ++r) {
                const float red = static_cast<float>(r) / 2.0F;
                const float green = static_cast<float>(g) / 2.0F;
                const float blue = static_cast<float>(b) / 2.0F;
                file << red << ' ' << green * green << ' ' << blue << '\n';
            }
        }
    }
}

void writeSteep1dCube(std::ofstream& file) {
    file << "LUT_1D_SIZE 4\n";
    file << "0 0 0\n";
    file << "0.001 0 0\n";
    file << "1 1 0.5\n";
    file << "1 1 1\n";
}

void writeIdentityClf(std::ofstream& file) {
    file << "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
            "<ProcessList id=\"identity\" compCLFversion=\"3\">\n"
            "<Matrix inBitDepth=\"32f\" outBitDepth=\"32f\"><Array dim=\"3 3\">1 0 0 0 1 0 0 0 "
            "1</Array></Matrix>\n"
            "</ProcessList>\n";
}

void writeCurve1dSpi(std::ofstream& file) {
    file << "Version 1\nFrom 0 1\nLength 4\nComponents 3\n{\n"
            "0 0 0\n0.3333 0.1111 0.1667\n0.6667 0.4444 0.3333\n1 1 0.5\n}\n";
}

void writeVolume3dSpi(std::ofstream& file) {
    file << "SPILUT 1.0\n3 3\n3 3 3\n";
    for (int r = 0; r < 3; ++r) {
        for (int g = 0; g < 3; ++g) {
            for (int b = 0; b < 3; ++b) {
                const float red = static_cast<float>(r) / 2.0F;
                const float green = static_cast<float>(g) / 2.0F;
                const float blue = static_cast<float>(b) / 2.0F;
                file << r << ' ' << g << ' ' << b << ' ' << red << ' ' << green * green << ' '
                     << blue << '\n';
            }
        }
    }
}

} // namespace bloom::color::ocio_gpu_native_test
