// Portable stub check: the resident-preview qualification runtime source must compile, link, and
// run with NO Vulkan dependency. The stub render backend honestly reports no device, so this
// exercises the CPU-only identity predicates and the report vocabulary rather than a fake GPU
// success. The actual device proof lives in gpu_resident_preview_qualification_tests.cpp.

#include <bloom/color/ocio_builtin_registry.hpp>
#include <bloom/color/ocio_cpu_display_processor.hpp>
#include <bloom/runtime/gpu_resident_preview_qualification.hpp>

#include <iostream>
#include <optional>
#include <string>

namespace {

using bloom::color::PreparedCpuDisplayProcessorHandle;

[[nodiscard]] std::optional<PreparedCpuDisplayProcessorHandle> buildCanonicalProcessor() {
    auto resolution = bloom::color::resolveBloomNeutralV1BuiltIn(
        bloom::color::OcioConfigLocatorKind::BloomBuiltIn, bloom::color::kBloomNeutralV1ConfigUri,
        bloom::color::kBloomNeutralV1ConfigDigest);
    if (!resolution.ready()) {
        return std::nullopt;
    }
    auto resolved = std::move(resolution).takeResolved();
    if (!resolved.has_value()) {
        return std::nullopt;
    }
    auto built = bloom::color::buildBloomNeutralCpuDisplayProcessor(*resolved);
    if (!built) {
        return std::nullopt;
    }
    return std::move(built).takeHandle();
}

[[nodiscard]] std::optional<PreparedCpuDisplayProcessorHandle> buildNonDefaultProcessor() {
    const auto revision = bloom::color::ocioBuiltInContentRevision(
        bloom::color::OcioConfigLocatorKind::BloomBuiltIn, bloom::color::kAcesCgV1ConfigUri);
    if (!revision.has_value()) {
        return std::nullopt;
    }
    auto resolution = bloom::color::resolveOcioBuiltIn(
        bloom::color::OcioConfigLocatorKind::BloomBuiltIn, bloom::color::kAcesCgV1ConfigUri,
        *revision, bloom::color::kAcesCgV1SceneLinearColorSpaceId);
    if (!resolution.ready()) {
        return std::nullopt;
    }
    auto resolved = std::move(resolution).takeResolved();
    if (!resolved.has_value()) {
        return std::nullopt;
    }
    for (const bloom::color::DisplayViewEntry& entry : resolved->displays()) {
        auto built =
            bloom::color::buildCpuDisplayProcessorForView(*resolved, entry.display, entry.view);
        if (built) {
            return std::move(built).takeHandle();
        }
    }
    return std::nullopt;
}

} // namespace

int main() {
    const auto canonical = buildCanonicalProcessor();
    if (!canonical.has_value()) {
        std::cerr << "FAIL: the canonical CPU OCIO processor is unavailable\n";
        return 1;
    }
    std::string reason;
    if (!bloom::runtime::gpuResidentPreviewProcessorIsEligible(*canonical, reason)) {
        std::cerr << "FAIL: the canonical processor was not eligible: " << reason << '\n';
        return 1;
    }
    if (bloom::runtime::kGpuResidentPreviewShaderPins.size() != 5) {
        std::cerr << "FAIL: the pinned shader set is incomplete\n";
        return 1;
    }
    const auto nonDefault = buildNonDefaultProcessor();
    if (!nonDefault.has_value()) {
        std::cerr << "FAIL: no non-default processor could be built for the negative check\n";
        return 1;
    }
    if (bloom::runtime::gpuResidentPreviewProcessorIsEligible(*nonDefault, reason)) {
        std::cerr << "FAIL: the non-default processor was accepted\n";
        return 1;
    }
    std::cout << "PASS: portable stub resident-preview predicates\n";
    return 0;
}
