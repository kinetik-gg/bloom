#pragma once

// Real-device harness helpers for the GPU media-scene executor tests: option parsing, the
// non-blocking begin/poll driver, pixel gates and descriptor checks. No expectation type lives here
// so the test translation unit can use the media-scene proof's own Expectations.

#include <bloom/render/gpu_device.hpp>
#include <bloom/render/gpu_image.hpp>
#include <bloom/render/image_types.hpp>
#include <bloom/runtime/gpu_scene_cache.hpp>
#include <bloom/runtime/gpu_scene_executor.hpp>
#include <bloom/runtime/prepared_gpu_scene.hpp>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

namespace bloom::runtime::media_executor_test {

using bloom::render::GpuImage;
using bloom::render::ImageWindow;
using bloom::render::readbackResidentImage;
using bloom::render::Rgba32f;

inline constexpr std::uint64_t kSceneBudget = 1ULL << 32U;
inline constexpr std::uint64_t kReadbackBudget = 1ULL << 32U;
inline constexpr std::uint64_t kCacheBudget = 1ULL << 32U;
inline constexpr std::uint64_t kMaxPollIterations = 5'000'000;

struct Options final {
    std::filesystem::path loader_path;
    // Optional real media-worker fixture directory for the real-video vector. Absent means the
    // video vector prints an explicit NOTE instead of silently passing.
    std::filesystem::path fixtures;
    bool require_device = false;
    bool valid = true;
};

[[nodiscard]] inline Options parseOptions(const int argc, char** argv) {
    Options options;
    for (int index = 1; index < argc; ++index) {
        const std::string_view argument = argv[index];
        if (argument == "--loader") {
            if (index + 1 >= argc) {
                std::cerr << "--loader requires a path argument\n";
                options.valid = false;
                return options;
            }
            options.loader_path = argv[++index];
        } else if (argument == "--fixtures") {
            if (index + 1 >= argc) {
                std::cerr << "--fixtures requires a path argument\n";
                options.valid = false;
                return options;
            }
            options.fixtures = argv[++index];
        } else if (argument == "--require-device") {
            options.require_device = true;
        } else {
            std::cerr << "unknown argument: " << argument << '\n';
            options.valid = false;
            return options;
        }
    }
    return options;
}

[[nodiscard]] inline const std::string& keyOf(const GpuSceneCommand& command) {
    return std::visit([](const auto& item) -> const std::string& { return item.semanticKey; },
                      command);
}

[[nodiscard]] inline bool closeEnough(const float lhs, const float rhs) noexcept {
    if (!std::isfinite(lhs) || !std::isfinite(rhs)) {
        return lhs == rhs;
    }
    const float difference = std::abs(lhs - rhs);
    const float scale = std::max(1.0F, std::max(std::abs(lhs), std::abs(rhs)));
    return difference <= 2.0e-6F * scale;
}

[[nodiscard]] inline bool pixelsClose(const std::span<const Rgba32f> lhs,
                                      const std::span<const Rgba32f> rhs) noexcept {
    if (lhs.size() != rhs.size()) {
        return false;
    }
    for (std::size_t i = 0; i < lhs.size(); ++i) {
        if (!closeEnough(lhs[i].red(), rhs[i].red()) ||
            !closeEnough(lhs[i].green(), rhs[i].green()) ||
            !closeEnough(lhs[i].blue(), rhs[i].blue()) ||
            !closeEnough(lhs[i].alpha(), rhs[i].alpha())) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] inline bool pixelsExact(const std::span<const Rgba32f> lhs,
                                      const std::span<const Rgba32f> rhs) noexcept {
    if (lhs.size() != rhs.size()) {
        return false;
    }
    return std::memcmp(lhs.data(), rhs.data(), lhs.size() * sizeof(Rgba32f)) == 0;
}

struct SceneRun final {
    GpuSceneExecutorDiagnostic begin;
    GpuSceneExecutorPollResult finalPoll = GpuSceneExecutorPollResult::Pending;
    std::shared_ptr<const GpuImage> image;
    bool ready = false;
    GpuSceneExecutorCounters countersAtReady;
};

[[nodiscard]] inline SceneRun runScene(GpuSceneExecutor& executor,
                                       std::shared_ptr<const PreparedGpuScene> scene,
                                       const std::uint64_t budget) {
    SceneRun run;
    run.begin = executor.begin(std::move(scene), budget);
    if (run.begin.code != GpuSceneExecutorDiagnosticCode::None) {
        std::cerr << "executor begin refused: code=" << static_cast<int>(run.begin.code)
                  << " message=" << run.begin.message << '\n';
        return run;
    }
    for (std::uint64_t iteration = 0; iteration < kMaxPollIterations; ++iteration) {
        const auto result = executor.poll();
        if (result == GpuSceneExecutorPollResult::Ready) {
            run.finalPoll = result;
            run.countersAtReady = executor.counters();
            run.image = executor.takeImage();
            run.ready = run.image != nullptr;
            return run;
        }
        if (result == GpuSceneExecutorPollResult::Failure ||
            result == GpuSceneExecutorPollResult::WrongThread) {
            run.finalPoll = result;
            return run;
        }
        std::this_thread::yield();
    }
    return run;
}

// Drives poll() until it stops returning Pending, without calling takeImage. Used by the failure /
// retention gates. Returns the last non-Pending result.
[[nodiscard]] inline GpuSceneExecutorPollResult
drainToTerminal(GpuSceneExecutor& executor,
                const std::uint64_t maxIterations = kMaxPollIterations) {
    for (std::uint64_t iteration = 0; iteration < maxIterations; ++iteration) {
        const auto result = executor.poll();
        if (result != GpuSceneExecutorPollResult::Pending) {
            return result;
        }
        std::this_thread::yield();
    }
    return GpuSceneExecutorPollResult::Pending;
}

[[nodiscard]] inline bool descriptorMatchesScene(const GpuImage& image,
                                                 const PreparedGpuScene& scene) noexcept {
    const auto& descriptor = scene.outputDescriptor();
    return image.dataWindow().has_value() && *image.dataWindow() == descriptor.dataWindow() &&
           image.displayWindow().has_value() &&
           *image.displayWindow() == descriptor.displayWindow() &&
           image.pixelAspect() == descriptor.pixelAspect();
}

} // namespace bloom::runtime::media_executor_test
