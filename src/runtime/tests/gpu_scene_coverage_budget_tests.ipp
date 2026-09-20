// Builder retained-host-byte acceptance for vector coverage: the charge is the ACTUAL
// PathRasterCoverageGeometry (row ranges + spans), never width*height R8, and a geometry
// shared through the cache is charged once. Included by gpu_scene_preparation_tests.cpp
// inside its anonymous namespace.

// The actual host-retained byte size of one prepared covered command, mirroring the production
// helper: the immutable geometry's row ranges + spans, or the integer-grid text bitmap.
[[nodiscard]] std::uint64_t
coverageHostBytes(const bloom::runtime::GpuSceneCoverageSolidCommand& command) {
    if (command.geometry != nullptr) {
        return static_cast<std::uint64_t>(command.geometry->rows.size()) *
                   sizeof(bloom::render::PathRasterCoverageRange) +
               static_cast<std::uint64_t>(command.geometry->spans.size()) *
                   sizeof(bloom::render::PathRasterCoverageSpan);
    }
    return command.coverage != nullptr ? static_cast<std::uint64_t>(command.coverage->size()) : 0;
}

// Unique retained coverage bytes over a prepared scene, deduped by representation identity.
[[nodiscard]] std::uint64_t uniqueCoverageHostBytes(const bloom::runtime::PreparedGpuScene& scene) {
    std::vector<const void*> seen;
    std::uint64_t total = 0;
    for (const auto& command : scene.commands()) {
        const auto* covered = std::get_if<bloom::runtime::GpuSceneCoverageSolidCommand>(&command);
        if (covered == nullptr) {
            continue;
        }
        const void* identity = covered->coverageIdentity();
        if (identity == nullptr || std::ranges::find(seen, identity) != seen.end()) {
            continue;
        }
        seen.push_back(identity);
        total += coverageHostBytes(*covered);
    }
    return total;
}

[[nodiscard]] std::size_t
uniqueCoverageRepresentations(const bloom::runtime::PreparedGpuScene& scene) {
    std::vector<const void*> seen;
    for (const auto& command : scene.commands()) {
        const auto* covered = std::get_if<bloom::runtime::GpuSceneCoverageSolidCommand>(&command);
        if (covered == nullptr || covered->coverageIdentity() == nullptr) {
            continue;
        }
        if (std::ranges::find(seen, covered->coverageIdentity()) == seen.end()) {
            seen.push_back(covered->coverageIdentity());
        }
    }
    return seen.size();
}

// A small composition with a large vector layer: the preflight working set is the composition
// output, while the coverage window is the layer's own support bounds. The retained coverage charge
// is therefore the binding constraint, so a width*height R8 charge refuses and the actual geometry
// charge admits.
void testCoverageHostGeometryBudget(Expectations& expectations) {
    constexpr std::size_t kBudget = 1U << 20U;
    const auto simplePlan = shapePlan(
        format(128, 128), LayerValues{.position = {64.3, 64.7}},
        ShapeValues{.kind = bloom::document::ShapeKind::Rectangle, .size = {6000.0, 4000.0}},
        250000);
    const auto simple = CpuGpuSceneBuilder{}.build(
        simplePlan, requestFor(*simplePlan, RationalTime::fromInteger(0), kBudget));
    expectations.expect(simple.hasValue(),
                        "a 6000x4000 vector prepares under a budget below its 24M R8 mask");
    if (!simple) {
        std::cerr << "simple diagnostic: " << simple.diagnostic.message << "\n";
        return;
    }
    const std::uint64_t simpleBytes = uniqueCoverageHostBytes(*simple.scene);
    const std::uint64_t r8Bytes = 6000ULL * 4000ULL;
    expectations.expect(simpleBytes > 0 && simpleBytes < r8Bytes,
                        "the retained geometry is far below the width*height R8 mask");
    expectations.expect(simpleBytes < kBudget,
                        "the actual geometry is what the builder charges against the allowance");

    // Complexity-heavy: a many-point star retains far more spans than the simple rectangle, so the
    // same allowance refuses it on its ACTUAL bytes (still far above the R8 mask).
    const auto heavyPlan = shapePlan(format(128, 128), LayerValues{.position = {64.3, 64.7}},
                                     ShapeValues{.kind = bloom::document::ShapeKind::Star,
                                                 .size = {6000.0, 4000.0},
                                                 .points = 64,
                                                 .innerRatio = 0.5},
                                     251000);
    const auto heavy = CpuGpuSceneBuilder{}.build(
        heavyPlan, requestFor(*heavyPlan, RationalTime::fromInteger(0), 1U << 28U));
    expectations.expect(heavy.hasValue(), "the complexity-heavy vector prepares under a large cap");
    if (!heavy) {
        std::cerr << "heavy diagnostic: " << heavy.diagnostic.message << "\n";
    }
    if (heavy) {
        const std::uint64_t heavyBytes = uniqueCoverageHostBytes(*heavy.scene);
        expectations.expect(heavyBytes > simpleBytes,
                            "the heavy geometry retains more host bytes than the simple one");
        const auto refused = CpuGpuSceneBuilder{}.build(
            heavyPlan, requestFor(*heavyPlan, RationalTime::fromInteger(0), kBudget));
        expectations.expect(
            !refused.hasValue() && refused.diagnostic.code ==
                                       PreparedGpuSceneDiagnosticCode::PixelStorageBudgetExceeded,
            "the complexity-heavy geometry is refused when its actual bytes exceed");
        const auto heavyBoundary = CpuGpuSceneBuilder{}.build(
            heavyPlan, requestFor(*heavyPlan, RationalTime::fromInteger(0), heavyBytes));
        expectations.expect(heavyBoundary.hasValue(),
                            "the heavy geometry is admitted exactly at its actual byte boundary");
        const auto heavyBelow = CpuGpuSceneBuilder{}.build(
            heavyPlan, requestFor(*heavyPlan, RationalTime::fromInteger(0), heavyBytes - 1));
        expectations.expect(!heavyBelow.hasValue() &&
                                heavyBelow.diagnostic.code ==
                                    PreparedGpuSceneDiagnosticCode::PixelStorageBudgetExceeded,
                            "one byte below the heavy geometry is refused at the boundary");
    }

    // Two identical coverage layers share one geometry object through the cache, so it is charged
    // once; without the cache the two independently built geometries are distinct.
    const auto sharedPlan = twoLayerPlan(format(64, 64), LayerValues{.position = {32.3, 32.3}},
                                         LayerValues{.position = {32.3, 32.3}}, 6.0, 5.0, 252000);
    auto cache = std::make_shared<bloom::runtime::GpuSceneCoverageCache>(8ULL * 1024ULL * 1024ULL);
    const auto shared = CpuGpuSceneBuilder{cache}.build(
        sharedPlan, requestFor(*sharedPlan, RationalTime::fromInteger(0), 1U << 28U));
    expectations.expect(shared.hasValue(), "the shared-geometry scene prepares with the cache");
    if (shared) {
        expectations.expect(uniqueCoverageRepresentations(*shared.scene) == 1,
                            "a geometry shared through the cache is one retained representation");
    }
    const auto unshared = CpuGpuSceneBuilder{}.build(
        sharedPlan, requestFor(*sharedPlan, RationalTime::fromInteger(0), 1U << 28U));
    expectations.expect(unshared.hasValue(), "the same scene prepares without the cache");
    if (unshared) {
        expectations.expect(uniqueCoverageRepresentations(*unshared.scene) == 2,
                            "without the cache the two geometries are distinct and charged twice");
    }
}
