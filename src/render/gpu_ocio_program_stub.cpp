#include <bloom/render/gpu_ocio_program.hpp>

// Portable CPU-unavailable stub: the same public API compiles and links without any Vulkan
// dependency. Every create() reports DeviceUnavailable; no native object exists.
namespace bloom::render {

// The resident-display stub translation unit defines this empty impl; the inline defaulted
// GpuDisplayImage constructor needs it complete, so provide the identical definition here too.
struct GpuDisplayImageImpl final {};

namespace {
[[nodiscard]] GpuOcioProgramDiagnostic unavailable() {
    GpuOcioProgramDiagnostic diagnostic;
    diagnostic.code = GpuOcioProgramDiagnosticCode::DeviceUnavailable;
    diagnostic.message = "the GPU OCIO program requires the Vulkan backend";
    return diagnostic;
}
} // namespace

struct GpuOcioProgram::Impl final {};

GpuOcioProgram::GpuOcioProgram(std::unique_ptr<Impl>) noexcept {}
GpuOcioProgram::GpuOcioProgram(GpuOcioProgram&&) noexcept = default;
GpuOcioProgram& GpuOcioProgram::operator=(GpuOcioProgram&&) noexcept = default;
GpuOcioProgram::~GpuOcioProgram() = default;
void GpuOcioProgram::releaseImpl() noexcept {}

GpuOcioProgramCreateResult GpuOcioProgram::create(GpuDevice&, OcioGpuProgramDesc,
                                                  std::span<const std::uint32_t>,
                                                  const GpuOcioProgramBudgets&,
                                                  GpuOcioProgramCancellation) {
    return {nullptr, unavailable()};
}
GpuOcioProgramJobState GpuOcioProgram::state() const noexcept {
    return GpuOcioProgramJobState::Failure;
}
const GpuOcioProgramDiagnostic& GpuOcioProgram::diagnostic() const noexcept {
    static const GpuOcioProgramDiagnostic diagnostic = unavailable();
    return diagnostic;
}
bool GpuOcioProgram::isBoundTo(GpuDevice&) const noexcept { return false; }
GpuOcioProgramDiagnostic GpuOcioProgram::beginEffect(std::shared_ptr<const GpuImage>,
                                                     std::span<const std::byte>, std::uint64_t) {
    return unavailable();
}
GpuOcioProgramDiagnostic GpuOcioProgram::beginDisplay(std::shared_ptr<const GpuImage>,
                                                      std::span<const std::byte>, std::uint64_t) {
    return unavailable();
}
GpuOcioProgramPollResult GpuOcioProgram::poll() { return GpuOcioProgramPollResult::Failure; }
std::shared_ptr<GpuImage> GpuOcioProgram::takeEffectOutput() noexcept { return {}; }
GpuDisplayImage GpuOcioProgram::takeDisplayOutput() noexcept { return {}; }
bool GpuOcioProgram::hasUnretiredSubmission() const noexcept { return false; }
std::uint64_t GpuOcioProgram::retainedAllocationBytes() const noexcept { return 0; }
std::uint64_t GpuOcioProgram::lastJobAllocationBytes() const noexcept { return 0; }
void GpuOcioProgram::cancel() noexcept {}
bool GpuOcioProgram::teardownDrainIncomplete() noexcept { return false; }

} // namespace bloom::render
