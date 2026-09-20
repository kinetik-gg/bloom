// General GPU display arm over the producer's GpuOcioProgramExecutor. Owner-thread only; see the
// header for the contract. This translation unit never reads back a full frame on the production
// path: the only readback is the parity qualification's local oracle comparison, which exists to
// prove the native display output and is never called by the service route.

#include <bloom/runtime/gpu_ocio_display_arm.hpp>

#include <bloom/color/ocio_cpu_display_frame.hpp>
#include <bloom/render/cpu_image_primitives.hpp>
#include <bloom/render/gpu_image.hpp>
#include <bloom/render/gpu_image_upload.hpp>
#include <bloom/render/image.hpp>
#include <bloom/render/image_types.hpp>

#include <algorithm>
#include <cmath>
#include <map>
#include <mutex>
#include <utility>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace bloom::runtime {
namespace {

[[nodiscard]] GpuOcioDisplayArmDiagnostic displayDiagnostic(
    const GpuOcioDisplayArmDiagnosticCode code, std::string message) {
    GpuOcioDisplayArmDiagnostic diagnostic;
    diagnostic.code = code;
    diagnostic.message = std::move(message);
    return diagnostic;
}

[[nodiscard]] GpuOcioDisplayArmDiagnosticCode
mapExecutorCode(const GpuOcioExecutorDiagnosticCode code) noexcept {
    switch (code) {
    case GpuOcioExecutorDiagnosticCode::None:
        return GpuOcioDisplayArmDiagnosticCode::None;
    case GpuOcioExecutorDiagnosticCode::InvalidArgument:
        return GpuOcioDisplayArmDiagnosticCode::InvalidRequest;
    case GpuOcioExecutorDiagnosticCode::WrongThread:
        return GpuOcioDisplayArmDiagnosticCode::WrongThread;
    case GpuOcioExecutorDiagnosticCode::DeviceUnavailable:
        return GpuOcioDisplayArmDiagnosticCode::DeviceUnavailable;
    case GpuOcioExecutorDiagnosticCode::DeviceLost:
        return GpuOcioDisplayArmDiagnosticCode::DeviceLost;
    case GpuOcioExecutorDiagnosticCode::IdentityMismatch:
        return GpuOcioDisplayArmDiagnosticCode::IdentityMismatch;
    case GpuOcioExecutorDiagnosticCode::Busy:
        return GpuOcioDisplayArmDiagnosticCode::Busy;
    case GpuOcioExecutorDiagnosticCode::OwnerDrainRequired:
        return GpuOcioDisplayArmDiagnosticCode::OwnerDrainRequired;
    case GpuOcioExecutorDiagnosticCode::OverBudget:
        return GpuOcioDisplayArmDiagnosticCode::OverBudget;
    case GpuOcioExecutorDiagnosticCode::Unsupported:
        return GpuOcioDisplayArmDiagnosticCode::DispatchRefused;
    case GpuOcioExecutorDiagnosticCode::ProgramRefused:
    case GpuOcioExecutorDiagnosticCode::DispatchRefused:
    case GpuOcioExecutorDiagnosticCode::InternalInvariant:
        return GpuOcioDisplayArmDiagnosticCode::DispatchRefused;
    case GpuOcioExecutorDiagnosticCode::Cancelled:
        return GpuOcioDisplayArmDiagnosticCode::Cancelled;
    case GpuOcioExecutorDiagnosticCode::NativeTimeout:
        return GpuOcioDisplayArmDiagnosticCode::NativeTimeout;
    }
    return GpuOcioDisplayArmDiagnosticCode::InternalInvariant;
}

} // namespace

bool gpuOcioDisplayCommandIsDisplay(const PreparedGpuOcioCommand& command) noexcept {
    return command.encoding() == GpuOcioOutputEncoding::DisplayRgba8 &&
           command.program().stage == render::OcioGpuProgramStage::DisplayPacking;
}

GpuOcioDisplayArm::GpuOcioDisplayArm(std::unique_ptr<GpuOcioProgramExecutor> executor) noexcept
    : executor_(std::move(executor)) {}

GpuOcioDisplayArm::GpuOcioDisplayArm(GpuOcioDisplayArm&&) noexcept = default;
GpuOcioDisplayArm& GpuOcioDisplayArm::operator=(GpuOcioDisplayArm&&) noexcept = default;
GpuOcioDisplayArm::~GpuOcioDisplayArm() = default;

GpuOcioDisplayArmCreateResult GpuOcioDisplayArm::create(render::GpuDevice& device) {
    if (!device.isOwnerThread()) {
        return {nullptr, displayDiagnostic(GpuOcioDisplayArmDiagnosticCode::WrongThread,
                                           "the GPU OCIO display arm must be created on the device "
                                           "owner thread")};
    }
    auto created = GpuOcioProgramExecutor::create(device);
    if (!created) {
        return {nullptr,
                displayDiagnostic(mapExecutorCode(created.diagnostic.code),
                                  created.diagnostic.message.empty()
                                      ? std::string("the GPU OCIO program executor could not be "
                                                    "created")
                                      : created.diagnostic.message)};
    }
    return {std::unique_ptr<GpuOcioDisplayArm>(
                new GpuOcioDisplayArm(std::move(created.executor))),
            GpuOcioDisplayArmDiagnostic{}};
}

bool GpuOcioDisplayArm::isBoundTo(const render::GpuDevice& device) const noexcept {
    return executor_ != nullptr && executor_->isBoundTo(device);
}

GpuOcioProgramExecutor& GpuOcioDisplayArm::executor() noexcept { return *executor_; }

const GpuOcioProgramExecutor& GpuOcioDisplayArm::executor() const noexcept { return *executor_; }

GpuOcioExecutorCounters GpuOcioDisplayArm::counters() const noexcept {
    return executor_ == nullptr ? GpuOcioExecutorCounters{} : executor_->counters();
}

bool GpuOcioDisplayArm::hasUnretiredSubmission() const noexcept {
    return executor_ != nullptr && executor_->hasUnretiredSubmission();
}

bool GpuOcioDisplayArm::deviceLost() const noexcept {
    return executor_ == nullptr || executor_->deviceLost();
}

GpuOcioDisplayArmDiagnostic GpuOcioDisplayArm::begin(const GpuOcioDisplayRequest& request) {
    if (executor_ == nullptr) {
        return displayDiagnostic(GpuOcioDisplayArmDiagnosticCode::DeviceUnavailable,
                                 "no GPU OCIO display executor exists");
    }
    if (request.command == nullptr) {
        return displayDiagnostic(GpuOcioDisplayArmDiagnosticCode::InvalidRequest,
                                 "the display request has no prepared command");
    }
    if (!gpuOcioDisplayCommandIsDisplay(*request.command)) {
        return displayDiagnostic(GpuOcioDisplayArmDiagnosticCode::NotADisplayCommand,
                                 "the prepared command is not a DisplayRgba8 display program");
    }
    if (!request.viewAdjust.valid()) {
        return displayDiagnostic(GpuOcioDisplayArmDiagnosticCode::InvalidRequest,
                                 "the view adjustment is out of range");
    }
    // The prepared command carries the exact baked post-display ViewAdjust; a request adjustment
    // that differs from the command's is an identity mismatch (a different adjustment is a different
    // command), never a silent substitution.
    if (!(request.viewAdjust == request.command->viewAdjust())) {
        return displayDiagnostic(
            GpuOcioDisplayArmDiagnosticCode::IdentityMismatch,
            "the request ViewAdjust does not match the prepared display command");
    }
    if (request.input == nullptr) {
        return displayDiagnostic(GpuOcioDisplayArmDiagnosticCode::InvalidRequest,
                                 "the display request has no input image");
    }
    const auto& geometry = request.command->geometry();
    if (request.width != geometry.width || request.height != geometry.height) {
        return displayDiagnostic(GpuOcioDisplayArmDiagnosticCode::IdentityMismatch,
                                 "the request geometry does not match the prepared command");
    }
    if (request.input->width() != geometry.width || request.input->height() != geometry.height) {
        return displayDiagnostic(GpuOcioDisplayArmDiagnosticCode::IdentityMismatch,
                                 "the input image geometry does not match the prepared command");
    }
    const auto diagnostic = executor_->begin(request.command, request.input, {}, request.byteBudget);
    return displayDiagnostic(mapExecutorCode(diagnostic.code), diagnostic.message);
}

GpuOcioExecutorPollResult GpuOcioDisplayArm::poll() {
    if (executor_ == nullptr) {
        return GpuOcioExecutorPollResult::Failure;
    }
    return executor_->poll();
}

std::optional<render::GpuDisplayImage> GpuOcioDisplayArm::takeDisplayImage() noexcept {
    if (executor_ == nullptr) {
        return std::nullopt;
    }
    return executor_->takeDisplayOutput();
}

void GpuOcioDisplayArm::cancel() noexcept {
    if (executor_ != nullptr) {
        executor_->cancel();
    }
}

bool GpuOcioDisplayArm::teardownDrainIncomplete() noexcept {
    return GpuOcioProgramExecutor::teardownDrainIncomplete();
}

// -----------------------------------------------------------------------------------------------
// Display qualification
// -----------------------------------------------------------------------------------------------

GpuOcioDisplayQualificationReport::GpuOcioDisplayQualificationReport(
    GpuOcioDisplayQualificationReport&&) noexcept = default;
GpuOcioDisplayQualificationReport& GpuOcioDisplayQualificationReport::operator=(
    GpuOcioDisplayQualificationReport&&) noexcept = default;

bool GpuOcioDisplayQualificationReport::eligibleFor(
    const render::GpuDevice& device, const PreparedGpuOcioCommand& command) const noexcept {
    if (outcome_ != GpuOcioDisplayOutcome::PreviewOnly) {
        return false;
    }
    if (device.state() != render::GpuDeviceState::Ready) {
        return false;
    }
    const std::uint64_t epoch = device.ownershipEpoch();
    if (epoch == 0 || epoch != ownershipEpoch_) {
        return false;
    }
    return command.identity() == commandIdentity_;
}

namespace {

constexpr std::string_view kGpuOcioDisplayNumericContract =
    "gpu-ocio-display rgb<=1-code alpha-exact neutral-viewadjust preview-only";

// Deterministic premultiplied fixture covering transparent, translucent, HDR and negative values
// that the display transform must map. Non-finite values are never generated.
[[nodiscard]] std::vector<render::Rgba32f> makeDisplayFixturePixels(const std::uint32_t width,
                                                                    const std::uint32_t height) {
    std::vector<render::Rgba32f> pixels(static_cast<std::size_t>(width) * height,
                                        render::Rgba32f::transparent());
    for (std::uint32_t y = 0; y < height; ++y) {
        for (std::uint32_t x = 0; x < width; ++x) {
            const float fx = static_cast<float>(x) / static_cast<float>(width);
            const float fy = static_cast<float>(y) / static_cast<float>(height);
            const float alpha = ((x + y) % 3U == 0U) ? 0.0F : 0.25F + 0.5F * fx;
            const auto pixel = render::Rgba32f::fromPremultiplied(
                (1.6F + fx) * alpha, (-0.2F + fy) * alpha, (0.125F + 2.0F * fx) * alpha, alpha);
            if (pixel.hasValue()) {
                pixels[static_cast<std::size_t>(y) * width + x] = *pixel.value();
            }
        }
    }
    return pixels;
}


// Bounded poll of the display arm. Returns false and sets the report diagnostic on failure.
[[nodiscard]] bool pollDisplayArm(GpuOcioDisplayArm& arm, std::uint64_t deadlineNanoseconds,
                                  GpuOcioDisplayDiagnostic& diagnostic) {
    const auto start = std::chrono::steady_clock::now();
    for (;;) {
        const auto polled = arm.poll();
        if (polled == GpuOcioExecutorPollResult::Ready) {
            return true;
        }
        if (polled != GpuOcioExecutorPollResult::Pending) {
            diagnostic.code = polled == GpuOcioExecutorPollResult::WrongThread
                                  ? GpuOcioDisplayDiagnosticCode::WrongThread
                                  : (arm.executor().diagnostic().code ==
                                             GpuOcioExecutorDiagnosticCode::NativeTimeout
                                         ? GpuOcioDisplayDiagnosticCode::NativeFailure
                                         : GpuOcioDisplayDiagnosticCode::NativeFailure);
            diagnostic.message = arm.executor().diagnostic().message;
            return false;
        }
        if (std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now() - start)
                .count() >= static_cast<std::int64_t>(deadlineNanoseconds)) {
            arm.cancel();
            diagnostic.code = GpuOcioDisplayDiagnosticCode::NativeFailure;
            diagnostic.message = "the display dispatch exceeded its bounded deadline";
            return false;
        }
    }
}

[[nodiscard]] bool displayMatchesOracle(const std::vector<render::Rgba8>& native,
                                        std::span<const render::Rgba8> expected) {
    if (native.size() != expected.size()) {
        return false;
    }
    for (std::size_t index = 0; index < native.size(); ++index) {
        const auto codeDistance = [](const std::uint8_t a, const std::uint8_t b) {
            return static_cast<std::uint32_t>(a > b ? a - b : b - a);
        };
        if (codeDistance(native[index].red, expected[index].red) >
                kGpuOcioDisplayRgbToleranceCodes ||
            codeDistance(native[index].green, expected[index].green) >
                kGpuOcioDisplayRgbToleranceCodes ||
            codeDistance(native[index].blue, expected[index].blue) >
                kGpuOcioDisplayRgbToleranceCodes) {
            return false;
        }
        if (native[index].alpha != expected[index].alpha) {
            return false;
        }
    }
    return true;
}

} // namespace

GpuOcioDisplayQualificationReport
qualifyGpuOcioDisplay(std::shared_ptr<const PreparedGpuOcioCommand> command,
                      const color::PreparedCpuDisplayProcessorHandle& cpuOracle,
                      render::GpuDevice& device,
                      const GpuOcioDisplayQualificationBudgets& budgets) noexcept {
    constexpr std::uint64_t kDeadlineNanoseconds = 5ULL * 1000ULL * 1000ULL * 1000ULL;
    const auto fail = [](const GpuOcioDisplayDiagnosticCode code) {
        GpuOcioDisplayQualificationReport report;
        report.outcome_ = GpuOcioDisplayOutcome::Unavailable;
        report.diagnostic_.code = code;
        return report;
    };
    try {
        if (device.state() != render::GpuDeviceState::Ready) {
            return fail(GpuOcioDisplayDiagnosticCode::DeviceUnavailable);
        }
        if (!device.isOwnerThread()) {
            return fail(GpuOcioDisplayDiagnosticCode::WrongThread);
        }
        if (command == nullptr || !gpuOcioDisplayCommandIsDisplay(*command)) {
            return fail(GpuOcioDisplayDiagnosticCode::NotADisplayCommand);
        }
        const auto& geometry = command->geometry();
        if (geometry.width == 0 || geometry.height == 0) {
            return fail(GpuOcioDisplayDiagnosticCode::IdentityMismatch);
        }
        const auto window = render::ImageWindow::create(0, 0, geometry.width, geometry.height);
        if (!window.hasValue()) {
            return fail(GpuOcioDisplayDiagnosticCode::IdentityMismatch);
        }
        const auto descriptor = render::Rgba32fImageDescriptor::create(
            *window.value(), *window.value(), core::PixelAspectRatio::square());
        if (!descriptor.hasValue()) {
            return fail(GpuOcioDisplayDiagnosticCode::InternalInvariant);
        }
        const auto fixture = makeDisplayFixturePixels(geometry.width, geometry.height);
        if (fixture.size() != descriptor.value()->layout().pixelCount) {
            return fail(GpuOcioDisplayDiagnosticCode::FixtureMismatch);
        }
        auto imageBuilder =
            render::Rgba32fImageBuilder::create(*descriptor.value(), budgets.maxImageBytes);
        if (!imageBuilder.hasValue()) {
            return fail(GpuOcioDisplayDiagnosticCode::OverBudget);
        }
        for (std::uint32_t y = 0; y < geometry.height; ++y) {
            const auto row = imageBuilder.value()->row(y);
            if (!row.hasValue()) {
                return fail(GpuOcioDisplayDiagnosticCode::InternalInvariant);
            }
            for (std::uint32_t x = 0; x < geometry.width; ++x) {
                (*row.value())[x] = fixture[static_cast<std::size_t>(y) * geometry.width + x];
            }
        }
        auto frozen = std::move(*imageBuilder.value()).freeze();
        if (!frozen.hasValue()) {
            return fail(GpuOcioDisplayDiagnosticCode::FixtureMismatch);
        }
        const auto source =
            std::make_shared<const render::Rgba32fImage>(std::move(*frozen.value()));

        auto upload = render::GpuImageUpload::create(device);
        if (!upload) {
            return fail(GpuOcioDisplayDiagnosticCode::DeviceUnavailable);
        }
        const render::GpuImageUploadParameters uploadParameters{.source = source};
        const auto uploadDiagnostic =
            upload.upload->begin(uploadParameters, budgets.maxImageBytes);
        if (uploadDiagnostic.code != render::GpuImageUploadDiagnosticCode::None) {
            return fail(GpuOcioDisplayDiagnosticCode::NativeFailure);
        }
        for (;;) {
            const auto polled = upload.upload->poll();
            if (polled == render::GpuImageUploadPollResult::Ready) {
                break;
            }
            if (polled != render::GpuImageUploadPollResult::Pending) {
                return fail(GpuOcioDisplayDiagnosticCode::NativeFailure);
            }
        }
        auto input = upload.upload->takeImage();

        auto armResult = GpuOcioDisplayArm::create(device);
        if (!armResult) {
            return fail(GpuOcioDisplayDiagnosticCode::DeviceUnavailable);
        }
        auto& arm = *armResult.arm;
        GpuOcioDisplayRequest request;
        request.command = std::move(command);
        request.input = std::make_shared<const render::GpuImage>(std::move(input));
        request.width = geometry.width;
        request.height = geometry.height;
        request.byteBudget = budgets.maxImageBytes;
        const auto begun = arm.begin(request);
        if (begun.code != GpuOcioDisplayArmDiagnosticCode::None) {
            return fail(GpuOcioDisplayDiagnosticCode::NativeFailure);
        }
        GpuOcioDisplayDiagnostic pollDiagnostic;
        if (!pollDisplayArm(arm, kDeadlineNanoseconds, pollDiagnostic)) {
            return fail(pollDiagnostic.code == GpuOcioDisplayDiagnosticCode::None
                            ? GpuOcioDisplayDiagnosticCode::NativeFailure
                            : pollDiagnostic.code);
        }
        auto display = arm.takeDisplayImage();
        if (!display.has_value()) {
            return fail(GpuOcioDisplayDiagnosticCode::NativeFailure);
        }
        const auto readback =
            render::readbackResidentDisplayImage(*display, budgets.maxImageBytes);
        if (!readback.hasValue()) {
            return fail(GpuOcioDisplayDiagnosticCode::NativeFailure);
        }

        // Independent CPU OCIO display oracle for the SAME display/view: the unchanged CPU display
        // frame path applied to the exact fixture. The native output must match it within one code
        // (alpha exact), the documented resident display numeric contract.
        const auto sourceView = source->view();
        if (!sourceView.hasValue()) {
            return fail(GpuOcioDisplayDiagnosticCode::CpuOracleFailure);
        }
        auto cpuFrame = color::produceBloomNeutralDisplayFrame(
            cpuOracle, *sourceView.value(), 65536, budgets.maxImageBytes);
        if (!cpuFrame.hasValue()) {
            return fail(GpuOcioDisplayDiagnosticCode::CpuOracleFailure);
        }
        if (!displayMatchesOracle(readback.pixels, cpuFrame.value()->pixels())) {
            return fail(GpuOcioDisplayDiagnosticCode::ParityFailure);
        }

        GpuOcioDisplayQualificationReport report;
        report.commandIdentity_ = request.command->identity();
        report.ownershipEpoch_ = device.ownershipEpoch();
        report.outcome_ = GpuOcioDisplayOutcome::PreviewOnly;
        report.numericContract_ = std::string(kGpuOcioDisplayNumericContract);
        return report;
    } catch (...) {
        return fail(GpuOcioDisplayDiagnosticCode::InternalInvariant);
    }
}

bool gpuDisplayProgramMatchesRequest(const GpuDisplayProgram& program,
                                     const GpuDisplayColorBinding& requested) noexcept {
    const auto& actual = program.binding;
    if (actual.locatorKind != requested.locatorKind ||
        actual.locatorValue != requested.locatorValue) {
        return false;
    }
    if (actual.expectedRevision != requested.expectedRevision) {
        return false;
    }
    // An empty requested display/view selects the config's own default pair; the program records the
    // actual resolved pair. An explicit request must match exactly. The config locator/revision
    // above are always compared, so an empty request never crosses configs.
    if ((!requested.display.empty() && actual.display != requested.display) ||
        (!requested.view.empty() && actual.view != requested.view)) {
        return false;
    }
    // The same rule for the working space: empty means the config's scene-linear default.
    return requested.workingColorSpaceId.empty() ||
           actual.workingColorSpaceId == requested.workingColorSpaceId;
}

GpuDisplayColorBinding
gpuDisplayColorBindingForIntent(const EvaluationColorIntent& intent, const std::string_view display,
                                const std::string_view view) noexcept {
    GpuDisplayColorBinding binding;
    binding.locatorKind = color::OcioConfigLocatorKind::BloomBuiltIn;
    const std::string_view uri = intent.ocioConfigUri.empty() ? color::kBloomNeutralV1ConfigUri
                                                              : intent.ocioConfigUri;
    binding.locatorValue = std::string(uri);
    binding.expectedRevision =
        intent.ocioConfigRevision == core::Sha256Digest{} &&
                uri == color::kBloomNeutralV1ConfigUri
            ? color::kBloomNeutralV1ConfigDigest
            : intent.ocioConfigRevision;
    binding.workingColorSpaceId = std::string(intent.workingColorSpaceId);
    binding.display = std::string(display);
    binding.view = std::string(view);
    return binding;
}

} // namespace bloom::runtime
