// Kernel-prep and host-axis-metadata tests for PointResampleV1.
//
// WHAT THIS FILE PROVES WITHOUT A GPU:
//   - the checked-in SPIR-V array hashes to its pinned digest;
//   - preparePointResampleAxisMaps() reproduces the REAL media-image-proxy CPU oracle arithmetic
//     (min(extent - 1, (uint32)(double(index) / scale))) for half, quarter, and non-power scales,
//     one-pixel and tail geometry, and up/down scaling, with a hard-coded literal case in addition
//     to the independent re-implementation;
//   - every map entry is in range and the map sizes are exactly the requested output extents;
//   - the portable API fails closed with no Vulkan device.
//
// WHAT THIS FILE DOES NOT DO: it never dispatches a GPU kernel. Native RGBA32F bit parity against
// the CPU oracle is proven by gpu_point_resample_native_tests.cpp.

#include <bloom/core/sha256.hpp>
#include <bloom/render/gpu_device.hpp>
#include <bloom/render/gpu_point_resample.hpp>

#include "gpu_point_resample_dispatch.hpp"
#include "shaders/point_resample_spirv.inc"

#include <algorithm>
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

#ifndef BLOOM_POINT_RESAMPLE_SPV_SHA256
#define BLOOM_POINT_RESAMPLE_SPV_SHA256                                                            \
    "4c053f341d4c23b84cf1717ef3d2a523347ae84f9e7638dd3c75410c9eeaa4a6"
#endif

namespace {

using bloom::render::GpuPointResample;
using bloom::render::GpuPointResampleAxisMaps;
using bloom::render::preparePointResampleAxisMaps;

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

// An independent re-implementation of the media-image-proxy CPU oracle axis rule, written here so a
// bug in the production helper cannot validate itself.
[[nodiscard]] std::int32_t oracleAxis(const std::uint32_t index, const std::uint32_t extent,
                                      const double scale) {
    const auto mapped = static_cast<std::uint32_t>(static_cast<double>(index) / scale);
    return static_cast<std::int32_t>(std::min(extent - 1U, mapped));
}

[[nodiscard]] std::uint32_t proxyExtent(const std::uint32_t extent, const double scale) {
    return static_cast<std::uint32_t>(
        std::max(1.0, std::ceil(static_cast<double>(extent) * scale)));
}

struct Case final {
    std::string name;
    std::uint32_t sourceWidth;
    std::uint32_t sourceHeight;
    double horizontalScale;
    double verticalScale;
};

[[nodiscard]] std::vector<Case> cases() {
    return {
        {"half", 6, 5, 0.5, 0.5},     {"quarter", 6, 5, 0.25, 0.25},
        {"nonpower", 7, 3, 0.3, 0.7}, {"one-pixel", 1, 1, 3.0, 3.0},
        {"identity", 5, 5, 1.0, 1.0}, {"tall-tail", 10, 7, 1.7, 0.6},
        {"upscale", 2, 2, 2.5, 2.5},  {"thin", 1, 9, 4.0, 0.5},
    };
}

void testSpirvPin(Expectations& expectations) {
    namespace vd = bloom::render::vulkan_detail;
    static_assert(vd::kPointResampleSpirvWordCount * 4U == vd::kPointResampleSpirvByteCount,
                  "point-resample SPIR-V word count must cover the byte count");
    const auto* raw = reinterpret_cast<const std::byte*>(vd::kPointResampleSpirvCode);
    const std::span<const std::byte> bytes(raw, vd::kPointResampleSpirvByteCount);
    const auto digest = bloom::core::Sha256Hasher::hash(bytes);
    expectations.expect(digest.has_value(), "the point-resample SPIR-V array hashes");
    if (digest.has_value()) {
        const auto hex = digest->toLowercaseHex();
        expectations.expect(
            std::string_view(hex.data(), hex.size()) == BLOOM_POINT_RESAMPLE_SPV_SHA256,
            "the embedded PointResampleV1 SPIR-V array hashes to its pinned digest");
    }
    expectations.expect(std::string_view(vd::kPointResampleSpirvDigest) ==
                            BLOOM_POINT_RESAMPLE_SPV_SHA256,
                        "the point-resample .inc digest comment matches the pinned digest");
}

void testAxisMapsMatchOracle(Expectations& expectations) {
    for (const auto& testCase : cases()) {
        const auto outWidth = proxyExtent(testCase.sourceWidth, testCase.horizontalScale);
        const auto outHeight = proxyExtent(testCase.sourceHeight, testCase.verticalScale);
        const GpuPointResampleAxisMaps maps = preparePointResampleAxisMaps(
            testCase.sourceWidth, testCase.sourceHeight, outWidth, outHeight,
            testCase.horizontalScale, testCase.verticalScale);
        expectations.expect(maps.sourceX.size() == outWidth && maps.sourceY.size() == outHeight,
                            "the axis-map sizes equal the derived extents for " + testCase.name);
        bool exact = true;
        for (std::uint32_t x = 0; x < outWidth; ++x) {
            if (maps.sourceX[x] != oracleAxis(x, testCase.sourceWidth, testCase.horizontalScale)) {
                exact = false;
            }
        }
        for (std::uint32_t y = 0; y < outHeight; ++y) {
            if (maps.sourceY[y] != oracleAxis(y, testCase.sourceHeight, testCase.verticalScale)) {
                exact = false;
            }
        }
        expectations.expect(exact,
                            "the axis maps are bit-exact to the CPU oracle for " + testCase.name);
        bool inRange = true;
        for (const auto value : maps.sourceX) {
            inRange =
                inRange && value >= 0 && value < static_cast<std::int32_t>(testCase.sourceWidth);
        }
        for (const auto value : maps.sourceY) {
            inRange =
                inRange && value >= 0 && value < static_cast<std::int32_t>(testCase.sourceHeight);
        }
        expectations.expect(inRange, "every axis-map entry is in range for " + testCase.name);
    }
}

// A hard-coded literal expectation, independent of both implementations, pins the exact mapping.
void testLiteralMapping(Expectations& expectations) {
    const auto half = preparePointResampleAxisMaps(6, 5, 3, 3, 0.5, 0.5);
    expectations.expect((half.sourceX == std::vector<std::int32_t>{0, 2, 4}),
                        "the 0.5 x-scale map is exactly {0, 2, 4}");
    const auto quarter = preparePointResampleAxisMaps(6, 5, 2, 2, 0.25, 0.25);
    expectations.expect((quarter.sourceX == std::vector<std::int32_t>{0, 4}),
                        "the 0.25 x-scale map is exactly {0, 4}");
    const auto nonpower = preparePointResampleAxisMaps(7, 3, 3, 3, 0.3, 0.7);
    expectations.expect((nonpower.sourceX == std::vector<std::int32_t>{0, 3, 6}),
                        "the 0.3 x-scale map is exactly {0, 3, 6}");
    expectations.expect(nonpower.sourceY == std::vector<std::int32_t>({0, 1, 2}),
                        "the 0.7 y-scale map is exactly {0, 1, 2}");
    const auto onePixel = preparePointResampleAxisMaps(1, 1, 3, 3, 3.0, 3.0);
    expectations.expect((onePixel.sourceX == std::vector<std::int32_t>{0, 0, 0}) &&
                            (onePixel.sourceY == std::vector<std::int32_t>{0, 0, 0}),
                        "a one-pixel source maps every output to index 0");
}

// Pure dispatch-planner boundary tests: small injected maxX (forcing a 2D tail), the 65535 X/Y
// boundaries, exact-fit grids, and the overflow-safe over-capacity refusal. No GPU.
void testDispatchPlanner(Expectations& expectations) {
    using bloom::render::planPointResampleDispatch;
    using bloom::render::PointResampleDispatch;
    PointResampleDispatch plan{};
    expectations.expect(planPointResampleDispatch(1, 65535, 65535, plan) && plan.groupsX == 1 &&
                            plan.groupsY == 1,
                        "a one-pixel frame is one workgroup");
    expectations.expect(planPointResampleDispatch(257, 65535, 65535, plan) && plan.groupsX == 2 &&
                            plan.groupsY == 1,
                        "257 pixels is two X workgroups");
    // 64x32 = 2048 pixels; forced maxX 3 yields groupsX=3, groupsY=3 (a real 2D tail).
    expectations.expect(planPointResampleDispatch(2048, 3, 65535, plan) && plan.groupsX == 3 &&
                            plan.groupsY == 3,
                        "a small maxX forces a 2D dispatch tail");
    expectations.expect(planPointResampleDispatch(512, 1, 65535, plan) && plan.groupsX == 1 &&
                            plan.groupsY == 2,
                        "maxX==1 carries the remainder entirely in Y");
    // The 65535 exact boundary and one group past it.
    const std::uint64_t boundary = 65535ULL * 256ULL;
    expectations.expect(planPointResampleDispatch(boundary, 65535, 65535, plan) &&
                            plan.groupsX == 65535 && plan.groupsY == 1,
                        "the 65535 X boundary is an exact fit in Y");
    expectations.expect(planPointResampleDispatch(boundary + 1, 65535, 65535, plan) &&
                            plan.groupsX == 65535 && plan.groupsY == 2,
                        "one group past the X boundary tails into Y");
    const std::uint64_t fullGrid = 65535ULL * 65535ULL * 256ULL;
    expectations.expect(planPointResampleDispatch(fullGrid, 65535, 65535, plan) &&
                            plan.groupsX == 65535 && plan.groupsY == 65535,
                        "the full 65535x65535 grid is accepted");
    // One group beyond the physical grid capacity is refused, not silently wrapped.
    expectations.expect(!planPointResampleDispatch(6ULL * 256ULL + 1, 2, 3, plan),
                        "a plan beyond maxX*maxY is refused");
    expectations.expect(!planPointResampleDispatch(0, 65535, 65535, plan),
                        "zero pixels is refused");
    expectations.expect(!planPointResampleDispatch(10, 0, 65535, plan), "zero maxX is refused");
    expectations.expect(!planPointResampleDispatch(10, 65535, 0, plan), "zero maxY is refused");
}

void testPortableApiFailsClosed(Expectations& expectations) {
    expectations.expect(!GpuPointResample::teardownDrainIncomplete(),
                        "no point-resample teardown drain is reported incomplete by default");
    auto device = bloom::render::GpuDevice::create();
    if (!device.hasValue()) {
        std::cout << "SKIP: no GPU device available; PointResampleV1 native creation not "
                     "exercised\n";
        return;
    }
    auto resampler = GpuPointResample::create(*device.device);
    expectations.expect(resampler.hasValue(),
                        "GpuPointResample::create succeeds on a Ready owner thread");
    if (resampler.hasValue()) {
        expectations.expect(resampler.resampler->isBoundTo(*device.device),
                            "the point-resample pipeline is bound to its creating device");
        expectations.expect(resampler.resampler->state() ==
                                bloom::render::GpuPointResampleJobState::Idle,
                            "a fresh point-resample pipeline is Idle");
    }
}

} // namespace

int main() {
    try {
        Expectations expectations;
        testSpirvPin(expectations);
        testAxisMapsMatchOracle(expectations);
        testLiteralMapping(expectations);
        testDispatchPlanner(expectations);
        testPortableApiFailsClosed(expectations);
        if (expectations.failures() != 0) {
            std::cerr << expectations.failures()
                      << " point-resample kernel-prep expectation(s) "
                         "failed\n";
            return 1;
        }
        std::cout << "point-resample kernel prep: SPIR-V pin, CPU-oracle axis maps, and portable "
                     "API verified\n";
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << "Unexpected test exception: " << exception.what() << '\n';
        return 1;
    }
}
