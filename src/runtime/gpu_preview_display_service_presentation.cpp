// Owner-thread presentation ownership for GpuPreviewDisplayService. See
// gpu_preview_display_service_presentation_private.hpp for the contract. This file is the only
// place the service creates, pumps, or retires the resident-frame lease registry and the
// presentation coordinator; every one of those calls is made on the service owner thread with the
// service's own device. The UI thread only ever reads an immutable shared client and a synchronized
// snapshot.

#include "gpu_preview_display_service_private.hpp"

#include <bloom/core/color.hpp>
#include <bloom/core/pixel_aspect_ratio.hpp>
#include <bloom/render/cpu_image_primitives.hpp>
#include <bloom/render/gpu_device.hpp>
#include <bloom/render/gpu_image.hpp>
#include <bloom/render/gpu_resident_display.hpp>
#include <bloom/render/gpu_solid.hpp>
#include <bloom/render/image_types.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <utility>

namespace bloom::runtime::detail {
namespace {

using bloom::core::Color4d;
using bloom::core::PixelAspectRatio;
using bloom::render::GpuDisplayImage;
using bloom::render::GpuImage;
using bloom::render::GpuResidentDisplay;
using bloom::render::GpuResidentDisplayDiagnosticCode;
using bloom::render::GpuResidentDisplayPollResult;
using bloom::render::GpuSolid;
using bloom::render::GpuSolidDiagnosticCode;
using bloom::render::GpuSolidParameters;
using bloom::render::GpuSolidPollResult;
using bloom::render::ImageWindow;

constexpr std::uint64_t kTestLeaseByteBudget = std::uint64_t{1} << 32;

[[nodiscard]] std::optional<ImageWindow> makeWindow(const std::int64_t x, const std::int64_t y,
                                                    const std::uint64_t width,
                                                    const std::uint64_t height) {
    const auto created = ImageWindow::create(x, y, width, height);
    return created ? std::optional(*created.value()) : std::nullopt;
}

[[nodiscard]] TaskDiagnostic presentationDiagnostic(std::string code, std::string summary,
                                                    std::string detail = {}) {
    return {.code = std::move(code),
            .severity = DiagnosticSeverity::Error,
            .summary = std::move(summary),
            .detail = std::move(detail),
            .suggestedAction = "Review the GPU preview display service presentation diagnostics."};
}

// Test-only: one real resident RGBA8 display image produced on the service owner thread through the
// same GpuSolid -> GpuResidentDisplay pipeline the coordinator tests use. No fabricated bytes.
[[nodiscard]] std::shared_ptr<const GpuDisplayImage>
produceResidentImageOnOwner(const std::shared_ptr<PreviewDisplayServiceCore>& core,
                            std::string& diagnostic) {
    if (core->device == nullptr || !core->device->isOwnerThread()) {
        diagnostic = "the test lease task did not run on the service owner thread";
        return nullptr;
    }
    auto solidResult = GpuSolid::create(*core->device);
    if (!solidResult) {
        diagnostic = solidResult.diagnostic.message;
        return nullptr;
    }
    auto displayResult = GpuResidentDisplay::create(*core->device);
    if (!displayResult) {
        diagnostic = displayResult.diagnostic.message;
        return nullptr;
    }
    auto solid = std::move(solidResult.solid);
    auto display = std::move(displayResult.display);

    const auto dataWindow = makeWindow(0, 0, 16, 8);
    const auto displayWindow = makeWindow(-2, 3, 16, 8);
    const auto aspect = PixelAspectRatio::create(4, 3);
    const auto pixel =
        bloom::render::solidPixelFromStraightLinearRec709Scene(Color4d{0.25, 0.5, 0.75, 1.0});
    if (!dataWindow || !displayWindow || !aspect || !pixel) {
        diagnostic = "the test solid primitive and geometry could not be built";
        return nullptr;
    }
    const auto solidBegin =
        solid->begin(GpuSolidParameters{*pixel.value(), *dataWindow, *displayWindow, *aspect},
                     kTestLeaseByteBudget);
    if (solidBegin.code != GpuSolidDiagnosticCode::None) {
        diagnostic = solidBegin.message;
        return nullptr;
    }
    GpuSolidPollResult solidPoll = GpuSolidPollResult::Pending;
    while (solidPoll == GpuSolidPollResult::Pending) {
        solidPoll = solid->poll();
    }
    if (solidPoll != GpuSolidPollResult::Ready) {
        diagnostic = "the test solid job did not complete";
        return nullptr;
    }
    auto input = std::make_shared<const GpuImage>(solid->takeImage());
    const auto displayBegin = display->begin(input, kTestLeaseByteBudget);
    if (displayBegin.code != GpuResidentDisplayDiagnosticCode::None) {
        diagnostic = displayBegin.message;
        return nullptr;
    }
    GpuResidentDisplayPollResult displayPoll = GpuResidentDisplayPollResult::Pending;
    while (displayPoll == GpuResidentDisplayPollResult::Pending) {
        displayPoll = display->poll();
    }
    if (displayPoll != GpuResidentDisplayPollResult::Ready) {
        diagnostic = "the test resident display job did not complete";
        return nullptr;
    }
    auto image = std::make_shared<const GpuDisplayImage>(display->takeImage());
    if (!image->isValid()) {
        diagnostic = "the test resident display image is invalid";
        return nullptr;
    }
    return image;
}

} // namespace

std::unique_ptr<PreviewDisplayPresentation>
createServicePresentation(const std::shared_ptr<PreviewDisplayServiceCore>& core) noexcept {
    auto presentation = std::make_unique<PreviewDisplayPresentation>();
    if (core->options.presentation == GpuPreviewDisplayServicePresentationMode::Disabled) {
        presentation->detail = "presentation was not requested";
        return presentation;
    }
    if (core->device == nullptr) {
        presentation->detail = "the service device is unavailable";
        return presentation;
    }
    if (!core->device->isOwnerThread()) {
        // Never construct native presentation state from a foreign thread.
        return nullptr;
    }
    const auto status = core->device->presentationStatus();
    if (status.availability != render::GpuPresentationAvailability::Ready) {
        presentation->detail =
            status.detail.empty() ? std::string("presentation is unavailable") : status.detail;
        return presentation;
    }
    try {
        presentation->registry = GpuResidentFrameLeaseRegistry::create(
            *core->device, core->options.residentLeaseBudgets);
        if (presentation->registry == nullptr) {
            presentation->detail = "the resident-frame lease registry could not be created";
            return presentation;
        }
        presentation->coordinator = std::make_unique<GpuPresentationCoordinator>(
            *core->device, *presentation->registry, core->options.presentationCoordinator);
        presentation->client = presentation->coordinator->client();
        presentation->coordinator->setWakeCallback([core] { core->notify(); });
        presentation->available = presentation->client != nullptr;
        if (!presentation->available) {
            presentation->detail = "the presentation coordinator client is unavailable";
        }
    } catch (...) {
        presentation = std::make_unique<PreviewDisplayPresentation>();
        presentation->detail = "presentation creation raised";
    }
    return presentation;
}

void publishServicePresentation(const std::shared_ptr<PreviewDisplayServiceCore>& core) noexcept {
    render::GpuPresentationAvailability availability =
        render::GpuPresentationAvailability::Unavailable;
    std::string detail;
    std::shared_ptr<GpuPresentationClient> client;
    GpuPresentationShutdownStatus shutdown;
    if (core->options.presentation == GpuPreviewDisplayServicePresentationMode::Disabled) {
        availability = render::GpuPresentationAvailability::NotRequested;
    }
    if (core->presentation != nullptr) {
        detail = core->presentation->detail;
        client = core->presentation->client;
        if (core->presentation->available) {
            availability = render::GpuPresentationAvailability::Ready;
        }
        if (core->presentation->coordinator != nullptr) {
            shutdown = core->presentation->coordinator->shutdownStatus();
        }
    }
    std::lock_guard lock(core->stateMutex);
    core->publishedPresentationAvailability = availability;
    core->publishedPresentationDetail = std::move(detail);
    core->publishedPresentationClient = std::move(client);
    core->publishedPresentationShutdown = std::move(shutdown);
}

void beginServicePresentationShutdown(
    const std::shared_ptr<PreviewDisplayServiceCore>& core) noexcept {
    if (core->presentationShutdownBegun) {
        return;
    }
    core->presentationShutdownBegun = true;
    core->presentationPumps = 0;
    if (core->presentation == nullptr || core->presentation->coordinator == nullptr) {
        publishServicePresentation(core);
        return;
    }
    core->presentation->coordinator->beginShutdown();
    publishServicePresentation(core);
}

void pumpServicePresentation(const std::shared_ptr<PreviewDisplayServiceCore>& core) noexcept {
    try {
        if (core->presentation != nullptr && !core->presentationRetired) {
            if (core->presentation->coordinator != nullptr) {
                core->presentation->coordinator->pump();
                if (core->presentationShutdownBegun) {
                    ++core->presentationPumps;
                }
            }
            if (core->presentation->registry != nullptr) {
                core->presentation->registry->collectExpired();
            }
        }
    } catch (...) { // NOLINT(bugprone-empty-catch)
        // A single pump fault must not terminate the service thread; the next pump retries.
    }
    publishServicePresentation(core);
}

bool servicePresentationNeedsPump(const std::shared_ptr<PreviewDisplayServiceCore>& core) noexcept {
    if (core->presentation == nullptr || !core->presentation->available ||
        core->presentationRetired) {
        return false;
    }
    if (!core->presentationShutdownBegun) {
        return true;
    }
    if (core->presentation->coordinator == nullptr) {
        return false;
    }
    // The coordinator's status counts every non-terminal target (Attaching/Active/Resizing/
    // Retiring) as "unproven", so it cannot be used as a stop signal. Keep pumping while any target
    // is not terminal AND the bounded owner pump budget (the coordinator's own drain budget plus a
    // small slack) has not been spent; after that budget the coordinator has itself marked the
    // remaining targets Unproven, so they are reported retained, never acked as safe.
    const std::size_t budget = core->options.presentationCoordinator.shutdownDrainPumps + 8U;
    if (core->presentationPumps >= budget) {
        return false;
    }
    return !core->presentation->coordinator->shutdownStatus().drained;
}

void retireServicePresentation(const std::shared_ptr<PreviewDisplayServiceCore>& core) noexcept {
    if (core->presentation == nullptr || core->presentationRetired) {
        return;
    }
    beginServicePresentationShutdown(core);
    const std::size_t maxPumps = core->options.presentationCoordinator.shutdownDrainPumps + 8U;
    for (std::size_t pump = 0; pump < maxPumps; ++pump) {
        if (!servicePresentationNeedsPump(core)) {
            break;
        }
        pumpServicePresentation(core);
    }

    // Capture the actual, final owner snapshot before any native ownership is released.
    GpuPresentationShutdownStatus finalStatus;
    if (core->presentation->coordinator != nullptr) {
        finalStatus = core->presentation->coordinator->shutdownStatus();
    }
    const bool wasAvailable = core->presentation->available;
    core->presentationRetirementUnproven = wasAvailable && !finalStatus.drained;

    // Destruction order is mandatory: coordinator (which hands any unproven native generation and
    // its resident-image pin to the process quarantine) before the registry (which may then
    // invalidate every lease exactly once) before the device.
    core->presentation->coordinator.reset();
    core->testForeignRegistry.reset();
    if (core->presentation->registry != nullptr) {
        core->presentation->registry->invalidateAll();
        core->presentation->registry->collectExpired();
    }
    core->presentation->registry.reset();
    core->presentation->client.reset();
    core->presentation->available = false;
    core->presentationRetired = true;

    {
        std::lock_guard lock(core->stateMutex);
        core->publishedPresentationClient = nullptr;
        core->publishedPresentationShutdown = finalStatus;
        if (core->options.presentation == GpuPreviewDisplayServicePresentationMode::Disabled) {
            core->publishedPresentationAvailability =
                render::GpuPresentationAvailability::NotRequested;
        } else {
            core->publishedPresentationAvailability =
                render::GpuPresentationAvailability::Unavailable;
        }
        if (wasAvailable && !finalStatus.drained) {
            core->publishedPresentationDetail =
                "presentation retirement could not be proven within the bounded drain; the native "
                "generation is retained and the host must not tear down Qt surfaces";
        }
    }
    core->presentation.reset();
}

bool requestServicePresentationTestLease(const std::shared_ptr<PreviewDisplayServiceCore>& core,
                                         PresentationTestLeaseResult& out,
                                         const std::chrono::milliseconds timeout,
                                         const bool foreign) {
    out = PresentationTestLeaseResult{};
    if (core->presentation == nullptr || !core->presentation->available ||
        core->presentation->registry == nullptr) {
        out.diagnostic = "the service owns no live presentation generation";
        return false;
    }
    if (core->scheduler == nullptr || !core->generation.isValid()) {
        out.diagnostic = "the service has no attached GPU executor";
        return false;
    }
    static std::atomic<std::uint64_t> nextLeaseRequest{1};
    const std::uint64_t requestId = nextLeaseRequest.fetch_add(1, std::memory_order_relaxed);
    auto shared = std::make_shared<PresentationTestLeaseResult>();
    auto done = std::make_shared<std::atomic_bool>(false);

    TaskRequest request("GPU preview presentation test lease",
                        TaskOwner{.kind = TaskOwnerKind::Application,
                                  .id = TaskOwnerId::fromRaw(core->generation.value())},
                        TaskPriority::Interactive, TaskExecutor::Gpu);
    request.coalescingKey = "bloom.preview.gpu.presentation.testlease." + std::to_string(requestId);
    auto submission = core->scheduler->submitGpu<int>(
        std::move(request), core->generation, GpuTaskAdmission{0, 0},
        [core, shared, done, foreign](TaskContext&, GpuTaskCompletion<int> completion) {
            std::string diagnostic;
            auto image = produceResidentImageOnOwner(core, diagnostic);
            if (image == nullptr) {
                shared->diagnostic = std::move(diagnostic);
                static_cast<void>(std::move(completion)
                                      .fail(presentationDiagnostic(
                                          "bloom.runtime.gpu-preview-presentation-test-lease",
                                          "The presentation test lease could not be produced.",
                                          shared->diagnostic)));
                done->store(true, std::memory_order_release);
                return;
            }
            GpuResidentFrameLeaseRegistry* registry = core->presentation->registry.get();
            if (foreign) {
                if (core->testForeignRegistry == nullptr) {
                    core->testForeignRegistry = GpuResidentFrameLeaseRegistry::create(
                        *core->device, core->options.residentLeaseBudgets);
                }
                registry = core->testForeignRegistry.get();
            }
            if (registry == nullptr) {
                shared->diagnostic = "the test lease registry is unavailable";
                static_cast<void>(std::move(completion)
                                      .fail(presentationDiagnostic(
                                          "bloom.runtime.gpu-preview-presentation-test-lease",
                                          "The presentation test lease registry is unavailable.")));
                done->store(true, std::memory_order_release);
                return;
            }
            auto published = registry->publish(std::move(image));
            if (!published.hasValue()) {
                shared->diagnostic = published.diagnostic.message;
                static_cast<void>(std::move(completion)
                                      .fail(presentationDiagnostic(
                                          "bloom.runtime.gpu-preview-presentation-test-lease",
                                          "The presentation test lease publication was refused.",
                                          shared->diagnostic)));
                done->store(true, std::memory_order_release);
                return;
            }
            shared->lease = published.lease;
            shared->ran = true;
            static_cast<void>(std::move(completion).succeed(0));
            done->store(true, std::memory_order_release);
        });
    if (!submission.accepted()) {
        out.diagnostic = "the presentation test lease task was not admitted";
        return false;
    }
    core->notify();
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (!done->load(std::memory_order_acquire) && std::chrono::steady_clock::now() < deadline) {
        core->notify();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    if (!done->load(std::memory_order_acquire)) {
        out.diagnostic = "timed out waiting for the owner-thread presentation test lease";
        return false;
    }
    out = std::move(*shared);
    return out.ran;
}

} // namespace bloom::runtime::detail
