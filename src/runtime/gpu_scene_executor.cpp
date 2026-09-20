#include <bloom/runtime/gpu_scene_executor.hpp>

#include "gpu_scene_executor_private.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace bloom::runtime {
namespace {

using gpu_scene_executor_detail::checkedImageBytes;
using gpu_scene_executor_detail::makeDiagnostic;

} // namespace

GpuSceneExecutor::GpuSceneExecutor(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}

GpuSceneExecutor::GpuSceneExecutor(GpuSceneExecutor&& other) noexcept
    : impl_(std::move(other.impl_)) {}

GpuSceneExecutor& GpuSceneExecutor::operator=(GpuSceneExecutor&& other) noexcept {
    if (this != &other) {
        releaseImpl();
        impl_ = std::move(other.impl_);
    }
    return *this;
}

GpuSceneExecutor::~GpuSceneExecutor() { releaseImpl(); }

void GpuSceneExecutor::releaseImpl() noexcept { impl_.reset(); }

GpuSceneExecutorCreateResult GpuSceneExecutor::create(render::GpuDevice& device,
                                                      GpuSceneCache& cache,
                                                      const GpuSceneExecutorBudgets& budgets) {
    if (!device.isOwnerThread()) {
        return {nullptr, makeDiagnostic(GpuSceneExecutorDiagnosticCode::WrongThread,
                                        "GpuSceneExecutor::create must run on the device owner "
                                        "thread")};
    }
    if (!cache.isValid() || !cache.isBoundTo(device)) {
        return {nullptr, makeDiagnostic(GpuSceneExecutorDiagnosticCode::InvalidArgument,
                                        "the content cache is not bound to this device")};
    }
    auto solid = render::GpuSolid::create(device, render::GpuSolidBudgets{budgets.maxImageBytes});
    if (!solid) {
        return {nullptr, makeDiagnostic(GpuSceneExecutorDiagnosticCode::DeviceUnavailable,
                                        solid.diagnostic.message)};
    }
    auto composite = render::GpuComposite::create(
        device, render::GpuCompositeBudgets{budgets.maxImageBytes, budgets.maxMetadataBytes});
    if (!composite) {
        return {nullptr, makeDiagnostic(GpuSceneExecutorDiagnosticCode::DeviceUnavailable,
                                        composite.diagnostic.message)};
    }
    // The upload's own pipeline ceiling is the per-operation image ceiling for both the resident
    // image and its transient staging buffer; the per-request headroom is charged at begin().
    auto upload = render::GpuImageUpload::create(
        device, render::GpuImageUploadBudgets{budgets.maxImageBytes, budgets.maxImageBytes});
    if (!upload) {
        return {nullptr, makeDiagnostic(GpuSceneExecutorDiagnosticCode::DeviceUnavailable,
                                        upload.diagnostic.message)};
    }
    auto affine = render::GpuAffine::create(
        device, render::GpuAffineBudgets{budgets.maxImageBytes, budgets.maxAffineMetadataBytes});
    if (!affine) {
        return {nullptr, makeDiagnostic(GpuSceneExecutorDiagnosticCode::DeviceUnavailable,
                                        affine.diagnostic.message)};
    }
    auto blend = render::GpuBlend::create(
        device, render::GpuBlendBudgets{budgets.maxImageBytes, budgets.maxMetadataBytes});
    if (!blend) {
        return {nullptr, makeDiagnostic(GpuSceneExecutorDiagnosticCode::DeviceUnavailable,
                                        blend.diagnostic.message)};
    }
    // The point resample's metadata is O(width + height) int32 indices, far smaller than the image;
    // it is bounded by the per-operation metadata ceiling.
    auto pointResample = render::GpuPointResample::create(
        device, render::GpuPointResampleBudgets{budgets.maxImageBytes, budgets.maxMetadataBytes});
    if (!pointResample) {
        return {nullptr, makeDiagnostic(GpuSceneExecutorDiagnosticCode::DeviceUnavailable,
                                        pointResample.diagnostic.message)};
    }
    auto impl = std::make_unique<Impl>();
    impl->device = &device;
    impl->cache = &cache;
    impl->budgets = budgets;
    impl->solid = std::move(solid.solid);
    impl->composite = std::move(composite.composite);
    impl->upload = std::move(upload.upload);
    impl->affine = std::move(affine.affine);
    impl->blend = std::move(blend.blend);
    impl->pointResample = std::move(pointResample.resampler);
    return {std::unique_ptr<GpuSceneExecutor>(new GpuSceneExecutor(std::move(impl))), {}};
}

GpuSceneExecutorJobState GpuSceneExecutor::state() const noexcept {
    return impl_ == nullptr ? GpuSceneExecutorJobState::Idle : impl_->state;
}

const GpuSceneExecutorDiagnostic& GpuSceneExecutor::diagnostic() const noexcept {
    static const GpuSceneExecutorDiagnostic empty{};
    return impl_ == nullptr ? empty : impl_->diagnostic;
}

bool GpuSceneExecutor::isBoundTo(const render::GpuDevice& device) const noexcept {
    return impl_ != nullptr && impl_->device == &device;
}

GpuSceneExecutorCounters GpuSceneExecutor::counters() const noexcept {
    if (impl_ == nullptr) {
        return GpuSceneExecutorCounters{};
    }
    auto snapshot = impl_->counters;
    snapshot.currentLiveImageBytes = impl_->liveBytes;
    // clearJob() resets the per-job member peak on failure, so preserve the accumulated peak: a
    // reported zero here means no peak was ever observed, never that a failure erased the evidence.
    snapshot.peakLiveImageBytes = std::max(snapshot.peakLiveImageBytes, impl_->peakLiveBytes);
    return snapshot;
}

GpuSceneExecutorProgress GpuSceneExecutor::progress() const noexcept {
    if (impl_ == nullptr) {
        return {};
    }
    const auto total = static_cast<std::uint32_t>(
        std::min<std::size_t>(impl_->steps.size(), std::numeric_limits<std::uint32_t>::max()));
    const auto completed = static_cast<std::uint32_t>(
        std::min<std::size_t>(impl_->cursor, std::numeric_limits<std::uint32_t>::max()));
    return GpuSceneExecutorProgress{completed, total};
}

bool GpuSceneExecutor::ownerDrainRequired() const noexcept {
    return impl_ != nullptr && impl_->drainRequired;
}

bool GpuSceneExecutor::deviceLost() const noexcept { return impl_ != nullptr && impl_->deviceLost; }

GpuSceneExecutorDiagnostic GpuSceneExecutor::begin(std::shared_ptr<const PreparedGpuScene> scene,
                                                   const std::uint64_t requestByteBudget) {
    if (impl_ == nullptr) {
        return makeDiagnostic(GpuSceneExecutorDiagnosticCode::DeviceUnavailable,
                              "the executor is not initialized");
    }
    Impl& impl = *impl_;
    if (!impl.onOwnerThread()) {
        return makeDiagnostic(GpuSceneExecutorDiagnosticCode::WrongThread,
                              "begin must run on the device owner thread");
    }
    if (impl.deviceLost) {
        return makeDiagnostic(GpuSceneExecutorDiagnosticCode::DeviceLost,
                              "the device generation was lost; this executor is terminal");
    }
    if (impl.drainRequired) {
        return makeDiagnostic(GpuSceneExecutorDiagnosticCode::OwnerDrainRequired,
                              "a prior unproven native submission must be drained before reuse");
    }
    if (impl.state == GpuSceneExecutorJobState::Pending ||
        impl.state == GpuSceneExecutorJobState::Ready || impl.nativeInFlight) {
        return makeDiagnostic(GpuSceneExecutorDiagnosticCode::Busy,
                              "a previous job has not been retired or taken");
    }
    if (!impl.cache->isBoundTo(*impl.device)) {
        return makeDiagnostic(GpuSceneExecutorDiagnosticCode::DeviceUnavailable,
                              "the content cache is no longer bound to this device");
    }
    if (scene == nullptr) {
        return makeDiagnostic(GpuSceneExecutorDiagnosticCode::InvalidArgument, "no scene");
    }
    impl.clearJob();
    impl.diagnostic = {};
    impl.state = GpuSceneExecutorJobState::Idle;

    const auto& commands = scene->commands();
    if (commands.empty()) {
        return makeDiagnostic(GpuSceneExecutorDiagnosticCode::InvalidScene, "the scene is empty");
    }
    if (commands.size() > impl.budgets.maxCommands) {
        return makeDiagnostic(GpuSceneExecutorDiagnosticCode::TooManyCommands,
                              "the scene exceeds the configured command ceiling");
    }
    const GpuSceneCommandIndex output = scene->outputCommand();
    if (output == kInvalidGpuSceneCommand || static_cast<std::size_t>(output) >= commands.size() ||
        !std::holds_alternative<GpuSceneCompositionOutputCommand>(commands[output])) {
        return makeDiagnostic(GpuSceneExecutorDiagnosticCode::InvalidScene,
                              "the scene output is not its terminal composition output");
    }

    impl.scene = std::move(scene);
    impl.requestBudget = requestByteBudget;
    impl.images.assign(commands.size(), nullptr);
    impl.remainingUses.assign(commands.size(), 0);

    std::vector<std::uint8_t> color(commands.size(), 0);
    auto planned = impl.planCommand(output, color);
    if (planned.code != GpuSceneExecutorDiagnosticCode::None) {
        impl.clearJob();
        impl.diagnostic = planned;
        impl.state = GpuSceneExecutorJobState::Idle;
        return planned;
    }

    // Count, per ACTUALLY emitted step, how many times each command's image is consumed. Only steps
    // generated after the cache cut exist here, so an edge to an unreachable or cache-hit subtree
    // is never counted and never holds a pin. A repeated or shared input is counted once per
    // consuming step, and an explicit destination is counted like an input. The current merge
    // accumulator and the terminal output are deliberately absent: the accumulator is replaced in
    // place by assignImage(), and the output is retained until takeImage(). The last consumer
    // therefore releases an intermediate only after the native operation that consumed it has
    // completed.
    for (const auto& step : impl.steps) {
        if (step.input != kInvalidGpuSceneCommand &&
            static_cast<std::size_t>(step.input) < impl.remainingUses.size()) {
            ++impl.remainingUses[step.input];
        }
        if (step.destination != kInvalidGpuSceneCommand &&
            static_cast<std::size_t>(step.destination) < impl.remainingUses.size()) {
            ++impl.remainingUses[step.destination];
        }
    }

    // Conservative early guard: the peak live set can never be smaller than the largest single
    // step's requested extent, so a budget below that cannot succeed. This is a lower bound only;
    // it never sums the graph's cumulative allocation and it is never reported as actual VMA bytes.
    std::uint64_t maxStepRequestedBytes = 0;
    for (const auto& step : impl.steps) {
        const std::optional<render::ImageWindow> window =
            step.kind == GpuSceneExecutorStepKind::Translation ||
                    step.kind == GpuSceneExecutorStepKind::Affine ||
                    step.kind == GpuSceneExecutorStepKind::Blend ||
                    step.kind == GpuSceneExecutorStepKind::PointResample ||
                    step.kind == GpuSceneExecutorStepKind::OcioEffect
                ? step.outputWindow
                : step.solidDataWindow;
        if (!window.has_value()) {
            continue;
        }
        std::uint64_t bytes = 0;
        if (checkedImageBytes(*window, bytes)) {
            maxStepRequestedBytes = std::max(maxStepRequestedBytes, bytes);
        }
    }
    if (maxStepRequestedBytes > requestByteBudget) {
        impl.clearJob();
        impl.state = GpuSceneExecutorJobState::Idle;
        ++impl.counters.budgetRefusals;
        return makeDiagnostic(GpuSceneExecutorDiagnosticCode::OverBudget,
                              "the request byte budget cannot cover the largest native step");
    }

    // The request budget bounds the actual LIVE unique pinned bytes. Planning already pinned every
    // cache hit and charged its real VMA size (deduped); produced commands are charged step by step
    // as the native op reports their actual size, with the per-step op budget taken from the live
    // headroom. A long sequential graph whose peak live set fits is therefore accepted even if its
    // cumulative allocation exceeds the budget, and a request whose cached inputs alone exceed the
    // budget is refused here before any Vulkan work.
    if (impl.liveBytes > requestByteBudget) {
        impl.clearJob();
        impl.state = GpuSceneExecutorJobState::Idle;
        ++impl.counters.budgetRefusals;
        return makeDiagnostic(GpuSceneExecutorDiagnosticCode::OverBudget,
                              "the request byte budget cannot cover the live cached input pins");
    }

    impl.cursor = 0;
    impl.state = GpuSceneExecutorJobState::Pending;
    impl.diagnostic = {};
    ++impl.counters.sceneBegins;
    return {};
}

GpuSceneExecutorPollResult GpuSceneExecutor::poll() {
    if (impl_ == nullptr) {
        return GpuSceneExecutorPollResult::Failure;
    }
    Impl& impl = *impl_;
    if (!impl.onOwnerThread()) {
        return GpuSceneExecutorPollResult::WrongThread;
    }
    if (impl.deviceLost) {
        return GpuSceneExecutorPollResult::Failure;
    }
    switch (impl.state) {
    case GpuSceneExecutorJobState::Ready:
        return GpuSceneExecutorPollResult::Ready;
    case GpuSceneExecutorJobState::Idle:
        return GpuSceneExecutorPollResult::Failure;
    case GpuSceneExecutorJobState::Failure:
        if (impl.drainRequired) {
            // The logical request already failed; this only advances owner-thread retirement.
            impl.advanceDrain();
        }
        return GpuSceneExecutorPollResult::Failure;
    case GpuSceneExecutorJobState::Pending:
        break;
    }
    if (impl.nativeInFlight) {
        if (impl.cancelRequested && !impl.cancelIssued) {
            if (impl.nativeKind == Impl::NativeKind::Solid) {
                impl.solid->cancel();
            } else if (impl.nativeKind == Impl::NativeKind::Upload) {
                impl.upload->cancel();
            } else if (impl.nativeKind == Impl::NativeKind::Affine) {
                impl.affine->cancel();
            } else if (impl.nativeKind == Impl::NativeKind::Blend) {
                impl.blend->cancel();
            } else if (impl.nativeKind == Impl::NativeKind::PointResample) {
                impl.cancelPointResample();
            } else if (impl.nativeKind == Impl::NativeKind::PathCoverage) {
                if (impl.pathCoverage != nullptr) {
                    impl.pathCoverage->cancel();
                }
            } else if (impl.nativeKind == Impl::NativeKind::Ocio) {
                if (impl.nativeOcioProgram != nullptr) {
                    impl.nativeOcioProgram->cancel();
                }
            } else {
                impl.composite->cancel();
            }
            impl.cancelIssued = true;
        }
        return impl.pollNative();
    }
    if (impl.cancelRequested) {
        impl.fail(GpuSceneExecutorDiagnosticCode::Cancelled,
                  "the scene execution was cancelled; no image was published", false);
        return GpuSceneExecutorPollResult::Failure;
    }
    if (impl.cursor >= impl.steps.size()) {
        impl.finishReady();
        return impl.state == GpuSceneExecutorJobState::Ready ? GpuSceneExecutorPollResult::Ready
                                                             : GpuSceneExecutorPollResult::Failure;
    }
    const auto started = impl.startStep(impl.steps[impl.cursor]);
    if (started.code != GpuSceneExecutorDiagnosticCode::None) {
        // Bounded accounting context: the step's own refusal is distinguished from the executor's
        // per-request allowance and its remaining headroom without a second probe.
        impl.fail(started.code,
                  started.message + " [requestBudget=" + std::to_string(impl.requestBudget) +
                      ", liveBytes=" + std::to_string(impl.liveBytes) +
                      ", remaining=" + std::to_string(impl.remainingBudget()) + ']',
                  false);
        return GpuSceneExecutorPollResult::Failure;
    }
    return GpuSceneExecutorPollResult::Pending;
}

const render::GpuImage* GpuSceneExecutor::image() const noexcept {
    if (impl_ == nullptr || impl_->state != GpuSceneExecutorJobState::Ready ||
        impl_->scene == nullptr) {
        return nullptr;
    }
    const GpuSceneCommandIndex output = impl_->scene->outputCommand();
    if (output == kInvalidGpuSceneCommand ||
        static_cast<std::size_t>(output) >= impl_->images.size()) {
        return nullptr;
    }
    return impl_->images[output].get();
}

std::shared_ptr<const render::GpuImage> GpuSceneExecutor::takeImage() {
    if (impl_ == nullptr || impl_->state != GpuSceneExecutorJobState::Ready ||
        impl_->scene == nullptr) {
        return nullptr;
    }
    const GpuSceneCommandIndex output = impl_->scene->outputCommand();
    if (output == kInvalidGpuSceneCommand ||
        static_cast<std::size_t>(output) >= impl_->images.size()) {
        return nullptr;
    }
    auto result = impl_->images[output];
    impl_->clearJob();
    impl_->state = GpuSceneExecutorJobState::Idle;
    impl_->diagnostic = {};
    return result;
}

void GpuSceneExecutor::cancel() noexcept {
    if (impl_ == nullptr) {
        return;
    }
    impl_->cancelRequested = true;
    if (impl_->nativeInFlight && !impl_->cancelIssued) {
        if (impl_->nativeKind == Impl::NativeKind::Solid) {
            impl_->solid->cancel();
        } else if (impl_->nativeKind == Impl::NativeKind::Upload) {
            impl_->upload->cancel();
        } else if (impl_->nativeKind == Impl::NativeKind::Composite) {
            impl_->composite->cancel();
        } else if (impl_->nativeKind == Impl::NativeKind::Affine) {
            impl_->affine->cancel();
        } else if (impl_->nativeKind == Impl::NativeKind::Blend) {
            impl_->blend->cancel();
        } else if (impl_->nativeKind == Impl::NativeKind::PointResample) {
            impl_->cancelPointResample();
        } else if (impl_->nativeKind == Impl::NativeKind::PathCoverage) {
            if (impl_->pathCoverage != nullptr) {
                impl_->pathCoverage->cancel();
            }
        } else if (impl_->nativeKind == Impl::NativeKind::Ocio) {
            if (impl_->nativeOcioProgram != nullptr) {
                impl_->nativeOcioProgram->cancel();
            }
        }
        impl_->cancelIssued = true;
    }
}

bool GpuSceneExecutor::teardownDrainIncomplete() noexcept {
    // Only the owned native pipelines can report an actual bounded-drain failure. The executor adds
    // no separate fuse: its pins are released by ordinary destruction after the native drain has
    // resolved ownership, so a merely-pending fault-injection poll before a successful native drain
    // must not be reported incomplete.
    return render::GpuSolid::teardownDrainIncomplete() ||
           render::GpuComposite::teardownDrainIncomplete() ||
           render::GpuImageUpload::teardownDrainIncomplete() ||
           render::GpuAffine::teardownDrainIncomplete() ||
           render::GpuBlend::teardownDrainIncomplete() ||
           render::GpuPointResample::teardownDrainIncomplete() ||
           render::GpuPathCoverage::teardownDrainIncomplete() ||
           render::GpuOcioProgram::teardownDrainIncomplete();
}

// Folded additive accessor (declared in the public header). It introduces no field or layout
// change; it reports both the executor's own native-in-flight flag and any owned pipeline's
// unretired submission. Kept here in the owning translation unit rather than a tiny separate one.
bool GpuSceneExecutor::hasUnretiredSubmission() const noexcept {
    return impl_ != nullptr && (impl_->nativeInFlight || impl_->hasUnretiredNative());
}

} // namespace bloom::runtime
