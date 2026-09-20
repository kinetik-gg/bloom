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
// returns true only when the real GPU path ran and matched; the evidence string is reported by
// name.
using NativeProofRunner = std::function<bool(
    bloom::render::GpuDevice&, const bloom::runtime::GpuSceneOcioContext&, std::string& evidence)>;

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

// The explicit, default-absent prerequisite of a Required fixture. Most fixtures depend only on the
// production code under test and carry no prerequisite. A colour-transform fixture additionally
// needs the packaged glslangValidator/spirv-val that the production GpuOcioContextResolver
// validates; a build that deliberately packages no tools (CPU stub, unqualified mode, or a missing
// prefix) cannot execute it. That environmental absence is reported as a typed NotRun, never as a
// prepared pass, never as a diagnostic-string guess, and never as a pixel exemption.
enum class FixturePrerequisite {
    None,
    PackagedShaderTools,
};

// How one fixture is handled in a CPU gate run. NativeNotRun and PrerequisiteNotRun are explicit
// environmental NotRuns; Run means the fixture genuinely executes and its result stands.
enum class FixtureExecution {
    Run,
    NativeNotRun,
    PrerequisiteNotRun,
};

struct Fixture final {
    std::string id;
    bloom::runtime::GpuCoverageFixtureCriterion criterion =
        bloom::runtime::GpuCoverageFixtureCriterion::Prepared;
    std::string owner;
    std::function<FixtureRun()> run;
    // Set only for a NativeRequired fixture that genuinely owns its native proof in this gate.
    NativeProofRunner nativeProof;
    // Defaults to no prerequisite. Only a fixture that genuinely needs the packaged GPU shader
    // tools sets this, so no independent fixture can be masked by tool availability.
    FixturePrerequisite prerequisite = FixturePrerequisite::None;
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

// Classify one fixture for a CPU gate run. This is the single decision point for the environmental
// NotRun paths: a NativeRequired fixture is always native-not-run on the CPU path, and a fixture
// whose explicit prerequisite is unavailable is prerequisite-not-run. Every other fixture runs.
// Tool availability is passed in so the decision is directly testable and never inferred from a
// failure diagnostic.
[[nodiscard]] inline FixtureExecution classifyFixtureExecution(const Fixture& fixture,
                                                               const bool toolsAvailable) {
    if (fixture.criterion == bloom::runtime::GpuCoverageFixtureCriterion::NativeRequired) {
        return FixtureExecution::NativeNotRun;
    }
    if (fixture.prerequisite != FixturePrerequisite::None && !toolsAvailable) {
        return FixtureExecution::PrerequisiteNotRun;
    }
    return FixtureExecution::Run;
}

// Structural requirement checks. A Required id with no fixture entry is a missing-fixture failure,
// and a fixture whose id is not a Required contract entry is an unlisted-fixture failure. This pass
// never consults prerequisite availability: a missing fixture stays a structural failure even when
// every optional prerequisite (for example the packaged GPU shader tools) is absent, so the
// environmental NotRun path can never absorb a hole.
[[nodiscard]] inline std::vector<std::string>
structuralFixtureFailures(const std::vector<Fixture>& list,
                          const std::vector<std::string>& requiredIds) {
    std::vector<std::string> failures;
    for (const auto& id : requiredIds) {
        if (id.rfind("route.", 0) == 0) {
            continue;
        }
        if (findFixture(list, id) == nullptr) {
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
    return failures;
}

// Focused regression for the unavailable-prerequisite path. It asserts the decision matrix
// directly: an explicit shader-tools prerequisite with the tools absent is a NotRun, the same
// fixture with the tools present runs, an independent fixture never becomes a skip, and a Required
// id with no fixture entry remains a structural failure at either tool state. Returns true only
// when every expectation holds; the driver turns a false into a gate failure.
[[nodiscard]] inline bool prerequisiteDecisionRegression(std::string& evidence) {
    Fixture toolDependent;
    toolDependent.id = "regression.shader-dependent";
    toolDependent.prerequisite = FixturePrerequisite::PackagedShaderTools;

    Fixture independent;
    independent.id = "regression.independent";

    const std::vector<Fixture> present{toolDependent, independent};
    const std::vector<std::string> required{"regression.shader-dependent", "regression.independent",
                                            "regression.missing"};

    const bool absentSkip =
        classifyFixtureExecution(toolDependent, false) == FixtureExecution::PrerequisiteNotRun;
    const bool presentRun = classifyFixtureExecution(toolDependent, true) == FixtureExecution::Run;
    const bool independentRun =
        classifyFixtureExecution(independent, false) == FixtureExecution::Run;
    const auto absentStructural = structuralFixtureFailures(present, required);
    const bool missingStillFails = !absentStructural.empty();
    bool missingNamed = false;
    for (const auto& failure : absentStructural) {
        missingNamed = missingNamed || failure.find("'regression.missing'") != std::string::npos;
    }
    const bool okay =
        absentSkip && presentRun && independentRun && missingStillFails && missingNamed;
    evidence =
        okay ? "prerequisite decision matrix holds; missing fixture fails with tools absent"
             : "prerequisite decision matrix violated (skip=" +
                   std::string{absentSkip ? "ok" : "bad"} +
                   ", run=" + std::string{presentRun ? "ok" : "bad"} +
                   ", independent=" + std::string{independentRun ? "ok" : "bad"} +
                   ", missing=" + std::string{missingStillFails && missingNamed ? "ok" : "bad"} +
                   ")";
    return okay;
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
