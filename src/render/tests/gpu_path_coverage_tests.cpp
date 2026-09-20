// Native GpuPathCoverage test entry point and embedded SPIR-V digest pin. The
// CPU oracle is the unchanged PathRaster::coverageRow, compared byte-for-byte
// with the resident mask the GPU producer reads back over shapes, transforms,
// fill rules, strokes, clipping, proxies and adversarial sample-on-edge cases.
// It also proves one resident coverage -> GpuSolid covered fill -> native output
// chain consumes the same device buffer (no download/re-upload), reports a
// positive native dispatch counter, and that empty/budget/cancel/refused inputs
// recover cleanly. Local mode pins the explicit loader and requires a device; a
// hardware-free CI image prints an explicit skip and --require-device fails
// closed. Fixtures live in gpu_path_coverage_test_support.hpp; parity and
// lifecycle tests live in the sibling translation units.

#include <bloom/core/sha256.hpp>

#include "gpu_path_coverage_test_support.hpp"
#include "shaders/path_coverage_spirv.inc"

#include <cstddef>
#include <exception>
#include <iostream>
#include <span>
#include <string_view>

namespace bloom::render::gpu_path_coverage_test {
namespace {

// Pins the checked-in source -> SPIR-V -> embedded-array chain.
void testEmbeddedSpirvDigest(Expectations& expectations) {
    static_assert(bloom::render::vulkan_detail::kPathCoverageSpirvWordCount * 4U ==
                      bloom::render::vulkan_detail::kPathCoverageSpirvByteCount,
                  "SPIR-V word count must exactly cover the byte count");
    const auto* raw =
        reinterpret_cast<const std::byte*>(bloom::render::vulkan_detail::kPathCoverageSpirvCode);
    const std::span<const std::byte> bytes(
        raw, bloom::render::vulkan_detail::kPathCoverageSpirvByteCount);
    const auto digest = bloom::core::Sha256Hasher::hash(bytes);
    expectations.expect(digest.has_value(), "the embedded GpuPathCoverage SPIR-V hashes");
    if (!digest.has_value()) {
        return;
    }
    const auto hex = digest->toLowercaseHex();
    expectations.expect(std::string_view(hex.data(), hex.size()) == BLOOM_PATH_COVERAGE_SPV_SHA256,
                        "the embedded GpuPathCoverage SPIR-V matches the pinned digest");
    expectations.expect(std::string_view(bloom::render::vulkan_detail::kPathCoverageSpirvDigest) ==
                            BLOOM_PATH_COVERAGE_SPV_SHA256,
                        "the .inc digest comment matches the pinned digest");
}

} // namespace
} // namespace bloom::render::gpu_path_coverage_test

int main(int argc, char** argv) {
    try {
        using namespace bloom::render;
        using namespace bloom::render::gpu_path_coverage_test;
        const Options options = parseOptions(argc, argv);
        if (!options.valid) {
            return 2;
        }
        Expectations expectations;
        testEmbeddedSpirvDigest(expectations);

        GpuDeviceCreationOptions createOptions;
        createOptions.loader_path = options.loader_path;
        auto device = GpuDevice::create(createOptions);
        if (!device) {
            if (options.require_device) {
                std::cerr << "FAIL: required device unavailable: " << device.diagnostic.message
                          << '\n';
                return 1;
            }
            std::cout << "SKIP: no compatible Vulkan device available: "
                      << device.diagnostic.message << '\n';
            return expectations.ok() ? 0 : 1;
        }

        auto produced = GpuPathCoverage::create(*device.device);
        expectations.expect(produced.hasValue(), "the GpuPathCoverage pipeline is created");
        if (!produced) {
            std::cerr << "FAIL: GpuPathCoverage create failed: " << produced.diagnostic.message
                      << '\n';
            return 1;
        }
        auto created = GpuSolid::create(*device.device);
        expectations.expect(created.hasValue(), "the GpuSolid pipeline is created");
        if (!created) {
            std::cerr << "FAIL: GpuSolid create failed: " << created.diagnostic.message << '\n';
            return 1;
        }

        testShapeMatrix(expectations, *produced.coverage);
        testGlyphs(expectations, *produced.coverage);
        testResidentConsumption(expectations, *produced.coverage, *created.solid, *device.device);
        testGuards(expectations, *produced.coverage, *created.solid);
        testDeviceIdentity(expectations, *produced.coverage, *created.solid, *device.device,
                           options.loader_path);
        testFaultLifecycle(expectations, *device.device);
        testTwoDimensionalPlan(expectations, *produced.coverage);
        testPerformance(expectations, *produced.coverage);
        testResidentPoolBound(expectations, *device.device);
        testResidentPoolOwnerIsolation(expectations, options.loader_path);

        if (!expectations.ok()) {
            std::cerr << "FAIL: GpuPathCoverage native checks failed\n";
            return 1;
        }
        std::cout << "PASS: GpuPathCoverage native checks\n";
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << "Unexpected test exception: " << exception.what() << '\n';
        return 1;
    }
}
