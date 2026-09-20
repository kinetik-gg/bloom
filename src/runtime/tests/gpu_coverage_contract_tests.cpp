// CPU-CI gate for the bounded GPU coverage contract. It drives the REAL production
// CpuGpuSceneBuilder over one genuine fixture per Required operation/feature/route/node-type, so a
// pixel operation that only reaches Unsupported is reported by name as a hole. A requirement with
// no genuine fixture is a missing-fixture failure, never a pass.
//
// The native acceptance path is a distinct CTest (`--native-acceptance`): with a device it reports
// the Missing native fixtures and fails; without a device it exits 77 (CTest SKIP). With
// `--require-device` an unavailable device is a hard failure. It never returns a passing status it
// did not earn.
//
// This gate is EXPECTED RED until the required production fixtures exist. It is the gate that
// later integration turns green without weakening the contract or adding opt-outs.

#include "gpu_coverage_fixture_support.hpp"
#include "gpu_coverage_media_support.hpp"
#include "gpu_coverage_video_support.hpp"
#include "gpu_coverage_native_support.hpp"

#include <bloom/document/graph.hpp>
#include <bloom/render/gpu_device.hpp>
#include <bloom/runtime/gpu_coverage_contract.hpp>
#include <bloom/runtime/gpu_scene_cache.hpp>
#include <bloom/runtime/prepared_gpu_scene.hpp>

#include <array>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using bloom::document::NodeId;
using bloom::document::ParameterId;
using bloom::document::ShapeKind;
using bloom::document::Vec2d;
using bloom::gpu_coverage_fixtures::codeName;
using bloom::gpu_coverage_fixtures::format;
using bloom::gpu_coverage_fixtures::LayerValues;
using bloom::gpu_coverage_fixtures::publish;
using bloom::gpu_coverage_fixtures::requestFor;
using bloom::gpu_coverage_fixtures::twoLayerPlan;
using bloom::runtime::CompiledColorParameter;
using bloom::runtime::CompiledCompositionSource;
using bloom::runtime::CompiledImageEffect;
using bloom::runtime::CompiledImageSource;
using bloom::runtime::CompiledOperation;
using bloom::runtime::CompiledScalarParameter;
using bloom::runtime::CompiledShape;
using bloom::runtime::CompiledText;
using bloom::runtime::CompiledTextLayout;
using bloom::runtime::CompiledVec2Parameter;
using bloom::runtime::CompiledVideoSource;
using bloom::runtime::CpuGpuSceneBuilder;
using bloom::runtime::EvaluationRequest;
using bloom::runtime::GpuCoverageFixtureCriterion;
using bloom::runtime::ImageEffectKernel;
using bloom::runtime::OperationIndex;
using bloom::runtime::PreparedGpuScene;
using bloom::runtime::PreparedGpuSceneDiagnosticCode;

// A fixture run retains the REAL prepared scenes (not just a bool) so the native acceptance pass
// can execute the same fixture through the production executor against the CPU oracle. A
// time-mapped fixture (video) carries more than one distinct frame.
struct FrameRun final {
    std::shared_ptr<const bloom::runtime::CompiledCompositionPlan> plan;
    EvaluationRequest request;
    std::filesystem::path baseDirectory;
    std::shared_ptr<const PreparedGpuScene> scene;
};

struct FixtureRun final {
    bool prepared = false;
    std::string evidence;
    std::vector<FrameRun> frames;
};

struct Fixture final {
    std::string id;
    GpuCoverageFixtureCriterion criterion = GpuCoverageFixtureCriterion::Prepared;
    std::string owner;
    std::function<FixtureRun()> run;
};

[[nodiscard]] FixtureRun runBuilder(
    const std::shared_ptr<const bloom::runtime::CompiledCompositionPlan>& plan) {
    const CpuGpuSceneBuilder builder;
    const auto request = requestFor(*plan);
    FixtureRun result;
    const auto prepared = builder.build(plan, request);
    if (prepared) {
        result.prepared = true;
        result.frames.push_back(FrameRun{plan, request, {}, prepared.scene});
        result.evidence =
            "prepared " + std::to_string(prepared.scene->commands().size()) + " commands";
        return result;
    }
    result.evidence = codeName(prepared.diagnostic.code) + ": " + prepared.diagnostic.message;
    return result;
}

[[nodiscard]] FixtureRun runMediaBuilder(
    const std::shared_ptr<const bloom::runtime::CompiledCompositionPlan>& plan,
    const bloom::runtime::CpuCompositionEvaluator& evaluator,
    const std::filesystem::path& baseDirectory) {
    auto context = bloom::runtime::GpuSceneMediaContext::fromEvaluator(evaluator);
    context.assetBaseDirectory = baseDirectory;
    const CpuGpuSceneBuilder builder(nullptr, context);
    const auto request = requestFor(*plan);
    FixtureRun result;
    const auto prepared = builder.build(plan, request);
    if (prepared) {
        result.prepared = true;
        result.frames.push_back(FrameRun{plan, request, baseDirectory, prepared.scene});
        result.evidence = "prepared media " +
                          std::to_string(prepared.scene->commands().size()) + " commands";
        return result;
    }
    result.evidence = codeName(prepared.diagnostic.code) + ": " + prepared.diagnostic.message;
    return result;
}

// Two genuine video frames at distinct composition times, sharing the probed asset/interpretation.
[[nodiscard]] FixtureRun runVideoFixture(const bloom::gpu_coverage_video::VideoFixture& fixture,
                                         const std::uint64_t idBase) {
    FixtureRun result;
    for (std::int64_t frame = 0; frame < 2; ++frame) {
        const bloom::runtime::CpuCompositionEvaluator evaluator;
        auto plan = bloom::gpu_coverage_video::videoPlan(
            fixture.asset, LayerValues{.position = {4.3, 3.1}, .opacity = 1.0}, 0,
            idBase + static_cast<std::uint64_t>(frame) * 100);
        auto request = requestFor(*plan);
        request.time = bloom::core::RationalTime::fromInteger(frame);
        auto context = bloom::runtime::GpuSceneMediaContext::fromEvaluator(evaluator);
        context.assetBaseDirectory = bloom::gpu_coverage_video::mediaFixturesDirectory();
        const CpuGpuSceneBuilder builder(nullptr, context);
        const auto prepared = builder.build(plan, request);
        if (!prepared) {
            result.prepared = false;
            result.evidence = "frame " + std::to_string(frame) + ": " +
                              codeName(prepared.diagnostic.code) + ": " +
                              prepared.diagnostic.message;
            return result;
        }
        result.frames.push_back(FrameRun{plan, request,
                                         bloom::gpu_coverage_video::mediaFixturesDirectory(),
                                         prepared.scene});
    }
    result.prepared = true;
    result.evidence = "prepared video 2 frames";
    return result;
}

[[nodiscard]] std::shared_ptr<const bloom::runtime::CompiledCompositionPlan> basePlan() {
    return twoLayerPlan(format(16, 12), LayerValues{.position = {4.3, 3.1}},
                        LayerValues{.position = {11.5, 8.2}, .opacity = 0.75}, 6.0, 5.0, 90000);
}

[[nodiscard]] std::shared_ptr<const bloom::runtime::CompiledCompositionPlan>
withSource(const CompiledOperation& source, const std::uint64_t idBase,
           const std::shared_ptr<const bloom::runtime::CompiledCompositionPlan>* nested = nullptr) {
    auto definition = basePlan()->copyDefinition();
    definition.operations[0] = source;
    if (nested != nullptr) {
        definition.nestedPlans.push_back(*nested);
    }
    (void)idBase;
    return publish(std::move(definition));
}

[[nodiscard]] CompiledShape shape(const ShapeKind kind, const std::uint64_t idBase) {
    CompiledShape value{};
    value.sourceNodeId = NodeId::fromRaw(idBase);
    value.kind = kind;
    value.size = CompiledVec2Parameter{ParameterId::fromRaw(idBase + 1), Vec2d{6.0, 5.0}};
    value.fillEnabled = true;
    value.fillColor = CompiledColorParameter{ParameterId::fromRaw(idBase + 2),
                                             bloom::core::Color4d{0.8, 0.4, 0.2, 1.0}};
    return value;
}

[[nodiscard]] CompiledText text(const std::uint64_t idBase) {
    return CompiledText{NodeId::fromRaw(idBase),
                        ParameterId::fromRaw(idBase + 1),
                        "gpu",
                        {ParameterId::fromRaw(idBase + 2), 12.0},
                        {ParameterId::fromRaw(idBase + 3),
                         bloom::core::Color4d{1.0, 1.0, 1.0, 1.0}},
                        CompiledTextLayout{ParameterId::fromRaw(idBase + 4), 0,
                                           {ParameterId::fromRaw(idBase + 5), 1.0},
                                           {ParameterId::fromRaw(idBase + 6), 0.0}}};
}

[[nodiscard]] CompiledImageEffect effect(const ImageEffectKernel& kernel,
                                         const std::uint64_t idBase) {
    return CompiledImageEffect{NodeId::fromRaw(idBase), OperationIndex::fromRaw(0), kernel, false,
                               false};
}

[[nodiscard]] std::shared_ptr<const bloom::runtime::CompiledCompositionPlan>
effectPlan(const ImageEffectKernel& kernel, const std::uint64_t idBase) {
    auto definition = basePlan()->copyDefinition();
    auto solid = definition.operations[0];
    auto layer = std::get<bloom::runtime::CompiledLayerOutput>(definition.operations[1]);
    layer.input = OperationIndex::fromRaw(1);
    auto merge = std::get<bloom::runtime::CompiledMerge>(definition.operations[4]);
    merge.entries = {bloom::runtime::CompiledMergeInput{bloom::document::LayerSlotId::fromRaw(idBase),
                                                        layer.layerId,
                                                        OperationIndex::fromRaw(2)}};
    definition.operations.clear();
    definition.operations.push_back(std::move(solid));
    definition.operations.push_back(effect(kernel, idBase + 10));
    definition.operations.push_back(layer);
    definition.operations.push_back(merge);
    definition.operations.push_back(bloom::runtime::CompiledCompositionOutput{
        NodeId::fromRaw(idBase + 30), OperationIndex::fromRaw(3)});
    definition.output = OperationIndex::fromRaw(4);
    return publish(std::move(definition));
}

[[nodiscard]] CompiledOperation compositionSource(const std::uint64_t idBase) {
    CompiledCompositionSource source{};
    source.sourceNodeId = NodeId::fromRaw(idBase);
    source.nestedPlanIndex = 0;
    return source;
}

[[nodiscard]] std::shared_ptr<const bloom::runtime::CompiledCompositionPlan>
modifiedLayer(const std::function<void(bloom::runtime::CompiledLayerOutput&)>& edit) {
    auto definition = basePlan()->copyDefinition();
    edit(std::get<bloom::runtime::CompiledLayerOutput>(definition.operations[1]));
    return publish(std::move(definition));
}

// A fully reachable layer fed by another layer (the generic graph-input axis): every operation is
// reachable from the output, so the refusal must be the layer-input screen, not an invalid plan.
[[nodiscard]] std::shared_ptr<const bloom::runtime::CompiledCompositionPlan>
layerOnLayerPlan(const std::uint64_t idBase) {
    auto definition = basePlan()->copyDefinition();
    auto solid = definition.operations[0];
    auto layerA = std::get<bloom::runtime::CompiledLayerOutput>(definition.operations[1]);
    auto layerB = std::get<bloom::runtime::CompiledLayerOutput>(definition.operations[3]);
    layerB.input = OperationIndex::fromRaw(1);
    bloom::runtime::CompiledMerge merge{
        NodeId::fromRaw(idBase + 1),
        {bloom::runtime::CompiledMergeInput{bloom::document::LayerSlotId::fromRaw(idBase + 2),
                                            layerB.layerId, OperationIndex::fromRaw(2)}}};
    definition.operations.clear();
    definition.operations.push_back(std::move(solid));
    definition.operations.push_back(layerA);
    definition.operations.push_back(layerB);
    definition.operations.push_back(std::move(merge));
    definition.operations.push_back(bloom::runtime::CompiledCompositionOutput{
        NodeId::fromRaw(idBase + 3), OperationIndex::fromRaw(3)});
    definition.output = OperationIndex::fromRaw(4);
    return publish(std::move(definition));
}

[[nodiscard]] std::vector<Fixture> fixtures() {
    std::vector<Fixture> list;
    const auto add = [&list](std::string id, GpuCoverageFixtureCriterion criterion,
                             std::string owner, std::function<FixtureRun()> run) {
        list.push_back(Fixture{std::move(id), criterion, std::move(owner), std::move(run)});
    };
    const auto addImageEffectPlan = [&add](std::string id, const ImageEffectKernel& kernel,
                                           const std::uint64_t idBase, std::string owner) {
        const auto base = std::string{"src/color OCIO image effect tests"};
        add(std::move(id), GpuCoverageFixtureCriterion::Prepared,
            owner.empty() ? base : std::move(owner),
            [kernel, idBase] { return runBuilder(effectPlan(kernel, idBase)); });
    };

    for (const std::string_view id : {"operation.CompiledSolid", "operation.CompiledLayerOutput",
                                      "operation.CompiledMerge",
                                      "operation.CompiledCompositionOutput"}) {
        add(std::string{id}, GpuCoverageFixtureCriterion::Prepared,
            "src/runtime gpu scene preparation tests", [] { return runBuilder(basePlan()); });
    }
    add("operation.CompiledText", GpuCoverageFixtureCriterion::Prepared,
        "src/render text_raster + text source tests",
        [] { return runBuilder(withSource(text(95000), 95000)); });
    add("operation.CompiledShape", GpuCoverageFixtureCriterion::Prepared,
        "src/render PathRaster + shape source tests",
        [] { return runBuilder(withSource(shape(ShapeKind::Rectangle, 96000), 96000)); });
    add("operation.CompiledCompositionSource", GpuCoverageFixtureCriterion::Prepared,
        "src/runtime SnapshotCompiler nested composition tests", [] {
            const auto nested = basePlan();
            return runBuilder(withSource(compositionSource(97000), 97000, &nested));
        });
    addImageEffectPlan("operation.CompiledImageEffect", bloom::runtime::IdentityImageKernel{},
                       98000, "src/color OCIO image effect tests");
    // Real still-image fixture: a synthetic signed/HDR EXR resolved through the production media
    // pipeline. VideoSource has no genuine fixture in this gate yet, so it stays explicitly absent
    // (missing-fixture RED) rather than counting InvalidPlan/MediaUnavailable as a GPU pass.
    add("operation.CompiledImageSource", GpuCoverageFixtureCriterion::Prepared,
        "src/media image source tests", [] {
            const bloom::runtime::CpuCompositionEvaluator evaluator;
            const auto& image = bloom::gpu_coverage_media::syntheticImage();
            const auto plan = bloom::gpu_coverage_media::imageSourcePlan(
                image.asset, LayerValues{.position = {4.3, 3.1}, .opacity = 1.0}, 99000);
            return runMediaBuilder(plan, evaluator, bloom::gpu_coverage_media::gateDirectory());
        });
    add("operation.CompiledVideoSource", GpuCoverageFixtureCriterion::Prepared,
        "src/media video source tests", [] {
            try {
                return runVideoFixture(bloom::gpu_coverage_video::probeVideoFixture(), 111000);
            } catch (const std::exception& error) {
                FixtureRun result;
                result.evidence = error.what();
                return result;
            }
        });

    addImageEffectPlan("effect.IdentityImageKernel", bloom::runtime::IdentityImageKernel{}, 100000,
                       {});
    addImageEffectPlan("effect.CstKernel",
                       bloom::runtime::CstKernel{"lin_rec709_scene", "lin_rec709_scene"}, 101000,
                       {});
    addImageEffectPlan(
        "effect.FileTransformKernel",
        bloom::runtime::FileTransformKernel{bloom::document::AssetId::fromRaw(0), 0, 0,
                                            "lin_rec709_scene", std::nullopt},
        102000, {});

    for (const auto mode : bloom::core::kBlendModes) {
        const auto id = std::string{"feature.blend."} +
                        std::to_string(bloom::core::blendModeStoredValue(mode));
        add(id, GpuCoverageFixtureCriterion::Prepared,
            "src/render GpuComposite + composite parity tests", [mode] {
                return runBuilder(modifiedLayer([mode](
                                                   bloom::runtime::CompiledLayerOutput& layer) {
                    layer.blendMode = mode;
                }));
            });
    }
    constexpr std::array<ShapeKind, 7> kShapeKinds{
        ShapeKind::Rectangle, ShapeKind::Ellipse, ShapeKind::Triangle,
        ShapeKind::Polygon,   ShapeKind::Star,    ShapeKind::Line,
        ShapeKind::Path};
    for (const auto kind : kShapeKinds) {
        const auto id =
            std::string{"feature.shape."} + std::to_string(static_cast<std::int64_t>(kind));
        add(id, GpuCoverageFixtureCriterion::Prepared,
            "src/render PathRaster + shape source tests",
            [kind] { return runBuilder(withSource(shape(kind, 103000), 103000)); });
    }
    add("feature.layer.affine.scale", GpuCoverageFixtureCriterion::Prepared,
        "src/render LayerTransform + gpu scene preparation tests", [] {
            return runBuilder(modifiedLayer([](bloom::runtime::CompiledLayerOutput& layer) {
                layer.scale = CompiledVec2Parameter{layer.scale.id, Vec2d{2.0, 2.0}};
            }));
        });
    add("feature.layer.affine.rotation", GpuCoverageFixtureCriterion::Prepared,
        "src/render LayerTransform + gpu scene preparation tests", [] {
            return runBuilder(modifiedLayer([](bloom::runtime::CompiledLayerOutput& layer) {
                layer.rotation = CompiledScalarParameter{layer.rotation.id, 30.0};
            }));
        });
    add("feature.layer.affine.anchor", GpuCoverageFixtureCriterion::Prepared,
        "src/render LayerTransform + gpu scene preparation tests", [] {
            return runBuilder(modifiedLayer([](bloom::runtime::CompiledLayerOutput& layer) {
                layer.anchor = CompiledVec2Parameter{layer.anchor.id, Vec2d{1.0, -0.5}};
            }));
        });
    add("feature.layer.parent", GpuCoverageFixtureCriterion::Prepared,
        "src/runtime layer_parent_transform tests", [] {
            return runBuilder(modifiedLayer([](bloom::runtime::CompiledLayerOutput& layer) {
                layer.parent = OperationIndex::fromRaw(0);
            }));
        });
    add("feature.layer.generic_input", GpuCoverageFixtureCriterion::Prepared,
        "src/runtime GpuSceneExecutor graph tests",
        [] { return runBuilder(layerOnLayerPlan(104000)); });

    add("feature.display.view_adjust", GpuCoverageFixtureCriterion::NativeRequired,
        "src/runtime gpu_neutral_display qualification tests",
        [] { return FixtureRun{}; });
    add("feature.display.custom_view_transform", GpuCoverageFixtureCriterion::NativeRequired,
        "src/runtime gpu_neutral_display qualification tests",
        [] { return FixtureRun{}; });

    // One fixture identity per built-in authoring node type that produces pixels. A new node type
    // registered against an existing lowering gets a new `node.<typeId>` requirement and this
    // explicit list cannot satisfy it, so it cannot reuse another node's fixture.
    add("node.bloom.solid-source", GpuCoverageFixtureCriterion::Prepared,
        "src/document node_definition_registry tests", [] { return runBuilder(basePlan()); });
    add("node.bloom.layer-output", GpuCoverageFixtureCriterion::Prepared,
        "src/document node_definition_registry tests", [] { return runBuilder(basePlan()); });
    add("node.bloom.layer-stack", GpuCoverageFixtureCriterion::Prepared,
        "src/document node_definition_registry tests", [] { return runBuilder(basePlan()); });
    add("node.bloom.composition-output", GpuCoverageFixtureCriterion::Prepared,
        "src/document node_definition_registry tests", [] { return runBuilder(basePlan()); });
    add("node.bloom.image-source", GpuCoverageFixtureCriterion::Prepared,
        "src/media image source tests", [] {
            const bloom::runtime::CpuCompositionEvaluator evaluator;
            const auto& image = bloom::gpu_coverage_media::syntheticImage();
            const auto plan = bloom::gpu_coverage_media::imageSourcePlan(
                image.asset, LayerValues{.position = {4.3, 3.1}, .opacity = 1.0}, 110000);
            return runMediaBuilder(plan, evaluator, bloom::gpu_coverage_media::gateDirectory());
        });
    add("node.bloom.video-source", GpuCoverageFixtureCriterion::Prepared,
        "src/media video source tests", [] {
            try {
                return runVideoFixture(bloom::gpu_coverage_video::probeVideoFixture(), 113000);
            } catch (const std::exception& error) {
                FixtureRun result;
                result.evidence = error.what();
                return result;
            }
        });
    add("node.bloom.text-source", GpuCoverageFixtureCriterion::Prepared,
        "src/render text_raster + text source tests",
        [] { return runBuilder(withSource(text(105000), 105000)); });
    add("node.bloom.shape-source", GpuCoverageFixtureCriterion::Prepared,
        "src/render PathRaster + shape source tests",
        [] { return runBuilder(withSource(shape(ShapeKind::Rectangle, 106000), 106000)); });
    add("node.bloom.composition-source", GpuCoverageFixtureCriterion::Prepared,
        "src/runtime SnapshotCompiler nested composition tests", [] {
            const auto nested = basePlan();
            return runBuilder(withSource(compositionSource(107000), 107000, &nested));
        });
    addImageEffectPlan("node.bloom.ocio-colour-space-transform",
                       bloom::runtime::CstKernel{"lin_rec709_scene", "lin_rec709_scene"}, 108000,
                       {});
    addImageEffectPlan(
        "node.bloom.ocio-file-transform",
        bloom::runtime::FileTransformKernel{bloom::document::AssetId::fromRaw(0), 0, 0,
                                            "lin_rec709_scene", std::nullopt},
        109000, {});
    return list;
}

// Mutation proof for the node registry: a new node type sharing an existing lowering must gain its
// own required id, with no fixture, so it cannot silently reuse another node's fixture.
void nodeTypeMutationProof(std::vector<std::string>& failures) {
    using bloom::document::NodeDefinition;
    using bloom::document::NodeDefinitionRegistry;
    using bloom::document::NodeRegistrationStatus;
    const auto* solid = bloom::document::builtInNodeDefinitions().find(
        bloom::document::kSolidSourceNodeType, bloom::document::kSolidSourceNodeSchemaVersion);
    if (solid == nullptr) {
        failures.emplace_back("node-type mutation proof: built-in solid definition missing");
        return;
    }
    NodeDefinition synthetic = *solid;
    synthetic.key.typeId = "test.synthetic-image-node";
    NodeDefinitionRegistry registry;
    if (registry.registerDefinition(std::move(synthetic)) != NodeRegistrationStatus::Registered) {
        failures.emplace_back("node-type mutation proof: synthetic definition was rejected");
        return;
    }
    registry.freeze();
    bool found = false;
    for (const auto& entry : bloom::runtime::gpuNodeTypeCoverage(registry)) {
        found = found || entry.id == "node.test.synthetic-image-node";
    }
    if (!found) {
        failures.emplace_back("node-type mutation proof: a new node type escaped node-type coverage");
        return;
    }
    for (const auto& fixture : fixtures()) {
        if (fixture.id == "node.test.synthetic-image-node") {
            failures.emplace_back("node-type mutation proof: synthetic node unexpectedly fixtured");
            return;
        }
    }
    std::cout << "MUTATION node-type: a new node type sharing the Solid lowering gains a required "
                 "id with no fixture (reported RED)\n";
}

[[nodiscard]] std::vector<std::string> requiredCoverageIds() {
    std::vector<std::string> ids;
    for (const auto& entry : bloom::runtime::gpuOperationCoverage()) {
        if (entry.disposition == bloom::runtime::GpuCoverageDisposition::Required) {
            ids.push_back(entry.id);
        }
    }
    for (const auto& entry : bloom::runtime::gpuImageEffectCoverage()) {
        if (entry.disposition == bloom::runtime::GpuCoverageDisposition::Required) {
            ids.push_back(entry.id);
        }
    }
    for (const auto& entry : bloom::runtime::gpuFeatureCoverage()) {
        if (entry.disposition == bloom::runtime::GpuCoverageDisposition::Required) {
            ids.push_back(entry.id);
        }
    }
    for (const auto& route : bloom::runtime::gpuRenderRouteCoverage()) {
        ids.emplace_back(route.id);
    }
    for (const auto& entry :
         bloom::runtime::gpuNodeTypeCoverage(bloom::document::builtInNodeDefinitions())) {
        if (entry.disposition == bloom::runtime::GpuCoverageDisposition::Required) {
            ids.push_back(entry.id);
        }
    }
    return ids;
}

[[nodiscard]] const Fixture* findFixture(const std::vector<Fixture>& list, const std::string& id) {
    for (const auto& fixture : list) {
        if (fixture.id == id) {
            return &fixture;
        }
    }
    return nullptr;
}

[[nodiscard]] std::string_view routeOwner(const std::string& id) {
    for (const auto& route : bloom::runtime::gpuRenderRouteCoverage()) {
        if (route.id == id) {
            return route.owner;
        }
    }
    return {};
}

[[nodiscard]] int runNativeAcceptance(const std::filesystem::path& loader,
                                      const bool requireDevice) {
    bloom::render::GpuDeviceCreationOptions options;
    options.loader_path = loader;
    auto device = bloom::render::GpuDevice::create(options);
    if (!device) {
        if (requireDevice) {
            std::cerr << "FAIL: --require-device was requested but no native GPU device is "
                         "available: "
                      << device.diagnostic.message << '\n';
            return 1;
        }
        std::cerr << "SKIP: no native GPU device available: " << device.diagnostic.message << '\n';
        return 77;
    }
    const auto list = fixtures();
    std::size_t passed = 0;
    std::size_t failed = 0;
    std::size_t missing = 0;
    for (const auto& id : requiredCoverageIds()) {
        // Routes are only absent until their real production harness is integrated; this executor
        // helper must never be relabelled as a viewer/RAM/export route proof.
        if (id.rfind("route.", 0) == 0) {
            std::cerr << "MISSING(native-route) " << id << " (owner " << routeOwner(id) << ")\n";
            ++missing;
            continue;
        }
        const auto* fixture = findFixture(list, id);
        if (fixture == nullptr) {
            std::cerr << "MISSING(no-fixture) " << id << '\n';
            ++missing;
            continue;
        }
        if (fixture->criterion == GpuCoverageFixtureCriterion::NativeRequired) {
            std::cerr << "MISSING(native-only) " << id << " (owner " << fixture->owner << ")\n";
            ++missing;
            continue;
        }
        const auto run = fixture->run();
        if (!run.prepared || run.frames.empty()) {
            std::cerr << "FAIL(no-gpu-prep) " << id << ": " << run.evidence << '\n';
            ++failed;
            continue;
        }
        bool allFrames = true;
        std::string frameEvidence;
        for (const auto& frame : run.frames) {
            const bloom::runtime::CpuCompositionEvaluator evaluator;
            if (!frame.baseDirectory.empty()) {
                evaluator.setAssetBaseDirectory(frame.baseDirectory);
            }
            const auto outcome = bloom::gpu_coverage_native::runNativeFixture(
                *device.device, evaluator, frame.plan, frame.request, frame.scene);
            if (!outcome.passed) {
                allFrames = false;
                frameEvidence = outcome.evidence;
                break;
            }
            frameEvidence = outcome.evidence;
        }
        if (allFrames) {
            std::cout << "PASS " << id << ": " << frameEvidence << '\n';
            ++passed;
        } else {
            std::cerr << "FAIL " << id << ": " << frameEvidence << '\n';
            ++failed;
        }
    }
    std::cout << "\nNATIVE coverage: " << passed << " pass, " << failed << " fail, " << missing
              << " missing\n";
    return (failed == 0 && missing == 0) ? 0 : 1;
}

} // namespace

int main(int argc, char** argv) {
    bool requireDevice = false;
    bool nativeAcceptance = false;
    std::filesystem::path loader;
    for (int i = 1; i < argc; ++i) {
        const std::string_view argument{argv[i]};
        if (argument == "--require-device") {
            requireDevice = true;
        } else if (argument == "--native-acceptance") {
            nativeAcceptance = true;
        } else if (argument == "--loader" && i + 1 < argc) {
            loader = argv[++i];
        } else if (argument == "--media-fixtures" && i + 1 < argc) {
            bloom::gpu_coverage_video::mediaFixturesDirectory() = argv[++i];
        }
    }
    if (nativeAcceptance || requireDevice) {
        return runNativeAcceptance(loader, requireDevice);
    }

    std::vector<std::string> failures;
    for (const auto& issue : bloom::runtime::validateGpuCoverageContract()) {
        failures.push_back("contract: " + issue.detail);
    }
    nodeTypeMutationProof(failures);

    const auto requiredIds = requiredCoverageIds();
    const auto list = fixtures();
    for (const auto& id : requiredIds) {
        bool present = false;
        for (const auto& fixture : list) {
            present = present || fixture.id == id;
        }
        if (!present) {
            failures.push_back("missing required fixture for '" + id + "'");
        }
    }
    for (const auto& fixture : list) {
        bool known = false;
        for (const auto& id : requiredIds) {
            known = known || id == fixture.id;
        }
        if (!known) {
            failures.push_back("fixture '" + fixture.id + "' has no Required contract entry");
        }
    }

    std::size_t passed = 0;
    std::size_t red = 0;
    std::size_t notRun = 0;
    for (const auto& fixture : list) {
        if (fixture.criterion == GpuCoverageFixtureCriterion::NativeRequired) {
            std::cout << "NOTRUN " << fixture.id << " (native fixture owned by " << fixture.owner
                      << ")\n";
            ++notRun;
            continue;
        }
        const auto result = fixture.run();
        if (result.prepared) {
            std::cout << "PASS " << fixture.id << ": " << result.evidence << '\n';
            ++passed;
        } else {
            std::cout << "FAIL " << fixture.id << ": " << result.evidence << '\n';
            failures.push_back("required coverage hole '" + fixture.id + "': " + result.evidence);
            ++red;
        }
    }

    std::cout << "\nGPU coverage contract: " << passed << " pass, " << red << " required holes, "
              << notRun << " native-not-run\n";
    if (!failures.empty()) {
        std::cerr << "\nGPU COVERAGE GATE RED (" << failures.size() << " failures):\n";
        for (const auto& failure : failures) {
            std::cerr << "  - " << failure << '\n';
        }
        return 1;
    }
    return 0;
}
