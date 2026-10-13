#pragma once

#include "gpu_composite_native_support.hpp"
#include "gpu_device_private.hpp"

#include <array>

namespace bloom::render::composite_proof {

inline void testSourceOverOffsets(Expectations& expectations, GpuDevice& device) {
    auto composite = GpuComposite::create(device);
    auto producer = GpuSolid::create(device);
    if (!composite || !producer) {
        expectations.expect(false, "source-over offset hosts create");
        return;
    }
    const auto control = GpuRendererAccess::state(device);
    struct Origins final {
        std::int64_t source;
        std::int64_t destination;
    };
    constexpr auto min64 = std::numeric_limits<std::int64_t>::min();
    constexpr auto max64 = std::numeric_limits<std::int64_t>::max();
    constexpr auto max32 = static_cast<std::int64_t>(std::numeric_limits<std::int32_t>::max());
    const std::array rejected{Origins{min64, max64 - 2}, Origins{max64 - 2, min64},
                              Origins{-max32 - 2, 0}, Origins{-max32, 0}};
    for (const auto origins : rejected) {
        const auto sourceWindow = window(origins.source, 0, 2, 1);
        const auto destinationWindow = window(origins.destination, 0, 2, 1);
        auto source = solid(*producer.solid, Color4d{0.5, 0.25, 0, 0.5}, sourceWindow, sourceWindow,
                            PixelAspectRatio::square());
        auto destination = solid(*producer.solid, Color4d{0.2, 0.4, 0.8, 1}, destinationWindow,
                                 destinationWindow, PixelAspectRatio::square());
        expectations.expect(source.has_value() && destination.has_value(),
                            "extreme-origin real resident inputs create");
        if (!source.has_value() || !destination.has_value())
            return;
        const auto imageAttempts = control->nativeImageBackingAttempts;
        const auto bufferAttempts = control->nativeBufferBackingAttempts;
        const auto result = composite.composite->beginSourceOver(
            {std::make_shared<const GpuImage>(std::move(*source)),
             std::make_shared<const GpuImage>(std::move(*destination))},
            kBudget);
        expectations.expect(
            result.code == GpuCompositeDiagnosticCode::Unsupported &&
                !composite.composite->hasUnretiredSubmission() &&
                !composite.composite->takeImage().isValid() &&
                control->nativeImageBackingAttempts == imageAttempts &&
                control->nativeBufferBackingAttempts == bufferAttempts,
            "unsafe origin subtraction/narrowing/local addition refuses before backing or submit");
    }
    // Keep subtraction exact near int64's limit: converting the origins to floating point first
    // would erase this one-pixel offset. A valid request still succeeds after every refusal.
    const auto sourceWindow = window(max64 - 5, 0, 2, 1);
    const auto destinationWindow = window(max64 - 4, 0, 2, 1);
    auto source = solid(*producer.solid, Color4d{0.5, 0.25, 0, 0.5}, sourceWindow, sourceWindow,
                        PixelAspectRatio::square());
    auto destination = solid(*producer.solid, Color4d{0.2, 0.4, 0.8, 1}, destinationWindow,
                             destinationWindow, PixelAspectRatio::square());
    if (!source.has_value() || !destination.has_value()) {
        expectations.expect(false, "near-limit resident inputs create");
        return;
    }
    const auto result = composite.composite->beginSourceOver(
        {std::make_shared<const GpuImage>(std::move(*source)),
         std::make_shared<const GpuImage>(std::move(*destination))},
        kBudget);
    expectations.expect(result.code == GpuCompositeDiagnosticCode::None,
                        "exact one-pixel near-limit origin difference begins");
    if (result.code != GpuCompositeDiagnosticCode::None)
        return;
    auto status = composite.composite->poll();
    while (status == GpuCompositePollResult::Pending)
        status = composite.composite->poll();
    expectations.expect(status == GpuCompositePollResult::Ready,
                        "near-limit source-over completes after refusals");
    if (status != GpuCompositePollResult::Ready)
        return;
    const auto read = readbackResidentImage(composite.composite->takeImage(), kBudget);
    const std::array alignedSource{pixel(0.25F, 0.125F, 0, 0.5F), Rgba32f::transparent()};
    std::array expected{pixel(0.2F, 0.4F, 0.8F, 1), pixel(0.2F, 0.4F, 0.8F, 1)};
    expectations.expect(!sourceOverLinearRec709SceneRow(alignedSource, expected).has_value(),
                        "near-limit aligned CPU reference succeeds");
    expectations.expect(read.hasValue(), "near-limit source-over real pixels read back");
    if (read.hasValue())
        static_cast<void>(pixelsMatch(read.pixels, expected, expectations,
                                      "exact near-limit source-over CPU reference"));
}

} // namespace bloom::render::composite_proof
