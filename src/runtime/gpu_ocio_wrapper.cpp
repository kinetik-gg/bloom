#include <bloom/runtime/gpu_ocio_wrapper.hpp>

#include <bloom/color/ocio_gpu_program.hpp>

#include <charconv>
#include <span>
#include <sstream>
#include <string>

namespace bloom::runtime {
namespace {

[[nodiscard]] bool validIdentifier(const std::string_view name) noexcept {
    if (name.empty()) {
        return false;
    }
    const auto first = static_cast<unsigned char>(name.front());
    if (!((first >= 'A' && first <= 'Z') || (first >= 'a' && first <= 'z') || first == '_')) {
        return false;
    }
    for (const char character : name) {
        const auto value = static_cast<unsigned char>(character);
        if (!((value >= 'A' && value <= 'Z') || (value >= 'a' && value <= 'z') ||
              (value >= '0' && value <= '9') || value == '_')) {
            return false;
        }
    }
    return true;
}

// The exact bit-exact straight RGBA8 quantizer proved by the accepted OCIO native tests. It
// reproduces floor(clamp(v, 0, 1) * 255 + 0.5) for the whole binary32 domain, including tiny
// normals and subnormals, without depending on an implicit float->uint conversion.
constexpr std::string_view kQuantizerGlsl = R"(
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

[[nodiscard]] GpuOcioWrapperResult failure(const GpuOcioWrapperError error) noexcept {
    GpuOcioWrapperResult result;
    result.error = error;
    return result;
}

} // namespace

std::string_view gpuOcioWrapperErrorName(const GpuOcioWrapperError error) noexcept {
    switch (error) {
    case GpuOcioWrapperError::None:
        return "none";
    case GpuOcioWrapperError::InvalidProgram:
        return "invalid-program";
    case GpuOcioWrapperError::UnsupportedStage:
        return "unsupported-stage";
    case GpuOcioWrapperError::InvalidFunctionName:
        return "invalid-function-name";
    case GpuOcioWrapperError::UnsupportedDescriptorSet:
        return "unsupported-descriptor-set";
    case GpuOcioWrapperError::AllocationFailure:
        return "allocation-failure";
    case GpuOcioWrapperError::InvalidViewAdjust:
        return "invalid-view-adjust";
    case GpuOcioWrapperError::UnsupportedViewAdjust:
        return "unsupported-view-adjust";
    }
    return "unknown";
}

// Exact GLSL replica of runtime::ViewAdjust::fromEncoded (the qualified-path post-display
// exposure/gamma from view_adjust.cpp): exposure acts on the decoded linear display light, gamma
// acts on the re-encoded value, and the result is clamped before the byte quantizer. The CPU
// reference is unchanged; the shader evaluates it in binary32 against the OCIO display output.
[[nodiscard]] std::string viewAdjustGlsl(const ViewAdjust& adjust) {
    const auto literal = [](const double value) {
        char buffer[40];
        const auto result =
            std::to_chars(buffer, buffer + sizeof(buffer), value, std::chars_format::general);
        return std::string(buffer, static_cast<std::size_t>(result.ptr - buffer));
    };
    std::string glsl =
        "\nfloat bloom_ocio_view_adjust(float value)\n{\n"
        "  const float exposure = " +
        literal(adjust.exposure) +
        ";\n"
        "  const float gamma = " +
        literal(adjust.gamma) +
        ";\n"
        "  if (exposure == 0.0) {\n"
        "    float clamped = clamp(value, 0.0, 1.0);\n"
        "    return (gamma == 1.0) ? clamped : pow(clamped, 1.0 / gamma);\n"
        "  }\n"
        "  float linear = (value <= 0.04045) ? value / 12.92 : pow((value + 0.055) / 1.055, "
        "2.4);\n"
        "  float lit = linear * exp2(exposure);\n"
        "  float encoded = (lit <= 0.0031308) ? 12.92 * lit : 1.055 * pow(lit, 1.0 / 2.4) - "
        "0.055;\n"
        "  float clamped = clamp(encoded, 0.0, 1.0);\n"
        "  return (gamma == 1.0) ? clamped : pow(clamped, 1.0 / gamma);\n"
        "}\n";
    return glsl;
}

GpuOcioWrapperResult buildGpuOcioWrapperGlsl(const render::OcioGpuProgramDesc& program,
                                             const ViewAdjust viewAdjust) noexcept {
    try {
        if (program.shaderText.empty()) {
            return failure(GpuOcioWrapperError::InvalidProgram);
        }
        if (!validIdentifier(program.functionName)) {
            return failure(GpuOcioWrapperError::InvalidFunctionName);
        }
        if (program.descriptorSetIndex != 0) {
            return failure(GpuOcioWrapperError::UnsupportedDescriptorSet);
        }
        if (!viewAdjust.valid()) {
            return failure(GpuOcioWrapperError::InvalidViewAdjust);
        }
        const bool display = program.stage == render::OcioGpuProgramStage::DisplayPacking;
        const bool effect = program.stage == render::OcioGpuProgramStage::ProcessEffect;
        if (!display && !effect) {
            return failure(GpuOcioWrapperError::UnsupportedStage);
        }
        if (effect && !viewAdjust.neutral()) {
            return failure(GpuOcioWrapperError::UnsupportedViewAdjust);
        }
        const bool adjusted = display && !viewAdjust.neutral();

        // Shared versioned per-sampler precise-sampling adapter. OCIO's generated body samples LUT
        // resources with the built-in texture(), whose hardware filtering is not full precision;
        // the adapter emits one texelFetch-based function per reflected sampler and rewrites the
        // body's texture() calls to it while leaving the generated transform body byte-for-byte
        // intact.
        const auto sampling = color::ocioGpuSamplingGlslFor(program);

        std::ostringstream out;
        out << "#version 460\n";
        out << "layout(local_size_x = " << kGpuOcioWrapperWorkgroupSize << ") in;\n";
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
        out << sampling.preamble;
        out << program.shaderText;
        if (program.shaderText.back() != '\n') {
            out << '\n';
        }
        out << sampling.definitions;
        if (display) {
            out << kQuantizerGlsl;
        }
        if (adjusted) {
            out << viewAdjustGlsl(viewAdjust);
        }
        out << "void main() {\n";
        out << "  uint index = gl_GlobalInvocationID.x;\n";
        out << "  if (index >= bloom_ocio_push.pixelCount) { return; }\n";
        out << "  ivec2 c = ivec2(int(index % bloom_ocio_push.width), int(index / "
               "bloom_ocio_push.width));\n";
        out << "  vec4 p = imageLoad(bloom_ocio_input, c);\n";
        out << "  float a = p.a;\n";
        if (effect) {
            // Exact CPU image-effect semantics: an alpha-zero source pixel is copied through
            // unchanged, including any hidden RGB, and is never un-premultiplied or transformed.
            out << "  if (a == 0.0) { imageStore(bloom_ocio_output, c, p); return; }\n";
            out << "  vec3 s = p.rgb / a;\n";
        } else {
            out << "  vec3 s = (a != 0.0) ? p.rgb / a : vec3(0.0);\n";
        }
        out << "  if (any(isnan(s)) || any(isinf(s))) { bloom_ocio_status.flags[0] = 1u; return; "
               "}\n";
        out << "  vec4 t = " << program.functionName << "(vec4(s, 1.0));\n";
        out << "  if (any(isnan(t.rgb)) || any(isinf(t.rgb))) { bloom_ocio_status.flags[0] = 1u; "
               "return; }\n";
        if (display) {
            if (adjusted) {
                out << "  vec3 adjusted = vec3(bloom_ocio_view_adjust(t.r), "
                       "bloom_ocio_view_adjust(t.g), bloom_ocio_view_adjust(t.b));\n";
                out << "  uint r = bloom_ocio_quantize(adjusted.r);\n";
                out << "  uint g = bloom_ocio_quantize(adjusted.g);\n";
                out << "  uint b = bloom_ocio_quantize(adjusted.b);\n";
            } else {
                out << "  uint r = bloom_ocio_quantize(t.r);\n";
                out << "  uint g = bloom_ocio_quantize(t.g);\n";
                out << "  uint b = bloom_ocio_quantize(t.b);\n";
            }
            out << "  uint qa = bloom_ocio_quantize(a);\n";
            out << "  bloom_ocio_output.words[index] = r | (g << 8) | (b << 16) | (qa << 24);\n";
        } else {
            out << "  imageStore(bloom_ocio_output, c, vec4(t.rgb * a, a));\n";
        }
        out << "}\n";

        GpuOcioWrapperResult result;
        result.source = out.str();
        result.entryPoint = std::string(kGpuOcioWrapperEntryPoint);
        result.samplingVersion = std::string(color::kOcioGpuPreciseSamplingVersion);
        const auto digest = core::Sha256Hasher::hash(
            std::as_bytes(std::span<const char>(result.source.data(), result.source.size())));
        if (!digest.has_value()) {
            return failure(GpuOcioWrapperError::AllocationFailure);
        }
        result.sourceDigest = *digest;
        result.error = GpuOcioWrapperError::None;
        return result;
    } catch (...) {
        // This function is noexcept by contract; an allocation failure (or any other exception from
        // the sampling adapter / string building) is reported as a typed error, never a terminate.
        return failure(GpuOcioWrapperError::AllocationFailure);
    }
}

} // namespace bloom::runtime
