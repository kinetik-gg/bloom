#include <bloom/runtime/gpu_ocio_wrapper.hpp>

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
    }
    return "unknown";
}

GpuOcioWrapperResult buildGpuOcioWrapperGlsl(const render::OcioGpuProgramDesc& program) noexcept {
    if (program.shaderText.empty()) {
        return failure(GpuOcioWrapperError::InvalidProgram);
    }
    if (!validIdentifier(program.functionName)) {
        return failure(GpuOcioWrapperError::InvalidFunctionName);
    }
    if (program.descriptorSetIndex != 0) {
        return failure(GpuOcioWrapperError::UnsupportedDescriptorSet);
    }
    const bool display = program.stage == render::OcioGpuProgramStage::DisplayPacking;
    const bool effect = program.stage == render::OcioGpuProgramStage::ProcessEffect;
    if (!display && !effect) {
        return failure(GpuOcioWrapperError::UnsupportedStage);
    }

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
    out << program.shaderText;
    if (program.shaderText.back() != '\n') {
        out << '\n';
    }
    if (display) {
        out << kQuantizerGlsl;
    }
    out << "void main() {\n";
    out << "  uint index = gl_GlobalInvocationID.x;\n";
    out << "  if (index >= bloom_ocio_push.pixelCount) { return; }\n";
    out << "  ivec2 c = ivec2(int(index % bloom_ocio_push.width), int(index / "
           "bloom_ocio_push.width));\n";
    out << "  vec4 p = imageLoad(bloom_ocio_input, c);\n";
    out << "  float a = p.a;\n";
    out << "  vec3 s = (a != 0.0) ? p.rgb / a : vec3(0.0);\n";
    out << "  if (any(isnan(s)) || any(isinf(s))) { bloom_ocio_status.flags[0] = 1u; return; }\n";
    out << "  vec4 t = " << program.functionName << "(vec4(s, 1.0));\n";
    out << "  if (any(isnan(t.rgb)) || any(isinf(t.rgb))) { bloom_ocio_status.flags[0] = 1u; "
           "return; }\n";
    if (display) {
        out << "  uint r = bloom_ocio_quantize(t.r);\n";
        out << "  uint g = bloom_ocio_quantize(t.g);\n";
        out << "  uint b = bloom_ocio_quantize(t.b);\n";
        out << "  uint qa = bloom_ocio_quantize(a);\n";
        out << "  bloom_ocio_output.words[index] = r | (g << 8) | (b << 16) | (qa << 24);\n";
    } else {
        out << "  imageStore(bloom_ocio_output, c, vec4(t.rgb * a, a));\n";
    }
    out << "}\n";

    GpuOcioWrapperResult result;
    result.source = out.str();
    result.entryPoint = std::string(kGpuOcioWrapperEntryPoint);
    const auto digest = core::Sha256Hasher::hash(
        std::as_bytes(std::span<const char>(result.source.data(), result.source.size())));
    if (!digest.has_value()) {
        return failure(GpuOcioWrapperError::InvalidProgram);
    }
    result.sourceDigest = *digest;
    result.error = GpuOcioWrapperError::None;
    return result;
}

} // namespace bloom::runtime
