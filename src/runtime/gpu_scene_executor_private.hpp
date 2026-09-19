#ifndef BLOOM_RUNTIME_GPU_SCENE_EXECUTOR_PRIVATE_HPP
#define BLOOM_RUNTIME_GPU_SCENE_EXECUTOR_PRIVATE_HPP

// Private to src/runtime. The executor is split into three cohesive translation units so each stays
// well under the project's 700-line budget without adding a generic abstraction:
//   gpu_scene_executor_planner.cpp   - scene validation + command DAG -> typed native step plan
//   gpu_scene_executor_execution.cpp - live-pin budget ledger, native dispatch/retirement, drain
//   gpu_scene_executor.cpp           - the public GpuSceneExecutor surface
// This header owns the shared step vocabulary, the small pure helpers, and the Impl definition.

#include <bloom/core/pixel_aspect_ratio.hpp>
#include <bloom/render/gpu_image_upload.hpp>
#include <bloom/runtime/gpu_scene_executor.hpp>
#include <bloom/runtime/prepared_gpu_scene.hpp>

#include <chrono>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

namespace bloom::runtime {

enum class GpuSceneExecutorStepKind : std::uint8_t {
    Solid,        // GpuSolid::begin: a solid command, a merge transparent base, or a transparent
                  // composition output.
    CoveredSolid, // GpuSolid::beginCovered: the exact R8 coverage path.
    // GpuImageUpload::begin: one ImageSource/VideoSource leaf. The exact converted host image is
    // uploaded once per builder semantic source key; the strong source reference is retained until
    // the submission's fence is proven retired (or the whole job is quarantined).
    Upload,
    Translation, // GpuComposite::beginTranslation: a translation command or a composition copy.
    SourceOver,  // GpuComposite::beginSourceOver: one merge layer.
};

// One native step. `command` is the scene command this step completes (kInvalid for an intermediate
// merge base); the completed image is stored in the executor's per-command image table, which a
// merge accumulator also reuses across its source-over steps.
struct GpuSceneExecutorStep final {
    GpuSceneExecutorStepKind kind = GpuSceneExecutorStepKind::Solid;
    GpuSceneCommandIndex command = kInvalidGpuSceneCommand;
    // Source-over foreground / translation input.
    GpuSceneCommandIndex input = kInvalidGpuSceneCommand;
    // Source-over destination. kInvalid means "the current image at `command`" (accumulator/base).
    GpuSceneCommandIndex destination = kInvalidGpuSceneCommand;
    // Semantic key to insert the completed image under. Empty for an intermediate accumulator step.
    std::string cacheKey;
    bool cacheOnComplete = false;

    // Solid / merge base / transparent output. The windows are optional because ImageWindow has no
    // default constructor.
    render::Rgba32f solidPixel = render::Rgba32f::transparent();
    std::optional<render::ImageWindow> solidDataWindow;
    std::optional<render::ImageWindow> solidDisplayWindow;
    core::PixelAspectRatio solidPixelAspect = core::PixelAspectRatio::square();
    // Covered solid (borrowed from the retained scene; the scene outlives the job).
    std::span<const std::uint8_t> coverage;
    float coveredOpacity = 1.0F;
    // Upload source: the strong immutable CPU source the converted pixels came from. It is retained
    // for the job lifetime so the staging copy and any unretired submission keep their source
    // alive.
    std::shared_ptr<const render::Rgba32fImage> uploadSource;
    // Translation output data window plus GPU-local translation and opacity.
    std::optional<render::ImageWindow> outputWindow;
    double translationX = 0.0;
    double translationY = 0.0;
    float translationOpacity = 1.0F;
};

namespace gpu_scene_executor_detail {

[[nodiscard]] inline GpuSceneExecutorDiagnostic
makeDiagnostic(const GpuSceneExecutorDiagnosticCode code, std::string message) {
    return GpuSceneExecutorDiagnostic{code, std::move(message)};
}

// Overflow-checked width*height*16 for a non-empty window. Used only by the planner to reject a
// structurally impossible window early; it is never reported as an actual byte count.
[[nodiscard]] inline bool checkedImageBytes(const render::ImageWindow window,
                                            std::uint64_t& out) noexcept {
    const std::uint64_t width = window.extent().width();
    const std::uint64_t height = window.extent().height();
    if (width == 0 || height == 0) {
        return false;
    }
    if (width > std::numeric_limits<std::uint64_t>::max() / height) {
        return false;
    }
    const std::uint64_t pixels = width * height;
    if (pixels > std::numeric_limits<std::uint64_t>::max() / sizeof(render::Rgba32f)) {
        return false;
    }
    out = pixels * sizeof(render::Rgba32f);
    return true;
}

// No std::visit: std::get_if is genuinely non-throwing, so the noexcept contract holds even for a
// valueless variant. A valueless command has no cache key; the empty key is the fail-closed cache
// miss, exactly like an empty semantic key.
inline const std::string kEmptyCommandKey;

[[nodiscard]] inline const std::string& commandKey(const GpuSceneCommand& command) noexcept {
    if (const auto* solid = std::get_if<GpuSceneSolidCommand>(&command)) {
        return solid->semanticKey;
    }
    if (const auto* translation = std::get_if<GpuSceneTranslationCommand>(&command)) {
        return translation->semanticKey;
    }
    if (const auto* covered = std::get_if<GpuSceneCoverageSolidCommand>(&command)) {
        return covered->semanticKey;
    }
    if (const auto* upload = std::get_if<GpuSceneUploadCommand>(&command)) {
        return upload->semanticKey;
    }
    if (const auto* merge = std::get_if<GpuSceneMergeCommand>(&command)) {
        return merge->semanticKey;
    }
    if (const auto* output = std::get_if<GpuSceneCompositionOutputCommand>(&command)) {
        return output->semanticKey;
    }
    return kEmptyCommandKey;
}

[[nodiscard]] inline bool windowsEqual(const std::optional<render::ImageWindow> lhs,
                                       const render::ImageWindow rhs) noexcept {
    return lhs.has_value() && *lhs == rhs;
}

// Full descriptor match for a produced image against a scene command. A translation's display
// window and pixel aspect are preserved from its resolved input image, so that input is passed when
// it is known.
[[nodiscard]] inline bool descriptorMatches(const GpuSceneCommand& command,
                                            const render::GpuImage& image,
                                            const render::GpuImage* translationInput) noexcept {
    if (const auto* solid = std::get_if<GpuSceneSolidCommand>(&command)) {
        return windowsEqual(image.dataWindow(), solid->dataWindow) &&
               windowsEqual(image.displayWindow(), solid->displayWindow) &&
               image.pixelAspect() == solid->pixelAspect;
    }
    if (const auto* output = std::get_if<GpuSceneCompositionOutputCommand>(&command)) {
        return windowsEqual(image.dataWindow(), output->dataWindow) &&
               windowsEqual(image.displayWindow(), output->displayWindow) &&
               image.pixelAspect() == output->pixelAspect;
    }
    if (const auto* covered = std::get_if<GpuSceneCoverageSolidCommand>(&command)) {
        return windowsEqual(image.dataWindow(), covered->outputWindow) &&
               windowsEqual(image.displayWindow(), covered->displayWindow) &&
               image.pixelAspect() == covered->pixelAspect;
    }
    if (const auto* merge = std::get_if<GpuSceneMergeCommand>(&command)) {
        return windowsEqual(image.dataWindow(), merge->outputWindow) &&
               windowsEqual(image.displayWindow(), merge->displayWindow) &&
               image.pixelAspect() == merge->pixelAspect;
    }
    if (const auto* translation = std::get_if<GpuSceneTranslationCommand>(&command)) {
        if (!windowsEqual(image.dataWindow(), translation->outputWindow)) {
            return false;
        }
        if (translationInput != nullptr) {
            return image.displayWindow() == translationInput->displayWindow() &&
                   image.pixelAspect() == translationInput->pixelAspect();
        }
        return true;
    }
    if (const auto* upload = std::get_if<GpuSceneUploadCommand>(&command)) {
        return windowsEqual(image.dataWindow(), upload->descriptor.dataWindow()) &&
               windowsEqual(image.displayWindow(), upload->descriptor.displayWindow()) &&
               image.pixelAspect() == upload->descriptor.pixelAspect();
    }
    return false;
}

[[nodiscard]] inline bool cachedDescriptorMatches(const GpuSceneCommand& command,
                                                  const render::GpuImage& image) noexcept {
    return descriptorMatches(command, image, nullptr);
}

struct SceneDescriptorInfo final {
    render::ImageWindow data;
    render::ImageWindow display;
    core::PixelAspectRatio pixelAspect;
};

// The descriptor a command's output image will carry, derived from the scene alone (a translation's
// data window is its requested output window while its display window and pixel aspect are
// inherited from its source). Used to choose the composition-output route before any image exists.
[[nodiscard]] inline std::optional<SceneDescriptorInfo>
expectedDescriptorOf(const PreparedGpuScene& scene, const GpuSceneCommandIndex index,
                     const std::size_t depth) {
    if (index == kInvalidGpuSceneCommand ||
        static_cast<std::size_t>(index) >= scene.commands().size() || depth > 64) {
        return std::nullopt;
    }
    return std::visit(
        [&](const auto& item) -> std::optional<SceneDescriptorInfo> {
            using T = std::decay_t<decltype(item)>;
            if constexpr (std::is_same_v<T, GpuSceneTranslationCommand>) {
                const auto source = expectedDescriptorOf(scene, item.input, depth + 1);
                if (!source.has_value()) {
                    return std::nullopt;
                }
                return SceneDescriptorInfo{item.outputWindow, source->display, source->pixelAspect};
            } else if constexpr (std::is_same_v<T, GpuSceneCompositionOutputCommand> ||
                                 std::is_same_v<T, GpuSceneSolidCommand>) {
                return SceneDescriptorInfo{item.dataWindow, item.displayWindow, item.pixelAspect};
            } else if constexpr (std::is_same_v<T, GpuSceneMergeCommand> ||
                                 std::is_same_v<T, GpuSceneCoverageSolidCommand>) {
                return SceneDescriptorInfo{item.outputWindow, item.displayWindow, item.pixelAspect};
            } else if constexpr (std::is_same_v<T, GpuSceneUploadCommand>) {
                return SceneDescriptorInfo{item.descriptor.dataWindow(),
                                           item.descriptor.displayWindow(),
                                           item.descriptor.pixelAspect()};
            } else {
                return std::nullopt;
            }
        },
        scene.commands()[index]);
}

[[nodiscard]] inline GpuSceneExecutorDiagnostic
diagnosticFromSolid(const render::GpuSolidDiagnostic& diagnostic) {
    switch (diagnostic.code) {
    case render::GpuSolidDiagnosticCode::OverBudget:
        return makeDiagnostic(GpuSceneExecutorDiagnosticCode::OverBudget, diagnostic.message);
    case render::GpuSolidDiagnosticCode::DeviceLost:
        return makeDiagnostic(GpuSceneExecutorDiagnosticCode::DeviceLost, diagnostic.message);
    case render::GpuSolidDiagnosticCode::WrongThread:
        return makeDiagnostic(GpuSceneExecutorDiagnosticCode::WrongThread, diagnostic.message);
    default:
        break;
    }
    return makeDiagnostic(GpuSceneExecutorDiagnosticCode::DispatchRefused, diagnostic.message);
}

[[nodiscard]] inline GpuSceneExecutorDiagnostic
diagnosticFromComposite(const render::GpuCompositeDiagnostic& diagnostic) {
    switch (diagnostic.code) {
    case render::GpuCompositeDiagnosticCode::OverBudget:
        return makeDiagnostic(GpuSceneExecutorDiagnosticCode::OverBudget, diagnostic.message);
    case render::GpuCompositeDiagnosticCode::DeviceLost:
        return makeDiagnostic(GpuSceneExecutorDiagnosticCode::DeviceLost, diagnostic.message);
    case render::GpuCompositeDiagnosticCode::WrongThread:
        return makeDiagnostic(GpuSceneExecutorDiagnosticCode::WrongThread, diagnostic.message);
    default:
        break;
    }
    return makeDiagnostic(GpuSceneExecutorDiagnosticCode::DispatchRefused, diagnostic.message);
}

[[nodiscard]] inline GpuSceneExecutorDiagnostic
diagnosticFromUpload(const render::GpuImageUploadDiagnostic& diagnostic) {
    switch (diagnostic.code) {
    case render::GpuImageUploadDiagnosticCode::OverBudget:
        return makeDiagnostic(GpuSceneExecutorDiagnosticCode::OverBudget, diagnostic.message);
    case render::GpuImageUploadDiagnosticCode::DeviceLost:
        return makeDiagnostic(GpuSceneExecutorDiagnosticCode::DeviceLost, diagnostic.message);
    case render::GpuImageUploadDiagnosticCode::WrongThread:
        return makeDiagnostic(GpuSceneExecutorDiagnosticCode::WrongThread, diagnostic.message);
    case render::GpuImageUploadDiagnosticCode::DeviceUnavailable:
    case render::GpuImageUploadDiagnosticCode::AllocationFailed:
        return makeDiagnostic(GpuSceneExecutorDiagnosticCode::DeviceUnavailable,
                              diagnostic.message);
    case render::GpuImageUploadDiagnosticCode::Unsupported:
        return makeDiagnostic(GpuSceneExecutorDiagnosticCode::Unsupported, diagnostic.message);
    default:
        break;
    }
    return makeDiagnostic(GpuSceneExecutorDiagnosticCode::DispatchRefused, diagnostic.message);
}

enum class NativePoll : std::uint8_t { Pending, Ready, Failure, WrongThread };

[[nodiscard]] inline NativePoll mapSolid(const render::GpuSolidPollResult result) noexcept {
    switch (result) {
    case render::GpuSolidPollResult::Pending:
        return NativePoll::Pending;
    case render::GpuSolidPollResult::Ready:
        return NativePoll::Ready;
    case render::GpuSolidPollResult::WrongThread:
        return NativePoll::WrongThread;
    case render::GpuSolidPollResult::Failure:
        break;
    }
    return NativePoll::Failure;
}

[[nodiscard]] inline NativePoll mapComposite(const render::GpuCompositePollResult result) noexcept {
    switch (result) {
    case render::GpuCompositePollResult::Pending:
        return NativePoll::Pending;
    case render::GpuCompositePollResult::Ready:
        return NativePoll::Ready;
    case render::GpuCompositePollResult::WrongThread:
        return NativePoll::WrongThread;
    case render::GpuCompositePollResult::Failure:
        break;
    }
    return NativePoll::Failure;
}

[[nodiscard]] inline NativePoll mapUpload(const render::GpuImageUploadPollResult result) noexcept {
    switch (result) {
    case render::GpuImageUploadPollResult::Pending:
        return NativePoll::Pending;
    case render::GpuImageUploadPollResult::Ready:
        return NativePoll::Ready;
    case render::GpuImageUploadPollResult::WrongThread:
        return NativePoll::WrongThread;
    case render::GpuImageUploadPollResult::Failure:
        break;
    }
    return NativePoll::Failure;
}

} // namespace gpu_scene_executor_detail

struct GpuSceneExecutor::Impl final {
    Impl() = default;
    Impl(const Impl&) = delete;
    Impl& operator=(const Impl&) = delete;
    ~Impl();

    render::GpuDevice* device = nullptr;
    GpuSceneCache* cache = nullptr;
    GpuSceneExecutorBudgets budgets;

    // Declared BEFORE the pipelines so that on destruction the pipelines (which drain/quarantine
    // the in-flight submission) are destroyed before the scene/input pins are released.
    std::shared_ptr<const PreparedGpuScene> scene;
    std::vector<GpuSceneExecutorStep> steps;
    std::vector<std::shared_ptr<const render::GpuImage>> images;
    std::vector<std::uint32_t> remainingUses;
    std::size_t cursor = 0;

    std::unique_ptr<render::GpuSolid> solid;
    std::unique_ptr<render::GpuComposite> composite;
    std::unique_ptr<render::GpuImageUpload> upload;

    GpuSceneExecutorJobState state = GpuSceneExecutorJobState::Idle;
    GpuSceneExecutorDiagnostic diagnostic;
    GpuSceneExecutorCounters counters;

    // Live-pin ledger: unique actual image bytes currently pinned by this request.
    struct LiveImageEntry final {
        std::uint64_t bytes = 0;
        std::uint32_t owners = 0;
    };
    std::unordered_map<const render::GpuImage*, LiveImageEntry> liveEntries;
    std::uint64_t liveBytes = 0;
    std::uint64_t peakLiveBytes = 0;

    std::uint64_t requestBudget = 0;
    std::uint64_t coverageBytes = 0;
    // Live pin bytes at the moment the in-flight op was dispatched; the true during-op peak adds
    // the op's own actual retained bytes to this.
    std::uint64_t liveBytesAtDispatch = 0;

    enum class NativeKind : std::uint8_t { None, Solid, Composite, Upload };
    NativeKind nativeKind = NativeKind::None;
    bool nativeInFlight = false;
    bool cancelRequested = false;
    bool cancelIssued = false;
    bool timeoutRequested = false;
    bool drainRequired = false;
    bool deviceLost = false;
    std::chrono::steady_clock::time_point nativeStart{};

    [[nodiscard]] bool onOwnerThread() const noexcept {
        return device != nullptr && device->isOwnerThread();
    }

    void clearJob() noexcept;
    [[nodiscard]] bool hasUnretiredNative() const noexcept;
    [[nodiscard]] std::uint64_t remainingBudget() const noexcept;
    void assignImage(GpuSceneCommandIndex index, std::shared_ptr<const render::GpuImage> pin);
    void releaseImage(GpuSceneCommandIndex index) noexcept;
    void consume(GpuSceneCommandIndex index) noexcept;
    void fail(GpuSceneExecutorDiagnosticCode code, std::string message, bool requireDrain) noexcept;
    void finishReady() noexcept;
    void advanceDrain() noexcept;

    [[nodiscard]] GpuSceneExecutorDiagnostic planCommand(GpuSceneCommandIndex index,
                                                         std::vector<std::uint8_t>& color);
    [[nodiscard]] GpuSceneExecutorDiagnostic startStep(const GpuSceneExecutorStep& step);
    [[nodiscard]] GpuSceneExecutorDiagnostic finishNative(render::GpuImage produced);
    [[nodiscard]] GpuSceneExecutorDiagnostic completeNative();
    [[nodiscard]] GpuSceneExecutorPollResult pollNative();
};

} // namespace bloom::runtime

#endif // BLOOM_RUNTIME_GPU_SCENE_EXECUTOR_PRIVATE_HPP
