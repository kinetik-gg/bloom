#include <bloom/host/output_analysis_attempt_runner.hpp>

#include <bloom/color/bloom_neutral_builtin.hpp>
#include <bloom/color/ocio_builtin_registry.hpp>
#include <bloom/core/color.hpp>
#include <bloom/core/rational_time.hpp>
#include <bloom/core/sha256.hpp>
#include <bloom/document/composition_settings.hpp>
#include <bloom/document/ids.hpp>
#include <bloom/host/gpu_export_provider.hpp>
#include <bloom/host/gpu_export_tool_package.hpp>
#include <bloom/platform/staged_artifact.hpp>
#include <bloom/runtime/compiled_plan.hpp>
#include <bloom/runtime/gpu_ocio_context.hpp>
#include <bloom/runtime/gpu_process_frame.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <memory>
#include <optional>
#include <source_location>
#include <string>
#include <string_view>
#include <thread>
#include <unistd.h>
#include <vector>

// Task F2 (issue #101): drives beginOutputAnalysisAttemptV1() -- the Resolving (BlockingIo) ->
// Evaluating/Identifying/Analyzing (Cpu) two-task chain -- against a REAL bloom::runtime::
// TaskScheduler with real BlockingIo/Cpu workers and a REAL platform::StagedArtifactCoordinator,
// exactly mirroring bloom/host/tests/session_async_io_tests.cpp's own top-of-file rationale for
// doing so.
//
// The test bodies are split by cohesive group into the *_tests.ipp fragments below (the shared
// fixture vocabulary lives in the _test_support.ipp fragment). They are included into this single
// translation unit inside the anonymous namespace, so every test is still called with its
// assertions token-preserved; the split is organizational only.
namespace {

namespace document = bloom::document;
namespace host = bloom::host;
namespace output = bloom::output;
namespace platform = bloom::platform;
namespace runtime = bloom::runtime;

using namespace std::chrono_literals;

// The numbered fragments are the dependency order (0 support, then the test groups); the names
// also make the sort-includes pass produce that exact order.
#include "output_analysis_attempt_runner_0_support.ipp"
#include "output_analysis_attempt_runner_1_attempt_tests.ipp"
#include "output_analysis_attempt_runner_2_provider_tests.ipp"
#include "output_analysis_attempt_runner_3_gpu_tests.ipp"

} // namespace

int main(const int argc, char** argv) {
    Expectations expectations;
    bool requireDevice = false;
    for (int index = 1; index < argc; ++index) {
        if (std::string_view(argv[index]) == "--require-device") {
            requireDevice = true;
        }
    }
    testFullGraphProducesStableDigestAcrossTwoRuns(expectations);
    testCancellationBeforeResolvingCompletes(expectations);
    testResourceExhaustionIsTypedWithZeroLeak(expectations);
    testGpuExportProviderDisabledFallback(expectations);
    testGpuExportProviderAsyncRetirement(expectations, requireDevice);
    testGpuExportProviderRetirementAfterAdmissionClosed(expectations);
    testGpuExportProviderShutdownDuringBootstrap(expectations);
    testGpuExportProviderBootstrapInFlightIsNotComplete(expectations);
    testGpuExportProviderQueuedBootstrapCancellationCompletes(expectations);
    testGpuDisplayConcurrentSetterPrepare(expectations);
    testGpuExportProviderFactoryDisplayPreparation(expectations);
    testGpuEvaluatorHostPath(expectations, requireDevice);
    return expectations.failures() == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
