#include "discrete_image_effect.hpp"
#include "image_effect.hpp"

#include <OpenColorIO/OpenColorIO.h>
#include <bloom/color/bloom_neutral_builtin.hpp>
#include <bloom/runtime/cpu_composition_evaluator.hpp>
#include <bloom/runtime/task_scheduler.hpp>
#include <bloom/render/image_coverage.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <future>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>

namespace {
using namespace bloom;
namespace OCIO = OCIO_NAMESPACE;
int failures = 0;
void expect(const bool value, const char* message) {
    if (!value) {
        std::cerr << message << '\n';
        ++failures;
    }
}

std::shared_ptr<const runtime::CompiledCompositionPlan>
planWith(const std::vector<runtime::ImageEffectKernel>& kernels,
         const core::Color4d color = {-0.125, 0.18, 2.0, 0.5}) {
    using namespace runtime;
    using namespace document;
    const auto op = [](const std::size_t i) { return OperationIndex::fromRaw(i); };
    std::vector<CompiledOperation> operations;
    operations.emplace_back(CompiledSolid{NodeId::fromRaw(1),
                                          {ParameterId::fromRaw(1), color},
                                          {ParameterId::fromRaw(2), 4.0},
                                          {ParameterId::fromRaw(3), 2.0}});
    for (const auto& kernel : kernels) {
        const auto index = operations.size();
        operations.emplace_back(
            CompiledImageEffect{NodeId::fromRaw(10 + index), op(index - 1), kernel, false});
    }
    const auto input = op(operations.size() - 1);
    operations.emplace_back(CompiledLayerOutput{NodeId::fromRaw(100),
                                                LayerId::fromRaw(1),
                                                input,
                                                {ParameterId::fromRaw(10), Vec2d{2, 1}},
                                                {ParameterId::fromRaw(11), Vec2d{}},
                                                {ParameterId::fromRaw(12), Vec2d{1, 1}},
                                                {ParameterId::fromRaw(13), 0.0},
                                                {ParameterId::fromRaw(14), 1.0},
                                                ParameterId::fromRaw(15)});
    const auto layer = op(operations.size() - 1);
    operations.emplace_back(CompiledMerge{NodeId::fromRaw(101),
                                          {{LayerSlotId::fromRaw(1), LayerId::fromRaw(1), layer}}});
    operations.emplace_back(
        CompiledCompositionOutput{NodeId::fromRaw(102), op(operations.size() - 1)});
    const auto output = op(operations.size() - 1);
    const auto format = CompositionFormat::create(4, 2);
    if (!format)
        throw std::logic_error("Invalid fixture format");
    return std::make_shared<const CompiledCompositionPlan>(CompiledCompositionPlanDefinition{
        Revision::fromRaw(1), ProjectId::fromRaw(1), CompositionId::fromRaw(1), *format,
        std::move(operations), output});
}

runtime::EvaluationRequest requestFor(const runtime::CompiledCompositionPlan& plan) {
    runtime::EvaluationRequest request{{}, plan.output(), runtime::CompositionFormatResolution{}};
    request.pixelStorageByteLimit = 1U << 20U;
    const auto revision = color::ocioBuiltInContentRevision(
        color::OcioConfigLocatorKind::BloomBuiltIn, color::kAcesCgV1ConfigUri);
    if (!revision)
        throw std::logic_error("ACES config is unavailable");
    request.colorIntent = {"ACEScg", *revision, color::kAcesCgV1ConfigUri};
    return request;
}

bool samePixels(const runtime::EvaluationResult& a, const runtime::EvaluationResult& b) {
    return a.frame() && b.frame() &&
           std::ranges::equal(a.frame()->processImage().pixels(),
                              b.frame()->processImage().pixels());
}

void testImageCombineWindows() {
    const auto make = [](const std::int64_t x, const std::int64_t y,
                         const std::uint32_t width, const std::uint32_t height,
                         const std::vector<render::Rgba32f>& pixels) {
        const auto window = render::ImageWindow::create(x, y, width, height);
        if (!window)
            throw std::logic_error("invalid image combine window");
        const auto descriptor = render::Rgba32fImageDescriptor::create(
            *window.value(), *window.value(), core::PixelAspectRatio::square());
        if (!descriptor)
            throw std::logic_error("invalid image combine descriptor");
        auto builder = render::Rgba32fImageBuilder::create(*descriptor.value(), 1U << 20U);
        if (!builder)
            throw std::logic_error("image combine builder refused fixture");
        for (std::uint32_t row = 0; row < height; ++row) {
            auto span = builder.value()->row(y + row);
            if (!span)
                throw std::logic_error("image combine fixture row missing");
            for (std::uint32_t column = 0; column < width; ++column)
                (*span.value())[column] = pixels[static_cast<std::size_t>(row) * width + column];
        }
        auto frozen = std::move(*builder.value()).freeze();
        if (!frozen)
            throw std::logic_error("image combine fixture did not freeze");
        return std::move(*frozen.value());
    };
    const auto rgba = [](const float r, const float g, const float b, const float a) {
        const auto pixel = render::Rgba32f::fromPremultiplied(r, g, b, a);
        if (!pixel)
            throw std::logic_error("invalid image combine pixel");
        return *pixel.value();
    };
    const std::vector basePixels{rgba(-0.5F, 1.0F, 2.0F, 0.5F),
                                 rgba(0.0F, 0.0F, 0.0F, 0.0F),
                                 rgba(0.25F, 0.5F, 0.75F, 1.0F)};
    const std::vector mattePixels{rgba(0.25F, -0.25F, 1.0F, 0.5F)};
    const auto base = make(-2, 3, 3, 1, basePixels);
    const auto matte = make(-1, 3, 1, 1, mattePixels);
    runtime::detail::PreparedImageEffect mix;
    mix.combine = {runtime::ImageCombineMode::Mix, 0.25F,
                   render::ImageCoverageChannel::Alpha};
    const auto operation = runtime::OperationIndex::fromRaw(1);
    const auto mixed = runtime::detail::applyImageEffect(mix, base, &matte, 1U << 20U, {},
                                                          operation, {});
    expect(mixed.image && !mixed.diagnostic, "offset Image Mix CPU evaluation succeeds");
    if (mixed.image) {
        expect(mixed.image->descriptor()->dataWindow() == base.descriptor()->dataWindow(),
               "Image Mix output follows the base image window");
        const auto expected = rgba(-0.375F, 0.75F, 1.5F, 0.375F);
        expect(mixed.image->pixels()[0] == expected,
               "Image Mix treats out-of-window second pixels as transparent");
        expect(mixed.image->pixels()[1] == rgba(0.0625F, -0.0625F, 0.25F, 0.125F),
               "Image Mix uses composition-space offset for the second image");
    }
    runtime::detail::PreparedImageEffect set;
    set.combine = {runtime::ImageCombineMode::SetAlpha, 1.0F,
                   render::ImageCoverageChannel::Luma};
    const auto replaced = runtime::detail::applyImageEffect(set, base, &matte, 1U << 20U, {},
                                                             operation, {});
    expect(replaced.image && !replaced.diagnostic, "Set Alpha CPU evaluation succeeds");
    if (replaced.image) {
        expect(replaced.image->pixels()[0] == render::Rgba32f::transparent(),
               "Set Alpha clears base pixels outside matte window");
        expect(replaced.image->pixels()[1] == render::Rgba32f::transparent(),
               "Set Alpha keeps zero-straight-RGB base black");
        expect(replaced.image->pixels()[2] == render::Rgba32f::transparent(),
               "Set Alpha clears base pixels after matte extent");
    }
    const auto budgetRefusal = runtime::detail::applyImageEffect(mix, base, &matte, 1, {},
                                                                 operation, {});
    expect(!budgetRefusal.image && budgetRefusal.diagnostic,
           "Image Mix refuses insufficient pixel budget");
}

void testDiscreteImages() {
    using namespace runtime;
    using namespace render;
    const auto sourceWindow = ImageWindow::create(-11, 17, 3, 2);
    const auto display = ImageWindow::create(0, 0, 4, 4);
    const auto descriptor = Rgba32fImageDescriptor::create(*sourceWindow.value(), *display.value(),
                                                           core::PixelAspectRatio::square());
    auto builder = Rgba32fImageBuilder::create(*descriptor.value(), 4096);
    for (std::int64_t y = 17; y < 19; ++y) {
        auto row = builder.value()->row(y);
        for (std::size_t x = 0; x < 3; ++x) {
            const float value = static_cast<float>((y - 17) * 3 + static_cast<std::int64_t>(x));
            const auto pixel = Rgba32f::fromPremultiplied(-value, value * 2.0F, value * 4.0F,
                                                          value == 0 ? 0.0F : 0.5F);
            (*row.value())[x] = *pixel.value();
        }
    }
    const auto frozen = std::move(*builder.value()).freeze();
    const auto& image = *frozen.value();
    // Independent expected source indices: clockwise quarter turns of [0 1 2; 3 4 5].
    const std::array<std::array<std::size_t, 6>, 4> expected{
        {{{0, 1, 2, 3, 4, 5}}, {{3, 0, 4, 1, 5, 2}}, {{5, 4, 3, 2, 1, 0}}, {{2, 5, 1, 4, 0, 3}}}};
    for (std::uint8_t turn = 0; turn < 4; ++turn) {
        const DiscreteImageKernel kernel{false, false, turn};
        const auto output = runtime::detail::applyDiscreteImageEffect(
            kernel, image, 4096, {}, OperationIndex::fromRaw(1), {});
        expect(output.image && !output.diagnostic, "quarter-turn CPU gather succeeds");
        if (!output.image)
            continue;
        const auto window = output.image->descriptor()->dataWindow();
        expect(window.originX() == -11 && window.originY() == 17 &&
                   window.extent().width() == ((turn & 1U) ? 2U : 3U) &&
                   window.extent().height() == ((turn & 1U) ? 3U : 2U) &&
                   output.image->descriptor()->displayWindow() == *display.value(),
               "quarter turns preserve off-canvas origin/display and exchange odd extents");
        const auto samples = prepareAffineMatrixSamples(
            runtime::detail::discreteAffineMatrix(kernel, *sourceWindow.value()),
            *sourceWindow.value(), window);
        for (std::size_t i = 0; i < 6; ++i) {
            expect(output.image->pixels()[i] == image.pixels()[expected[turn][i]],
                   "quarter turns preserve exact signed/HDR RGBA samples");
            expect(samples[i].factorX == 0.0F && samples[i].factorY == 0.0F &&
                       samples[i].baseX == static_cast<std::int32_t>(expected[turn][i] % 3) &&
                       samples[i].baseY == static_cast<std::int32_t>(expected[turn][i] / 3),
                   "resident affine sampler selects the exact tap without interpolation");
        }
    }
    for (const auto kernel :
         {DiscreteImageKernel{true, false, 0}, DiscreteImageKernel{false, true, 0},
          DiscreteImageKernel{true, true, 0}}) {
        const auto output = runtime::detail::applyDiscreteImageEffect(
            kernel, image, 4096, {}, OperationIndex::fromRaw(1), {});
        expect(output.image != nullptr, "flip CPU gather succeeds");
        const auto samples = prepareAffineMatrixSamples(
            runtime::detail::discreteAffineMatrix(kernel, *sourceWindow.value()),
            *sourceWindow.value(), *sourceWindow.value());
        for (std::size_t i = 0; i < 6 && output.image; ++i) {
            const auto x = kernel.horizontal ? 2 - i % 3 : i % 3;
            const auto y = kernel.vertical ? 1 - i / 3 : i / 3;
            expect(output.image->pixels()[i] == image.pixels()[y * 3 + x] &&
                       samples[i].baseX == static_cast<std::int32_t>(x) &&
                       samples[i].baseY == static_cast<std::int32_t>(y) &&
                       samples[i].factorX == 0 && samples[i].factorY == 0,
                   "flip CPU/reference GPU sampling permute the same exact taps");
        }
    }
    const auto refused = runtime::detail::applyDiscreteImageEffect({true, false, 0}, image, 1, {},
                                                                   OperationIndex::fromRaw(1), {});
    expect(!refused.image && refused.diagnostic &&
               refused.diagnostic->code == EvaluationDiagnosticCode::PixelStorageBudgetExceeded,
           "discrete image enforces allocation budget");
    const auto baseline = planWith({DiscreteImageKernel{false, false, 1}});
    CpuCompositionEvaluator evaluator;
    const auto cold = evaluator.evaluate(baseline, requestFor(*baseline), {});
    const auto warm = evaluator.evaluate(baseline, requestFor(*baseline), {});
    expect(samePixels(cold, warm) && cold.diagnostics().empty() && warm.diagnostics().empty(),
           "discrete effect production evaluation and warm cache preserve pixels");
    expect(!baseline->operationTimeDependent(OperationIndex::fromRaw(1)),
           "static discrete controls retain static source time dependence");
    auto timeDefinition = baseline->copyDefinition();
    timeDefinition.operations[0] = CompiledVideoSource{};
    const auto timePlan =
        std::make_shared<const CompiledCompositionPlan>(std::move(timeDefinition));
    expect(timePlan->operationTimeDependent(OperationIndex::fromRaw(1)),
           "discrete effects propagate time-varying input dependence");
    auto directDefinition = baseline->copyDefinition();
    directDefinition.operations.resize(2);
    directDefinition.operations.emplace_back(CompiledMerge{
        document::NodeId::fromRaw(500),
        {{document::LayerSlotId::fromRaw(500), document::LayerId{}, OperationIndex::fromRaw(1)}}});
    directDefinition.operations.emplace_back(
        CompiledCompositionOutput{document::NodeId::fromRaw(501), OperationIndex::fromRaw(2)});
    directDefinition.output = OperationIndex::fromRaw(3);
    const auto direct =
        std::make_shared<const CompiledCompositionPlan>(std::move(directDefinition));
    auto roiRequest = requestFor(*direct);
    const auto roi = ImageWindow::create(1, 0, 1, 2);
    roiRequest.roi = *roi.value();
    const auto roiFrame = evaluator.evaluate(direct, roiRequest, {});
    const auto fullFrame = evaluator.evaluate(direct, requestFor(*direct), {});
    expect(roiFrame.frame() && fullFrame.frame(), "direct discrete output accepts an ROI");
    if (roiFrame.frame() && fullFrame.frame()) {
        const auto fullView = fullFrame.frame()->processImage().view();
        for (std::size_t y = 0; y < 2; ++y) {
            const auto fullRow = fullView.value()->row(static_cast<std::int64_t>(y));
            expect(roiFrame.frame()->processImage().pixels()[y] == (*fullRow.value())[1],
                   "ROI crops rotated output after preserving complete source support");
        }
    }
}

void testUnaryImageEffectInputArity() {
    using namespace runtime;
    for (const ImageEffectKernel& kernel :
         {ImageEffectKernel{DiscreteImageKernel{true, false, 0}},
          ImageEffectKernel{DiscreteImageKernel{false, false, 1}},
          ImageEffectKernel{ResizeFitKernel{4, 2}}}) {
        for (const auto secondary : {std::size_t{0}, std::numeric_limits<std::size_t>::max()}) {
            auto definition = planWith({kernel})->copyDefinition();
            std::get<CompiledImageEffect>(definition.operations[1]).secondaryInput =
                OperationIndex::fromRaw(secondary);
            const auto plan =
                std::make_shared<const CompiledCompositionPlan>(std::move(definition));
            CpuCompositionEvaluator evaluator;
            const auto result = evaluator.evaluate(plan, requestFor(*plan), {});
            expect(!result.frame() &&
                       std::ranges::any_of(result.diagnostics(), [](const auto& diagnostic) {
                           return diagnostic.code == EvaluationDiagnosticCode::InvalidPlan;
                       }),
                   "Flip, Rotate90 and Resize reject unexpected secondary inputs before evaluation");
        }
    }
}

void testExposureAndOffset() {
    runtime::CpuCompositionEvaluator evaluator;
    const auto baseline = planWith({});
    const auto baselineResult = evaluator.evaluate(baseline, requestFor(*baseline), {});
    const auto amount = [](const runtime::PointwiseImageEffect kind, const double value) {
        return runtime::PointwiseImageKernel{
            kind, runtime::CompiledScalarParameter{document::ParameterId::fromRaw(90), value}};
    };
    for (const auto kind :
         {runtime::PointwiseImageEffect::Exposure, runtime::PointwiseImageEffect::Offset}) {
        const auto neutral = planWith({amount(kind, 0.0)});
        const auto result = evaluator.evaluate(neutral, requestFor(*neutral), {});
        expect(samePixels(result, baselineResult), "neutral pointwise effects preserve pixels");
    }
    const auto check = [&](const runtime::PointwiseImageEffect kind, const double value,
                           const std::array<float, 3> expected) {
        const auto plan = planWith({amount(kind, value)});
        const auto cold = evaluator.evaluate(plan, requestFor(*plan), {});
        const auto warm = evaluator.evaluate(plan, requestFor(*plan), {});
        expect(cold.frame() && cold.diagnostics().empty(),
               "pointwise effect evaluates without diagnostic");
        expect(samePixels(cold, warm), "pointwise warm memo preserves exact pixels");
        if (!cold.frame())
            return;
        bool foundContent = false;
        for (const auto pixel : cold.frame()->processImage().pixels()) {
            if (pixel.alpha() == 0.0F) {
                expect(pixel.red() == 0.0F && pixel.green() == 0.0F && pixel.blue() == 0.0F,
                       "transparent pointwise output remains black");
                continue;
            }
            foundContent = true;
            expect(pixel.alpha() == 0.5F && std::abs(pixel.red() - expected[0]) < 1e-6F &&
                       std::abs(pixel.green() - expected[1]) < 1e-6F &&
                       std::abs(pixel.blue() - expected[2]) < 1e-6F,
                   "pointwise effect preserves signed HDR straight-RGB and alpha semantics");
        }
        expect(foundContent, "pointwise fixture has visible content");
    };
    check(runtime::PointwiseImageEffect::Exposure, 1.0, {-0.125F, 0.18F, 2.0F});
    check(runtime::PointwiseImageEffect::Offset, -0.25, {-0.1875F, -0.035F, 0.875F});
}

void testCanvasEffects() {
    const auto window = [](const std::int64_t x, const std::int64_t y,
                           const std::uint64_t width, const std::uint64_t height) {
        const auto created = render::ImageWindow::create(x, y, width, height);
        return *created.value();
    };
    const auto data = render::ImageWindow::create(-3, 2, 4, 3);
    const auto display = render::ImageWindow::create(-5, 0, 10, 8);
    expect(data && display, "canvas fixture windows are valid");
    if (!data || !display)
        return;
    const auto aspect = core::PixelAspectRatio::create(4, 3);
    const auto descriptor = render::Rgba32fImageDescriptor::create(*data.value(), *display.value(),
                                                                    *aspect);
    expect(static_cast<bool>(descriptor), "canvas fixture descriptor is valid");
    if (!descriptor)
        return;
    auto builder = render::Rgba32fImageBuilder::create(*descriptor.value(), 1U << 20U);
    expect(static_cast<bool>(builder), "canvas fixture storage is available");
    if (!builder)
        return;
    const auto pixel = render::Rgba32f::fromPremultiplied(-0.25F, 0.75F, 2.5F, 0.5F);
    expect(static_cast<bool>(pixel), "signed HDR premultiplied pixel is valid");
    if (!pixel)
        return;
    for (auto y = data.value()->originY(); y < data.value()->maxYExclusive(); ++y) {
        auto row = builder.value()->row(y);
        std::fill(row.value()->begin(), row.value()->end(), *pixel.value());
    }
    auto source = std::move(*builder.value()).freeze();
    expect(static_cast<bool>(source), "canvas fixture freezes");
    if (!source)
        return;
    const auto run = [&](const runtime::CanvasImageEffect kind,
                         const std::array<double, 4> sides, const double scaleX = 1.0,
                         const double scaleY = 1.0) {
        runtime::detail::PreparedImageEffect effect;
        effect.canvas = runtime::detail::CanvasMargins{kind, sides};
        return runtime::detail::applyImageEffect(effect, *source.value(), nullptr, 1U << 20U, {},
                                                 runtime::OperationIndex::fromRaw(0), {},
                                                 scaleX, scaleY);
    };
    runtime::TaskScheduler scheduler;
    const runtime::TaskOwner owner{runtime::TaskOwnerKind::PanelRequest,
                                    runtime::TaskOwnerId::fromRaw(9090)};
    std::promise<runtime::CancellationToken> promised;
    auto future = promised.get_future();
    const auto task = scheduler.submit<void>(
        runtime::TaskRequest("Canvas cancellation", owner),
        [&](runtime::TaskContext& context) {
            static_cast<void>(scheduler.cancelOwner(owner));
            promised.set_value(context.cancellation());
            return runtime::TaskResult<void>::cancelled();
        });
    expect(task.accepted(), "canvas cancellation task is admitted");
    if (task.accepted()) {
        const auto token = future.get();
        for (const auto kind : {runtime::CanvasImageEffect::Crop, runtime::CanvasImageEffect::Pad}) {
            runtime::detail::PreparedImageEffect effect;
            effect.canvas = runtime::detail::CanvasMargins{kind, {1, 0, 0, 0}};
            const auto cancelled = runtime::detail::applyImageEffect(
                effect, *source.value(), nullptr, 0, token,
                runtime::OperationIndex::fromRaw(0), {});
            expect(cancelled.cancelled && !cancelled.diagnostic && !cancelled.image,
                   "cancelled canvas work stops before output allocation or budget refusal");
        }
    }
    const auto cropped = run(runtime::CanvasImageEffect::Crop, {1, 1, 1, 0});
    expect(cropped.image && cropped.image->descriptor()->dataWindow() ==
                                window(-2, 3, 2, 2),
           "crop moves a negative-origin data window and removes exact edge pixels");
    if (cropped.image) {
        expect(cropped.image->descriptor()->displayWindow() ==
                   window(-4, 1, 8, 7) &&
                   cropped.image->descriptor()->pixelAspect() == *aspect,
               "crop narrows display window and preserves pixel aspect");
        const auto sample = cropped.image->read(-2, 3);
        expect(sample && *sample.value() == *pixel.value(),
               "crop preserves signed HDR and partial alpha exactly");
    }
    const auto padded = run(runtime::CanvasImageEffect::Pad, {2, 1, 0, 2});
    expect(padded.image && padded.image->descriptor()->dataWindow() ==
                               window(-5, 1, 6, 6),
           "pad grows the data window without shifting source pixels");
    if (padded.image) {
        const auto transparent = padded.image->read(-5, 1);
        const auto retained = padded.image->read(-3, 2);
        expect(transparent && *transparent.value() == render::Rgba32f::transparent() &&
                   retained && *retained.value() == *pixel.value(),
               "pad writes transparent border and preserves interior pixels");
        expect(padded.image->descriptor()->displayWindow() ==
                   window(-7, -1, 12, 11) &&
                   padded.image->descriptor()->pixelAspect() == *aspect,
               "pad expands display window and preserves pixel aspect");
    }
    const auto proxy = run(runtime::CanvasImageEffect::Crop, {2, 2, 0, 0}, 0.5, 0.5);
    expect(proxy.image && proxy.image->descriptor()->dataWindow() ==
                              window(-2, 3, 3, 2),
           "proxy crop rounds authored margins once on each axis");
    for (const auto kind : {runtime::CanvasImageEffect::Crop, runtime::CanvasImageEffect::Pad}) {
        const runtime::detail::CanvasMargins invalid{kind, {16777217.0, 0, 0, 0}};
        expect(!runtime::detail::canvasOutputWindow(*data.value(), invalid, 1.0, 1.0) &&
                   !runtime::detail::canvasOutputWindow(*data.value(), invalid, 0.5, 0.5),
               "authored canvas margin domain is independent of proxy resolution");
    }
    const auto empty = run(runtime::CanvasImageEffect::Crop, {4, 0, 0, 0});
    expect(!empty.image && empty.diagnostic,
           "crop that removes its entire data window refuses with a diagnostic");
    runtime::detail::PreparedImageEffect limited;
    limited.canvas = runtime::detail::CanvasMargins{runtime::CanvasImageEffect::Pad, {2, 1, 0, 2}};
    const auto refused = runtime::detail::applyImageEffect(
        limited, *source.value(), nullptr, sizeof(render::Rgba32f), {},
        runtime::OperationIndex::fromRaw(0), {});
    expect(!refused.image && refused.diagnostic,
           "pad obeys the output pixel storage budget");
}

void testCanvasAnimation() {
    using namespace runtime;
    using namespace document;
    for (const auto kind : {CanvasImageEffect::Crop, CanvasImageEffect::Pad}) {
        CanvasImageKernel kernel{kind, {}};
        for (std::size_t side = 0; side < kernel.margins.size(); ++side)
            kernel.margins[side] = {ParameterId::fromRaw(90 + side), 0.0};
        kernel.margins[0].source = ScalarCurveIndex::fromRaw(0);
        auto definition = planWith({kernel})->copyDefinition();
        definition.duration = core::RationalTime::fromInteger(2);
        definition.scalarCurves.push_back(
            CompiledScalarCurve{AnimationCurveId::fromRaw(90),
                                {{KeyframeId::fromRaw(90), core::RationalTime::fromInteger(0), 0.0},
                                 {KeyframeId::fromRaw(91), core::RationalTime::fromInteger(1), 1.0}}});
        const auto plan = std::make_shared<const CompiledCompositionPlan>(std::move(definition));
        CpuCompositionEvaluator evaluator;
        auto request = requestFor(*plan);
        const auto first = evaluator.evaluate(plan, request, {});
        request.time = core::RationalTime::fromInteger(1);
        const auto next = evaluator.evaluate(plan, request, {});
        expect(first.frame() && next.frame(), "animated canvas margins pass evaluator preflight");
        if (kind == CanvasImageEffect::Crop && first.frame() && next.frame())
            expect(!samePixels(first, next),
                   "animated crop margins change the cached composition pixels");
    }
}

void testBlurEvaluation() {
    runtime::CpuCompositionEvaluator evaluator;
    const auto baseline = planWith({});
    const auto original = evaluator.evaluate(baseline, requestFor(*baseline), {});
    for (const auto kind :
         {runtime::SeparableBlurKind::Gaussian, runtime::SeparableBlurKind::Box}) {
        const auto blur = [&](const double radius, const runtime::BlurEdgeMode edge) {
            return runtime::SeparableBlurKernel{
                kind, {document::ParameterId::fromRaw(90), radius}, edge};
        };
        const auto zero = planWith({blur(0, runtime::BlurEdgeMode::Transparent)});
        expect(samePixels(original, evaluator.evaluate(zero, requestFor(*zero), {})),
               "zero blur preserves exact evaluator pixels");
        const auto plan = planWith({blur(2, runtime::BlurEdgeMode::Transparent)});
        const auto cold = evaluator.evaluate(plan, requestFor(*plan), {});
        const auto warm = evaluator.evaluate(plan, requestFor(*plan), {});
        expect(cold.frame() && cold.diagnostics().empty() && samePixels(cold, warm) &&
                   warm.frame()->operationCacheStatistics().misses == 0,
               "blur warm memo reuses identical process pixels");
        auto roiRequest = requestFor(*plan);
        const auto roiWindow = render::ImageWindow::create(1, 0, 2, 2);
        roiRequest.roi = *roiWindow.value();
        const auto roi = evaluator.evaluate(plan, roiRequest, {});
        expect(roi.frame() != nullptr, "blur ROI evaluates with full upstream halo support");
        if (roi.frame() && cold.frame()) {
            const auto& image = roi.frame()->processImage();
            for (std::int64_t y = 0; y < 2; ++y)
                for (std::int64_t x = 1; x < 3; ++x) {
                    const auto actual = image.read(x, y);
                    const auto expected = cold.frame()->processImage().read(x, y);
                    expect(actual && expected && *actual.value() == *expected.value(),
                           "ROI blur pixels equal corresponding final full-frame pixels");
                }
        }
        auto animatedDefinition = plan->copyDefinition();
        std::get<runtime::SeparableBlurKernel>(
            std::get<runtime::CompiledImageEffect>(animatedDefinition.operations[1]).kernel)
            .radius.source = runtime::ScalarCurveIndex::fromRaw(0);
        animatedDefinition.scalarCurves.push_back(
            {document::AnimationCurveId::fromRaw(190),
             {{document::KeyframeId::fromRaw(190), {}, 0.0},
              {document::KeyframeId::fromRaw(191), *core::RationalTime::create(1, 1), 2.0}}});
        const auto animated =
            std::make_shared<const runtime::CompiledCompositionPlan>(std::move(animatedDefinition));
        expect(animated->operationTimeDependent(runtime::OperationIndex::fromRaw(1)),
               "animated blur marks downstream render time-dependent");
        auto start = requestFor(*animated);
        auto end = start;
        end.time = *core::RationalTime::create(1, 1);
        const auto startResult = evaluator.evaluate(animated, start, {});
        const auto endResult = evaluator.evaluate(animated, end, {});
        expect(startResult.frame() && endResult.frame() && !samePixels(startResult, endResult),
               "animated radius cannot reuse an earlier frame");
        const auto changed = planWith({blur(1, runtime::BlurEdgeMode::Clamp)});
        const auto changedResult = evaluator.evaluate(changed, requestFor(*changed), {});
        runtime::CpuCompositionEvaluator fresh;
        expect(samePixels(changedResult, fresh.evaluate(changed, requestFor(*changed), {})),
               "radius/edge changes invalidate the memoized image");
    }
}

void testSpatialEffectBudgets() {
    const auto check = [](const runtime::ImageEffectKernel& kernel, const std::size_t peak) {
        auto definition = planWith({kernel})->copyDefinition();
        definition.operations.resize(2);
        definition.operations.emplace_back(runtime::CompiledMerge{
            document::NodeId::fromRaw(101),
            {{document::LayerSlotId::fromRaw(1), {}, runtime::OperationIndex::fromRaw(1)}}});
        definition.operations.emplace_back(runtime::CompiledCompositionOutput{
            document::NodeId::fromRaw(102), runtime::OperationIndex::fromRaw(2)});
        definition.output = runtime::OperationIndex::fromRaw(3);
        const auto plan = std::make_shared<const runtime::CompiledCompositionPlan>(
            std::move(definition));
        auto request = requestFor(*plan);
        request.bypassOperationCache = true;
        request.pixelStorageByteLimit = peak;
        runtime::CpuCompositionEvaluator evaluator;
        const auto accepted = evaluator.evaluate(plan, request, {});
        expect(accepted.frame() != nullptr,
               "spatial effect admits its exact live input plus output/intermediate budget");
        --request.pixelStorageByteLimit;
        const auto refused = evaluator.evaluate(plan, request, {});
        expect(!refused.frame() &&
                   std::ranges::any_of(refused.diagnostics(), [](const auto& diagnostic) {
                       return diagnostic.code ==
                              runtime::EvaluationDiagnosticCode::PixelStorageBudgetExceeded;
                   }),
               "spatial effect refuses one byte below its actual allocation peak");
    };
    // 4x2 RGBA32F source: 128 bytes. Fit needs one equal-sized destination.
    check(runtime::ResizeFitKernel{4, 2, 1, 1, 0, 1}, 256);
    for (const auto kind :
         {runtime::SeparableBlurKind::Gaussian, runtime::SeparableBlurKind::Box}) {
        // Clamp: two 128-byte buffers. Transparent: two 6x4x16-byte halo buffers.
        check(runtime::SeparableBlurKernel{
                  kind, {document::ParameterId::fromRaw(90), 1.0}, runtime::BlurEdgeMode::Clamp},
              384);
        check(runtime::SeparableBlurKernel{kind, {document::ParameterId::fromRaw(90), 1.0},
                                           runtime::BlurEdgeMode::Transparent},
              896);
    }
}

void testAliasedBlurBudgets() {
    for (const auto kind :
         {runtime::SeparableBlurKind::Gaussian, runtime::SeparableBlurKind::Box}) {
        auto definition = planWith({runtime::SeparableBlurKernel{
            kind, {document::ParameterId::fromRaw(90), 0.0}, runtime::BlurEdgeMode::Clamp}})
                              ->copyDefinition();
        definition.format = *document::CompositionFormat::create(1, 1);
        definition.operations.resize(2);
        definition.operations.emplace_back(runtime::CompiledMerge{
            document::NodeId::fromRaw(101),
            {{document::LayerSlotId::fromRaw(1), {}, runtime::OperationIndex::fromRaw(1)}}});
        definition.operations.emplace_back(runtime::CompiledCompositionOutput{
            document::NodeId::fromRaw(102), runtime::OperationIndex::fromRaw(2)});
        definition.output = runtime::OperationIndex::fromRaw(3);
        const auto plan = std::make_shared<const runtime::CompiledCompositionPlan>(definition);
        auto request = requestFor(*plan);
        // Blur retains its upstream support; ROI clips the merge to one pixel.
        // The 4x2 source/identity allocation plus the 1x1 merged output is 128+16 bytes.
        const auto roiWindow = render::ImageWindow::create(0, 0, 1, 1);
        request.roi = *roiWindow.value();
        request.pixelStorageByteLimit = 144;
        runtime::CpuCompositionEvaluator evaluator;
        const auto cold = evaluator.evaluate(plan, request, {});
        const auto warm = evaluator.evaluate(plan, request, {});
        if (!cold.frame() || !warm.frame()) {
            for (const auto& diagnostic : cold.diagnostics())
                std::cerr << "cold alias budget: " << diagnostic.summary << ' '
                          << diagnostic.detail << '\n';
            for (const auto& diagnostic : warm.diagnostics())
                std::cerr << "warm alias budget: " << diagnostic.summary << ' '
                          << diagnostic.detail << '\n';
        }
        expect(cold.frame() && samePixels(cold, warm) &&
                   warm.frame()->operationCacheStatistics().hits > 0,
               "cached zero-radius blur reuses resident storage without charging it again");
        request.bypassOperationCache = true;
        request.pixelStorageByteLimit = 143;
        expect(!evaluator.evaluate(plan, request, {}).frame(),
               "alias reuse does not waive the merged output allocation budget");

        // Retain the original source alongside the zero-radius alias while a second blur runs.
        definition.operations.resize(2);
        definition.operations.emplace_back(runtime::CompiledImageEffect{
            document::NodeId::fromRaw(12), runtime::OperationIndex::fromRaw(1),
            runtime::SeparableBlurKernel{kind, {document::ParameterId::fromRaw(91), 1.0},
                                         runtime::BlurEdgeMode::Clamp}, false});
        definition.operations.emplace_back(runtime::CompiledMerge{
            document::NodeId::fromRaw(101),
            {{document::LayerSlotId::fromRaw(1), {}, runtime::OperationIndex::fromRaw(0)},
             {document::LayerSlotId::fromRaw(2), {}, runtime::OperationIndex::fromRaw(2)}}});
        definition.operations.emplace_back(runtime::CompiledCompositionOutput{
            document::NodeId::fromRaw(102), runtime::OperationIndex::fromRaw(3)});
        definition.output = runtime::OperationIndex::fromRaw(4);
        const auto branched = std::make_shared<const runtime::CompiledCompositionPlan>(
            std::move(definition));
        auto branchedRequest = requestFor(*branched);
        branchedRequest.bypassOperationCache = true;
        branchedRequest.pixelStorageByteLimit = 384;
        expect(evaluator.evaluate(branched, branchedRequest, {}).frame() != nullptr,
               "branched blur charges its shared source once plus both scratch buffers");
        branchedRequest.pixelStorageByteLimit = 383;
        expect(!evaluator.evaluate(branched, branchedRequest, {}).frame(),
               "branched blur still refuses below its true three-buffer peak");
    }
}

void testCst() {
    runtime::CpuCompositionEvaluator evaluator;
    const auto source = planWith({});
    const auto baseline = evaluator.evaluate(source, requestFor(*source), {});
    const auto identity = planWith({runtime::CstKernel{}});
    const auto identical = evaluator.evaluate(identity, requestFor(*identity), {});
    expect(samePixels(baseline, identical),
           "identity CST between Solid and Layer is bit-identical");
    expect(identical.diagnostics().empty(), "identity CST has no diagnostic");

    const auto roundTrip = planWith(
        {runtime::CstKernel{"ACEScg", "ACEScct"}, runtime::CstKernel{"ACEScct", "ACEScg"}});
    const auto result = evaluator.evaluate(roundTrip, requestFor(*roundTrip), {});
    expect(result.frame() && result.diagnostics().empty(),
           "ACES round trip evaluates without diagnostics");
    const auto config = OCIO::Config::CreateFromBuiltinConfig(
        std::string(color::kAcesCgV1BuiltinConfigName).c_str());
    const auto forward = config->getProcessor("ACEScg", "ACEScct")->getDefaultCPUProcessor();
    const auto inverse = config->getProcessor("ACEScct", "ACEScg")->getDefaultCPUProcessor();
    std::array<float, 3> rgb{-0.125F, 0.18F, 2.0F};
    forward->applyRGB(rgb.data());
    inverse->applyRGB(rgb.data());
    float maxDelta = 0;
    if (result.frame()) {
        for (const auto& pixel : result.frame()->processImage().pixels()) {
            maxDelta = std::max({maxDelta, std::abs(pixel.red() - rgb[0] * 0.5F),
                                 std::abs(pixel.green() - rgb[1] * 0.5F),
                                 std::abs(pixel.blue() - rgb[2] * 0.5F)});
            expect(pixel.alpha() == 0.5F, "CST preserves partial alpha");
        }
    }
    expect(maxDelta < 1e-5F, "CST round trip matches independent OCIO processors below 1e-5");
    std::cout << "CST ACES oracle maximum delta: " << maxDelta << '\n';

    const auto missing = planWith({runtime::CstKernel{"missing-colour-space", "ACEScg"}});
    for (int i = 0; i < 2; ++i) {
        const auto refused = evaluator.evaluate(missing, requestFor(*missing), {});
        expect(samePixels(baseline, refused), "missing colour space passes through exactly");
        expect(refused.diagnostics().size() == 1 &&
                   refused.diagnostics()[0].code ==
                       runtime::EvaluationDiagnosticCode::ColorSpaceMissing,
               "missing colour space retains typed diagnostic on cold and warm caches");
    }
    const auto transparent = planWith({runtime::CstKernel{"ACEScg", "ACEScct"}}, {1, 2, 3, 0});
    const auto zero = evaluator.evaluate(transparent, requestFor(*transparent), {});
    expect(zero.frame() && std::ranges::all_of(zero.frame()->processImage().pixels(),
                                               [](const auto pixel) {
                                                   return pixel.red() == 0 && pixel.green() == 0 &&
                                                          pixel.blue() == 0 && pixel.alpha() == 0;
                                               }),
           "zero alpha remains canonical transparent black");

    runtime::detail::ImageEffectContext context;
    const runtime::CompiledImageEffect effect{document::NodeId::fromRaw(20),
                                              runtime::OperationIndex::fromRaw(0),
                                              runtime::CstKernel{"ACEScg", "ACEScct"}};
    const auto prepared = context.prepare(effect, requestFor(*source).colorIntent);
    const auto reused = context.prepare(effect, requestFor(*source).colorIntent);
    expect(prepared.processor && prepared.processor == reused.processor,
           "config/from/to processor prepares once and is shared");
}
void testLookBypass() {
    runtime::CpuCompositionEvaluator evaluator;
    const runtime::CstKernel transform{"ACEScg", "ACEScct"};
    auto definition = planWith({transform, transform, transform})->copyDefinition();
    std::get<runtime::CompiledImageEffect>(definition.operations[1]).look = true;
    std::get<runtime::CompiledImageEffect>(definition.operations[3]).look = true;
    const auto plan =
        std::make_shared<const runtime::CompiledCompositionPlan>(std::move(definition));
    auto request = requestFor(*plan);
    expect(!request.bypassLookNodes, "new evaluation/export requests include the look by default");
    const auto withLook = evaluator.evaluate(plan, request, {});
    request.bypassLookNodes = true;
    const auto withoutLook = evaluator.evaluate(plan, request, {});
    const auto retained = planWith({transform});
    const auto expected = evaluator.evaluate(retained, requestFor(*retained), {});
    expect(samePixels(withoutLook, expected),
           "look bypass skips tagged effects and retains untagged colour processing");
    expect(!samePixels(withLook, withoutLook), "look flag changes evaluated pixels");
    expect(withLook.frame() && withoutLook.frame() &&
               withLook.frame()->identity() != withoutLook.frame()->identity(),
           "look flag participates in process frame identity");
    const auto warm = evaluator.evaluate(plan, request, {});
    expect(samePixels(warm, withoutLook) && warm.frame()->operationCacheStatistics().misses == 0,
           "look-bypassed chain memoizes each node");
    request.bypassLookNodes = false;
    const auto restored = evaluator.evaluate(plan, request, {});
    expect(samePixels(restored, withLook),
           "turning the look on restores the original cached graph result");
}

void testFileTransform() {
#ifdef __linux__
    const auto directory = std::filesystem::current_path() / "color3-effect-fixtures";
    std::filesystem::create_directories(directory);
    struct Cleanup final {
        std::filesystem::path directory;
        ~Cleanup() { std::filesystem::remove_all(directory); }
    } cleanup{directory};
    const auto path = directory / "show.cube";
    const auto write = [&path](const int gain) {
        std::ofstream output(path);
        output << "LUT_3D_SIZE 2\n";
        for (int b = 0; b < 2; ++b)
            for (int g = 0; g < 2; ++g)
                for (int r = 0; r < 2; ++r)
                    output << r * gain << ' ' << g * gain << ' ' << b * gain << '\n';
    };
    const auto kernelFor = [&path] {
        const auto resource = color::readLutFile(path);
        document::AssetRecord asset;
        asset.id = document::AssetId::fromRaw(77);
        asset.kind = document::AssetKind::Lut;
        asset.name = "Show LUT";
        asset.locator = {"file", "project-relative", "show.cube", "file://" + path.string()};
        asset.contentDigest = resource.digest;
        return runtime::FileTransformKernel{asset.id, 1, 0, "ACEScct", asset};
    };
    const core::Color4d sample{0.18, 0.5, 2.0, 0.5};
    runtime::CpuCompositionEvaluator evaluator;
    const auto source = planWith({}, sample);
    const auto baseline = evaluator.evaluate(source, requestFor(*source), {});
    write(1);
    const auto identity = planWith({kernelFor()}, sample);
    const auto identical = evaluator.evaluate(identity, requestFor(*identity), {});
    expect(identical.diagnostics().empty() && samePixels(identical, baseline),
           "identity cube in ACEScct is bit-identical to bypass");
    write(2);
    const auto changed = evaluator.evaluate(identity, requestFor(*identity), {});
    expect(samePixels(changed, baseline) && changed.diagnostics().size() == 1 &&
               changed.diagnostics()[0].code ==
                   runtime::EvaluationDiagnosticCode::LutTransformFailed &&
               changed.diagnostics()[0].summary.find("ChangedFile") != std::string::npos,
           "changed LUT invalidates a memoized frame and refuses the stale asset digest");
    const auto kernel = kernelFor();
    runtime::detail::ImageEffectContext context;
    const runtime::CompiledImageEffect effect{document::NodeId::fromRaw(20),
                                              runtime::OperationIndex::fromRaw(0), kernel};
    const auto first = context.prepare(effect, requestFor(*source).colorIntent);
    const auto reused = context.prepare(effect, requestFor(*source).colorIntent);
    expect(first.fileProcessor && first.fileProcessor == reused.fileProcessor,
           "file processor is reused by config, digest, process space and options");
    {
        const auto renamed = directory / "same-bytes.spi1d";
        std::filesystem::copy_file(path, renamed);
        auto differentFormat = effect;
        auto& capturedAsset = std::get<runtime::FileTransformKernel>(differentFormat.kernel).asset;
        if (!capturedAsset)
            throw std::logic_error("Missing LUT fixture asset");
        auto& captured = *capturedAsset;
        captured.locator.path = "same-bytes.spi1d";
        captured.locator.relinkHint = "file://" + renamed.string();
        const auto separatelyPrepared =
            context.prepare(differentFormat, requestFor(*source).colorIntent);
        expect(separatelyPrepared.cacheIdentity != first.cacheIdentity &&
                   !separatelyPrepared.fileProcessor && separatelyPrepared.diagnostic &&
                   separatelyPrepared.diagnostic->detail == "MalformedFile",
               "relabelled cube bytes are refused without reusing the prepared processor");
    }
    if (first.fileProcessor) {
        std::array<std::array<float, 4>, 1> pixels{{{0.25F, 0.5F, 0.75F, 1}}};
        unsigned polls = 0;
        const auto cancelDuringExchange = [&polls] {
            ++polls;
            return polls >= 2;
        };
        expect(first.fileProcessor->apply(pixels, cancelDuringExchange) ==
                   color::LutError::HelperCancelled,
               "active file processor can be cancelled");
        const auto recovered = context.prepare(effect, requestFor(*source).colorIntent);
        expect(recovered.fileProcessor && recovered.fileProcessor != first.fileProcessor &&
                   recovered.fileProcessor->isAvailable(),
               "cancelled prepared tokens are replaced on the next frame");
    }
    const auto gain = planWith({kernel}, sample);
    const auto transformed = evaluator.evaluate(gain, requestFor(*gain), {});
    expect(transformed.frame() && transformed.diagnostics().empty(), "gain LUT evaluates");
    const auto config = OCIO::Config::CreateFromBuiltinConfig(
        std::string(color::kAcesCgV1BuiltinConfigName).c_str());
    auto lut = OCIO::FileTransform::Create();
    lut->setSrc(path.string().c_str());
    lut->setInterpolation(OCIO::INTERP_TETRAHEDRAL);
    std::array<float, 3> rgb{0.18F, 0.5F, 2.0F};
    config->getProcessor("ACEScg", "ACEScct")->getDefaultCPUProcessor()->applyRGB(rgb.data());
    OCIO::Config::CreateRaw()->getProcessor(lut)->getDefaultCPUProcessor()->applyRGB(rgb.data());
    config->getProcessor("ACEScct", "ACEScg")->getDefaultCPUProcessor()->applyRGB(rgb.data());
    float delta = 0;
    if (transformed.frame())
        for (const auto pixel : transformed.frame()->processImage().pixels())
            delta = std::max({delta, std::abs(pixel.red() - rgb[0] * 0.5F),
                              std::abs(pixel.green() - rgb[1] * 0.5F),
                              std::abs(pixel.blue() - rgb[2] * 0.5F)});
    expect(delta < 1e-5F, "ACEScct gain LUT matches an independent OCIO pipeline");
    std::cout << "File Transform ACEScct oracle maximum delta: " << delta << '\n';
    runtime::CpuCompositionEvaluator recoveringEvaluator;
    std::vector<color::LutFile> pressure;
    pressure.reserve(10);
    for (int i = 0; i < 10; ++i)
        pressure.push_back(color::readLutFile(path));
    const auto limited = recoveringEvaluator.evaluate(gain, requestFor(*gain), {});
    expect(samePixels(limited, baseline) && !limited.diagnostics().empty() &&
               limited.diagnostics().front().detail == "HelperMemoryLimit",
           "helper preparation is refused under the aggregate host resource limit");
    pressure.clear();
    const auto recoveredFrame = recoveringEvaluator.evaluate(gain, requestFor(*gain), {});
    expect(recoveredFrame.diagnostics().empty() && samePixels(recoveredFrame, transformed),
           "resource recovery cannot reuse the cached preparation-refusal image");
    const auto chain = planWith({kernel, kernel, kernel}, sample);
    const auto cold = evaluator.evaluate(chain, requestFor(*chain), {});
    const auto warm = evaluator.evaluate(chain, requestFor(*chain), {});
    expect(samePixels(cold, warm) && warm.frame()->operationCacheStatistics().hits == 7 &&
               warm.frame()->operationCacheStatistics().misses == 0,
           "three file effects memoize independently");
    auto definition = gain->copyDefinition();
    std::get<runtime::CompiledImageEffect>(definition.operations[1]).look = true;
    const auto tagged =
        std::make_shared<const runtime::CompiledCompositionPlan>(std::move(definition));
    auto request = requestFor(*tagged);
    request.bypassLookNodes = true;
    expect(samePixels(evaluator.evaluate(tagged, request, {}), baseline),
           "look-tagged File Transform bypasses without rewriting the graph");
#endif
}

void testMultiplyAndInvert() {
    using runtime::CompiledScalarParameter;
    using runtime::PointwiseImageEffect;
    using runtime::PointwiseImageKernel;
    const auto multiply = [](const double factor) -> runtime::ImageEffectKernel {
        return PointwiseImageKernel{
            PointwiseImageEffect::Multiply,
            CompiledScalarParameter{document::ParameterId::fromRaw(30), factor}};
    };
    const runtime::ImageEffectKernel invert =
        PointwiseImageKernel{PointwiseImageEffect::Invert, {}};
    const auto color = core::Color4d{-0.125, 0.18, 2.0, 0.5};
    runtime::CpuCompositionEvaluator evaluator;
    const auto source = planWith({}, color);
    const auto original = evaluator.evaluate(source, requestFor(*source), {});
    const auto neutralPlan = planWith({multiply(1.0)}, color);
    const auto neutral = evaluator.evaluate(neutralPlan, requestFor(*neutralPlan), {});
    expect(neutral.frame() && neutral.diagnostics().empty() && samePixels(original, neutral),
           "Multiply factor one preserves signed, HDR and partially transparent pixels");

    const auto multipliedPlan = planWith({multiply(-2.0)}, color);
    const auto multiplied = evaluator.evaluate(multipliedPlan, requestFor(*multipliedPlan), {});
    expect(multiplied.frame() && multiplied.diagnostics().empty(),
           "negative Multiply factor evaluates without a display clamp");
    if (multiplied.frame()) {
        const auto pixel = multiplied.frame()->processImage().pixels().front();
        expect(std::abs(pixel.red() - 0.125F) < 1e-6F && std::abs(pixel.green() + 0.18F) < 1e-6F &&
                   std::abs(pixel.blue() + 2.0F) < 1e-6F && pixel.alpha() == 0.5F,
               "Multiply scales straight scene-linear RGB then restores original association");
    }
    const auto multipliedWarm = evaluator.evaluate(multipliedPlan, requestFor(*multipliedPlan), {});
    expect(samePixels(multiplied, multipliedWarm) &&
               multipliedWarm.frame()->operationCacheStatistics().misses == 0,
           "Multiply warm cache preserves the signed HDR result");

    const auto invertedPlan = planWith({invert}, color);
    auto request = requestFor(*invertedPlan);
    const auto inverted = evaluator.evaluate(invertedPlan, request, {});
    expect(inverted.frame() && inverted.diagnostics().empty(), "color Invert evaluates");
    if (inverted.frame()) {
        const auto pixel = inverted.frame()->processImage().pixels().front();
        expect(std::abs(pixel.red() - 0.5625F) < 1e-6F && std::abs(pixel.green() - 0.41F) < 1e-6F &&
                   std::abs(pixel.blue() + 0.5F) < 1e-6F && pixel.alpha() == 0.5F,
               "Invert computes one minus straight RGB, preserving HDR, negative and alpha");
    }
    const auto window = render::ImageWindow::create(1, 0, 2, 1);
    expect(window.hasValue(), "Invert ROI fixture has a valid window");
    if (window && inverted.frame()) {
        request.roi = *window.value();
        const auto cropped = evaluator.evaluate(invertedPlan, request, {});
        expect(cropped.frame() && cropped.diagnostics().empty() &&
                   cropped.frame()->processImage().descriptor()->dataWindow() == *window.value() &&
                   cropped.frame()->processImage().pixels().front() ==
                       inverted.frame()->processImage().pixels()[1],
               "Invert ROI keeps the requested data window and exact full-frame pixels");
    }

    const auto transparentPlan = planWith({multiply(-2.0), invert}, {2.0, -0.25, 0.4, 0.0});
    const auto transparent = evaluator.evaluate(transparentPlan, requestFor(*transparentPlan), {});
    expect(transparent.frame() && transparent.diagnostics().empty() &&
               std::ranges::all_of(transparent.frame()->processImage().pixels(),
                                   [](const auto pixel) {
                                       return pixel.red() == 0 && pixel.green() == 0 &&
                                              pixel.blue() == 0 && pixel.alpha() == 0;
                                   }),
           "Multiply and Invert keep zero-alpha process pixels transparent black");
}

void testPointwiseFailureAndTimeDependence() {
    using runtime::CompiledScalarParameter;
    using runtime::PointwiseImageEffect;
    using runtime::PointwiseImageKernel;
    const auto parameterId = document::ParameterId::fromRaw(90);
    runtime::CpuCompositionEvaluator evaluator;
    for (const auto kernel :
         {PointwiseImageKernel{PointwiseImageEffect::Exposure,
                               CompiledScalarParameter{parameterId, 1024.0}},
          PointwiseImageKernel{
              PointwiseImageEffect::Offset,
              CompiledScalarParameter{parameterId, std::numeric_limits<double>::max()}},
          PointwiseImageKernel{
              PointwiseImageEffect::Multiply,
              CompiledScalarParameter{parameterId,
                                      static_cast<double>(std::numeric_limits<float>::max())}}}) {
        const auto plan = planWith({kernel});
        const auto result = evaluator.evaluate(plan, requestFor(*plan), {});
        expect(!result.frame() && !result.diagnostics().empty(),
               "unrepresentable pointwise parameters and pixels fail instead of bypassing");
    }
    const auto base = planWith({PointwiseImageKernel{PointwiseImageEffect::Exposure,
                                                     CompiledScalarParameter{parameterId, 1.0}}});
    expect(!base->operationTimeDependent(base->output()),
           "constant pointwise amounts retain static-frame reuse");
    auto definition = base->copyDefinition();
    definition.duration = core::RationalTime::fromInteger(10);
    definition.valueOperations = {{document::NodeId::fromRaw(200),
                                   runtime::ValueOutputIndex::fromRaw(0), 2,
                                   runtime::CompiledValueTime{}}};
    definition.valueOutputCount = 2;
    auto& effect = std::get<runtime::CompiledImageEffect>(definition.operations[1]);
    std::get<PointwiseImageKernel>(effect.kernel).amount->source =
        runtime::ValueOutputIndex::fromRaw(0);
    const auto animated =
        std::make_shared<const runtime::CompiledCompositionPlan>(std::move(definition));
    expect(!animated->operationTimeDependent(runtime::OperationIndex::fromRaw(0)) &&
               animated->operationTimeDependent(runtime::OperationIndex::fromRaw(1)) &&
               animated->operationTimeDependent(animated->output()),
           "a driven pointwise amount marks its effect and downstream output time dependent");
    auto request = requestFor(*animated);
    const auto first = evaluator.evaluate(animated, request, {});
    request.time = core::RationalTime::fromInteger(1);
    const auto next = evaluator.evaluate(animated, request, {});
    expect(first.frame() && next.frame() && !samePixels(first, next),
           "a Time-driven pointwise amount changes the cached image at a new time");
}

#include "pointwise_preflight_tests.ipp"
void testImageCombineDrivers() {
    using namespace runtime;
    using namespace document;
    const auto makeDefinition = [](const ImageCombineKernel kernel) {
        auto definition = planWith({kernel})->copyDefinition();
        std::get<CompiledImageEffect>(definition.operations[1]).secondaryInput =
            OperationIndex::fromRaw(0);
        definition.duration = core::RationalTime::fromInteger(2);
        return definition;
    };
    const auto makePlan = [](CompiledCompositionPlanDefinition definition) {
        return std::make_shared<const CompiledCompositionPlan>(std::move(definition));
    };
    const ImageCombineKernel multiplyAlpha{
        ImageCombineMode::MultiplyAlpha,
        CompiledScalarParameter{ParameterId::fromRaw(90), ScalarCurveIndex::fromRaw(0)},
        render::ImageCoverageChannel::Alpha};
    auto animatedDefinition = makeDefinition(multiplyAlpha);
    animatedDefinition.scalarCurves.push_back(
        {AnimationCurveId::fromRaw(90),
         {{KeyframeId::fromRaw(90), core::RationalTime{}, 0.0,
           CompiledKeyframeInterpolation::Linear},
          {KeyframeId::fromRaw(91), core::RationalTime::fromInteger(1), 1.0,
           CompiledKeyframeInterpolation::Linear}}});
    const auto animated = makePlan(animatedDefinition);
    CpuCompositionEvaluator evaluator;
    auto request = requestFor(*animated);
    const auto first = evaluator.evaluate(animated, request, {});
    request.time = core::RationalTime::fromInteger(1);
    const auto last = evaluator.evaluate(animated, request, {});
    expect(first.frame() && last.frame() && !samePixels(first, last),
           "Image Combine registers and samples its animated factor curve");
    const auto warm = evaluator.evaluate(animated, request, {});
    expect(samePixels(last, warm), "animated Image Combine warm result is stable");
    animatedDefinition.scalarCurves.front().keyframes.back().value = 1.5;
    const auto invalidCurve = makePlan(animatedDefinition);
    const auto rejectedCurve = evaluator.evaluate(invalidCurve, requestFor(*invalidCurve), {});
    expect(!rejectedCurve.frame() && !rejectedCurve.diagnostics().empty(),
           "Image Combine rejects out-of-domain factor keys before image evaluation");

    auto drivenDefinition = makeDefinition(multiplyAlpha);
    std::get<ImageCombineKernel>(
        std::get<CompiledImageEffect>(drivenDefinition.operations[1]).kernel).factor->source =
        ValueOutputIndex::fromRaw(0);
    drivenDefinition.valueOperations = {
        {NodeId::fromRaw(200), ValueOutputIndex::fromRaw(0), 2, CompiledValueTime{}}};
    drivenDefinition.valueOutputCount = 2;
    const auto driven = makePlan(std::move(drivenDefinition));
    request = requestFor(*driven);
    const auto drivenFirst = evaluator.evaluate(driven, request, {});
    request.time = core::RationalTime::fromInteger(1);
    const auto drivenLast = evaluator.evaluate(driven, request, {});
    expect(drivenFirst.frame() && drivenLast.frame() && !samePixels(drivenFirst, drivenLast),
           "Image Combine resolves a linked Time factor without stale pixels");

    ImageCombineKernel setAlpha{ImageCombineMode::SetAlpha, {},
                               render::ImageCoverageChannel::Alpha};
    setAlpha.drivenChannel = ValueOutputIndex::fromRaw(0);
    auto channelDefinition = makeDefinition(setAlpha);
    channelDefinition.valueOperations = {
        {NodeId::fromRaw(201), ValueOutputIndex::fromRaw(0), 1,
         CompiledValueUtility{ValueUtilityKernel::FrameNumber, {}, {}, {}}}};
    channelDefinition.valueOutputCount = 1;
    const auto channelPlan = makePlan(channelDefinition);
    request = requestFor(*channelPlan);
    const auto alpha = evaluator.evaluate(channelPlan, request, {});
    request.time = *core::RationalTime::create(1, 24);
    const auto red = evaluator.evaluate(channelPlan, request, {});
    const auto redWarm = evaluator.evaluate(channelPlan, request, {});
    expect(alpha.frame() && red.frame() && !samePixels(alpha, red) && samePixels(red, redWarm),
           "linked Integer channel changes coverage and cache identity across frames");

    for (const auto& value : {CompiledValue{0.0}, CompiledValue{std::int64_t{-1}},
                              CompiledValue{std::int64_t{5}}}) {
        auto invalidDefinition = channelDefinition;
        invalidDefinition.valueOperations.front().kernel = CompiledValuePassthrough{
            {ParameterId::fromRaw(201), value}};
        const auto invalid = makePlan(std::move(invalidDefinition));
        const auto result = evaluator.evaluate(invalid, requestFor(*invalid), {});
        expect(!result.frame() && !result.diagnostics().empty(),
               "Image Combine refuses non-Integer and out-of-domain channel drivers");
    }
    auto invalidIndex = channelDefinition;
    std::get<ImageCombineKernel>(std::get<CompiledImageEffect>(invalidIndex.operations[1]).kernel)
        .drivenChannel = ValueOutputIndex::fromRaw(100);
    const auto invalid = makePlan(std::move(invalidIndex));
    expect(!evaluator.evaluate(invalid, requestFor(*invalid), {}).frame(),
           "Image Combine refuses an invalid channel output index");

    runtime::detail::ImageEffectContext context;
    const auto& effect = std::get<CompiledImageEffect>(channelPlan->operations()[1]);
    const auto intent = requestFor(*channelPlan).colorIntent;
    const auto alphaPrepared = context.prepare(effect, intent, {}, {}, {}, {}, 1.0, 1.0, 0);
    const auto redPrepared = context.prepare(effect, intent, {}, {}, {}, {}, 1.0, 1.0, 1);
    expect(!alphaPrepared.diagnostic && !redPrepared.diagnostic &&
               alphaPrepared.cacheIdentity != redPrepared.cacheIdentity,
           "prepared channel identity includes the resolved channel");
    for (const auto value : {std::optional<std::int64_t>{}, std::optional<std::int64_t>{-1},
                             std::optional<std::int64_t>{5}})
        expect(context.prepare(effect, intent, {}, {}, {}, {}, 1.0, 1.0, value)
                   .diagnostic.has_value(),
               "channel preparation refuses missing and invalid samples");
}

} // namespace

int main(const int argc, char** argv) {
    try {
        if (argc == 2 && std::string_view(argv[1]) == "--combine-only") {
            testImageCombineWindows();
            testImageCombineDrivers();
            return failures == 0 ? 0 : 1;
        }
        if (argc == 2 && std::string_view(argv[1]) == "--blur-only") {
            testBlurEvaluation();
            testSpatialEffectBudgets();
            testAliasedBlurBudgets();
            return failures == 0 ? 0 : 1;
        }
        if (argc == 2 && std::string_view(argv[1]) == "--discrete-only") {
            testDiscreteImages();
            testUnaryImageEffectInputArity();
            return failures == 0 ? 0 : 1;
        }
        testCst();
        testExposureAndOffset();
        testCanvasEffects();
        testCanvasAnimation();
        testImageCombineWindows();
        testImageCombineDrivers();
        testDiscreteImages();
        testUnaryImageEffectInputArity();
        testBlurEvaluation();
        testSpatialEffectBudgets();
        testAliasedBlurBudgets();
        testLookBypass();
        testFileTransform();
        testMultiplyAndInvert();
        testPointwiseFailureAndTimeDependence();
        testPointwiseAnimatedPreflight();
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    return failures == 0 ? 0 : 1;
}
