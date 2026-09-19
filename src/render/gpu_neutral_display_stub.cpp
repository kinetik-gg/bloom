#include <bloom/render/gpu_neutral_display.hpp>

#include <memory>
#include <utility>

// Portable CPU-unavailable stub for the fixed Bloom Neutral v1 display compute operation. It
// carries the exact same public API and includes no Vulkan header, so a build without GPU
// dependencies still links and the CPU reference display path remains the correctness oracle. No
// shader is embedded or executed here.

namespace bloom::render {

struct GpuNeutralDisplay::Impl final {};

GpuNeutralDisplay::GpuNeutralDisplay(std::unique_ptr<Impl> impl) noexcept
    : impl_(std::move(impl)) {}

GpuNeutralDisplay::GpuNeutralDisplay(GpuNeutralDisplay&& other) noexcept = default;

GpuNeutralDisplay& GpuNeutralDisplay::operator=(GpuNeutralDisplay&& other) noexcept = default;

GpuNeutralDisplay::~GpuNeutralDisplay() = default;

GpuNeutralDisplayCreateResult GpuNeutralDisplay::create(GpuDevice&,
                                                        const GpuNeutralDisplayBudgets&) {
    return {nullptr,
            {GpuNeutralDisplayDiagnosticCode::DeviceUnavailable,
             "Bloom was built without Vulkan dependencies; the CPU display path remains active"}};
}

GpuNeutralDisplayJobState GpuNeutralDisplay::state() const noexcept {
    return GpuNeutralDisplayJobState::Idle;
}

bool GpuNeutralDisplay::isBoundTo(GpuDevice&) const noexcept { return false; }

const GpuNeutralDisplayDiagnostic& GpuNeutralDisplay::diagnostic() const noexcept {
    static const GpuNeutralDisplayDiagnostic unavailable{
        GpuNeutralDisplayDiagnosticCode::DeviceUnavailable,
        "Bloom was built without Vulkan dependencies; the CPU display path remains active"};
    return unavailable;
}

GpuNeutralDisplayDiagnostic GpuNeutralDisplay::begin(std::span<const Rgba32f>, std::uint64_t) {
    return diagnostic();
}

GpuNeutralDisplayPollResult GpuNeutralDisplay::poll() {
    return GpuNeutralDisplayPollResult::Failure;
}

GpuNeutralDisplayReadback GpuNeutralDisplay::readback() {
    return {std::vector<Rgba8>{}, diagnostic()};
}

void GpuNeutralDisplay::cancel() noexcept {}

bool GpuNeutralDisplay::teardownDrainIncomplete() noexcept { return false; }

} // namespace bloom::render
