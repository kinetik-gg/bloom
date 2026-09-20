// ROI parity for CPU-side GPU scene preparation.
//
// A request ROI is resolved exactly as the CPU evaluator resolves it: the process image
// descriptor's data window is the requested ROI, while the prepared scene keeps native
// source/output windows for every intermediate command and clips only the terminal Composition
// Output. These tests drive the REAL production CpuGpuSceneBuilder with a genuine ROI request and
// compare identity, bounds, the output descriptor, and every process pixel against the unchanged
// CpuCompositionEvaluator oracle by replaying the prepared commands with the existing CPU
// primitives. They also prove that the per-command semantic keys of the unchanged intermediate work
// stay stable across an ROI edit, so only the clipped output command is cold.

#include "gpu_roi_scene_fixture_support.hpp"

namespace {

void checkRoiParity(Expectations& expectations, const CpuCompositionEvaluator& evaluator,
                    const std::shared_ptr<const CompiledCompositionPlan>& plan,
                    const EvaluationRequest& request, const std::string& label) {
    const CpuGpuSceneBuilder builder;
    const auto prepared = builder.build(plan, request);
    expectations.expect(prepared.hasValue(), label + ": prepares");
    if (!prepared) {
        std::cerr << label << " diagnostic: " << prepared.diagnostic.message << "\n";
        return;
    }
    auto oracleRequest = request;
    oracleRequest.bypassOperationCache = true;
    const auto frame = evaluator.evaluate(plan, oracleRequest, {});
    expectations.expect(frame.frame() != nullptr, label + ": CPU frame evaluates");
    if (!frame.frame()) {
        return;
    }
    expectations.expect(prepared.scene->processIdentity() == frame.frame()->identity(),
                        label + ": identity matches");
    expectations.expect(
        std::ranges::equal(prepared.scene->bounds(), frame.frame()->evaluatedBounds()),
        label + ": ROI retains complete evaluated geometry");
    expectations.expect(prepared.scene->outputDescriptor() ==
                            *frame.frame()->processImage().descriptor(),
                        label + ": output descriptor is the CPU ROI descriptor");
    if (request.roi.has_value()) {
        expectations.expect(prepared.scene->outputDescriptor().dataWindow() == *request.roi,
                            label + ": process data window is exactly the requested ROI");
    }
    double hScale = 1.0;
    double vScale = 1.0;
    if (const auto* proxy = std::get_if<bloom::runtime::ProxyResolution>(&request.resolution)) {
        hScale = static_cast<double>(proxy->extent.width()) /
                 static_cast<double>(plan->format().width());
        vScale = static_cast<double>(proxy->extent.height()) /
                 static_cast<double>(plan->format().height());
    }
    std::vector<std::shared_ptr<const Rgba32fImage>> images;
    expectations.expect(replayScene(*prepared.scene, images, hScale, vScale), label + ": replays");
    const auto& replayed = images[prepared.scene->outputCommand()];
    expectations.expect(
        replayed != nullptr &&
            replayed->pixels().size() == frame.frame()->processImage().pixels().size() &&
            std::memcmp(replayed->pixels().data(), frame.frame()->processImage().pixels().data(),
                        replayed->pixels().size() * sizeof(Rgba32f)) == 0,
        replayed == nullptr ? label + ": replay produced no image"
                            : label + ": ROI pixel parity " +
                                  firstMismatch(*replayed, frame.frame()->processImage()));
}

void testSolidRois(Expectations& expectations, const CpuCompositionEvaluator& evaluator) {
    const auto plan =
        twoLayerPlan(format(16, 12), LayerValues{.position = {4.3, 3.1}},
                     LayerValues{.position = {11.5, 8.2}, .opacity = 0.75}, 6.0, 5.0, 70000);
    struct Region final {
        std::int64_t x;
        std::int64_t y;
        std::uint64_t w;
        std::uint64_t h;
        const char* label;
    };
    const Region regions[] = {
        {0, 0, 4, 4, "origin ROI"},          {3, 2, 5, 6, "nonzero-origin ROI"},
        {0, 0, 16, 12, "full-frame ROI"},    {15, 11, 1, 1, "single-pixel ROI"},
        {0, 8, 2, 3, "ROI below the solid"},
    };
    for (const auto& region : regions) {
        auto request = requestFor(*plan);
        request.roi = roi(region.x, region.y, region.w, region.h);
        checkRoiParity(expectations, evaluator, plan, request, region.label);
    }
}

void testAllTransparentRoi(Expectations& expectations, const CpuCompositionEvaluator& evaluator) {
    // Both layers sit far outside the requested ROI: the whole process image is transparent.
    const auto plan = twoLayerPlan(format(32, 24), LayerValues{.position = {26.5, 20.5}},
                                   LayerValues{.position = {28.0, 22.0}}, 4.0, 3.0, 70100);
    auto request = requestFor(*plan);
    request.roi = roi(1, 1, 4, 4);
    const CpuGpuSceneBuilder builder;
    const auto prepared = builder.build(plan, request);
    expectations.expect(prepared.hasValue(), "all-transparent ROI prepares");
    checkRoiParity(expectations, evaluator, plan, request, "all-transparent ROI");
}

void testVectorRoi(Expectations& expectations, const CpuCompositionEvaluator& evaluator) {
    const auto text = textPlan(format(24, 16), LayerValues{.position = {12.3, 8.1}}, 70200);
    auto textRequest = requestFor(*text);
    textRequest.roi = roi(7, 3, 9, 6);
    checkRoiParity(expectations, evaluator, text, textRequest, "text fractional ROI");

    ShapeValues values;
    values.kind = bloom::document::ShapeKind::Star;
    values.points = 7;
    values.innerRatio = 0.4;
    values.strokeEnabled = true;
    values.strokeWidth = 1.25;
    const auto shape =
        shapePlan(format(24, 16), LayerValues{.position = {12.3, 8.1}}, values, 70300);
    auto shapeRequest = requestFor(*shape);
    shapeRequest.roi = roi(6, 2, 10, 8);
    checkRoiParity(expectations, evaluator, shape, shapeRequest, "shape coverage ROI");
}

void testAffineRoi(Expectations& expectations, const CpuCompositionEvaluator& evaluator) {
    const auto plan = twoLayerPlan(
        format(24, 16),
        LayerValues{.position = {12.0, 8.0}, .scale = {1.75, -0.5}, .rotation = 30.0},
        LayerValues{
            .position = {7.0, 9.0}, .anchor = {0.25, 0.5}, .scale = {2.0, 0.5}, .rotation = -15.0},
        9.0, 7.0, 70400);
    for (const auto region : {roi(2, 1, 12, 9), roi(10, 6, 6, 4), roi(0, 0, 3, 3)}) {
        auto request = requestFor(*plan);
        request.roi = region;
        checkRoiParity(expectations, evaluator, plan, request, "fractional affine ROI");
    }
}

void testProxyRoi(Expectations& expectations, const CpuCompositionEvaluator& evaluator) {
    const auto plan = twoLayerPlan(
        format(11, 7, pixelAspect(4, 3)), LayerValues{.position = {5.3, 3.1}, .rotation = 25.0},
        LayerValues{.position = {2.7, 4.9}, .anchor = {1.0, -0.5}, .scale = {-1.0, 1.5}}, 5.0, 4.0,
        70500);
    const auto extent = bloom::render::ImageExtent::create(7, 5);
    expectations.expect(static_cast<bool>(extent), "the proxy extent builds");
    if (!extent) {
        return;
    }
    auto request = requestFor(*plan);
    request.resolution = bloom::runtime::ProxyResolution{*extent.value()};
    request.roi = roi(1, 0, 3, 3);
    checkRoiParity(expectations, evaluator, plan, request, "proxy non-square PAR ROI");
}

void testRoiKeyIsolation(Expectations& expectations) {
    const auto plan =
        twoLayerPlan(format(16, 12), LayerValues{.position = {4.3, 3.1}},
                     LayerValues{.position = {11.5, 8.2}, .opacity = 0.75}, 6.0, 5.0, 70600);
    const CpuGpuSceneBuilder builder;
    const auto keyOf = [](const auto& command) -> const std::string& {
        return std::visit([](const auto& item) -> const std::string& { return item.semanticKey; },
                          command);
    };
    std::vector<std::string> fullKeys;
    const auto preparedFull = builder.build(plan, requestFor(*plan));
    expectations.expect(preparedFull.hasValue(), "full-frame scene prepares");
    if (!preparedFull) {
        return;
    }
    for (const auto& command : preparedFull.scene->commands()) {
        fullKeys.push_back(keyOf(command));
    }
    auto request = requestFor(*plan);
    request.roi = roi(3, 2, 5, 6);
    const auto preparedRoi = builder.build(plan, request);
    expectations.expect(preparedRoi.hasValue(), "ROI scene prepares");
    if (!preparedRoi) {
        return;
    }
    std::vector<std::string> roiKeys;
    for (const auto& command : preparedRoi.scene->commands()) {
        roiKeys.push_back(keyOf(command));
    }
    // Every intermediate command (all but the terminal output) keeps its exact semantic key, so a
    // resident cache reuses the unchanged source and composed work across the ROI edit. The output
    // command is the only command whose key changes because its clipped data window changed.
    expectations.expect(fullKeys.size() >= 2 && roiKeys.size() == fullKeys.size(),
                        "both scenes have the same command count");
    if (fullKeys.size() >= 2 && roiKeys.size() == fullKeys.size()) {
        for (std::size_t i = 0; i + 1 < fullKeys.size(); ++i) {
            expectations.expect(fullKeys[i] == roiKeys[i],
                                "intermediate command key is stable across an ROI edit");
        }
        expectations.expect(fullKeys.back() != roiKeys.back(),
                            "the clipped output command identity changes with the ROI");
    }
}

void testNestedRoi(Expectations& expectations, const CpuCompositionEvaluator& evaluator) {
    const auto child = nestedChildPlan(70700, 707, Color4d{0.2, 0.4, 0.6, 0.75});
    const auto parent = nestedParentPlan(child, 708);
    auto request = requestFor(*parent);
    request.roi = roi(2, 1, 6, 5);
    checkRoiParity(expectations, evaluator, parent, request, "nested child ROI");
}

void testInvalidRoiRejected(Expectations& expectations) {
    const auto plan = twoLayerPlan(format(8, 8), LayerValues{}, LayerValues{}, 8.0, 8.0, 70800);
    const CpuGpuSceneBuilder builder;
    for (const auto& region :
         {roi(0, 0, 9, 8), roi(0, 0, 8, 9), roi(4, 4, 5, 5), roi(8, 0, 1, 1), roi(0, 8, 1, 1)}) {
        auto request = requestFor(*plan);
        request.roi = region;
        const auto prepared = builder.build(plan, request);
        expectations.expect(!prepared.hasValue(), "an out-of-resolution ROI is refused");
    }
}

} // namespace

int main() {
    try {
        Expectations expectations;
        const CpuCompositionEvaluator evaluator;
        testSolidRois(expectations, evaluator);
        testAllTransparentRoi(expectations, evaluator);
        testVectorRoi(expectations, evaluator);
        testAffineRoi(expectations, evaluator);
        testProxyRoi(expectations, evaluator);
        testRoiKeyIsolation(expectations);
        testNestedRoi(expectations, evaluator);
        testInvalidRoiRejected(expectations);
        if (!expectations.ok()) {
            std::cerr << "FAIL: ROI GPU scene preparation expectations failed\n";
            return 1;
        }
        std::cout << "PASS: CPU GPU scene preparation ROI\n";
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << "Unexpected test exception: " << exception.what() << '\n';
        return 1;
    }
}
