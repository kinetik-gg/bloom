// Portable no-Vulkan fallback for ViewerGpuPresenter, split out of
// viewer_gpu_presenter.cpp so the native-ownership TU stays focused. Truthful
// unsupported behavior only: no Qt Vulkan object, no container, no attach, no
// fabricated retention claim. The Vulkan branch lives in
// viewer_gpu_presenter.cpp; exactly one of the two TUs defines the class in a
// given build.

#include <bloom/ui/viewer_gpu_presenter.hpp>

#include <memory>
#include <string>
#include <utility>

#ifdef BLOOM_UI_HAS_VULKAN

// Nothing to define here: viewer_gpu_presenter.cpp provides the Vulkan
// implementation.

#else

namespace bloom::ui {

struct ViewerGpuPresenter::Impl final {
    State state = State::Unsupported;
    std::string diagnostic = "this build has no Vulkan presentation support";
    bool safeToDestroy = true;
    ReadyCallback readyCallback;
};

ViewerGpuPresenter::ViewerGpuPresenter(std::shared_ptr<runtime::GpuPresentationClient>, Config,
                                       QObject* parent)
    : QObject(parent), impl_(std::make_unique<Impl>()) {}

ViewerGpuPresenter::ViewerGpuPresenter(std::shared_ptr<ViewerGpuPort>, Config, QObject* parent)
    : QObject(parent), impl_(std::make_unique<Impl>()) {}

ViewerGpuPresenter::~ViewerGpuPresenter() = default;

bool ViewerGpuPresenter::pinVulkanLoader(const std::string&) { return false; }

bool ViewerGpuPresenter::initialize() {
    if (impl_->readyCallback) {
        impl_->readyCallback(false, impl_->diagnostic);
    }
    return false;
}

bool ViewerGpuPresenter::initialized() const noexcept { return false; }

QWidget* ViewerGpuPresenter::container() const noexcept { return nullptr; }

QWindow* ViewerGpuPresenter::window() const noexcept { return nullptr; }

ViewerGpuPresenter::State ViewerGpuPresenter::state() const noexcept { return impl_->state; }

bool ViewerGpuPresenter::attached() const noexcept { return false; }

bool ViewerGpuPresenter::acceptingPresent() const noexcept { return false; }

runtime::GpuPresentationTargetId ViewerGpuPresenter::targetId() const noexcept {
    return runtime::kInvalidPresentationTarget;
}

std::uint64_t ViewerGpuPresenter::lastSequence() const noexcept { return 0; }

bool ViewerGpuPresenter::surfaceSafeToDestroy() const noexcept { return impl_->safeToDestroy; }

const std::string& ViewerGpuPresenter::diagnostic() const noexcept { return impl_->diagnostic; }

render::GpuBorrowedInstanceView ViewerGpuPresenter::borrowedInstanceView() const { return {}; }

std::uint64_t ViewerGpuPresenter::surfaceBits() const noexcept { return 0; }

std::uint64_t ViewerGpuPresenter::lastEnqueuedSequence() const noexcept { return 0; }

std::uint64_t ViewerGpuPresenter::appliedSequence() const noexcept { return 0; }

std::uint64_t ViewerGpuPresenter::presentCount() const noexcept { return 0; }

void ViewerGpuPresenter::setContainerParent(QWidget*) noexcept {}

void ViewerGpuPresenter::setReadyCallback(ReadyCallback callback) {
    impl_->readyCallback = std::move(callback);
}

void ViewerGpuPresenter::setInputCallback(InputCallback) {}

bool ViewerGpuPresenter::present(const runtime::GpuResidentFrameLease&,
                                 const render::GpuPresentImageParams&,
                                 std::shared_ptr<const runtime::GpuPresentationOverlay>) {
    return false;
}

bool ViewerGpuPresenter::requestResize(std::uint32_t, std::uint32_t) { return false; }

bool ViewerGpuPresenter::prepareForMutation(MutationCallback completion) {
    if (completion) {
        completion(
            MutationResult{MutationOutcome::SafeToMutate, impl_->state, true, impl_->diagnostic});
    }
    return true;
}

bool ViewerGpuPresenter::pollNow() { return false; }

bool ViewerGpuPresenter::eventFilter(QObject*, QEvent*) { return false; }

} // namespace bloom::ui

#endif // BLOOM_UI_HAS_VULKAN
