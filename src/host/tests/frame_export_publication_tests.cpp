#include <bloom/host/frame_export_publication.hpp>

#include "gpu_route_proof_export_support.hpp"

#include <bloom/color/bloom_neutral_builtin.hpp>
#include <bloom/color/ocio_builtin_registry.hpp>
#include <bloom/host/gpu_export_provider.hpp>
#include <bloom/host/output_analysis_attempt_runner.hpp>
#include <bloom/host/publication_coordinator.hpp>
#include <bloom/output/flat_exr_reopen_verifier.hpp>
#include <bloom/output/png_reopen_verifier.hpp>
#include <bloom/runtime/qualified_display_preparation.hpp>
#include <bloom/runtime/qualified_display_processor_provider.hpp>

// Used only by independentlyDecodePng() below -- a from-scratch reader (raw chunk parse + zlib
// inflate) that never calls into bloom::output's own PNG writer/verifier, so the PNG round-trip
// test proves the published artifact against a second, independent reading. Mirrors
// src/output/tests/png_test_support.hpp's own helper, duplicated here because a src module's tests
// may not reach across a sibling module's tests/ directory (the same boundary
// src/ui/tests/main_window_readonly_placeholder_tests.cpp documents for its own duplicate).
#include <zlib.h>

#include <bloom/core/color.hpp>
#include <bloom/core/rational_time.hpp>
#include <bloom/document/composition_settings.hpp>
#include <bloom/document/ids.hpp>
#include <bloom/platform/staged_artifact.hpp>
#include <bloom/runtime/compiled_plan.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
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
#include <unistd.h>
#include <vector>

// Task F2 (issue #101): drives approveFrameExportV1()/executeExportPublication() against a real
// platform::StagedArtifactCoordinator + PublicationCoordinator, mirroring bloom/host/tests/
// save_publication_tests.cpp / copy_publication_tests.cpp's own top-of-file rationale for doing so.
//
// The test bodies are split by cohesive group into the *_tests.ipp fragments below (the shared
// fixture vocabulary lives in frame_export_publication_test_support.ipp). They are included into
// this single translation unit inside the anonymous namespace, so every test is still called with
// its assertions token-preserved; the split is organizational only.
namespace {

namespace document = bloom::document;
namespace host = bloom::host;
namespace output = bloom::output;
namespace platform = bloom::platform;
namespace runtime = bloom::runtime;

using namespace std::chrono_literals;

namespace routeproof = bloom::gpu_route_proof_export;

// The numbered fragments are the dependency order (0 support, 1 approval, 2 PNG, 3 GPU); the names
// also make the sort-includes pass produce that exact order.
#include "frame_export_publication_0_support.ipp"
#include "frame_export_publication_1_approval_tests.ipp"
#include "frame_export_publication_2_png_tests.ipp"
#include "frame_export_publication_3_gpu_tests.ipp"

} // namespace

int main(const int argc, char** argv) {
    Expectations expectations;
    bool requireDevice = false;
    std::filesystem::path proofDirectory;
    for (int index = 1; index < argc; ++index) {
        const std::string_view argument{argv[index]};
        if (argument == "--require-device") {
            requireDevice = true;
        } else if (argument == "--route-proof-dir" && index + 1 < argc) {
            proofDirectory = argv[++index];
        }
    }
    GpuProofOutcome stillOutcome = GpuProofOutcome::NotRequested;
    try {
        testDigestMismatchRejected(expectations);
        testIntentRegisteredExactlyOnce(expectations);
        testEndToEndExportPublished(expectations);
        testSupersessionOlderPublishesSecond(expectations);
        testExternalModificationConflict(expectations);
        testOverwritePolicyCreateOnlyDeniesExisting(expectations);
        testOverwritePolicyReplaceExistingSucceeds(expectations);
        testDurabilityWarningViaFaultInjection(expectations);
        testCancellationBeforeStagingLeavesTargetIntact(expectations);
        testDeadlineExpiryViaInjectedClock(expectations);
        testNoProgressExpiryViaInjectedClock(expectations);
        testEndToEndPngExportPublished(expectations);
        testPngSemanticDigestStableAcrossTwoRuns(expectations);
        testBothPresetsExportFromTheSameFixture(expectations);
        testPngPreparedBytesLimitExceededIsTyped(expectations);
        testPngColorPreparingCancellationPublishesNothing(expectations);
        testGpuStaleDisplayBindingRefused(expectations);
        testGpuSameGeometryWrongTransformRefused(expectations);
        testGpuCrossRevisionCommandRefused(expectations);
        stillOutcome = testGpuCompositedExportParity(expectations, requireDevice, proofDirectory);
    } catch (const std::exception& error) {
        std::cerr << "unexpected exception: " << error.what() << '\n';
        return EXIT_FAILURE;
    }
    if (expectations.failures() != 0) {
        return EXIT_FAILURE;
    }
    // With --route-proof-dir and no loader/device the honest result is a CTest skip, never a pass
    // labelled as a proof.
    return stillOutcome == GpuProofOutcome::Skipped ? 77 : EXIT_SUCCESS;
}
