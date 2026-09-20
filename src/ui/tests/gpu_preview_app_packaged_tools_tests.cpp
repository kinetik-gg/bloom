// Desktop composition-root regression for the shared packaged GPU shader tools.
//
// apps/bloom/main.cpp (and bloom-cli/bloom-mcp) must compose their ONE shared OCIO resolver from
// the shared host helper `bloom::host::makePackagedGpuOcioResolver`, which derives the package from
// the target's OWN BLOOM_GPU_TOOLS_* definitions. That helper supplies the expected staged digest
// pins (BLOOM_GPU_TOOLS_*_STAGED_SHA256) for a NON-relocated package, which the resolver mandates;
// a relocated package deliberately carries no pins and instead verifies the post-patchelf inventory
// (STAGED_IDENTITY_IN_INVENTORY=1). The old hand-copied desktop request omitted the pins, so on the
// non-relocated desktop build the resolver rejected the package, the preview stage never received a
// general display program, and every >4K request fell back to the fixed Neutral interval (the
// reported desktop CPU-only regression).
//
// This test calls the helper EXACTLY as the app does and proves the resolver qualifies the REAL
// packaged tool bytes staged beside this executable and prepares the default general Neutral
// DisplayRgba8 program at 6000x4000. It then proves the negatives on a forced-NON-relocated
// package: missing pins and corrupt pins are both rejected. No device, no qualification bypass, no
// interval change.

#include <bloom/host/gpu_export_tool_package.hpp>

#include <bloom/runtime/evaluation.hpp>
#include <bloom/runtime/gpu_ocio_context.hpp>
#include <bloom/runtime/gpu_ocio_display_arm.hpp>

#include <cstdint>
#include <iostream>
#include <string>
#include <utility>

namespace {

#if defined(BLOOM_GPU_TOOLS_AVAILABLE) && BLOOM_GPU_TOOLS_AVAILABLE

int failures = 0;

void expect(const bool condition, const std::string& label) {
    if (!condition) {
        ++failures;
        std::cerr << "FAIL: " << label << '\n';
    }
}

#endif // BLOOM_GPU_TOOLS_AVAILABLE

} // namespace

int main() {
#if !defined(BLOOM_GPU_TOOLS_AVAILABLE) || (BLOOM_GPU_TOOLS_AVAILABLE == 0)
    // CTest skip code, never a false PASS.
    std::cout << "SKIP: the packaged GPU shader tools are unavailable\n";
    return 77;
#else
    // 1. The shared helper (the one the desktop root uses) must satisfy the resolver's package
    // contract for the mode it was built in: a NON-relocated package pins both expected staged
    // digests (the resolver mandates them), while a relocated package deliberately relies on the
    // post-patchelf inventory instead (STAGED_IDENTITY_IN_INVENTORY=1) and the real resolve() below
    // verifies that inventory.
    const auto package = bloom::host::packagedGpuShaderTools();
    expect(package.has_value(), "the shared helper returns a packaged tool descriptor");
    if (package.has_value() && !package->relocated) {
        expect(package->glslangStagedDigest.has_value() &&
                   package->spirvValStagedDigest.has_value(),
               "a non-relocated package supplies both expected staged digest pins");
    }

    // 2. The exact app call shape: executable-relative packaged tools.
    const auto executable = bloom::host::currentExecutablePath();
    expect(!executable.empty(), "the running test executable path is available");
    auto resolver = bloom::host::makePackagedGpuOcioResolver(executable);
    expect(resolver != nullptr, "the shared helper composes a resolver from the packaged tools");
    if (resolver == nullptr) {
        return 1;
    }

    const auto resolved = resolver->resolve();
    expect(resolved.hasValue(), "the resolver qualifies the REAL packaged tool bytes");
    if (!resolved.hasValue()) {
        std::cerr << "resolver diagnostic: " << resolved.diagnostic << '\n';
        return 1;
    }

    // 3. The default general Neutral DisplayRgba8 program prepares at the reported 6000x4000
    // composition -- the >4K route the fixed Neutral interval cannot cover.
    const auto binding = bloom::runtime::gpuDisplayColorBindingForIntent(
        bloom::runtime::EvaluationColorIntent::LinearRec709Scene, {}, {});
    bloom::runtime::GpuDisplayProgramService programService(resolver);
    const auto prepared =
        programService.prepare(binding, 6000U, 4000U, bloom::runtime::ViewAdjust{});
    if (!prepared.hasValue()) {
        std::cerr << "general display diagnostic: " << prepared.diagnostic << '\n';
    }
    expect(prepared.hasValue(),
           "the default general Neutral DisplayRgba8 program prepares at 6000x4000");
    if (prepared.hasValue()) {
        expect(prepared.program.command != nullptr &&
                   bloom::runtime::gpuOcioDisplayCommandIsDisplay(*prepared.program.command),
               "the prepared program is a genuine DisplayRgba8 display command");
    }

    // 4. Negative: a package that claims to be NON-relocated but omits the expected staged digest
    // pins is rejected by the resolver's non-relocated fail guard. `relocated` is forced false so
    // this tests that guard regardless of the mode this build actually packaged (universal).
    if (package.has_value()) {
        auto strippedPackage = *package;
        strippedPackage.relocated = false;
        strippedPackage.glslangStagedDigest.reset();
        strippedPackage.spirvValStagedDigest.reset();
        bloom::runtime::GpuOcioContextRequest request;
        request.applicationExecutable = executable;
        request.toolPackage = std::move(strippedPackage);
        bloom::runtime::GpuOcioContextResolver strippedResolver(std::move(request));
        const auto stripped = strippedResolver.resolve();
        expect(!stripped.hasValue(),
               "a non-relocated package missing its staged digest pins is rejected");
    }

    // 5. Universal negative: a NON-relocated package with WRONG (corrupt) pins is rejected by
    // digest verification regardless of the build's mode.
    if (package.has_value()) {
        auto corruptPackage = *package;
        corruptPackage.relocated = false;
        const auto bogus = bloom::core::Sha256Digest::fromLowercaseHex(std::string(64, 'a'));
        corruptPackage.glslangStagedDigest = bogus;
        corruptPackage.spirvValStagedDigest = bogus;
        bloom::runtime::GpuOcioContextRequest request;
        request.applicationExecutable = executable;
        request.toolPackage = std::move(corruptPackage);
        bloom::runtime::GpuOcioContextResolver corruptResolver(std::move(request));
        const auto corrupt = corruptResolver.resolve();
        expect(!corrupt.hasValue(),
               "a non-relocated package with corrupt staged digest pins is rejected");
    }

    if (failures != 0) {
        std::cerr << failures << " packaged-tool expectation(s) failed\n";
        return 1;
    }
    std::cout
        << "PASS: shared packaged-tool helper qualifies and prepares the >4K general display\n";
    return 0;
#endif
}
