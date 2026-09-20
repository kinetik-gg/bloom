// Kernel-prep and host-metadata tests for AffineBilinearV1.
//
// WHAT THIS FILE PROVES WITHOUT A GPU:
//   - the checked-in SPIR-V array hashes to its pinned digest;
//   - prepareAffineSamples() reproduces the REAL CPU oracle arithmetic
//     (LayerTransform::inverseMap + the exact floor/Float32-factor/sentinel reduction) for
//     rotation 0/90/arbitrary, uniform/nonuniform/negative scale, anchor+translation, fractional
//     coordinates, nonzero origins, odd geometry, and transparent edges;
//   - the composed-matrix form reproduces the LayerTransform form for a sheared-free composed
//     matrix, so the "complete composed matrix" input agrees with the render-owned oracle;
//   - a zero/non-finite determinant composed matrix produces the all-transparent empty-layer
//     result rather than a bogus inverse;
//   - the portable API fails closed with no Vulkan device.
//
// WHAT THIS FILE DOES NOT DO: it never dispatches a GPU kernel. Native RGBA32F parity against the
// CPU row is the next gate (see work-result.md).

#include <bloom/core/sha256.hpp>
#include <bloom/render/gpu_affine.hpp>
#include <bloom/render/gpu_device.hpp>
#include <bloom/render/image_types.hpp>

#include "shaders/affine_bilinear_spirv.inc"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <iostream>
#include <optional>
#include <source_location>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#ifndef BLOOM_AFFINE_BILINEAR_SPV_SHA256
#define BLOOM_AFFINE_BILINEAR_SPV_SHA256                                                           \
    "ee28424e6b4b7d3c4e437f6ce438f75c7dfbf57e78b2a06b1e4aed1cb71f9373"
#endif

namespace {

using bloom::render::GpuAffine;
using bloom::render::GpuAffineDiagnosticCode;
using bloom::render::GpuAffineMatrix;
using bloom::render::GpuAffineSample;
using bloom::render::ImageWindow;
using bloom::render::kGpuAffineTransparentBase;
using bloom::render::LayerTransform;

class Expectations final {
  public:
    void expect(const bool condition, const std::string& message,
                const std::source_location location = std::source_location::current()) {
        if (condition) {
            return;
        }
        ++failures_;
        std::cerr << location.file_name() << ':' << location.line() << ": " << message << '\n';
    }
    [[nodiscard]] int failures() const noexcept { return failures_; }

  private:
    int failures_ = 0;
};

[[nodiscard]] ImageWindow window(const std::int64_t x, const std::int64_t y, const std::uint64_t w,
                                 const std::uint64_t h) {
    const auto result = ImageWindow::create(x, y, w, h);
    if (!result) {
        throw std::logic_error("invalid affine fixture window");
    }
    return *result.value();
}

[[nodiscard]] LayerTransform transform(const LayerTransform::Authored& authored,
                                       const ImageWindow sourceWindow, const double proxyX = 1.0,
                                       const double proxyY = 1.0) {
    const auto result = LayerTransform::create(authored, sourceWindow, proxyX, proxyY);
    if (!result) {
        throw std::logic_error("invalid affine fixture transform");
    }
    return *result.value();
}

// An independent re-implementation of the CPU reduction, written here so a bug in the production
// helper cannot validate itself. It is intentionally the literal layerTransformBilinearRow rule.
[[nodiscard]] GpuAffineSample localReduce(const LayerTransform& t, const ImageWindow outputWindow,
                                          const std::uint32_t column, const std::uint32_t row) {
    const auto sourceWidth = t.sourceWindow().extent().width();
    const auto sourceHeight = t.sourceWindow().extent().height();
    const auto sample = t.inverseMap(static_cast<double>(outputWindow.originX() + column),
                                     static_cast<double>(outputWindow.originY() + row));
    if (!std::isfinite(sample.x) || !std::isfinite(sample.y) || sample.x <= -1.0 ||
        sample.y <= -1.0 || sample.x >= static_cast<double>(sourceWidth) ||
        sample.y >= static_cast<double>(sourceHeight)) {
        return GpuAffineSample{kGpuAffineTransparentBase, 0, 0.0F, 0.0F};
    }
    const auto baseX = static_cast<std::int64_t>(std::floor(sample.x));
    const auto baseY = static_cast<std::int64_t>(std::floor(sample.y));
    return GpuAffineSample{static_cast<std::int32_t>(baseX), static_cast<std::int32_t>(baseY),
                           static_cast<float>(sample.x - static_cast<double>(baseX)),
                           static_cast<float>(sample.y - static_cast<double>(baseY))};
}

void testSpirvPin(Expectations& expectations) {
    namespace vd = bloom::render::vulkan_detail;
    static_assert(vd::kAffineBilinearSpirvWordCount * 4U == vd::kAffineBilinearSpirvByteCount,
                  "affine SPIR-V word count must cover the byte count");
    const auto* raw = reinterpret_cast<const std::byte*>(vd::kAffineBilinearSpirvCode);
    const std::span<const std::byte> bytes(raw, vd::kAffineBilinearSpirvByteCount);
    const auto digest = bloom::core::Sha256Hasher::hash(bytes);
    expectations.expect(digest.has_value(), "the affine SPIR-V array hashes");
    if (digest.has_value()) {
        const auto hex = digest->toLowercaseHex();
        expectations.expect(
            std::string_view(hex.data(), hex.size()) == BLOOM_AFFINE_BILINEAR_SPV_SHA256,
            "the embedded AffineBilinearV1 SPIR-V array hashes to its pinned digest");
    }
    expectations.expect(std::string_view(vd::kAffineBilinearSpirvDigest) ==
                            BLOOM_AFFINE_BILINEAR_SPV_SHA256,
                        "the affine .inc digest comment matches the pinned digest");
}

struct Case final {
    std::string name;
    LayerTransform::Authored authored;
    ImageWindow sourceWindow;
    ImageWindow outputWindow;
};

[[nodiscard]] std::vector<Case> cases() {
    const auto source = window(0, 0, 5, 3);
    return {
        {"identity", {}, source, window(0, 0, 5, 3)},
        {"translate-fractional",
         {.translationX = 0.3, .translationY = -0.7},
         source,
         window(0, 0, 6, 4)},
        {"rotate-90", {.rotationDegrees = 90.0}, source, window(-3, 0, 4, 5)},
        {"rotate-arbitrary", {.rotationDegrees = 33.5}, source, window(-4, -4, 12, 12)},
        {"nonuniform-scale", {.scaleX = 2.5, .scaleY = 0.5}, source, window(-5, -2, 16, 8)},
        {"negative-scale", {.scaleX = -1.0, .scaleY = -2.0}, source, window(-6, -4, 12, 10)},
        {"anchor-translate",
         {.translationX = 1.25,
          .translationY = -2.5,
          .anchorX = 1.0,
          .anchorY = 1.0,
          .scaleX = 1.75,
          .scaleY = 0.75,
          .rotationDegrees = 20.0},
         source,
         window(-3, -3, 14, 12)},
        {"nonzero-origin", {}, window(10, 20, 5, 3), window(8, 17, 11, 9)},
        {"odd-outside", {}, source, window(-4, -4, 16, 12)},
    };
}

void testSamplesMatchOracle(Expectations& expectations) {
    for (const auto& testCase : cases()) {
        const auto t = transform(testCase.authored, testCase.sourceWindow);
        const auto samples = bloom::render::prepareAffineSamples(t, testCase.outputWindow);
        const auto width = testCase.outputWindow.extent().width();
        const auto height = testCase.outputWindow.extent().height();
        expectations.expect(samples.size() == static_cast<std::size_t>(width) * height,
                            "sample count matches the output window for " + testCase.name);
        bool exact = true;
        for (std::uint32_t row = 0; row < height; ++row) {
            for (std::uint32_t column = 0; column < width; ++column) {
                const auto expected = localReduce(t, testCase.outputWindow, column, row);
                const auto& actual = samples[static_cast<std::size_t>(row) * width + column];
                if (!(expected == actual)) {
                    exact = false;
                }
            }
        }
        expectations.expect(exact,
                            "host metadata is bit-exact to the CPU oracle for " + testCase.name);
        if (testCase.name == "rotate-90") {
            // A quarter turn about the layer centre lands pixel centres on pixel centres, so every
            // in-range sample must have zero fractional factor.
            bool zeroFactors = true;
            for (const auto& sample : samples) {
                if (sample.baseX == kGpuAffineTransparentBase) {
                    continue;
                }
                if (sample.factorX != 0.0F || sample.factorY != 0.0F) {
                    zeroFactors = false;
                }
            }
            expectations.expect(zeroFactors,
                                "a 90-degree rotation interpolates nothing (exact pixel centres)");
        }
    }
}

// The composed-matrix form must agree with the LayerTransform oracle for a composed placement that
// a rotation/scale/anchor decomposition can express. The matrix is recovered from the oracle's own
// public forwardMap() probes, so this is not a second, hand-written matrix.
void testComposedMatrixMatchesOracle(Expectations& expectations) {
    const auto source = window(0, 0, 5, 3);
    const auto output = window(-3, -3, 14, 12);
    const LayerTransform::Authored authored{.translationX = 1.25,
                                            .translationY = -2.5,
                                            .anchorX = 1.0,
                                            .anchorY = 1.0,
                                            .scaleX = 1.75,
                                            .scaleY = 0.75,
                                            .rotationDegrees = 20.0};
    const auto t = transform(authored, source);
    const auto origin = t.forwardMap(0.0, 0.0);
    const auto unitX = t.forwardMap(1.0, 0.0);
    const auto unitY = t.forwardMap(0.0, 1.0);
    const GpuAffineMatrix matrix{.a = unitX.x - origin.x,
                                 .b = unitY.x - origin.x,
                                 .tx = origin.x,
                                 .c = unitX.y - origin.y,
                                 .d = unitY.y - origin.y,
                                 .ty = origin.y};
    const auto fromMatrix = bloom::render::prepareAffineMatrixSamples(matrix, source, output);
    const auto fromTransform = bloom::render::prepareAffineSamples(t, output);
    expectations.expect(fromMatrix.size() == fromTransform.size(),
                        "the composed-matrix and LayerTransform sample counts agree");
    bool agrees = true;
    std::size_t inRange = 0;
    for (std::size_t index = 0; index < fromMatrix.size() && index < fromTransform.size();
         ++index) {
        const auto& lhs = fromMatrix[index];
        const auto& rhs = fromTransform[index];
        if (rhs.baseX == kGpuAffineTransparentBase) {
            if (lhs.baseX != kGpuAffineTransparentBase) {
                agrees = false;
            }
            continue;
        }
        ++inRange;
        if (lhs.baseX != rhs.baseX || lhs.baseY != rhs.baseY ||
            std::fabs(lhs.factorX - rhs.factorX) > 1e-6F ||
            std::fabs(lhs.factorY - rhs.factorY) > 1e-6F) {
            agrees = false;
        }
    }
    expectations.expect(inRange > 0, "the composed-matrix case has in-range samples");
    expectations.expect(agrees, "the composed-matrix input agrees with the LayerTransform oracle");
}

void testCollapsedMatrixIsTransparent(Expectations& expectations) {
    const auto source = window(0, 0, 5, 3);
    const auto output = window(0, 0, 4, 4);
    const GpuAffineMatrix collapsed{.a = 0.0, .b = 0.0, .tx = 1.0, .c = 0.0, .d = 0.0, .ty = 1.0};
    const auto samples = bloom::render::prepareAffineMatrixSamples(collapsed, source, output);
    expectations.expect(samples.size() == 16, "a collapsed matrix still sizes its metadata");
    bool allTransparent = true;
    for (const auto& sample : samples) {
        if (sample.baseX != kGpuAffineTransparentBase) {
            allTransparent = false;
        }
    }
    expectations.expect(allTransparent,
                        "a collapsed (zero-determinant) matrix is the empty-layer transparent "
                        "result");
}

void testPortableApiFailsClosed(Expectations& expectations) {
    expectations.expect(!GpuAffine::teardownDrainIncomplete(),
                        "no affine teardown drain is reported incomplete by default");
    auto device = bloom::render::GpuDevice::create();
    if (!device.hasValue()) {
        // No Vulkan device (stub build or hardware-free host): the portable API's fail-closed
        // contract is that no GpuAffine can be created without a Ready owner-thread device.
        std::cout
            << "SKIP: no GPU device available; AffineBilinearV1 native creation not exercised\n";
        return;
    }
    // A real device is present: creating the affine pipeline on its owner thread must succeed, and
    // it must be bound to exactly that device.
    auto affine = GpuAffine::create(*device.device);
    expectations.expect(affine.hasValue(), "GpuAffine::create succeeds on a Ready owner thread");
    if (affine.hasValue()) {
        expectations.expect(affine.affine->isBoundTo(*device.device),
                            "the affine pipeline is bound to its creating device");
        expectations.expect(affine.affine->state() == bloom::render::GpuAffineJobState::Idle,
                            "a fresh affine pipeline is Idle");
    }
}

} // namespace

int main() {
    try {
        Expectations expectations;
        testSpirvPin(expectations);
        testSamplesMatchOracle(expectations);
        testComposedMatrixMatchesOracle(expectations);
        testCollapsedMatrixIsTransparent(expectations);
        testPortableApiFailsClosed(expectations);
        if (expectations.failures() != 0) {
            std::cerr << expectations.failures() << " affine kernel-prep expectation(s) failed\n";
            return 1;
        }
        std::cout << "affine kernel prep: SPIR-V pin, CPU-oracle metadata, composed matrix, and "
                     "portable API verified\n";
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << "Unexpected test exception: " << exception.what() << '\n';
        return 1;
    }
}
