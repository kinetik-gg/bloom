#include "ocio_gpu_program_resources_test_support.hpp"
#include "vulkan/gpu_ocio_program_dispatch_plan.hpp"

namespace bloom::color::ocio_resources_test {

// Pure capacity-planning test with injected device limits. No device is required, so it runs even
// when the native lane skips.
void testDispatchPlan(Expectations& expectations) {
    using bloom::render::ocio_program_detail::effectiveWorkGroupCount;
    using bloom::render::ocio_program_detail::OcioDispatchPlan;
    using bloom::render::ocio_program_detail::OcioDispatchPlanError;
    using bloom::render::ocio_program_detail::planOcioDispatch;
    constexpr std::uint32_t kWorkgroup = 64;

    const auto covers = [](const OcioDispatchPlan& plan, const std::uint64_t pixels,
                           const std::uint32_t maxX, const std::uint32_t maxY) {
        if (!plan.valid() || plan.groupsX == 0 || plan.groupsY == 0) {
            return false;
        }
        if (plan.groupsX > maxX || plan.groupsY > maxY) {
            return false;
        }
        const std::uint64_t stride =
            static_cast<std::uint64_t>(plan.groupsX) * kWorkgroup;
        if (stride > 0xFFFFFFFFULL) {
            return false;
        }
        const std::uint64_t total = stride * plan.groupsY;
        return total >= pixels && total <= 0x100000000ULL;
    };

    const auto single = planOcioDispatch(1, kWorkgroup, 65535, 65535);
    expectations.expect(single.valid() && single.groupsX == 1 && single.groupsY == 1,
                        "one pixel plans to a 1x1 dispatch");
    const auto tail = planOcioDispatch(65, kWorkgroup, 65535, 65535);
    expectations.expect(tail.valid() && tail.groupsX == 2 && tail.groupsY == 1,
                        "a 65-pixel tail plans to 2x1");
    // The conformant maxComputeWorkGroupCount[0] == 65535 boundary: 65536 groups must go 2D.
    const std::uint64_t boundary = static_cast<std::uint64_t>(65535) * kWorkgroup + 1ULL;
    const auto boundaryPlan = planOcioDispatch(boundary, kWorkgroup, 65535, 65535);
    expectations.expect(boundaryPlan.valid() && boundaryPlan.groupsX == 65535 &&
                            boundaryPlan.groupsY == 2 && covers(boundaryPlan, boundary, 65535, 65535),
                        "the >4M-pixel X boundary plans a bounded 2D grid");
    // Injected tiny limits force many rows.
    const auto tiny = planOcioDispatch(256, kWorkgroup, 3, 64);
    expectations.expect(tiny.valid() && tiny.groupsX == 3 && tiny.groupsY == 2 &&
                            covers(tiny, 256, 3, 64),
                        "tiny injected limits plan 3x2 for 256 pixels");
    const auto tooFewRows = planOcioDispatch(256, kWorkgroup, 3, 1);
    expectations.expect(!tooFewRows.valid() &&
                            tooFewRows.error == OcioDispatchPlanError::DeviceCapacity,
                        "a geometry the injected limits cannot cover is refused");
    expectations.expect(!planOcioDispatch(0, kWorkgroup, 65535, 65535).valid(),
                        "zero pixels are refused");
    expectations.expect(!planOcioDispatch(1, 0, 65535, 65535).valid(),
                        "a zero workgroup size is refused");
    expectations.expect(!planOcioDispatch(1, kWorkgroup, 0, 65535).valid(),
                        "a zero max-X is refused");
    // Enormous checked math: X is capped so the uint32 stride cannot wrap, then Y exceeds maxY.
    const auto hugeY = planOcioDispatch(0xFFFFFFFFULL, kWorkgroup, 1, 65535);
    expectations.expect(!hugeY.valid() && hugeY.error == OcioDispatchPlanError::DeviceCapacity,
                        "an enormous geometry is refused rather than wrapping");
    const auto hugeIndex = planOcioDispatch(0xFFFFFFFFULL, kWorkgroup, 0xFFFFFFFFu, 0xFFFFFFFFu);
    expectations.expect(!hugeIndex.valid() &&
                            hugeIndex.error == OcioDispatchPlanError::DeviceCapacity,
                        "an unrepresentable flattened index is refused");

    // The effective limit clamps a caller cap to the physical device limit: a nonzero request may
    // only lower it, and zero selects the physical limit. An oversized request can never authorize
    // a dispatch above maxComputeWorkGroupCount.
    constexpr std::uint32_t kPhysical = 65535;
    expectations.expect(effectiveWorkGroupCount(0, kPhysical) == kPhysical,
                        "zero selects the physical limit");
    expectations.expect(effectiveWorkGroupCount(13, kPhysical) == 13,
                        "a smaller requested cap lowers the limit");
    expectations.expect(effectiveWorkGroupCount(kPhysical, kPhysical) == kPhysical,
                        "a request equal to the physical limit is unchanged");
    expectations.expect(effectiveWorkGroupCount(0xFFFFFFFFu, kPhysical) == kPhysical,
                        "an oversized requested cap is clamped to the physical limit");
    const auto clampedPlan =
        planOcioDispatch(boundary, kWorkgroup, effectiveWorkGroupCount(0xFFFFFFFFu, kPhysical),
                         effectiveWorkGroupCount(0xFFFFFFFFu, kPhysical));
    expectations.expect(clampedPlan.valid() && clampedPlan.groupsX == kPhysical &&
                            clampedPlan.groupsY == 2 &&
                            covers(clampedPlan, boundary, kPhysical, kPhysical),
                        "an oversized requested cap still plans the physical 2D boundary");
}

void testForcedDispatchPlanning(Expectations& expectations, GpuDevice& device,
                                const bloom::color::ResolvedBloomNeutralConfig& aces) {
    auto desc = bloom::color::buildOcioGpuProgramForCst(aces, "ACES2065-1", "ACEScg");
    expectations.expect(desc.succeeded(), "the forced-planning CST extracts");
    if (!desc.succeeded()) {
        return;
    }
    bloom::render::GpuOcioProgramBudgets budgets;
    budgets.maxWorkGroupCountX = 13;
    budgets.maxWorkGroupCountY = 512;
    auto program = makeProgram(device, *desc.program(), budgets);
    auto uploader = GpuImageUpload::create(device);
    expectations.expect(program != nullptr && uploader.hasValue(),
                        "the forced-planning program hosts");
    if (program == nullptr || !uploader) {
        return;
    }
    const auto cpu = acesCstCpu();
    expectations.expect(cpu != nullptr, "the forced-planning CPU oracle prepares");
    if (cpu == nullptr) {
        return;
    }
    const auto run = [&](const std::uint32_t width, const std::uint32_t height,
                         const std::string_view label) {
        const auto pixels = fixturePixels(width, height);
        const auto input = uploadImage(*uploader.upload, width, height, pixels);
        expectations.expect(input != nullptr, "a forced-planning input uploads");
        if (input == nullptr) {
            return;
        }
        const auto accepted = program->beginEffect(input, {}, kBudget);
        expectations.expect(accepted.code == bloom::render::GpuOcioProgramDiagnosticCode::None,
                            "a forced-planning begin is accepted");
        if (accepted.code != bloom::render::GpuOcioProgramDiagnosticCode::None) {
            return;
        }
        if (!pollOcio(expectations, *program, label)) {
            return;
        }
        auto output = program->takeEffectOutput();
        expectations.expect(output != nullptr, "a forced-planning output is published");
        if (output == nullptr) {
            return;
        }
        const auto readback = bloom::render::readbackResidentImage(*output, kBudget);
        expectations.expect(readback.hasValue(), "a forced-planning output reads back");
        if (!readback) {
            return;
        }
        compareEffect(
            expectations, pixels, readback.pixels,
            [&](std::array<float, 3>& rgb) { static_cast<void>(cpu->applyRGB(rgb.data())); }, label);
    };
    run(5, 7, "small forced-planning geometry");
    run(401, 4, "tail forced-planning geometry");
    run(4096, 32, ">4K forced-planning geometry");
}

// A geometry the injected limits cannot cover must be refused before any submission.
void testDispatchRefusal(Expectations& expectations, GpuDevice& device,
                         const bloom::color::ResolvedBloomNeutralConfig& aces) {
    auto desc = bloom::color::buildOcioGpuProgramForCst(aces, "ACES2065-1", "ACEScg");
    expectations.expect(desc.succeeded(), "the refusal CTS extracts");
    if (!desc.succeeded()) {
        return;
    }
    bloom::render::GpuOcioProgramBudgets budgets;
    budgets.maxWorkGroupCountX = 1;
    budgets.maxWorkGroupCountY = 1;
    auto program = makeProgram(device, *desc.program(), budgets);
    auto uploader = GpuImageUpload::create(device);
    expectations.expect(program != nullptr && uploader.hasValue(), "the refusal program hosts");
    if (program == nullptr || !uploader) {
        return;
    }
    constexpr std::uint32_t width = 65;
    constexpr std::uint32_t height = 1;
    const auto pixels = fixturePixels(width, height);
    const auto input = uploadImage(*uploader.upload, width, height, pixels);
    expectations.expect(input != nullptr, "the refusal input uploads");
    if (input == nullptr) {
        return;
    }
    const auto refused = program->beginEffect(input, {}, kBudget);
    expectations.expect(refused.code == bloom::render::GpuOcioProgramDiagnosticCode::Unsupported,
                        "an unplannable geometry is refused typed before submit");
    expectations.expect(!program->hasUnretiredSubmission(),
                        "a refused geometry leaves no unretired submission");
    expectations.expect(program->lastJobAllocationBytes() == 0,
                        "a refused geometry publishes no job bytes");
    // A geometry that fits the injected limits still completes with parity.
    constexpr std::uint32_t okWidth = 32;
    constexpr std::uint32_t okHeight = 2;
    const auto okPixels = fixturePixels(okWidth, okHeight);
    const auto okInput = uploadImage(*uploader.upload, okWidth, okHeight, okPixels);
    expectations.expect(okInput != nullptr, "the recovery input uploads");
    if (okInput == nullptr) {
        return;
    }
    const auto recovered = program->beginEffect(okInput, {}, kBudget);
    expectations.expect(recovered.code == bloom::render::GpuOcioProgramDiagnosticCode::None,
                        "a fitting geometry recovers after the refusal");
    if (recovered.code != bloom::render::GpuOcioProgramDiagnosticCode::None) {
        return;
    }
    if (!pollOcio(expectations, *program, "refusal recovery")) {
        return;
    }
    auto output = program->takeEffectOutput();
    expectations.expect(output != nullptr, "the recovered output is published");
    if (output == nullptr) {
        return;
    }
    const auto readback = bloom::render::readbackResidentImage(*output, kBudget);
    expectations.expect(readback.hasValue(), "the recovered output reads back");
    if (!readback) {
        return;
    }
    const auto cpu = acesCstCpu();
    if (cpu != nullptr) {
        compareEffect(
            expectations, okPixels, readback.pixels,
            [&](std::array<float, 3>& rgb) { static_cast<void>(cpu->applyRGB(rgb.data())); },
            "refusal recovery");
    }
}

} // namespace bloom::color::ocio_resources_test
