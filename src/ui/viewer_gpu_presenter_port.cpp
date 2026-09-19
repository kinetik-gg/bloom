// Production pass-through from ViewerGpuPort to the existing runtime GpuPresentationClient.

#include "viewer_gpu_presenter_port.hpp"

#include <utility>

namespace bloom::ui {
namespace {

class RuntimeViewerGpuPort final : public ViewerGpuPort {
  public:
    explicit RuntimeViewerGpuPort(std::shared_ptr<runtime::GpuPresentationClient> client)
        : client_(std::move(client)) {}

    [[nodiscard]] render::GpuBorrowedInstanceView instanceView() const override {
        return client_->instanceView();
    }

    [[nodiscard]] runtime::GpuPresentationPortResult
    attach(const render::GpuBorrowedSurface& surface, const std::uint32_t width,
           const std::uint32_t height) override {
        return client_->attach(surface, width, height);
    }

    [[nodiscard]] runtime::GpuPresentationPortResult
    update(const runtime::GpuPresentationTargetId target, const std::uint64_t sequence,
           runtime::GpuPresentationUpdate update) override {
        return client_->update(target, sequence, std::move(update));
    }

    [[nodiscard]] runtime::GpuPresentationPortResult
    resize(const runtime::GpuPresentationTargetId target, const std::uint64_t sequence,
           const std::uint32_t width, const std::uint32_t height) override {
        return client_->resize(target, sequence, width, height);
    }

    [[nodiscard]] runtime::GpuPresentationPortResult
    retire(const runtime::GpuPresentationTargetId target, const std::uint64_t sequence) override {
        return client_->retire(target, sequence);
    }

    [[nodiscard]] runtime::GpuPresentationPortResult
    forget(const runtime::GpuPresentationTargetId target) override {
        return client_->forget(target);
    }

    [[nodiscard]] runtime::GpuPresentationTargetSnapshot
    status(const runtime::GpuPresentationTargetId target) const override {
        return client_->status(target);
    }

    [[nodiscard]] bool ownerAlive() const noexcept override { return client_->ownerAlive(); }

  private:
    std::shared_ptr<runtime::GpuPresentationClient> client_;
};

} // namespace

std::shared_ptr<ViewerGpuPort>
makeRuntimeViewerGpuPort(std::shared_ptr<runtime::GpuPresentationClient> client) {
    return std::make_shared<RuntimeViewerGpuPort>(std::move(client));
}

} // namespace bloom::ui
