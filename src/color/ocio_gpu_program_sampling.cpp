#include <bloom/color/ocio_gpu_program.hpp>

#include "ocio_internal.hpp"

#include <algorithm>
#include <cmath>
#include <string>
#include <string_view>
#include <unordered_set>

namespace bloom::color {
namespace {

[[nodiscard]] bool isAsciiAlpha(const char character) noexcept {
    return (character >= 'a' && character <= 'z') || (character >= 'A' && character <= 'Z') ||
           character == '_';
}

[[nodiscard]] bool validIdentifier(const std::string_view name) noexcept {
    if (name.empty() || !isAsciiAlpha(name.front())) {
        return false;
    }
    return std::all_of(name.begin() + 1, name.end(), [](const char character) {
        return isAsciiAlpha(character) || (character >= '0' && character <= '9');
    });
}

[[nodiscard]] const char*
coordinateType(const render::OcioGpuTextureDimensions dimensions) noexcept {
    switch (dimensions) {
    case render::OcioGpuTextureDimensions::OneD:
        return "float";
    case render::OcioGpuTextureDimensions::TwoD:
        return "vec2";
    case render::OcioGpuTextureDimensions::ThreeD:
        return "vec3";
    }
    return nullptr;
}

[[nodiscard]] std::string linearSampling(const std::string& name,
                                         const render::OcioGpuTextureDimensions dimensions) {
    switch (dimensions) {
    case render::OcioGpuTextureDimensions::OneD:
        return "    const int size = textureSize(" + name +
               ", 0);\n    const float position = coordinate * float(size) - 0.5;\n"
               "    const float base = floor(position);\n    const float fraction = position - "
               "base;\n    const int lower = clamp(int(base), 0, size - 1);\n"
               "    const int upper = clamp(int(base) + 1, 0, size - 1);\n"
               "    return mix(texelFetch(" +
               name + ", lower, 0), texelFetch(" + name + ", upper, 0), fraction);\n";
    case render::OcioGpuTextureDimensions::TwoD:
        return "    const ivec2 size = textureSize(" + name +
               ", 0);\n    const vec2 position = coordinate * vec2(size) - vec2(0.5);\n"
               "    const vec2 base = floor(position);\n    const vec2 fraction = position - "
               "base;\n    const ivec2 lower = clamp(ivec2(base), ivec2(0), size - ivec2(1));\n"
               "    const ivec2 upper = clamp(ivec2(base) + ivec2(1), ivec2(0), size - ivec2(1));\n"
               "    const vec4 v00 = texelFetch(" +
               name + ", ivec2(lower.x, lower.y), 0);\n    const vec4 v10 = texelFetch(" + name +
               ", ivec2(upper.x, lower.y), 0);\n    const vec4 v01 = texelFetch(" + name +
               ", ivec2(lower.x, upper.y), 0);\n    const vec4 v11 = texelFetch(" + name +
               ", ivec2(upper.x, upper.y), 0);\n    return mix(mix(v00, v10, fraction.x), "
               "mix(v01, v11, fraction.x), fraction.y);\n";
    case render::OcioGpuTextureDimensions::ThreeD:
        return "    const ivec3 size = textureSize(" + name +
               ", 0);\n    const vec3 position = coordinate * vec3(size) - vec3(0.5);\n"
               "    const vec3 base = floor(position);\n    const vec3 fraction = position - "
               "base;\n    const ivec3 lower = clamp(ivec3(base), ivec3(0), size - ivec3(1));\n"
               "    const ivec3 upper = clamp(ivec3(base) + ivec3(1), ivec3(0), size - ivec3(1));\n"
               "    const vec4 v000 = texelFetch(" +
               name + ", ivec3(lower.x, lower.y, lower.z), 0);\n    const vec4 v100 = texelFetch(" +
               name + ", ivec3(upper.x, lower.y, lower.z), 0);\n    const vec4 v010 = texelFetch(" +
               name + ", ivec3(lower.x, upper.y, lower.z), 0);\n    const vec4 v110 = texelFetch(" +
               name + ", ivec3(upper.x, upper.y, lower.z), 0);\n    const vec4 v001 = texelFetch(" +
               name + ", ivec3(lower.x, lower.y, upper.z), 0);\n    const vec4 v101 = texelFetch(" +
               name + ", ivec3(upper.x, lower.y, upper.z), 0);\n    const vec4 v011 = texelFetch(" +
               name + ", ivec3(lower.x, upper.y, upper.z), 0);\n    const vec4 v111 = texelFetch(" +
               name +
               ", ivec3(upper.x, upper.y, upper.z), 0);\n    const vec4 v00 = mix(v000, "
               "v100, fraction.x);\n    const vec4 v01 = mix(v010, v110, fraction.x);\n"
               "    const vec4 v10 = mix(v001, v101, fraction.x);\n    const vec4 v11 = mix(v011, "
               "v111, fraction.x);\n    return mix(mix(v00, v01, fraction.y), mix(v10, v11, "
               "fraction.y), fraction.z);\n";
    }
    return {};
}

[[nodiscard]] std::string nearestSampling(const std::string& name,
                                          const render::OcioGpuTextureDimensions dimensions) {
    switch (dimensions) {
    case render::OcioGpuTextureDimensions::OneD:
        return "    const int size = textureSize(" + name + ", 0);\n    return texelFetch(" + name +
               ", clamp(int(floor(coordinate * float(size))), 0, size - 1), 0);\n";
    case render::OcioGpuTextureDimensions::TwoD:
        return "    const ivec2 size = textureSize(" + name + ", 0);\n    return texelFetch(" +
               name +
               ", clamp(ivec2(floor(coordinate * vec2(size))), ivec2(0), size - ivec2(1)), 0);\n";
    case render::OcioGpuTextureDimensions::ThreeD:
        return "    const ivec3 size = textureSize(" + name + ", 0);\n    return texelFetch(" +
               name +
               ", clamp(ivec3(floor(coordinate * vec3(size))), ivec3(0), size - ivec3(1)), 0);\n";
    }
    return {};
}

// Whether the linked OpenColorIO actually takes the fast path is qualified at runtime from its own
// default CPU processor, never guessed from the host architecture: a raw v2 config builds an
// ExponentWithLinear (sRGB) transform and its default CPU processor is applied to 1.75. The oracle
// is OCIO's own fast polynomial, whose error against the exact value at that point (~8e-6) is far
// above binary32 rounding, so a simple exact-value comparison distinguishes the two paths. That
// qualification is correct for any pinned OCIO build (SSE2, SSE2NEON, or an exact std::pow build),
// so macOS ARM cannot silently take the wrong branch.
[[nodiscard]] bool linkedOcioCpuUsesFastPower() noexcept {
    static const bool fast = []() noexcept -> bool {
        try {
            const auto config = OCIO::Config::CreateRaw();
            auto transform = OCIO::ExponentWithLinearTransform::Create();
            const double gamma[4] = {2.4, 2.4, 2.4, 1.0};
            const double offset[4] = {0.055, 0.055, 0.055, 0.0};
            transform->setGamma(gamma);
            transform->setOffset(offset);
            transform->setDirection(OCIO::TRANSFORM_DIR_INVERSE);
            const auto processor = config->getProcessor(transform);
            const auto cpu = processor->getDefaultCPUProcessor();
            if (!cpu) {
                return false;
            }
            float pixel[4] = {1.75F, 1.75F, 1.75F, 1.0F};
            cpu->applyRGB(pixel);
            const double exact = 1.055 * std::pow(1.75, 1.0 / 2.4) - 0.055;
            return std::fabs(static_cast<double>(pixel[0]) - exact) > 2.0e-6;
        } catch (const std::exception&) {
            // An unqualifiable build never receives the adapter: the caller keeps the generated
            // hardware pow rather than a guessed substitution.
            return false;
        }
    }();
    return fast;
}

// OCIO's GammaOp is the op this adapter specializes: its CPU renderer takes the fast-power path and
// its generated GLSL uses a vec4 `gamma` exponent. The pinned OCIO emits the exact comment
// `// Add Gamma '...' processing` once per gamma op and declares `vec4 gamma = vec4(...)`.
// ExponentOp (v1 configs only) uses `// Add an Exponent processing` and is never covered.
[[nodiscard]] bool programContainsGammaOp(const std::string_view shaderText) noexcept {
    return shaderText.find("// Add Gamma '") != std::string_view::npos &&
           shaderText.find("vec4 gamma = vec4(") != std::string_view::npos;
}

// The macro redirects the generated body's `pow` token; the overloads then specialize only the
// GammaOp call, which is always `pow(vec4, vec4)` because OCIO's pixel is a vec4. Every other
// `pow` signature (a LogOp's vec3 exponent, an ACEScc scalar exponent, a fixed-function scalar or
// vec3 exponent) forwards to the generated hardware pow unchanged, so a program that mixes a
// GammaOp with another power renderer has its GammaOp corrected and its other power calls left
// exactly as OCIO generated them. The generated OCIO body stays byte-for-byte intact; only the
// preprocessor token is redirected.
constexpr std::string_view kFastPowPreamble = R"(
float bloom_ocio_cpu_fast_pow(float x, float e);
vec2 bloom_ocio_cpu_fast_pow(vec2 x, vec2 e);
vec3 bloom_ocio_cpu_fast_pow(vec3 x, vec3 e);
vec4 bloom_ocio_cpu_fast_pow(vec4 x, vec4 e);
#define pow(x, y) bloom_ocio_cpu_fast_pow(x, y)
)";

// Exact transcription of OpenColorIO 2.5.2 `SSE.h` sseLog2/sseExp2/ssePower. The Chebyshev
// coefficients, their evaluation order, the bit-level mantissa/exponent extraction and the
// underflow/overflow handling are copied verbatim; changing any of them breaks oracle parity. The
// non-vec4 overloads delegate to the hardware pow that OCIO's generated body already used.
constexpr std::string_view kFastPowDefinitions = R"(
#undef pow
float bloom_ocio_cpu_fast_pow(float x, float e) { return pow(x, e); }
vec2 bloom_ocio_cpu_fast_pow(vec2 x, vec2 e) { return pow(x, e); }
vec3 bloom_ocio_cpu_fast_pow(vec3 x, vec3 e) { return pow(x, e); }
precise float bloom_ocio_cpu_log2(float x)
{
    int bits = floatBitsToInt(x);
    float mantissa = intBitsToFloat((bits & 0x807FFFFF) | 0x3F800000);
    float value = 4.487361286440374006195e-2;
    value = value * mantissa - 4.165637071209677112635e-1;
    value = value * mantissa + 1.631148826119436277100;
    value = value * mantissa - 3.550793018041176193407;
    value = value * mantissa + 5.091710879305474367557;
    value = value * mantissa - 2.800364054395965731506;
    int exponent = ((bits & 0x7F800000) >> 23) - 127;
    return value + float(exponent);
}
precise float bloom_ocio_cpu_exp2(float x)
{
    float floor_x = floor(x);
    float zf = intBitsToFloat((int(floor_x) + 127) << 23);
    float fraction = x - floor_x;
    float value = 1.353416792833547468620e-2;
    value = value * fraction + 5.201146058412685018921e-2;
    value = value * fraction + 2.414427569091865207710e-1;
    value = value * fraction + 6.930038344665415134202e-1;
    value = value * fraction + 1.000002593370603213644;
    float result = zf * value;
    if (x < -126.0) { result = 0.0; }
    if (x >= 128.0) { result = uintBitsToFloat(0x7F800000u); }
    return result;
}
precise float bloom_ocio_cpu_sse_pow(float x, float e)
{
    float value = bloom_ocio_cpu_log2(x);
    value = e * value;
    value = bloom_ocio_cpu_exp2(value);
    if (!(x > 0.0)) { value = 0.0; }
    return value;
}
vec4 bloom_ocio_cpu_fast_pow(vec4 x, vec4 e)
{
    return vec4(bloom_ocio_cpu_sse_pow(x.x, e.x), bloom_ocio_cpu_sse_pow(x.y, e.y),
                bloom_ocio_cpu_sse_pow(x.z, e.z), bloom_ocio_cpu_sse_pow(x.w, e.w));
}
)";

} // namespace

OcioGpuSamplingGlsl ocioGpuSamplingGlslFor(const render::OcioGpuProgramDesc& program) {
    OcioGpuSamplingGlsl result;
    std::string declarations;
    std::string definitions;

    // Per-sampler precise-sampling adapter (unchanged).
    bool samplingDispatches = false;
    if (!program.textures.empty()) {
        std::unordered_set<std::string> samplerNames;
        bool valid = true;
        for (const auto& texture : program.textures) {
            if (!validIdentifier(texture.samplerName) ||
                coordinateType(texture.dimensions) == nullptr ||
                !samplerNames.insert(texture.samplerName).second) {
                // An unnameable/duplicated sampler cannot receive a safe per-sampler override;
                // leave the whole program on the generated hardware sampling rather than emitting
                // a partial or ambiguous adapter.
                valid = false;
                break;
            }
        }
        if (valid) {
            samplingDispatches = true;
            definitions = "#undef texture\n";
            for (const auto& texture : program.textures) {
                const std::string function = "bloom_ocio_sample_" + texture.samplerName;
                const std::string coordinate = coordinateType(texture.dimensions);
                declarations += "vec4 " + function + "(" + coordinate + " coordinate);\n";
                definitions += "vec4 " + function + "(" + coordinate + " coordinate)\n{\n";
                switch (texture.interpolation) {
                case render::OcioGpuInterpolation::Linear:
                    definitions += linearSampling(texture.samplerName, texture.dimensions);
                    break;
                case render::OcioGpuInterpolation::Nearest:
                case render::OcioGpuInterpolation::Tetrahedral:
                    // OCIO samples its tetrahedral body only at exact texel centers, so a nearest
                    // fetch is exact. Linear-interpolated resources instead get the full-precision
                    // linear sampler.
                    definitions += nearestSampling(texture.samplerName, texture.dimensions);
                    break;
                default:
                    definitions += "    return texture(" + texture.samplerName + ", coordinate);\n";
                    break;
                }
                definitions += "}\n\n";
            }
        }
    }
    if (samplingDispatches) {
        declarations += "#define texture(s, c) bloom_ocio_sample_##s(c)\n";
    }

    // CPU fast-power oracle parity adapter (GammaOp programs).
    const bool fastPower =
        linkedOcioCpuUsesFastPower() && programContainsGammaOp(program.shaderText);
    if (fastPower) {
        declarations += std::string(kFastPowPreamble);
        definitions += std::string(kFastPowDefinitions);
    }

    if (declarations.empty()) {
        return result;
    }
    result.preamble = std::move(declarations);
    result.definitions = std::move(definitions);
    return result;
}

} // namespace bloom::color
