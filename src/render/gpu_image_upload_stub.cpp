#include <bloom/render/gpu_image_upload.hpp>

#include <memory>
#include <string>
#include <utility>

// Portable CPU/unavailable stub: the same public API as the Vulkan backend, compiled when the
// renderer has no Vulkan dependency. No device, buffer, or native type is touched. Every creation
// and begin fails closed with DeviceUnavailable, exactly like the other GPU operation stubs, so a
// caller takes its ordinary CPU fallback with no special case.

namespace bloom::render {

// Complete only in a stub build so GpuImage's inline default constructor can instantiate
// unique_ptr<GpuImageImpl>'s deleter. This matches the solid/composite stubs' identical definition;
// a class definition may appear in multiple translation units. It carries no Vulkan type. The
// out-of-line GpuImage members are provided by the solid stub (or the real gpu_image.cpp) in the
// same build; this stub deliberately does not redefine them.
struct GpuImageImpl final {};

namespace {

[[nodiscard]] GpuImageUploadDiagnostic unavailable(std::string message) {
    return GpuImageUploadDiagnostic{GpuImageUploadDiagnosticCode::DeviceUnavailable,
                                    std::move(message)};
}

} // namespace

struct GpuImageUpload::Impl final {
    GpuImageUploadBudgets budgets;
    GpuImageUploadJobState jobState = GpuImageUploadJobState::Idle;
    GpuImageUploadDiagnostic jobDiagnostic;
};

GpuImageUpload::GpuImageUpload(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}
GpuImageUpload::GpuImageUpload(GpuImageUpload&& other) noexcept = default;
GpuImageUpload& GpuImageUpload::operator=(GpuImageUpload&& other) noexcept {
    if (this != &other) {
        releaseImpl();
        impl_ = std::move(other.impl_);
    }
    return *this;
}
GpuImageUpload::~GpuImageUpload() { releaseImpl(); }

void GpuImageUpload::releaseImpl() noexcept { impl_.reset(); }

GpuImageUploadCreateResult GpuImageUpload::create(GpuDevice&,
                                                  const GpuImageUploadBudgets& budgets) {
    if (budgets.maxImageBytes == 0 || budgets.maxStagingBytes == 0) {
        return {nullptr, GpuImageUploadDiagnostic{GpuImageUploadDiagnosticCode::InvalidArgument,
                                                  "the upload budget is out of range"}};
    }
    return {nullptr, unavailable("GPU image upload is not available in this build")};
}

GpuImageUploadJobState GpuImageUpload::state() const noexcept {
    return GpuImageUploadJobState::Idle;
}

const GpuImageUploadDiagnostic& GpuImageUpload::diagnostic() const noexcept {
    static const GpuImageUploadDiagnostic none{};
    return none;
}

bool GpuImageUpload::isBoundTo(GpuDevice&) const noexcept { return false; }

GpuImageUploadDiagnostic GpuImageUpload::begin(const GpuImageUploadParameters&, std::uint64_t) {
    return unavailable("GPU image upload is not available in this build");
}

GpuImageUploadPollResult GpuImageUpload::poll() { return GpuImageUploadPollResult::Failure; }

const GpuImage* GpuImageUpload::image() const noexcept { return nullptr; }

GpuImage GpuImageUpload::takeImage() noexcept { return GpuImage{}; }

GpuImageReadback GpuImageUpload::readback() noexcept {
    GpuImageReadback result;
    result.code = GpuImageReadbackCode::DeviceUnavailable;
    result.message = "GPU image upload is not available in this build";
    return result;
}

void GpuImageUpload::cancel() noexcept {}

bool GpuImageUpload::teardownDrainIncomplete() noexcept { return false; }

} // namespace bloom::render
