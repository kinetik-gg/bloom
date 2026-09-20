#include <bloom/color/ocio_gpu_program.hpp>

#include "ocio_internal.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

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

// The pinned OCIO marks every generated op region with a deterministic comment. The adapter keys
// its per-op specialization on those comments, never on a broad `pow(` signature: a program that
// contains both a GammaOp and an ExponentOp (both use `pow(vec4, vec4)`) has only the GammaOp block
// rewritten, and a program with a fixed-function vec3 power keeps that hardware pow.
[[nodiscard]] bool programContainsGammaOp(const std::string_view shaderText) noexcept {
    return shaderText.find("// Add Gamma '") != std::string_view::npos &&
           shaderText.find("vec4 gamma = vec4(") != std::string_view::npos;
}
[[nodiscard]] bool programContainsLogOp(const std::string_view shaderText) noexcept {
    return shaderText.find("// Add Log '") != std::string_view::npos;
}
[[nodiscard]] bool programContainsMatrixOp(const std::string_view shaderText) noexcept {
    return shaderText.find("// Add Matrix processing") != std::string_view::npos;
}

[[nodiscard]] std::size_t countOccurrences(const std::string_view text,
                                           const std::string_view needle) noexcept {
    std::size_t count = 0;
    for (std::size_t position = text.find(needle); position != std::string_view::npos;
         position = text.find(needle, position + needle.size())) {
        ++count;
    }
    return count;
}

// A GammaOp-only program keeps the accepted `pow` macro path (backward compatible with the runtime
// wrapper as it stands). Every other program uses the per-op body specialization.
[[nodiscard]] bool programPowerIsGammaOnly(const std::string_view shaderText) noexcept {
    const std::size_t gammaOps = countOccurrences(shaderText, "// Add Gamma '");
    return gammaOps != 0 && gammaOps == countOccurrences(shaderText, "pow(");
}

enum class OpKind : std::uint8_t { None, Gamma, Log, Matrix };

struct OpBlock final {
    std::size_t begin = 0;
    std::size_t end = 0;
    OpKind kind = OpKind::None;
};

// Locate each OCIO op region: the comment that precedes it, and the matching closing brace of the
// `{ ... }` block that follows. OCIO's generated blocks are flat (no nested braces).
[[nodiscard]] std::vector<OpBlock> findOpBlocks(const std::string_view body) {
    std::vector<OpBlock> blocks;
    std::size_t at = 0;
    while (at < body.size()) {
        OpKind kind = OpKind::None;
        std::size_t comment = std::string_view::npos;
        const auto consider = [&](const std::string_view needle, const OpKind candidate) {
            const std::size_t found = body.find(needle, at);
            if (found != std::string_view::npos && found < comment) {
                comment = found;
                kind = candidate;
            }
        };
        consider("// Add Gamma '", OpKind::Gamma);
        consider("// Add Log '", OpKind::Log);
        consider("// Add Matrix processing", OpKind::Matrix);
        if (comment == std::string_view::npos) {
            break;
        }
        const std::size_t brace = body.find('{', comment);
        if (brace == std::string_view::npos) {
            break;
        }
        int depth = 0;
        std::size_t index = brace;
        for (; index < body.size(); ++index) {
            if (body[index] == '{') {
                ++depth;
            } else if (body[index] == '}') {
                --depth;
                if (depth == 0) {
                    ++index;
                    break;
                }
            }
        }
        blocks.push_back(OpBlock{comment, index, kind});
        at = index;
    }
    return blocks;
}

void replaceAll(std::string& text, const std::string_view from, const std::string_view to) {
    std::size_t at = text.find(from);
    while (at != std::string::npos) {
        text.replace(at, from.size(), to);
        at = text.find(from, at + to.size());
    }
}

struct BodySpecialization final {
    std::string body;
    bool gamma = false;
    bool log = false;
    bool matrix = false;
};

// Copy the generated body, rewriting only the identified op regions. Everything outside those
// regions, including the op comments themselves and any unrelated `pow` call, is copied verbatim.
//
// * GammaOp: `pow(x, gamma)` -> `bloom_ocio_cpu_gamma_pow(x, gamma)`.
// * LogOp: the GPU bakes `1/logSlope` and `pow(base, (in-logOffset)*slopeInv)`, while OCIO's SSE
//   CPU bakes `kinv = log2(base)/logSlope` and evaluates `sseExp2((in-logOffset)*kinv)`. The two
//   lines are folded so the multiply by `slopeInv` no longer rounds before the power; remaining
//   `pow(log_base, x)` forms fall back to `exp2(x*log2(base))`.
// * MatrixOp: `mat4(...) * tmp` -> `bloom_ocio_cpu_matrix(mat4(...), tmp)`, reproducing OCIO's SSE
//   `(col0*x + col1*y) + (col2*z + col3*w)` add order. Diagonal/offset forms are left untouched.
[[nodiscard]] BodySpecialization specializeOcioShaderBody(const std::string_view source) {
    BodySpecialization result;
    result.body.reserve(source.size() + 128);
    const std::vector<OpBlock> blocks = findOpBlocks(source);
    std::size_t cursor = 0;
    for (const auto& block : blocks) {
        if (block.begin < cursor) {
            continue;
        }
        result.body.append(source.substr(cursor, block.begin - cursor));
        std::string region(source.substr(block.begin, block.end - block.begin));
        switch (block.kind) {
        case OpKind::Gamma:
            replaceAll(region, "pow(", "bloom_ocio_cpu_gamma_pow(");
            result.gamma = true;
            break;
        case OpKind::Log:
            replaceAll(region,
                       "vec3 linSeg = ( outColor.rgb - linear_segment_offset ) * "
                       "linear_segment_slopeinv;",
                       "vec3 linSeg = bloom_ocio_cpu_mul3(bloom_ocio_cpu_sub3(outColor.rgb, "
                       "linear_segment_offset), linear_segment_slopeinv);");
            replaceAll(region, "vec3 logSeg = (outColor.rgb - log_offset) * log_slopeinv;",
                       "vec3 logSeg = bloom_ocio_cpu_sub3(outColor.rgb, log_offset);");
            // OCIO's CPU camera-log renderer uses `kinv = log2(base)/logSlope`. For base 2 that is
            // exactly the GPU's baked `1/logSlope`; the device `log2(2.0)` is not guaranteed to be
            // exactly 1.0, so the base-2 form must not multiply by it.
            if (region.find("log_base = vec3(2.") != std::string::npos) {
                replaceAll(region, "logSeg = pow(log_base, logSeg);",
                           "logSeg = bloom_ocio_cpu_log_exp2(logSeg, log_slopeinv);");
            } else {
                replaceAll(region, "logSeg = pow(log_base, logSeg);",
                           "logSeg = bloom_ocio_cpu_log_exp(log_base, logSeg, log_slopeinv);");
            }
            replaceAll(region, "logSeg = lin_slopeinv * (logSeg - lin_offset);",
                       "logSeg = bloom_ocio_cpu_mul3(lin_slopeinv, bloom_ocio_cpu_sub3(logSeg, "
                       "lin_offset));");
            replaceAll(region, "pow(log_base,", "bloom_ocio_cpu_log_pow(log_base,");
            result.log = true;
            break;
        case OpKind::Matrix:
            replaceAll(region, "mat4(", "bloom_ocio_cpu_matrix(mat4(");
            replaceAll(region, ") * tmp", "), tmp)");
            result.matrix = true;
            break;
        case OpKind::None:
            break;
        }
        result.body.append(region);
        cursor = block.end;
    }
    if (cursor < source.size()) {
        result.body.append(source.substr(cursor));
    }
    return result;
}

// Forward declarations emitted before the generated body when it is specialized.
constexpr std::string_view kCpuHelperDeclarations = R"(
vec4 bloom_ocio_cpu_gamma_pow(vec4 x, vec4 e);
vec3 bloom_ocio_cpu_log_pow(vec3 base, vec3 x);
vec3 bloom_ocio_cpu_log_exp(vec3 base, vec3 x, vec3 slopeinv);
vec3 bloom_ocio_cpu_log_exp2(vec3 x, vec3 slopeinv);
vec3 bloom_ocio_cpu_sub3(vec3 a, vec3 b);
vec3 bloom_ocio_cpu_mul3(vec3 a, vec3 b);
vec3 bloom_ocio_cpu_mul3f(vec3 a, float b);
vec4 bloom_ocio_cpu_sub4(vec4 a, vec4 b);
vec4 bloom_ocio_cpu_mul4(vec4 a, vec4 b);
vec4 bloom_ocio_cpu_mul4f(vec4 a, float b);
vec4 bloom_ocio_cpu_add4(vec4 a, vec4 b);
vec4 bloom_ocio_cpu_max4(vec4 a, vec4 b);
precise vec4 bloom_ocio_cpu_matrix(mat4 m, vec4 v);
)";

// Exact transcription of OpenColorIO 2.5.2 `SSE.h` sseLog2/sseExp2/ssePower plus the LogOp
// anti-log and MatrixOp add-order helpers. The Chebyshev coefficients, their evaluation order, the
// bit-level mantissa/exponent extraction and the underflow/overflow handling are copied verbatim;
// changing any of them breaks oracle parity.
constexpr std::string_view kCpuHelperDefinitions = R"(
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
precise vec3 bloom_ocio_cpu_exp2(vec3 x)
{
    return vec3(bloom_ocio_cpu_exp2(x.x), bloom_ocio_cpu_exp2(x.y), bloom_ocio_cpu_exp2(x.z));
}
precise vec3 bloom_ocio_cpu_sub3(vec3 a, vec3 b) { return a - b; }
precise vec3 bloom_ocio_cpu_mul3(vec3 a, vec3 b) { return a * b; }
precise vec3 bloom_ocio_cpu_mul3f(vec3 a, float b) { return a * b; }
precise vec4 bloom_ocio_cpu_sub4(vec4 a, vec4 b) { return a - b; }
precise vec4 bloom_ocio_cpu_mul4(vec4 a, vec4 b) { return a * b; }
precise vec4 bloom_ocio_cpu_mul4f(vec4 a, float b) { return a * b; }
precise vec4 bloom_ocio_cpu_add4(vec4 a, vec4 b) { return a + b; }
precise vec4 bloom_ocio_cpu_max4(vec4 a, vec4 b) { return max(a, b); }
precise float bloom_ocio_cpu_sse_pow(float x, float e)
{
    float value = bloom_ocio_cpu_log2(x);
    value = e * value;
    value = bloom_ocio_cpu_exp2(value);
    if (!(x > 0.0)) { value = 0.0; }
    return value;
}
precise vec4 bloom_ocio_cpu_gamma_pow(vec4 x, vec4 e)
{
    return vec4(bloom_ocio_cpu_sse_pow(x.x, e.x), bloom_ocio_cpu_sse_pow(x.y, e.y),
                bloom_ocio_cpu_sse_pow(x.z, e.z), bloom_ocio_cpu_sse_pow(x.w, e.w));
}
precise vec3 bloom_ocio_cpu_log_pow(vec3 base, vec3 x)
{
    return bloom_ocio_cpu_exp2(x * log2(base));
}
precise vec3 bloom_ocio_cpu_log_exp(vec3 base, vec3 x, vec3 slopeinv)
{
    return bloom_ocio_cpu_exp2(x * (log2(base) * slopeinv));
}
precise vec3 bloom_ocio_cpu_log_exp2(vec3 x, vec3 slopeinv)
{
    return bloom_ocio_cpu_exp2(x * slopeinv);
}
precise vec4 bloom_ocio_cpu_matrix(mat4 m, vec4 v)
{
    return (m[0] * v.x + m[1] * v.y) + (m[2] * v.z + m[3] * v.w);
}
)";

// Backward-compatible GammaOp-only `pow` macro. The non-vec4 overloads forward to the generated
// hardware pow, so only the GammaOp call is redirected.
constexpr std::string_view kFastPowPreamble = R"(
float bloom_ocio_cpu_fast_pow(float x, float e);
vec2 bloom_ocio_cpu_fast_pow(vec2 x, vec2 e);
vec3 bloom_ocio_cpu_fast_pow(vec3 x, vec3 e);
vec4 bloom_ocio_cpu_fast_pow(vec4 x, vec4 e);
#define pow(x, y) bloom_ocio_cpu_fast_pow(x, y)
)";

constexpr std::string_view kFastPowOverloads = R"(
#undef pow
float bloom_ocio_cpu_fast_pow(float x, float e) { return pow(x, e); }
vec2 bloom_ocio_cpu_fast_pow(vec2 x, vec2 e) { return pow(x, e); }
vec3 bloom_ocio_cpu_fast_pow(vec3 x, vec3 e) { return pow(x, e); }
vec4 bloom_ocio_cpu_fast_pow(vec4 x, vec4 e) { return bloom_ocio_cpu_gamma_pow(x, e); }
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
                declarations.append("vec4 ")
                    .append(function)
                    .append("(")
                    .append(coordinate)
                    .append(" coordinate);\n");
                definitions.append("vec4 ")
                    .append(function)
                    .append("(")
                    .append(coordinate)
                    .append(" coordinate)\n{\n");
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

    // CPU-math parity adapter. A GammaOp-only program keeps the accepted `pow` macro path; any
    // program with a GammaOp, LogOp, or MatrixOp region gets a per-op body specialization instead.
    const bool cpuParity = linkedOcioCpuUsesFastPower();
    const bool gammaOnly = programPowerIsGammaOnly(program.shaderText);
    const bool hasGamma = programContainsGammaOp(program.shaderText);
    const bool hasLog = programContainsLogOp(program.shaderText);
    const bool hasMatrix = programContainsMatrixOp(program.shaderText);

    if (cpuParity && gammaOnly && !hasLog && !hasMatrix) {
        declarations += std::string(kFastPowPreamble);
        definitions += std::string(kCpuHelperDefinitions);
        definitions += std::string(kFastPowOverloads);
    } else if (cpuParity && (hasGamma || hasLog || hasMatrix)) {
        BodySpecialization specialized = specializeOcioShaderBody(program.shaderText);
        declarations += std::string(kCpuHelperDeclarations);
        definitions += std::string(kCpuHelperDefinitions);
        result.shaderBody = std::move(specialized.body);
    }

    if (declarations.empty()) {
        return result;
    }
    result.preamble = std::move(declarations);
    result.definitions = std::move(definitions);
    return result;
}

std::string_view ocioGpuPreciseDivisionGlsl() noexcept {
    // Correctly-rounded binary32 division. `a / b` gives a quotient within one ULP on a
    // non-conforming device; `fma(-b, q, a)` is then the exact residual, and folding `r / b` back
    // yields the nearest-even result the unchanged CPU oracle produces with scalar division. The
    // residual is exact because FMA computes a - b*q with a single rounding.
    return R"(
precise float bloom_ocio_cpu_next_up(float x)
{
    const uint u = floatBitsToUint(x);
    if (x == 0.0) { return uintBitsToFloat(1u); }
    return (x > 0.0) ? uintBitsToFloat(u + 1u) : uintBitsToFloat(u - 1u);
}
precise float bloom_ocio_cpu_next_down(float x)
{
    const uint u = floatBitsToUint(x);
    if (x == 0.0) { return uintBitsToFloat(0x80000001u); }
    return (x > 0.0) ? uintBitsToFloat(u - 1u) : uintBitsToFloat(u + 1u);
}
precise float bloom_ocio_cpu_div(float a, float b)
{
    precise float q = a / b;
    precise float r = fma(-b, q, a);
    q = q + r / b;
    r = fma(-b, q, a);
    q = q + r / b;
    r = fma(-b, q, a);
    if (r != 0.0) {
        // The exact quotient is q + r/b. Move q by one ULP toward it when the residual passes the
        // rounding midpoint; break an exact tie toward the even mantissa. The midpoint uses the
        // gap toward the chosen neighbour, which differs from the opposite gap at powers of two.
        precise float up = bloom_ocio_cpu_next_up(q);
        precise float down = bloom_ocio_cpu_next_down(q);
        precise bool away = (r > 0.0) == (b > 0.0);
        precise float gap = away ? abs(up - q) : abs(q - down);
        precise float midpoint = abs(b) * 0.5 * gap;
        if (abs(r) > midpoint) {
            q = away ? up : down;
        } else if (abs(r) == midpoint && (floatBitsToUint(q) & 1u) != 0u) {
            q = away ? up : down;
        }
    }
    return q;
}
precise vec3 bloom_ocio_cpu_premul(vec3 rgb, float a) { return rgb * a; }
)";
}

} // namespace bloom::color
