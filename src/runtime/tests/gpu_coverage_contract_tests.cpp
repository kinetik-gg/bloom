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

#include "gpu_coverage_contract_plans.hpp"
#include "gpu_coverage_display_support.hpp"
#include "gpu_coverage_fixture_support.hpp"
#include "gpu_coverage_gate_support.hpp"
#include "gpu_coverage_media_support.hpp"
#include "gpu_coverage_mutation_support.hpp"
#include "gpu_coverage_native_support.hpp"
#include "gpu_coverage_ocio_support.hpp"
#include "gpu_coverage_route_proof_support.hpp"
#include "gpu_coverage_video_support.hpp"
#include "gpu_coverage_working_space_support.hpp"
#include "gpu_route_proof_io.hpp"

#include <bloom/document/graph.hpp>
#include <bloom/render/gpu_device.hpp>
#include <bloom/runtime/gpu_coverage_contract.hpp>
#include <bloom/runtime/gpu_coverage_route_proof_contract.hpp>
#include <bloom/runtime/gpu_scene_cache.hpp>
#include <bloom/runtime/input_color_context.hpp>
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
using bloom::runtime::PreparedGpuSceneDiagnosticCode;

using bloom::gpu_coverage_gate::findFixture;
using bloom::gpu_coverage_gate::Fixture;
using bloom::gpu_coverage_gate::FixtureRun;
using bloom::gpu_coverage_gate::FrameRun;
using bloom::gpu_coverage_gate::requiredCoverageIds;
using bloom::gpu_coverage_gate::routeOwner;
using bloom::gpu_coverage_plans::AffineAxis;
using bloom::gpu_coverage_plans::basePlan;
using bloom::gpu_coverage_plans::effectPlan;
using bloom::gpu_coverage_plans::layerOnLayerPlan;
using bloom::gpu_coverage_plans::modifiedLayer;
using bloom::gpu_coverage_plans::nestedBranchReusePlans;
using bloom::gpu_coverage_plans::nestedChildPlan;
using bloom::gpu_coverage_plans::nestedParentPlan;
using bloom::gpu_coverage_plans::parentedPlan;
using bloom::gpu_coverage_plans::rasterAffinePlan;
using bloom::gpu_coverage_plans::shape;
using bloom::gpu_coverage_plans::text;
using bloom::gpu_coverage_plans::withSource;

[[nodiscard]] FixtureRun
runBuilder(const std::shared_ptr<const bloom::runtime::CompiledCompositionPlan>& plan) {
    const CpuGpuSceneBuilder builder(nullptr, {}, bloom::gpu_coverage_ocio::context());
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

[[nodiscard]] FixtureRun
runMediaBuilder(const std::shared_ptr<const bloom::runtime::CompiledCompositionPlan>& plan,
                const bloom::runtime::CpuCompositionEvaluator& evaluator,
                const std::filesystem::path& baseDirectory) {
    auto context = bloom::runtime::GpuSceneMediaContext::fromEvaluator(evaluator);
    context.assetBaseDirectory = baseDirectory;
    const CpuGpuSceneBuilder builder(nullptr, context, bloom::gpu_coverage_ocio::context());
    const auto request = requestFor(*plan);
    FixtureRun result;
    const auto prepared = builder.build(plan, request);
    if (prepared) {
        result.prepared = true;
        result.frames.push_back(FrameRun{plan, request, baseDirectory, prepared.scene});
        result.evidence =
            "prepared media " + std::to_string(prepared.scene->commands().size()) + " commands";
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
        const CpuGpuSceneBuilder builder(nullptr, context, bloom::gpu_coverage_ocio::context());
        const auto prepared = builder.build(plan, request);
        if (!prepared) {
            result.prepared = false;
            result.evidence = "frame " + std::to_string(frame) + ": " +
                              codeName(prepared.diagnostic.code) + ": " +
                              prepared.diagnostic.message;
            return result;
        }
        result.frames.push_back(FrameRun{
            plan, request, bloom::gpu_coverage_video::mediaFixturesDirectory(), prepared.scene});
    }
    result.prepared = true;
    result.evidence = "prepared video 2 frames";
    return result;
}

// A genuine non-identity CST effect: the resolved Bloom Neutral process space -> its sRGB texture
// space. The shared OCIO context compiles the real ProcessEffect command, so the native gate
// dispatches an OCIO kernel instead of the identity alias a same-from/to CST would produce.
[[nodiscard]] FixtureRun runCstEffectFixture(const std::uint64_t idBase) {
    const auto config = bloom::runtime::detail::resolveInputColorConfig(
        bloom::runtime::EvaluationColorIntent::LinearRec709Scene);
    if (!config.has_value()) {
        FixtureRun result;
        result.evidence = "the Bloom Neutral OCIO config is unavailable";
        return result;
    }
    return runBuilder(effectPlan(bloom::runtime::CstKernel{std::string{config->processColorSpaceId()},
                                                           std::string{
                                                               config->sRgbTextureColorSpaceId()}},
                                 idBase));
}

// A genuine non-identity FileTransform: a real 1D .cube read back through color::readLutFile. The
// builder emits the accepted LUT chain and the CPU oracle consumes the identical digest.
[[nodiscard]] FixtureRun runFileTransformFixture(const std::uint64_t idBase) {
    const bloom::runtime::CpuCompositionEvaluator evaluator;
    const auto& lut = bloom::gpu_coverage_media::syntheticLut();
    const bloom::runtime::FileTransformKernel kernel{lut.asset.id, 0, 0, "lin_rec709_scene",
                                                     lut.asset};
    return runMediaBuilder(effectPlan(kernel, idBase), evaluator,
                           bloom::gpu_coverage_media::gateDirectory());
}

// The working-space colour transform axis: a mixed solid/text/media/effect ACEScg composition. It
// uses the production media context (so the real EXR is decoded) plus the shared OCIO context (so
// the CST legs and the media input transform are genuine GPU ProcessEffects). The request carries
// the exact ACES built-in identity, and the native gate compares the GPU result to the unchanged
// CPU evaluator at 2e-6.
[[nodiscard]] FixtureRun runWorkingSpaceFixture() {
    const bloom::runtime::CpuCompositionEvaluator evaluator;
    const auto asset = bloom::gpu_coverage_working_space::textureInputAsset();
    const auto plan = bloom::gpu_coverage_working_space::mixedPlan(
        format(8, 8), asset, bloom::core::Color4d{-0.2, 1.6, 0.35, 0.5}, 130000);
    auto context = bloom::runtime::GpuSceneMediaContext::fromEvaluator(evaluator);
    context.assetBaseDirectory = bloom::gpu_coverage_media::gateDirectory();
    const CpuGpuSceneBuilder builder(nullptr, context, bloom::gpu_coverage_ocio::context());
    const auto request = bloom::gpu_coverage_working_space::acesRequest(*plan);
    FixtureRun result;
    const auto prepared = builder.build(plan, request);
    if (prepared) {
        result.prepared = true;
        result.frames.push_back(FrameRun{plan, request, bloom::gpu_coverage_media::gateDirectory(),
                                         prepared.scene});
        result.evidence =
            "prepared working-space " + std::to_string(prepared.scene->commands().size()) +
            " commands";
        return result;
    }
    result.evidence = codeName(prepared.diagnostic.code) + ": " + prepared.diagnostic.message;
    return result;
}

// A native-only display proof runner: the display support function reports its own genuine evidence.
[[nodiscard]] bloom::gpu_coverage_gate::NativeProofRunner
displayProofRunner(const bool customView) {
    return [customView](bloom::render::GpuDevice& device,
                        const bloom::runtime::GpuSceneOcioContext& ocioContext,
                        std::string& evidence) {
        const auto outcome =
            bloom::gpu_coverage_display::runDisplayProof(device, ocioContext, customView);
        evidence = outcome.evidence;
        return outcome.passed;
    };
}

[[nodiscard]] std::vector<Fixture> fixtures() {
    std::vector<Fixture> list;
    const auto add = [&list](std::string id, GpuCoverageFixtureCriterion criterion,
                             std::string owner, std::function<FixtureRun()> run,
                             bloom::gpu_coverage_gate::NativeProofRunner nativeProof = {}) {
        list.push_back(Fixture{std::move(id), criterion, std::move(owner), std::move(run),
                               std::move(nativeProof)});
    };
    const auto addImageEffectPlan = [&add](std::string id, const ImageEffectKernel& kernel,
                                           const std::uint64_t idBase, std::string owner) {
        const auto base = std::string{"src/color OCIO image effect tests"};
        add(std::move(id), GpuCoverageFixtureCriterion::Prepared,
            owner.empty() ? base : std::move(owner),
            [kernel, idBase] { return runBuilder(effectPlan(kernel, idBase)); });
    };

    for (const std::string_view id :
         {"operation.CompiledSolid", "operation.CompiledLayerOutput", "operation.CompiledMerge",
          "operation.CompiledCompositionOutput"}) {
        add(std::string{id}, GpuCoverageFixtureCriterion::Prepared,
            "src/runtime gpu scene preparation tests", [] { return runBuilder(basePlan()); });
    }
    add("operation.CompiledText", GpuCoverageFixtureCriterion::Prepared,
        "src/render text_raster + text source tests",
        [] { return runBuilder(withSource(text(95000), 95000, nullptr, 0.6)); });
    add("operation.CompiledShape", GpuCoverageFixtureCriterion::Prepared,
        "src/render PathRaster + shape source tests", [] {
            return runBuilder(
                withSource(shape(ShapeKind::Rectangle, 96000, true), 96000, nullptr, 0.7));
        });
    add("operation.CompiledCompositionSource", GpuCoverageFixtureCriterion::Prepared,
        "src/runtime gpu nested scene preparation tests", [] {
            const auto child =
                nestedChildPlan(120000, 201, bloom::core::Color4d{0.125, 0.375, 0.75, 0.5});
            return runBuilder(nestedParentPlan(child, 121000, 100));
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
    add("effect.CstKernel", GpuCoverageFixtureCriterion::Prepared,
        "src/color OCIO image effect tests", [] { return runCstEffectFixture(101000); });
    add("effect.FileTransformKernel", GpuCoverageFixtureCriterion::Prepared,
        "src/color OCIO image effect tests", [] { return runFileTransformFixture(102000); });

    // Every one of the eight blend modes is emitted by the real production builder now: Normal
    // stays the retained SourceOver merge, every other mode an explicit BlendV1 fold.
    for (const auto mode : bloom::core::kBlendModes) {
        const auto id =
            std::string{"feature.blend."} + std::to_string(bloom::core::blendModeStoredValue(mode));
        add(id, GpuCoverageFixtureCriterion::Prepared,
            "src/render GpuComposite + gpu scene blend integration tests", [mode] {
                return runBuilder(modifiedLayer([mode](bloom::runtime::CompiledLayerOutput& layer) {
                    layer.blendMode = mode;
                }));
            });
    }
    constexpr std::array<ShapeKind, 7> kShapeKinds{
        ShapeKind::Rectangle, ShapeKind::Ellipse, ShapeKind::Triangle, ShapeKind::Polygon,
        ShapeKind::Star,      ShapeKind::Line,    ShapeKind::Path};
    for (const auto kind : kShapeKinds) {
        const auto id =
            std::string{"feature.shape."} + std::to_string(static_cast<std::int64_t>(kind));
        add(id, GpuCoverageFixtureCriterion::Prepared, "src/render PathRaster + shape source tests",
            [kind] { return runBuilder(withSource(shape(kind, 103000), 103000)); });
    }
    // The native vector-coverage axis: the production builder emits immutable PathRaster coverage
    // geometry (never a host mask) and the native gate must observe a real GpuPathCoverage compute
    // dispatch for it, cold and zero warm. A fill+stroke shape exercises both coverage passes.
    add("feature.geometry.vector_coverage", GpuCoverageFixtureCriterion::Prepared,
        "src/runtime GpuSceneExecutor native coverage tests",
        [] { return runBuilder(withSource(shape(ShapeKind::Ellipse, 107500, true), 107500)); });
    // A layer over a raster merge result is a raster input, so the production builder must emit the
    // accepted GpuAffine command for each transform axis (scale, rotation, anchor).
    add("feature.layer.affine.scale", GpuCoverageFixtureCriterion::Prepared,
        "src/render LayerTransform + gpu scene affine integration tests",
        [] { return runBuilder(rasterAffinePlan(AffineAxis::Scale, 112000)); });
    add("feature.layer.affine.rotation", GpuCoverageFixtureCriterion::Prepared,
        "src/render LayerTransform + gpu scene affine integration tests",
        [] { return runBuilder(rasterAffinePlan(AffineAxis::Rotation, 112100)); });
    add("feature.layer.affine.anchor", GpuCoverageFixtureCriterion::Prepared,
        "src/render LayerTransform + gpu scene affine integration tests",
        [] { return runBuilder(rasterAffinePlan(AffineAxis::Anchor, 112200)); });
    // A real parented layer: layer B composes layer A's matrix (its parent), so the parented shear
    // is prepared through the production builder.
    add("feature.layer.parent", GpuCoverageFixtureCriterion::Prepared,
        "src/runtime layer_parent_transform tests",
        [] { return runBuilder(parentedPlan(112300)); });
    add("feature.layer.generic_input", GpuCoverageFixtureCriterion::Prepared,
        "src/runtime GpuSceneExecutor graph tests",
        [] { return runBuilder(layerOnLayerPlan(104000)); });
    // Working-space colour conversion is a pixel transformation: a mixed solid/text/media/effect
    // ACEScg composition prepared by the production builder with the shared OCIO context, then
    // executed and compared to the unchanged CPU evaluator at 2e-6. The owner is this gate's
    // native working-space proof, never an external test name.
    add("feature.color.working_space_transform", GpuCoverageFixtureCriterion::Prepared,
        "src/runtime GpuSceneBuilder working-space effect native proof",
        [] { return runWorkingSpaceFixture(); });

    // Display features are native-only. Each owns a genuine native proof in this gate: a real OCIO
    // display program compiled with the shared context and dispatched through the production
    // executor, compared to the CPU display oracle at one RGBA8 code with exact alpha. The
    // view-adjust proof uses a non-neutral exposure/gamma; the custom-view proof uses a non-default
    // display/view pair.
    add("feature.display.view_adjust", GpuCoverageFixtureCriterion::NativeRequired,
        "src/runtime GpuSceneExecutor display view-adjust native proof",
        [] { return FixtureRun{}; }, displayProofRunner(false));
    add("feature.display.custom_view_transform", GpuCoverageFixtureCriterion::NativeRequired,
        "src/runtime GpuSceneExecutor custom display/view native proof",
        [] { return FixtureRun{}; }, displayProofRunner(true));

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
        [] { return runBuilder(withSource(text(105000), 105000, nullptr, 0.6)); });
    add("node.bloom.shape-source", GpuCoverageFixtureCriterion::Prepared,
        "src/render PathRaster + shape source tests",
        [] { return runBuilder(withSource(shape(ShapeKind::Rectangle, 106000, true), 106000)); });
    add("node.bloom.composition-source", GpuCoverageFixtureCriterion::Prepared,
        "src/runtime gpu nested scene preparation tests", [] {
            const auto child =
                nestedChildPlan(122000, 203, bloom::core::Color4d{0.2, 0.6, 0.9, 1.0});
            return runBuilder(nestedParentPlan(child, 123000, 101));
        });
    add("node.bloom.ocio-colour-space-transform", GpuCoverageFixtureCriterion::Prepared,
        "src/color OCIO image effect tests", [] { return runCstEffectFixture(108000); });
    add("node.bloom.ocio-file-transform", GpuCoverageFixtureCriterion::Prepared,
        "src/color OCIO image effect tests", [] { return runFileTransformFixture(109000); });
    return list;
}

// Mutation proof for the node registry: a new node type sharing an existing lowering must gain its
// own required id, with no fixture, so it cannot silently reuse another node's fixture.
[[nodiscard]] int runNativeAcceptance(const std::filesystem::path& loader, const bool requireDevice,
                                      const std::filesystem::path& routeProofDirectory) {
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
    // Genuine route proofs are produced by the real harnesses and handed off through the run-scoped
    // directory. No proof is invented here: when the directory or nonce is absent, or a harness did
    // not run, every route stays MISSING.
    std::string routeNonce;
    std::string routeProofUnavailable;
    std::vector<bloom::gpu_route_proof_io::RouteProofLoadResult> routeProofs;
    if (!routeProofDirectory.empty()) {
        if (bloom::gpu_route_proof_io::readRunNonce(routeProofDirectory, routeNonce) !=
            bloom::gpu_route_proof_io::RouteProofIoStatus::Ok) {
            routeProofUnavailable = "no fresh run nonce in " + routeProofDirectory.string();
        } else {
            routeProofs =
                bloom::gpu_route_proof_io::readKnownRouteProofs(routeProofDirectory, routeNonce);
        }
    }
    const auto findRouteProof = [&routeProofs](const std::string& id) {
        for (const auto& result : routeProofs) {
            if (result.routeId == id) {
                return &result;
            }
        }
        return static_cast<const bloom::gpu_route_proof_io::RouteProofLoadResult*>(nullptr);
    };
    std::size_t passed = 0;
    std::size_t failed = 0;
    std::size_t missing = 0;
    for (const auto& id : requiredCoverageIds()) {
        // Routes are covered only by a genuine proof from their real harness; this executor helper
        // is never relabelled as a viewer/RAM/export route proof.
        if (id.rfind("route.", 0) == 0) {
            if (routeProofDirectory.empty()) {
                std::cerr << "MISSING(native-route) " << id << " (owner " << routeOwner(id)
                          << ")\n";
                ++missing;
                continue;
            }
            const auto* proof = findRouteProof(id);
            if (proof != nullptr && proof->accepted) {
                std::cout << "PASS(route-proof) " << id << ": nonce " << routeNonce << ", frames "
                          << proof->proof.verifiedFrames << ", submissions "
                          << proof->proof.readbackSubmissions << ", payloads "
                          << proof->proof.payloads << ", bytes " << proof->proof.transferredBytes
                          << '\n';
                ++passed;
            } else {
                std::cerr << "MISSING(route-proof) " << id << ": "
                          << (proof == nullptr ? routeProofUnavailable : proof->detail)
                          << " (owner " << routeOwner(id) << ")\n";
                ++missing;
            }
            continue;
        }
        const auto* fixture = findFixture(list, id);
        if (fixture == nullptr) {
            std::cerr << "MISSING(no-fixture) " << id << '\n';
            ++missing;
            continue;
        }
        if (fixture->criterion == GpuCoverageFixtureCriterion::NativeRequired) {
            if (!fixture->nativeProof) {
                std::cerr << "MISSING(native-only) " << id << " (owner " << fixture->owner << ")\n";
                ++missing;
                continue;
            }
            std::string evidence;
            const auto ocioContext = bloom::gpu_coverage_ocio::context();
            if (fixture->nativeProof(*device.device, ocioContext, evidence)) {
                std::cout << "PASS " << id << ": " << evidence << '\n';
                ++passed;
            } else {
                std::cerr << "FAIL " << id << ": " << evidence << '\n';
                ++failed;
            }
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
    // Dedicated nested proof: the per-fixture run above checks cold/warm parity, but the child
    // branch reuse across a single-branch edit is proven here through the same production builder
    // and executor, on two parent scenes whose children differ in one branch only.
    {
        const auto plans = nestedBranchReusePlans();
        const bloom::runtime::CpuGpuSceneBuilder builder;
        const auto requestA = requestFor(*plans.planA);
        const auto requestB = requestFor(*plans.planB);
        const auto preparedA = builder.build(plans.planA, requestA);
        const auto preparedB = builder.build(plans.planB, requestB);
        std::string evidence;
        const bloom::runtime::CpuCompositionEvaluator evaluator;
        if (!preparedA || !preparedB) {
            std::cerr << "FAIL nested-branch-reuse: both parent scenes must prepare\n";
            ++failed;
        } else if (!bloom::gpu_coverage_native::runNestedBranchReuse(
                       *device.device, evaluator, plans.planA, requestA, preparedA.scene,
                       plans.planB, requestB, preparedB.scene, evidence)) {
            std::cerr << "FAIL nested-branch-reuse: " << evidence << '\n';
            ++failed;
        } else {
            std::cout << "PASS nested-branch-reuse: " << evidence << '\n';
            ++passed;
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
    std::filesystem::path routeProofDirectory;
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
        } else if (argument == "--route-proof-dir" && i + 1 < argc) {
            routeProofDirectory = argv[++i];
        }
    }
    if (nativeAcceptance || requireDevice) {
        return runNativeAcceptance(loader, requireDevice, routeProofDirectory);
    }

    const auto list = fixtures();
    std::vector<std::string> fixtureIds;
    fixtureIds.reserve(list.size());
    for (const auto& fixture : list) {
        fixtureIds.push_back(fixture.id);
    }

    std::vector<std::string> failures;
    for (const auto& issue : bloom::runtime::validateGpuCoverageContract()) {
        failures.push_back("contract: " + issue.detail);
    }
    bloom::gpu_coverage_mutation::nodeTypeMutationProof(fixtureIds, failures);
    bloom::gpu_coverage_route_proof::routeProofSinkRejectsFakes(failures);
    std::string nativeNegativeEvidence;
    if (bloom::gpu_coverage_native::nativeNegativeFixturesDetectMissedGpu(nativeNegativeEvidence)) {
        std::cout << "NEGATIVE native: " << nativeNegativeEvidence << '\n';
    } else {
        failures.push_back("native acceptance negative proof: " + nativeNegativeEvidence);
    }

    const auto requiredIds = requiredCoverageIds();
    // Routes are proven only by the genuine external route harnesses (viewer/RAM/export/headless),
    // which the CPU-only gate cannot run. It reports them as externally owned and never relabels an
    // executor-prepared scene as a route proof; the distinct native acceptance CTest still requires
    // an actual proof for every route.
    for (const auto& id : requiredIds) {
        if (id.rfind("route.", 0) == 0) {
            std::cout << "NOTRUN(route) " << id << " (externally-owned genuine proof; owner "
                      << routeOwner(id) << ")\n";
            continue;
        }
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
