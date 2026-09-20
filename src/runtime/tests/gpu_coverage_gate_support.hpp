#pragma once

// Private gate-support types and id helpers for the bounded GPU coverage contract test. Kept out of
// the driver translation unit so the driver stays focused on running the fixtures and reporting.

#include <bloom/runtime/compiled_plan.hpp>
#include <bloom/runtime/evaluation.hpp>
#include <bloom/runtime/gpu_coverage_contract.hpp>
#include <bloom/runtime/prepared_gpu_scene.hpp>

#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace bloom::render {
class GpuDevice;
} // namespace bloom::render

namespace bloom::gpu_coverage_gate {

// A native-only fixture runs its own genuine proof on the owner thread (for example a display
// program dispatched through the production executor and compared to the CPU display oracle). It
// returns true only when the real GPU path ran and matched; the evidence string is reported by name.
using NativeProofRunner = std::function<bool(bloom::render::GpuDevice&,
                                             const bloom::runtime::GpuSceneOcioContext&,
                                             std::string& evidence)>;

// One prepared frame of a fixture. A time-mapped fixture (video) carries more than one distinct
// frame, and a real prepared scene is retained so the native acceptance pass can execute the same
// fixture through the production executor against the CPU oracle.
struct FrameRun final {
    std::shared_ptr<const bloom::runtime::CompiledCompositionPlan> plan;
    bloom::runtime::EvaluationRequest request;
    std::filesystem::path baseDirectory;
    std::shared_ptr<const bloom::runtime::PreparedGpuScene> scene;
};

struct FixtureRun final {
    bool prepared = false;
    std::string evidence;
    std::vector<FrameRun> frames;
};

struct Fixture final {
    std::string id;
    bloom::runtime::GpuCoverageFixtureCriterion criterion =
        bloom::runtime::GpuCoverageFixtureCriterion::Prepared;
    std::string owner;
    std::function<FixtureRun()> run;
    // Set only for a NativeRequired fixture that genuinely owns its native proof in this gate.
    NativeProofRunner nativeProof;
};

[[nodiscard]] inline std::vector<std::string> requiredCoverageIds() {
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

[[nodiscard]] inline const Fixture* findFixture(const std::vector<Fixture>& list,
                                                const std::string& id) {
    for (const auto& fixture : list) {
        if (fixture.id == id) {
            return &fixture;
        }
    }
    return nullptr;
}

[[nodiscard]] inline std::string_view routeOwner(const std::string& id) {
    for (const auto& route : bloom::runtime::gpuRenderRouteCoverage()) {
        if (route.id == id) {
            return route.owner;
        }
    }
    return {};
}

} // namespace bloom::gpu_coverage_gate
