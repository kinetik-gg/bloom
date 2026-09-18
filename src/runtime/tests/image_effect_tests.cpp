#include "image_effect.hpp"

#include <OpenColorIO/OpenColorIO.h>
#include <bloom/color/bloom_neutral_builtin.hpp>
#include <bloom/runtime/cpu_composition_evaluator.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
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

} // namespace

int main() {
    try {
        testCst();
        testLookBypass();
        testFileTransform();
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    return failures == 0 ? 0 : 1;
}
