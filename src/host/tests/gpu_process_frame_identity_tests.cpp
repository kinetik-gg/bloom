// Host-side integration proof that a real GPU-provenance ProcessFrame produces the SAME canonical
// output semantic identity as the CPU reference evaluator.
//
// This lives in bloom.host because it exercises bloom::output's ProcessFrameSemanticIdentityV1
// preparer; the runtime module (and its tests) must not depend on bloom::output. The runtime
// fixture bloom.runtime.gpu_process_frame keeps only runtime/render dependencies and proves the
// process pixels, dispatch counters, and provenance; this test adds the canonical identity-byte and
// process-pixel-digest equality plus the GPU provenance assertion.
//
// Without a loader/device it prints an explicit SKIP unless --require-device is passed.

#include "gpu_media_executor_test_support.hpp"
#include "gpu_scene_executor_test_support.hpp"

#include <bloom/core/color.hpp>
#include <bloom/output/process_frame_semantic_identity.hpp>
#include <bloom/runtime/cpu_composition_evaluator.hpp>
#include <bloom/runtime/evaluation.hpp>
#include <bloom/runtime/gpu_process_frame.hpp>

#include <algorithm>
#include <cstdint>
#include <iostream>
#include <memory>
#include <source_location>
#include <string_view>

namespace {

namespace output = bloom::output;
namespace runtime = bloom::runtime;

using bloom::core::Color4d;
using bloom::runtime::EvaluationProvider;
using bloom::runtime::EvaluationStatus;
using namespace bloom::runtime::executor_test;
using namespace bloom::runtime::media_executor_test;

constexpr std::uint64_t kReadbackBudget = 1ULL << 32U;
constexpr std::uint64_t kRequestBudget = 1ULL << 32U;

class Expectations final {
  public:
    void expect(const bool condition, const std::string_view message,
                const std::source_location location = std::source_location::current()) {
        if (condition) {
            return;
        }
        ++failures_;
        std::cerr << location.file_name() << ':' << location.line() << ": " << message << '\n';
    }
    [[nodiscard]] bool ok() const noexcept { return failures_ == 0; }

  private:
    int failures_ = 0;
};

std::shared_ptr<const runtime::ProcessFrame>
evaluateCpu(const std::shared_ptr<const runtime::CompiledCompositionPlan>& plan,
            const runtime::EvaluationRequest& request) {
    const runtime::CpuCompositionEvaluator oracle;
    const auto result = oracle.evaluate(plan, request, {});
    if (result.status() != EvaluationStatus::Evaluated) {
        return nullptr;
    }
    return result.frame();
}

int run(const Options& options) {
    Expectations expectations;

    const auto plan = twoSolidPlan(
        format(24, 18), Color4d{0.5, 0.25, 0.125, 1.0}, LayerValues{.position = {4.25, 3.5}},
        Color4d{0.125, 0.375, 0.75, 0.5}, LayerValues{.position = {7.5, 6.25}}, 9.0, 7.0, 9000);
    const auto request = requestFor(*plan);

    runtime::GpuProcessFrameEvaluatorOptions evaluatorOptions;
    evaluatorOptions.enabled = true;
    evaluatorOptions.loaderPath = options.loader_path;
    evaluatorOptions.requestByteBudget = kRequestBudget;
    evaluatorOptions.readbackByteBudget = kReadbackBudget;
    auto evaluator = runtime::GpuProcessFrameEvaluator::create(evaluatorOptions);
    expectations.expect(evaluator != nullptr, "the evaluator is constructed");
    if (evaluator == nullptr) {
        return 1;
    }
    if (!evaluator->gpuAvailable()) {
        if (options.require_device) {
            std::cerr << "FAIL: required device unavailable: "
                      << evaluator->availabilityDiagnostic().message << '\n';
            return 1;
        }
        std::cout << "SKIP: no compatible Vulkan device: "
                  << evaluator->availabilityDiagnostic().message << '\n';
        return expectations.ok() ? 0 : 1;
    }

    const auto cpuFrame = evaluateCpu(plan, request);
    expectations.expect(cpuFrame != nullptr, "the CPU oracle produced a reference frame");

    const auto gpu = evaluator->evaluate(plan, request);
    expectations.expect(gpu.status == runtime::GpuProcessFrameStatus::Evaluated &&
                            gpu.frame != nullptr,
                        "the GPU bridge produced a genuine process frame");
    if (gpu.frame == nullptr || cpuFrame == nullptr) {
        std::cerr << "GPU diagnostic: " << static_cast<int>(gpu.diagnostic.code) << ' '
                  << gpu.diagnostic.message << '\n';
        return 1;
    }
    expectations.expect(gpu.frame->identity().provider == EvaluationProvider::GpuResident,
                        "the frame provenance is GpuResident");

    // The canonical semantic identity is derived from the exact process pixels plus the closed
    // identity fields; a bit-equal GPU frame must produce the same canonical bytes and digest.
    const output::ProcessFrameSemanticIdentityV1Preparer preparer;
    const auto cpuIdentity = preparer.prepare(cpuFrame, {});
    const auto gpuIdentity = preparer.prepare(gpu.frame, {});
    expectations.expect(
        cpuIdentity.status() == output::ProcessFrameSemanticIdentityPreparationStatus::Prepared &&
            gpuIdentity.status() == output::ProcessFrameSemanticIdentityPreparationStatus::Prepared,
        "both frames prepare a semantic identity");
    if (cpuIdentity.identity() != nullptr && gpuIdentity.identity() != nullptr) {
        const auto cpuBytes = cpuIdentity.identity()->canonicalBytes();
        const auto gpuBytes = gpuIdentity.identity()->canonicalBytes();
        expectations.expect(cpuBytes.size() == gpuBytes.size() &&
                                std::equal(cpuBytes.begin(), cpuBytes.end(), gpuBytes.begin()),
                            "the GPU frame's canonical identity bytes equal the CPU oracle");
        expectations.expect(cpuIdentity.identity()->processPixelDigest() ==
                                gpuIdentity.identity()->processPixelDigest(),
                            "the GPU process-pixel digest equals the CPU oracle");
    }

    evaluator->beginShutdown();

    if (!expectations.ok()) {
        std::cerr << "FAIL: GPU process-frame identity expectations failed\n";
        return 1;
    }
    std::cout << "PASS: GPU process-frame identity\n";
    return 0;
}

} // namespace

int main(const int argc, char** argv) {
    try {
        const Options options = parseOptions(argc, argv);
        if (!options.valid) {
            return 2;
        }
        return run(options);
    } catch (const std::exception& exception) {
        std::cerr << "Unexpected test exception: " << exception.what() << '\n';
        return 1;
    }
}
