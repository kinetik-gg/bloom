// Proves that Render::run reuses an injected GpuExportProvider without retiring it, while a
// locally created provider is still retired with proof before the call returns. This is the
// contract the MCP server relies on to share ONE server-lifetime provider across still and video
// renders instead of re-bootstrapping (or prematurely retiring) a device per frame.
//
// The native routes additionally exercise the genuine PNG output-colour arm through the production
// packaged resolver: one final readback submission per frame, two payloads (process + encoded
// display), and an independent decoded-PNG parity check. A CPU output-colour omission can never
// publish a green proof.
//
// The test bodies are split by cohesive group into the *_tests.ipp fragment below (the shared
// fixture vocabulary lives in render_injected_provider_test_support.ipp). They are included into
// this single translation unit inside the anonymous namespace, so every test is still called with
// its assertions token-preserved; the split is organizational only.

#include "gpu_route_proof_export_support.hpp"

#include <bloom/commands/animation_operations.hpp>
#include <bloom/commands/operations.hpp>
#include <bloom/commands/transaction.hpp>
#include <bloom/core/color.hpp>
#include <bloom/document/node_definition_registry.hpp>
#include <bloom/host/frame_range_runner.hpp>
#include <bloom/host/gpu_export_provider.hpp>
#include <bloom/host/gpu_export_tool_package.hpp>
#include <bloom/runtime/compiled_plan.hpp>
#include <bloom/runtime/snapshot_compiler.hpp>
#include <bloom/runtime/task_scheduler.hpp>
#include <bloom/scripting/render.hpp>
#include <bloom/scripting/session.hpp>

#include <zlib.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <optional>
#include <source_location>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {

namespace document = bloom::document;
namespace commands = bloom::commands;
namespace core = bloom::core;
namespace host = bloom::host;
namespace runtime = bloom::runtime;
namespace scripting = bloom::scripting;
namespace routeproof = bloom::gpu_route_proof_export;

using namespace std::chrono_literals;

// The numbered fragments are the dependency order (0 support, then 1 the routes); the names also
// make the sort-includes pass produce that exact order.
#include "render_injected_provider_0_support.ipp"
#include "render_injected_provider_1_native_tests.ipp"

} // namespace

int main(int argc, char** argv) {
    Expectations expectations;
    bool requireDevice = false;
    std::filesystem::path proofDir;
    for (int index = 1; index < argc; ++index) {
        const std::string_view argument{argv[index]};
        if (argument == "--require-device") {
            requireDevice = true;
        } else if (argument == "--route-proof-dir" && index + 1 < argc) {
            proofDir = argv[++index];
        }
    }
    GpuProofOutcome headlessOutcome = GpuProofOutcome::NotRequested;
    GpuProofOutcome sequenceOutcome = GpuProofOutcome::NotRequested;
    std::string proofRoute = "both";
    for (int index = 1; index < argc; ++index) {
        if (std::string_view(argv[index]) == "--route" && index + 1 < argc) {
            proofRoute = argv[++index];
        }
    }
    try {
        testInjectedProviderIsReusedAndNotRetired(expectations);
        testLocalProviderIsRetired(expectations);
        if (proofRoute != "sequence") {
            headlessOutcome =
                testNativeHeadlessRenderProvenanceAndParity(expectations, requireDevice, proofDir);
        }
        if (proofRoute != "headless") {
            sequenceOutcome =
                testNativeSequenceRangeProvenanceAndParity(expectations, requireDevice, proofDir);
        }
    } catch (const std::exception& error) {
        std::cerr << "unexpected exception: " << error.what() << '\n';
        return EXIT_FAILURE;
    }
    if (expectations.failures() != 0) {
        return EXIT_FAILURE;
    }
    if (headlessOutcome == GpuProofOutcome::Skipped ||
        sequenceOutcome == GpuProofOutcome::Skipped) {
        return 77;
    }
    return EXIT_SUCCESS;
}
