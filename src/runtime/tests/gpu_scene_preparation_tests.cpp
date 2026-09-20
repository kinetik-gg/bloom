#include "gpu_scene_preparation_test_support.hpp"

namespace {

void checkParity(Expectations& expectations, const CpuCompositionEvaluator& evaluator,
                 const std::shared_ptr<const CompiledCompositionPlan>& plan,
                 const EvaluationRequest& request, const std::string& label,
                 std::shared_ptr<bloom::runtime::GpuSceneCoverageCache> cache = nullptr) {
    const CpuGpuSceneBuilder builder(std::move(cache));
    const auto prepared = builder.build(plan, request);
    expectations.expect(prepared.hasValue(), label + ": prepares");
    if (!prepared) {
        std::cerr << label << " diagnostic: " << prepared.diagnostic.message << "\n";
        return;
    }
    // Evaluate the oracle uncached: the evaluator's semantic cache deliberately ignores node and
    // layer IDs, so a cached frame from an equivalent plan would carry the other plan's bounds IDs.
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
        label + ": bounds match");
    expectations.expect(prepared.scene->outputDescriptor() ==
                            *frame.frame()->processImage().descriptor(),
                        label + ": output descriptor matches");
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
        replayed == nullptr
            ? label + ": replay produced no image"
            : label + ": pixel parity " + firstMismatch(*replayed, frame.frame()->processImage()));
}

// A text -> translation-only layer -> output plan. Text is a single premultiplied colour through an
// 8-bit glyph coverage, so it must prepare through the same CoveredSolidV1 coverage command a
// fractional solid uses, with the real render::textOutlines geometry.
void testTextCoverage(Expectations& expectations, const CpuCompositionEvaluator& evaluator) {
    const auto plan = textPlan(format(24, 16), LayerValues{.position = {12.3, 8.1}}, 50000);
    const auto prepared = CpuGpuSceneBuilder{}.build(plan, requestFor(*plan));
    expectations.expect(prepared.hasValue(), "a fractional text layer prepares");
    if (!prepared) {
        std::cerr << "text diagnostic: " << prepared.diagnostic.message << "\n";
        return;
    }
    bool sawCoverage = false;
    for (const auto& command : prepared.scene->commands()) {
        sawCoverage = sawCoverage ||
                      std::holds_alternative<bloom::runtime::GpuSceneCoverageSolidCommand>(command);
    }
    expectations.expect(sawCoverage, "text prepares a native coverage command");
    checkParity(expectations, evaluator, plan, requestFor(*plan), "text fractional coverage");

    // Integer device grid: place the layer so the translation is exactly integral. The text leaf
    // bounds do not depend on the layer position, so they are read from the probe above.
    const auto centre = prepared.scene->bounds()[0].output;
    if (!centre.empty()) {
        auto definition = plan->copyDefinition();
        auto& layer = std::get<CompiledLayerOutput>(definition.operations[1]);
        const auto positionId = layer.position.id;
        layer.position = CompiledVec2Parameter{
            positionId, bloom::document::Vec2d{(centre.left + centre.right) * 0.5,
                                               (centre.top + centre.bottom) * 0.5}};
        const auto integerPlan = publish(std::move(definition));
        checkParity(expectations, evaluator, integerPlan, requestFor(*integerPlan),
                    "text integer-grid coverage");
    }
}

void testShapeCoverage(Expectations& expectations, const CpuCompositionEvaluator& evaluator) {
    using Kind = bloom::document::ShapeKind;
    std::uint64_t base = 60000;
    for (const auto kind :
         {Kind::Rectangle, Kind::Ellipse, Kind::Triangle, Kind::Polygon, Kind::Star, Kind::Path}) {
        ShapeValues shapeValues;
        shapeValues.kind = kind;
        shapeValues.points = 6;
        shapeValues.cornerRadius = 0.75;
        const auto plan =
            shapePlan(format(24, 16), LayerValues{.position = {12.3, 8.1}}, shapeValues, base);
        checkParity(expectations, evaluator, plan, requestFor(*plan), "shape fill coverage");
        base += 100;
    }
    // Line suppresses the fill entirely; it is a stroke-only shape.
    {
        ShapeValues v;
        v.kind = Kind::Line;
        v.strokeEnabled = true;
        v.strokeWidth = 1.5;
        const auto plan = shapePlan(format(24, 16), LayerValues{.position = {12.3, 8.1}}, v, base);
        checkParity(expectations, evaluator, plan, requestFor(*plan), "line stroke-only coverage");
        base += 100;
    }
    // Closed stroke-only shape.
    {
        ShapeValues v;
        v.kind = Kind::Ellipse;
        v.fillEnabled = false;
        v.strokeEnabled = true;
        v.strokeWidth = 2.5;
        const auto plan = shapePlan(format(24, 16), LayerValues{.position = {12.3, 8.1}}, v, base);
        checkParity(expectations, evaluator, plan, requestFor(*plan),
                    "ellipse stroke-only coverage");
        base += 100;
    }

    // Fill + stroke: exercises the SourceOver merge and, at opacity != 1, the post-opacity pass.
    for (const double opacity : {1.0, 0.65}) {
        ShapeValues v;
        v.kind = Kind::Ellipse;
        v.strokeEnabled = true;
        v.strokeWidth = 2.0;
        v.fillColor = Color4d{0.7, 0.2, 0.1, 0.8};
        v.strokeColor = Color4d{0.1, 0.4, 0.9, 0.6};
        const auto plan = shapePlan(
            format(24, 16), LayerValues{.position = {9.7, 6.2}, .opacity = opacity}, v, base);
        checkParity(expectations, evaluator, plan, requestFor(*plan), "shape fill+stroke coverage");
        base += 100;
    }
    // Non-square PAR proxy.
    {
        ShapeValues v;
        v.kind = Kind::Star;
        v.points = 7;
        v.innerRatio = 0.4;
        v.strokeEnabled = true;
        v.strokeWidth = 1.0;
        const auto plan = shapePlan(format(11, 7, pixelAspect(4, 3)),
                                    LayerValues{.position = {5.3, 3.1}}, v, base);
        const auto extent = bloom::render::ImageExtent::create(7, 5);
        auto request = requestFor(*plan);
        request.resolution = bloom::runtime::ProxyResolution{*extent.value()};
        checkParity(expectations, evaluator, plan, request, "star proxy non-square PAR");
    }
}

void testBasicAndMerge(Expectations& expectations, const CpuCompositionEvaluator& evaluator) {
    checkParity(expectations, evaluator,
                twoLayerPlan(format(16, 12), LayerValues{.position = {4.3, 3.1}, .opacity = 1.0},
                             LayerValues{.position = {11.5, 8.2}, .opacity = 0.75}, 6.0, 5.0, 1000),
                requestFor(*twoLayerPlan(
                    format(16, 12), LayerValues{.position = {4.3, 3.1}, .opacity = 1.0},
                    LayerValues{.position = {11.5, 8.2}, .opacity = 0.75}, 6.0, 5.0, 1000)),
                "basic translated/padded");
}

void testProxyNonSquareParAndCentered(Expectations& expectations,
                                      const CpuCompositionEvaluator& evaluator) {
    const auto plan = twoLayerPlan(
        format(9, 6, pixelAspect(4, 3)), LayerValues{.position = {4.5, 3.0}, .opacity = 1.0},
        LayerValues{.position = {2.7, 4.9}, .anchor = {1.0, -0.5}, .opacity = 0.5}, 4.0, 3.0, 2000);
    const auto extent = bloom::render::ImageExtent::create(5, 4);
    expectations.expect(static_cast<bool>(extent), "the proxy extent builds");
    if (!extent) {
        return;
    }
    auto request = requestFor(*plan);
    request.resolution = bloom::runtime::ProxyResolution{*extent.value()};
    checkParity(expectations, evaluator, plan, request, "proxy / non-square PAR");
}

void testAnimatedOpacity(Expectations& expectations, const CpuCompositionEvaluator& evaluator) {
    auto definition =
        twoLayerPlan(format(8, 8), LayerValues{.position = {4.0, 4.0}, .opacity = 1.0},
                     LayerValues{.position = {4.0, 4.0}, .opacity = 1.0}, 8.0, 8.0, 3000)
            ->copyDefinition();
    auto& layer = std::get<CompiledLayerOutput>(definition.operations[1]);
    layer.opacity.source = bloom::runtime::ScalarCurveIndex::fromRaw(0);
    bloom::runtime::CompiledScalarCurve curve;
    curve.id = bloom::document::AnimationCurveId::fromRaw(7000);
    curve.keyframes.push_back({bloom::document::KeyframeId::fromRaw(7001),
                               RationalTime::fromInteger(0), 0.25,
                               bloom::runtime::CompiledKeyframeInterpolation::Linear});
    curve.keyframes.push_back({bloom::document::KeyframeId::fromRaw(7002),
                               RationalTime::fromInteger(1), 0.75,
                               bloom::runtime::CompiledKeyframeInterpolation::Linear});
    definition.scalarCurves.push_back(std::move(curve));
    const auto plan = publish(std::move(definition));
    checkParity(expectations, evaluator, plan, requestFor(*plan, rationalTime(1, 2)),
                "animated opacity");
}

void testKeyStability(Expectations& expectations) {
    const auto first = twoLayerPlan(format(8, 8), LayerValues{.position = {4.0, 4.0}},
                                    LayerValues{.position = {4.0, 4.0}}, 8.0, 8.0, 4000);
    // Same pixels and parameters, but the two independent layer subtrees are emitted in the
    // opposite order, so every operation index changes while no parameter value does. The semantic
    // pixel keys must be stable because they never contain an operation index or a node ID.
    auto shifted = first->copyDefinition();
    std::vector<CompiledOperation> reordered;
    reordered.push_back(shifted.operations[2]); // solid B
    reordered.push_back(shifted.operations[3]); // layer B
    reordered.push_back(shifted.operations[0]); // solid A
    reordered.push_back(shifted.operations[1]); // layer A
    reordered.push_back(shifted.operations[4]); // merge
    reordered.push_back(shifted.operations[5]); // output
    std::get<CompiledLayerOutput>(reordered[1]).input = OperationIndex::fromRaw(0);
    std::get<CompiledLayerOutput>(reordered[3]).input = OperationIndex::fromRaw(2);
    auto& merge = std::get<CompiledMerge>(reordered[4]);
    // Keep the same bottom-to-top layer order the original entries implied: the evaluator folds
    // entries reversed, so entries {A(top),B(bottom)} composite B then A exactly as the original
    // {A,B} did. Only the operation indexes move.
    merge.entries[0].input = OperationIndex::fromRaw(3);
    merge.entries[0].layerId = std::get<CompiledLayerOutput>(reordered[3]).layerId;
    merge.entries[1].input = OperationIndex::fromRaw(1);
    merge.entries[1].layerId = std::get<CompiledLayerOutput>(reordered[1]).layerId;
    std::get<CompiledCompositionOutput>(reordered[5]).input = OperationIndex::fromRaw(4);
    std::visit([](auto& item) { item.sourceNodeId = bloom::document::NodeId::fromRaw(7777); },
               reordered[1]);
    shifted.operations = std::move(reordered);
    const auto second = publish(std::move(shifted));

    const CpuGpuSceneBuilder builder;
    const auto a = builder.build(first, requestFor(*first));
    const auto b = builder.build(second, requestFor(*second));
    if (!a) {
        std::cerr << "NOTE first plan not prepared: " << a.diagnostic.message << "\n";
    }
    if (!b) {
        std::cerr << "NOTE shifted plan not prepared: " << b.diagnostic.message << "\n";
    }
    expectations.expect(a.hasValue() && b.hasValue(), "both equivalent plans prepare");
    if (!a || !b) {
        return;
    }
    const auto keyOf = [](const auto& command) -> const std::string& {
        return std::visit([](const auto& item) -> const std::string& { return item.semanticKey; },
                          command);
    };
    std::vector<std::string> leftKeys;
    std::vector<std::string> rightKeys;
    for (const auto& command : a.scene->commands()) {
        leftKeys.push_back(keyOf(command));
    }
    for (const auto& command : b.scene->commands()) {
        rightKeys.push_back(keyOf(command));
    }
    std::ranges::sort(leftKeys);
    std::ranges::sort(rightKeys);
    expectations.expect(leftKeys == rightKeys,
                        "a reordered split keeps every semantic pixel key stable");
}

void testIntegerNativeGrid(Expectations& expectations, const CpuCompositionEvaluator& evaluator) {
    // Integer device translation: the CPU keeps the bilinear reference path, so the prepared route
    // must be the translation command, not the coverage raster.
    const auto plan =
        twoLayerPlan(format(16, 12), LayerValues{.position = {4.0, 3.5}, .opacity = 1.0},
                     LayerValues{.position = {9.0, 7.5}, .opacity = 0.5}, 6.0, 5.0, 11000);
    const auto prepared = CpuGpuSceneBuilder{}.build(plan, requestFor(*plan));
    expectations.expect(prepared.hasValue(), "integer grid plan prepares");
    if (prepared) {
        bool sawTranslation = false;
        for (const auto& command : prepared.scene->commands()) {
            sawTranslation =
                sawTranslation ||
                std::holds_alternative<bloom::runtime::GpuSceneTranslationCommand>(command);
        }
        expectations.expect(sawTranslation, "an integer grid uses the native translation command");
    }
    checkParity(expectations, evaluator, plan, requestFor(*plan), "integer grid");
}

void testFractionalSignsAndOpacity(Expectations& expectations,
                                   const CpuCompositionEvaluator& evaluator) {
    checkParity(expectations, evaluator,
                twoLayerPlan(format(16, 12), LayerValues{.position = {4.3, 3.1}},
                             LayerValues{.position = {2.7, 2.4}}, 6.0, 5.0, 12000),
                requestFor(*twoLayerPlan(format(16, 12), LayerValues{.position = {4.3, 3.1}},
                                         LayerValues{.position = {2.7, 2.4}}, 6.0, 5.0, 12000)),
                "+0.3/-0.3 fractional");
    checkParity(expectations, evaluator,
                twoLayerPlan(format(16, 12), LayerValues{.position = {4.3, 3.1}, .opacity = 0.0},
                             LayerValues{.position = {2.7, 2.4}, .opacity = 1.0}, 6.0, 5.0, 13000),
                requestFor(*twoLayerPlan(
                    format(16, 12), LayerValues{.position = {4.3, 3.1}, .opacity = 0.0},
                    LayerValues{.position = {2.7, 2.4}, .opacity = 1.0}, 6.0, 5.0, 13000)),
                "opacity endpoints 0/1");
}

void testAnimatedPosition(Expectations& expectations, const CpuCompositionEvaluator& evaluator) {
    auto definition = twoLayerPlan(format(16, 12), LayerValues{.position = {4.3, 3.1}},
                                   LayerValues{.position = {4.3, 3.1}}, 6.0, 5.0, 14000)
                          ->copyDefinition();
    auto& layer = std::get<CompiledLayerOutput>(definition.operations[1]);
    layer.position.source = bloom::runtime::Vec2CurveIndex::fromRaw(0);
    bloom::runtime::CompiledVec2Curve curve;
    curve.id = bloom::document::AnimationCurveId::fromRaw(8000);
    curve.components[0] = {
        {bloom::document::KeyframeId::fromRaw(8001), RationalTime::fromInteger(0), 4.0,
         bloom::runtime::CompiledKeyframeInterpolation::Linear},
        {bloom::document::KeyframeId::fromRaw(8002), RationalTime::fromInteger(1), 5.0,
         bloom::runtime::CompiledKeyframeInterpolation::Linear}};
    curve.components[1] = {
        {bloom::document::KeyframeId::fromRaw(8003), RationalTime::fromInteger(0), 3.0,
         bloom::runtime::CompiledKeyframeInterpolation::Linear},
        {bloom::document::KeyframeId::fromRaw(8004), RationalTime::fromInteger(1), 4.0,
         bloom::runtime::CompiledKeyframeInterpolation::Linear}};
    definition.vec2Curves.push_back(std::move(curve));
    const auto plan = publish(std::move(definition));
    checkParity(expectations, evaluator, plan, requestFor(*plan, rationalTime(1, 2)),
                "animated position");
}

void testCoverageCacheReuse(Expectations& expectations, const CpuCompositionEvaluator& evaluator) {
    auto cache = std::make_shared<bloom::runtime::GpuSceneCoverageCache>(32ULL * 1024ULL * 1024ULL);
    const auto plan = twoLayerPlan(format(16, 12), LayerValues{.position = {4.3, 3.1}},
                                   LayerValues{.position = {2.7, 2.4}}, 6.0, 5.0, 15000);
    checkParity(expectations, evaluator, plan, requestFor(*plan), "cache first build", cache);
    const auto missesAfterFirst = cache->misses();
    const auto hitsAfterFirst = cache->hits();
    checkParity(expectations, evaluator, plan, requestFor(*plan), "cache second build", cache);
    expectations.expect(cache->misses() == missesAfterFirst,
                        "an unchanged geometry subtree does not rasterize again");
    expectations.expect(cache->hits() >= hitsAfterFirst + 2,
                        "the second build serves both coverage layers from the cache");
    expectations.expect(cache->retainedBytes() > 0 &&
                            cache->retainedBytes() <= 32ULL * 1024ULL * 1024ULL,
                        "the coverage cache stays within its byte budget");
}

void testTimeActivation(Expectations& expectations, const CpuCompositionEvaluator& evaluator) {
    auto definition = twoLayerPlan(format(16, 12), LayerValues{.position = {4.3, 3.1}},
                                   LayerValues{.position = {2.7, 2.4}}, 6.0, 5.0, 16000)
                          ->copyDefinition();
    auto& active = std::get<CompiledLayerOutput>(definition.operations[1]);
    active.inPoint = RationalTime::fromInteger(1);
    active.outPoint = RationalTime::fromInteger(3);
    const auto plan = publish(std::move(definition));
    // Before in, exactly in, inside, exactly out, after out. The CPU publishes no image and no
    // bounds outside [inPoint, outPoint); the prepared scene must match pixel-for-pixel and
    // bound-for-bound.
    for (const auto time : {std::int64_t{-1}, std::int64_t{0}, std::int64_t{1}, std::int64_t{2},
                            std::int64_t{3}, std::int64_t{4}}) {
        checkParity(expectations, evaluator, plan,
                    requestFor(*plan, RationalTime::fromInteger(time)),
                    "time activation t=" + std::to_string(time));
    }
    // The same active pixels at different times inside the active range keep the same semantic key.
    const auto first =
        CpuGpuSceneBuilder{}.build(plan, requestFor(*plan, RationalTime::fromInteger(1)));
    const auto second =
        CpuGpuSceneBuilder{}.build(plan, requestFor(*plan, RationalTime::fromInteger(2)));
    expectations.expect(first.hasValue() && second.hasValue(), "both active-time builds prepare");
    if (first && second) {
        const auto keyOf = [](const auto& command) -> const std::string& {
            return std::visit(
                [](const auto& item) -> const std::string& { return item.semanticKey; }, command);
        };
        std::vector<std::string> left;
        std::vector<std::string> right;
        for (const auto& command : first.scene->commands())
            left.push_back(keyOf(command));
        for (const auto& command : second.scene->commands())
            right.push_back(keyOf(command));
        std::ranges::sort(left);
        std::ranges::sort(right);
        expectations.expect(left == right,
                            "the same active pixels keep the same semantic keys across time");
    }
}

void testInactiveAndMuteSolo(Expectations& expectations, const CpuCompositionEvaluator& evaluator) {
    // Mixed: layer A inactive before its range, layer B always active.
    auto mixedDefinition = twoLayerPlan(format(16, 12), LayerValues{.position = {4.3, 3.1}},
                                        LayerValues{.position = {2.7, 2.4}}, 6.0, 5.0, 17000)
                               ->copyDefinition();
    std::get<CompiledLayerOutput>(mixedDefinition.operations[1]).inPoint =
        RationalTime::fromInteger(2);
    const auto mixed = publish(std::move(mixedDefinition));
    checkParity(expectations, evaluator, mixed, requestFor(*mixed, RationalTime::fromInteger(0)),
                "mixed active/inactive");

    // Only layer inactive: an empty merge and an empty composition output.
    auto emptyDefinition = twoLayerPlan(format(16, 12), LayerValues{.position = {4.3, 3.1}},
                                        LayerValues{.position = {2.7, 2.4}}, 6.0, 5.0, 18000)
                               ->copyDefinition();
    std::get<CompiledLayerOutput>(emptyDefinition.operations[1]).inPoint =
        RationalTime::fromInteger(2);
    std::get<CompiledLayerOutput>(emptyDefinition.operations[3]).inPoint =
        RationalTime::fromInteger(2);
    const auto empty = publish(std::move(emptyDefinition));
    checkParity(expectations, evaluator, empty, requestFor(*empty, RationalTime::fromInteger(0)),
                "inactive-only empty merge");

    // Mute/solo lower at COMPILE time: a muted layer's subtree is not in the compiled graph, and a
    // solo leaves only the selected entry. Removing the merge entry alone would leave an
    // unreachable subtree, which preflight rightly rejects, so build the lowered single-layer graph
    // instead.
    const auto base = twoLayerPlan(format(16, 12), LayerValues{.position = {4.3, 3.1}},
                                   LayerValues{.position = {2.7, 2.4}}, 6.0, 5.0, 19000);
    auto lowered = base->copyDefinition();
    std::get<CompiledLayerOutput>(lowered.operations[1]).input = OperationIndex::fromRaw(0);
    auto& merge = std::get<CompiledMerge>(lowered.operations[4]);
    merge.entries = {merge.entries[0]};
    merge.entries[0].input = OperationIndex::fromRaw(1);
    std::get<CompiledCompositionOutput>(lowered.operations[5]).input = OperationIndex::fromRaw(2);
    std::vector<CompiledOperation> ops;
    ops.push_back(std::move(lowered.operations[0]));
    ops.push_back(std::move(lowered.operations[1]));
    ops.push_back(std::move(lowered.operations[4]));
    ops.push_back(std::move(lowered.operations[5]));
    lowered.operations = std::move(ops);
    lowered.output = OperationIndex::fromRaw(3);
    const auto loweredPlan = publish(std::move(lowered));
    checkParity(expectations, evaluator, loweredPlan, requestFor(*loweredPlan),
                "muted/solo lowered single layer");
}

void testBudgetRefusal(Expectations& expectations) {
    const auto plan =
        twoLayerPlan(format(4096, 4096), LayerValues{.position = {2048.0, 2048.0}},
                     LayerValues{.position = {1000.3, 1000.7}}, 4096.0, 4096.0, 21000);
    auto request = requestFor(*plan);
    request.pixelStorageByteLimit = 1U << 20U; // 1 MiB, far below a 4096x4096 RGBA32F output.
    const auto prepared = CpuGpuSceneBuilder{}.build(plan, request);
    expectations.expect(!prepared.hasValue(), "a huge scene under a tiny budget fails closed");
    expectations.expect(!prepared.hasValue() &&
                            prepared.diagnostic.code ==
                                PreparedGpuSceneDiagnosticCode::PixelStorageBudgetExceeded,
                        "the failure is a diagnosed pixel-storage budget, not an allocation throw");
}

#include "gpu_scene_coverage_budget_tests.ipp"

void testUnsupported(Expectations& expectations) {
    const CpuGpuSceneBuilder builder;
    const auto solidPlan = twoLayerPlan(format(8, 8), LayerValues{}, LayerValues{}, 8.0, 8.0, 5000);

    // ROI.
    {
        auto request = requestFor(*solidPlan);
        const auto roiWindow = ImageWindow::create(0, 0, 4, 4);
        request.roi = *roiWindow.value();
        const auto prepared = builder.build(solidPlan, request);
        expectations.expect(!prepared && prepared.diagnostic.code ==
                                             PreparedGpuSceneDiagnosticCode::UnsupportedRequest,
                            "ROI is refused before any work");
    }
    // A non-lin_rec709_scene working space is admitted for the colour-agnostic operations: a solid
    // applies no working-space transform (the CPU reference only premultiplies the authored value),
    // so it now prepares and carries the requested working space in its process identity. A
    // transform that must resolve the working space still fails closed; that path is covered by the
    // media/effect acceptance tests.
    {
        auto request = requestFor(*solidPlan);
        request.colorIntent.workingColorSpaceId = "acescg";
        const auto prepared = builder.build(solidPlan, request);
        expectations.expect(prepared.hasValue(),
                            "a colour-agnostic solid scene prepares under a non-neutral working "
                            "space");
        if (prepared) {
            expectations.expect(prepared.scene->processIdentity().colorIntent.workingColorSpaceId ==
                                    "acescg",
                                "the prepared identity carries the requested working space");
        }
    }
    // An operation kind still outside the prepared subset (nested Composition Source).
    {
        auto definition = solidPlan->copyDefinition();
        definition.operations[0] = bloom::runtime::CompiledCompositionSource{
            bloom::document::NodeId::fromRaw(6000), 0,
            bloom::runtime::CompiledCompositionTimeMapping{
                {bloom::document::ParameterId::fromRaw(6001), 0.0},
                {bloom::document::ParameterId::fromRaw(6002), 1.0},
                0}};
        const auto nested = publish(std::move(definition));
        const auto prepared = builder.build(nested, requestFor(*nested));
        expectations.expect(!prepared && prepared.diagnostic.code ==
                                             PreparedGpuSceneDiagnosticCode::UnsupportedOperation,
                            "an out-of-subset operation is refused before any resolution");
    }
}

void testBlendModes(Expectations& expectations, const CpuCompositionEvaluator& evaluator) {
    using bloom::core::BlendMode;
    const std::array modes{BlendMode::Normal,  BlendMode::Add,       BlendMode::Multiply,
                           BlendMode::Screen,  BlendMode::Overlay,   BlendMode::Darken,
                           BlendMode::Lighten, BlendMode::Difference};
    std::uint64_t idBase = 30000;
    for (const auto mode : modes) {
        const LayerValues top{.position = {2.7, 2.4}, .opacity = 0.75, .blendMode = mode};
        const auto plan = twoLayerPlan(format(16, 12), LayerValues{.position = {4.3, 3.1}}, top,
                                       6.0, 5.0, idBase);
        checkParity(expectations, evaluator, plan, requestFor(*plan),
                    "blend mode " + std::to_string(static_cast<unsigned>(mode)));
        idBase += 100;
    }
}

// Full affine, parent composition and generic (layer-on-layer) input parity against the live CPU
// composition evaluator, including rotated/nonuniform/signed scale, anchor, parented shear, a
// parent outside its active span with an active child, a vector chain through two layers, and a
// proxy.
void testAffineAndParent(Expectations& expectations, const CpuCompositionEvaluator& evaluator) {
    // Rotation on both (fractional) layers.
    {
        const auto plan =
            twoLayerPlan(format(16, 12), LayerValues{.position = {4.3, 3.1}, .rotation = 30.0},
                         LayerValues{.position = {2.7, 2.4}, .rotation = -15.0}, 6.0, 5.0, 40000);
        checkParity(expectations, evaluator, plan, requestFor(*plan), "affine rotation");
    }
    // Nonuniform + signed scale with a nonzero anchor.
    {
        const auto plan = twoLayerPlan(
            format(16, 12),
            LayerValues{.position = {4.3, 3.1}, .anchor = {1.0, -0.5}, .scale = {1.75, -0.5}},
            LayerValues{.position = {2.7, 2.4}, .scale = {-2.0, 0.75}}, 6.0, 5.0, 40100);
        checkParity(expectations, evaluator, plan, requestFor(*plan), "affine signed/anchor");
    }
    // Parented shear: a rotated, nonuniformly scaled parent composed with a rotated child.
    {
        auto definition = twoLayerPlan(format(24, 16),
                                       LayerValues{.position = {12.0, 8.0},
                                                   .anchor = {1.5, -0.5},
                                                   .scale = {1.5, -0.75},
                                                   .rotation = 30.0},
                                       LayerValues{.position = {7.0, 9.0},
                                                   .anchor = {0.25, 0.5},
                                                   .scale = {2.0, 0.5},
                                                   .rotation = -15.0},
                                       9.0, 7.0, 40200)
                              ->copyDefinition();
        std::get<CompiledLayerOutput>(definition.operations[3]).parent = OperationIndex::fromRaw(1);
        const auto plan = publish(std::move(definition));
        checkParity(expectations, evaluator, plan, requestFor(*plan), "parented shear");
    }
    // A parent outside its active span still composes its matrix into the active child.
    {
        auto definition =
            twoLayerPlan(
                format(24, 16),
                LayerValues{.position = {12.0, 8.0}, .scale = {1.5, 1.5}, .rotation = 30.0},
                LayerValues{.position = {7.0, 9.0}, .scale = {0.5, 2.0}, .rotation = 20.0}, 9.0,
                7.0, 40300)
                ->copyDefinition();
        std::get<CompiledLayerOutput>(definition.operations[1]).inPoint =
            RationalTime::fromInteger(2);
        std::get<CompiledLayerOutput>(definition.operations[3]).parent = OperationIndex::fromRaw(1);
        const auto plan = publish(std::move(definition));
        checkParity(expectations, evaluator, plan, requestFor(*plan, RationalTime::fromInteger(0)),
                    "parent outside active span");
    }
    // Generic layer-on-layer input: the child consumes the parent's vector chain, so the original
    // solid geometry rasterizes through the full composed matrix, not an intermediate raster. The
    // unused second solid is removed so every operation stays reachable.
    {
        auto definition =
            twoLayerPlan(
                format(24, 16),
                LayerValues{.position = {12.0, 8.0}, .scale = {1.25, 1.25}, .rotation = 10.0},
                LayerValues{.position = {8.0, 7.0}, .scale = {0.75, 1.5}, .rotation = -12.0}, 10.0,
                8.0, 40400)
                ->copyDefinition();
        auto layerA = std::get<CompiledLayerOutput>(definition.operations[1]);
        auto layerB = std::get<CompiledLayerOutput>(definition.operations[3]);
        layerB.input = OperationIndex::fromRaw(1);
        auto merge = std::get<CompiledMerge>(definition.operations[4]);
        merge.entries = {CompiledMergeInput{merge.entries[1].slotId, layerB.layerId,
                                            OperationIndex::fromRaw(2)}};
        std::vector<CompiledOperation> operations;
        operations.push_back(std::move(definition.operations[0]));
        operations.push_back(std::move(layerA));
        operations.push_back(std::move(layerB));
        operations.push_back(std::move(merge));
        operations.push_back(CompiledCompositionOutput{bloom::document::NodeId::fromRaw(40453),
                                                       OperationIndex::fromRaw(3)});
        definition.operations = std::move(operations);
        definition.output = OperationIndex::fromRaw(4);
        const auto plan = publish(std::move(definition));
        checkParity(expectations, evaluator, plan, requestFor(*plan), "generic layer input");
    }
    // Non-square PAR proxy with affine layers.
    {
        const auto plan =
            twoLayerPlan(format(11, 7, pixelAspect(4, 3)),
                         LayerValues{.position = {5.3, 3.1}, .scale = {1.5, 0.5}, .rotation = 25.0},
                         LayerValues{.position = {2.7, 4.9},
                                     .anchor = {1.0, -0.5},
                                     .scale = {-1.0, 1.5},
                                     .rotation = -40.0},
                         5.0, 4.0, 40500);
        const auto extent = bloom::render::ImageExtent::create(7, 5);
        auto request = requestFor(*plan);
        request.resolution = bloom::runtime::ProxyResolution{*extent.value()};
        checkParity(expectations, evaluator, plan, request, "affine proxy non-square PAR");
    }
}

} // namespace

int main() {
    try {
        Expectations expectations;
        const CpuCompositionEvaluator evaluator;
        testBasicAndMerge(expectations, evaluator);
        testProxyNonSquareParAndCentered(expectations, evaluator);
        testAnimatedOpacity(expectations, evaluator);
        testIntegerNativeGrid(expectations, evaluator);
        testFractionalSignsAndOpacity(expectations, evaluator);
        testAnimatedPosition(expectations, evaluator);
        testCoverageCacheReuse(expectations, evaluator);
        testTimeActivation(expectations, evaluator);
        testInactiveAndMuteSolo(expectations, evaluator);
        testBudgetRefusal(expectations);
        testCoverageHostGeometryBudget(expectations);
        testKeyStability(expectations);
        testTextCoverage(expectations, evaluator);
        testShapeCoverage(expectations, evaluator);
        testBlendModes(expectations, evaluator);
        testAffineAndParent(expectations, evaluator);
        testUnsupported(expectations);
        if (!expectations.ok()) {
            std::cerr << "FAIL: GPU scene preparation expectations failed\n";
            return 1;
        }
        std::cout << "PASS: CPU GPU scene preparation\n";
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << "Unexpected test exception: " << exception.what() << '\n';
        return 1;
    }
}
