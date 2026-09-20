// CPU-only proof for the versioned CPU fast-power parity adapter in color::ocioGpuSamplingGlslFor.
//
// The neutral config's process -> sRGB-texture CST is a real OCIO GammaOp whose unchanged CPU
// oracle evaluates power with OCIO's `ssePower` polynomial (OPTIMIZATION_FAST_LOG_EXP_POW). The
// adapter must redirect the generated body's `pow` token to that polynomial, must leave the
// generated OCIO body byte-for-byte intact, and must not be emitted for a program without a
// GammaOp. The device proof that this actually matches the oracle lives in the runtime native test.

#include <bloom/color/ocio_builtin_registry.hpp>
#include <bloom/color/ocio_gpu_program.hpp>

#include <iostream>
#include <string>
#include <string_view>

namespace {

int failures = 0;

void expect(const bool condition, const std::string_view message) {
    if (condition) {
        return;
    }
    ++failures;
    std::cerr << "FAILED: " << message << '\n';
}

[[nodiscard]] bool contains(const std::string_view haystack, const std::string_view needle) {
    return haystack.find(needle) != std::string_view::npos;
}

} // namespace

int main() {
    auto resolution = bloom::color::resolveBloomNeutralV1BuiltIn(
        bloom::color::OcioConfigLocatorKind::BloomBuiltIn, bloom::color::kBloomNeutralV1ConfigUri,
        bloom::color::kBloomNeutralV1ConfigDigest);
    auto neutral = std::move(resolution).takeResolved();
    if (!neutral.has_value()) {
        std::cerr << "FAILED: the Bloom Neutral built-in does not resolve\n";
        return 1;
    }

    const auto built = bloom::color::buildOcioGpuProgramForCst(
        *neutral, neutral->processColorSpaceId(), neutral->sRgbTextureColorSpaceId());
    expect(built.succeeded(), "the neutral nonlinear CST program extracts");
    if (!built.succeeded()) {
        return 1;
    }
    const auto* const program = built.program();
    expect(program != nullptr, "the extracted program exists");
    if (program == nullptr) {
        return 1;
    }
    expect(contains(program->shaderText, "// Add Gamma '"),
           "the real CST body contains an OCIO GammaOp");
    expect(!contains(program->shaderText, "bloom_ocio_cpu_fast_pow"),
           "the generated OCIO body is never rewritten in place");

    const auto adapter = bloom::color::ocioGpuSamplingGlslFor(*program);
    expect(adapter.dispatches(), "the GammaOp program emits an adapter");
    expect(contains(adapter.preamble, "#define pow(x, y) bloom_ocio_cpu_fast_pow(x, y)"),
           "the preamble redirects the pow token");
    expect(contains(adapter.preamble, "vec4 bloom_ocio_cpu_fast_pow(vec4 x, vec4 e);"),
           "the preamble forward-declares the vec4 overload");
    expect(contains(adapter.definitions, "#undef pow") &&
               contains(adapter.definitions, "bloom_ocio_cpu_log2") &&
               contains(adapter.definitions, "bloom_ocio_cpu_exp2"),
           "the definitions transcribe OCIO's sseLog2/sseExp2 fast power");

    // A program with no GammaOp signature keeps the generated hardware pow.
    bloom::render::OcioGpuProgramDesc matrixOnly;
    matrixOnly.functionName = "bloom_ocio_transform";
    matrixOnly.semanticsId = "bloom.test.matrix";
    matrixOnly.shaderText = "vec4 bloom_ocio_transform(vec4 inPixel) { return inPixel; }\n";
    const auto plain = bloom::color::ocioGpuSamplingGlslFor(matrixOnly);
    expect(!plain.dispatches() && !contains(plain.preamble, "#define pow") &&
               !contains(plain.definitions, "bloom_ocio_cpu_log2"),
           "a program without a GammaOp does not receive the fast-power adapter");

    // A program that mixes a GammaOp with a LogOp must specialize only the identified op regions,
    // leaving no global `pow` redirection behind.
    bloom::render::OcioGpuProgramDesc mixed;
    mixed.functionName = "bloom_ocio_transform";
    mixed.semanticsId = "bloom.test.mixed";
    mixed.shaderText = "  // Add Log 'Camera Log to Lin' processing\n"
                       "  {\n"
                       "    vec3 log_base = vec3(2., 2., 2.);\n"
                       "    vec3 logSeg = (outColor.rgb - log_offset) * log_slopeinv;\n"
                       "    logSeg = pow(log_base, logSeg);\n"
                       "  }\n"
                       "  // Add Gamma 'monCurveRev' processing\n"
                       "  {\n"
                       "    vec4 gamma = vec4(0.416666657);\n"
                       "    vec4 powSeg = pow( max( vec4(0., 0., 0., 0.), outColor ), gamma );\n"
                       "  }\n";
    const auto mixedAdapter = bloom::color::ocioGpuSamplingGlslFor(mixed);
    expect(mixedAdapter.dispatches() && !mixedAdapter.shaderBody.empty(),
           "a mixed GammaOp/LogOp program emits a specialized body");
    expect(!contains(mixedAdapter.preamble, "#define pow") &&
               contains(mixedAdapter.shaderBody, "bloom_ocio_cpu_gamma_pow(") &&
               contains(mixedAdapter.shaderBody, "bloom_ocio_cpu_log_exp2(logSeg, log_slopeinv)"),
           "only the identified Gamma and Log op regions are specialized");
    expect(contains(mixedAdapter.shaderBody,
                    "logSeg = bloom_ocio_cpu_sub3(outColor.rgb, log_offset);") &&
               !contains(mixedAdapter.shaderBody, "logSeg = (outColor.rgb - log_offset) *"),
           "the LogOp slope multiply is folded into the CPU's kinv decomposition");

    // A GammaOp next to an ExponentOp (both `pow(vec4, vec4)`) must not rewrite the ExponentOp.
    bloom::render::OcioGpuProgramDesc gammaExponent;
    gammaExponent.functionName = "bloom_ocio_transform";
    gammaExponent.semanticsId = "bloom.test.gamma-exponent";
    gammaExponent.shaderText =
        "  // Add an Exponent processing\n"
        "  {\n"
        "    res = pow( max( res, vec4(0.) ), vec4(2.2, 2.2, 2.2, 1.) );\n"
        "  }\n"
        "  // Add Gamma 'monCurveRev' processing\n"
        "  {\n"
        "    vec4 gamma = vec4(0.416666657);\n"
        "    vec4 powSeg = pow( max( vec4(0., 0., 0., 0.), outColor ), gamma );\n"
        "  }\n";
    const auto gammaExponentAdapter = bloom::color::ocioGpuSamplingGlslFor(gammaExponent);
    expect(contains(gammaExponentAdapter.shaderBody, "bloom_ocio_cpu_gamma_pow(") &&
               contains(gammaExponentAdapter.shaderBody, "res = pow( max( res, vec4(0.) )"),
           "an unrelated ExponentOp pow call is left untouched next to a GammaOp");

    if (failures != 0) {
        std::cerr << failures << " OCIO precision adapter expectation(s) failed\n";
        return 1;
    }
    std::cout << "PASS: OCIO CPU fast-power parity adapter\n";
    return 0;
}
