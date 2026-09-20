#pragma once

// Native acceptance driver for the bounded GPU coverage gate, extracted from the gate translation
// unit so the driver stays within the source-size budget. This is the strict native path: on a real
// device every Required operation/effect/feature/node-type fixture must genuinely execute and match
// the unchanged CPU oracle, every Required render route must carry a fresh run-nonce-matched proof
// from its owning harness, and a fixture that only prepared or only ran on the CPU is a MISSED_GPU.
// The helper never invents a proof, never relabels the executor helper as a route proof, and never
// adds an opt-out. The caller passes the same fixture list the CPU gate uses.

#include "gpu_coverage_contract_plans.hpp"
#include "gpu_coverage_gate_support.hpp"
#include "gpu_coverage_native_support.hpp"
#include "gpu_coverage_ocio_support.hpp"
#include "gpu_route_proof_io.hpp"

#include <bloom/render/gpu_device.hpp>
#include <bloom/runtime/cpu_composition_evaluator.hpp>
#include <bloom/runtime/gpu_coverage_contract.hpp>
#include <bloom/runtime/prepared_gpu_scene.hpp>

#include <cstddef>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

namespace bloom::gpu_coverage_gate {

// Runs the genuine native acceptance for the supplied fixture list. Returns 0 only when every
// Required id passed, 1 when any fixture or route failed or is missing, and 77 (CTest SKIP) when no
// device is available and one is not required.
[[nodiscard]] inline int runNativeAcceptance(const std::vector<Fixture>& list,
                                             const std::filesystem::path& loader,
                                             const bool requireDevice,
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
        if (fixture->criterion == bloom::runtime::GpuCoverageFixtureCriterion::NativeRequired) {
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
        const auto plans = bloom::gpu_coverage_plans::nestedBranchReusePlans();
        const bloom::runtime::CpuGpuSceneBuilder builder;
        const auto requestA = bloom::gpu_coverage_fixtures::requestFor(*plans.planA);
        const auto requestB = bloom::gpu_coverage_fixtures::requestFor(*plans.planB);
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

} // namespace bloom::gpu_coverage_gate
