#include <bloom/render/gpu_device.hpp>

#include <cstddef>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <iostream>
#include <source_location>
#include <string_view>
#include <thread>
#include <type_traits>
#include <utility>

namespace {

using bloom::render::GpuBufferAllocation;
using bloom::render::GpuDevice;
using bloom::render::GpuDeviceCreationOptions;
using bloom::render::GpuDeviceState;
using bloom::render::GpuDiagnosticCode;
using bloom::render::GpuOperationId;
using bloom::render::GpuPrecision;
using bloom::render::GpuQualification;

static_assert(!std::is_copy_constructible_v<GpuDevice>);
static_assert(!std::is_copy_assignable_v<GpuDevice>);
static_assert(std::is_move_constructible_v<GpuDevice>);
static_assert(!std::is_copy_constructible_v<GpuBufferAllocation>);
static_assert(!std::is_copy_assignable_v<GpuBufferAllocation>);
static_assert(std::is_move_constructible_v<GpuBufferAllocation>);

class ExpectationContext final {
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
    std::size_t failures_ = 0;
};

struct TestOptions final {
    std::filesystem::path loader_path;
    bool require_device = false;
    bool valid = true;
};

// --loader <path> pins the exact runtime loader the supervisor wants exercised (the qualified
// prefix's libvulkan.so.1). --require-device turns an unavailable probe into a failure so a GPU
// machine can never silently report SKIP. With no arguments the probe is optional and a
// hardware-free CI image prints an explicit skip.
[[nodiscard]] TestOptions parseOptions(const int argc, char** argv) {
    TestOptions options;
    for (int index = 1; index < argc; ++index) {
        const std::string_view argument = argv[index];
        if (argument == "--loader") {
            if (index + 1 >= argc) {
                std::cerr << "--loader requires a path argument\n";
                options.valid = false;
                return options;
            }
            options.loader_path = argv[++index];
        } else if (argument == "--require-device") {
            options.require_device = true;
        } else {
            std::cerr << "unknown argument: " << argument << '\n';
            options.valid = false;
            return options;
        }
    }
    return options;
}

// A forced missing loader is the portable unavailable path. It must yield the backend's exact typed
// diagnostic, never an arbitrary failure: LoaderUnavailable when the Vulkan backend is compiled,
// BackendNotBuilt when this is the CPU stub.
void testForcedLoaderUnavailable(ExpectationContext& expectations) {
    GpuDeviceCreationOptions options;
    options.loader_path = "/nonexistent/bloom-missing-vulkan-loader";
    const auto result = GpuDevice::create(options);
    expectations.expect(!result && result.device == nullptr,
                        "a forced missing loader yields no device");
#if defined(BLOOM_RENDER_TEST_VULKAN_BACKEND)
    expectations.expect(result.diagnostic.code == GpuDiagnosticCode::LoaderUnavailable,
                        "the Vulkan backend reports LoaderUnavailable for a missing loader");
#else
    expectations.expect(result.diagnostic.code == GpuDiagnosticCode::BackendNotBuilt,
                        "the CPU stub reports BackendNotBuilt");
#endif
    expectations.expect(!result.diagnostic.message.empty(),
                        "the unavailable diagnostic is actionable");
}

// Exposed operations fail closed on a non-owner thread. The worker only calls allocateHostBuffer;
// the device and every owned object stay on the owner thread and are destroyed there.
void testWrongThreadFailsClosed(ExpectationContext& expectations, GpuDevice& device) {
    auto observed = GpuDiagnosticCode::None;
    std::thread worker([&device, &observed]() {
        const auto allocation = device.allocateHostBuffer(64U);
        observed = allocation.diagnostic.code;
    });
    worker.join();
    expectations.expect(observed == GpuDiagnosticCode::WrongThread,
                        "allocateHostBuffer from a joined non-owner thread fails closed");
}

void testRealDeviceIfAvailable(ExpectationContext& expectations, const TestOptions& options) {
    GpuDeviceCreationOptions createOptions;
    createOptions.loader_path = options.loader_path;
    auto result = GpuDevice::create(createOptions);
    if (!result) {
        if (options.require_device) {
            expectations.expect(false, "a device was required but the probe returned Unavailable");
            std::cerr << "FAIL: required device unavailable: " << result.diagnostic.message << '\n';
            return;
        }
        std::cout << "SKIP: no compatible Vulkan device available: " << result.diagnostic.message
                  << '\n';
        expectations.expect(result.diagnostic.code != GpuDiagnosticCode::None,
                            "an unavailable device is explained by a typed diagnostic");
        return;
    }

    GpuDevice& device = *result.device;
    expectations.expect(device.state() == GpuDeviceState::Ready, "a bootstrapped device is Ready");

    const auto& report = device.capabilityReport();
    expectations.expect(report.state == GpuDeviceState::Ready && report.generation >= 1,
                        "the capability report is Ready and generation-scoped");
    expectations.expect(report.compute_queue, "device bootstrap selects and owns a compute queue");
    expectations.expect(report.timeline_semaphore,
                        "Vulkan 1.2 timeline semaphores are a checked bootstrap requirement");
    expectations.expect(report.identity.backend == "vulkan" && !report.identity.device_name.empty(),
                        "the report names the selected Vulkan backend and physical device");
    expectations.expect(report.identity.api_version_major == 1 &&
                            report.identity.api_version_minor >= 2,
                        "the selected device meets the Vulkan 1.2 floor");
    std::cout << "GPU: backend=" << report.identity.backend << " device=\""
              << report.identity.device_name << "\""
              << " driver_version=" << report.identity.driver_version
              << " api=" << report.identity.api_version_major << '.'
              << report.identity.api_version_minor << " compute_queue=" << report.compute_queue
              << " timeline=" << report.timeline_semaphore
              << " memory_bytes=" << report.device_memory_bytes << '\n';

    expectations.expect(device.qualificationFor(GpuOperationId::SolidV1, GpuPrecision::Rgba32f) ==
                            GpuQualification::Unavailable,
                        "operations stay Unavailable until the frozen fixture gate passes");
    expectations.expect(report.operations.empty(),
                        "the bootstrap report invents no operation qualification entries");

    const auto oversized = device.allocateHostBuffer(bloom::render::kMaxGpuHostBufferBytes + 1U);
    expectations.expect(!oversized &&
                            oversized.diagnostic.code == GpuDiagnosticCode::AllocationLimitExceeded,
                        "allocation is bounded before reaching the allocator");
    const auto empty = device.allocateHostBuffer(0U);
    expectations.expect(!empty && empty.diagnostic.code == GpuDiagnosticCode::InvalidArgument,
                        "zero-byte allocations are rejected");

    auto allocation = device.allocateHostBuffer(256U);
    expectations.expect(allocation && allocation.allocation.isValid(),
                        "a tiny host-visible buffer is allocated through VMA");
    if (allocation) {
        const auto info = allocation.allocation.info();
        expectations.expect(info.size_bytes >= 256U && info.host_visible,
                            "the host buffer reports its bounded size and host visibility");
        std::cout << "VMA host buffer: result=ok size_bytes=" << info.size_bytes
                  << " host_visible=" << info.host_visible << '\n';

        auto moved = std::move(allocation.allocation);
        // NOLINTNEXTLINE(bugprone-use-after-move,clang-analyzer-cplusplus.Move)
        expectations.expect(!allocation.allocation.isValid() && moved.isValid(),
                            "buffer ownership moves without duplicating the allocation");
    }

    testWrongThreadFailsClosed(expectations, device);
}

} // namespace

int main(int argc, char** argv) {
    try {
        const TestOptions options = parseOptions(argc, argv);
        if (!options.valid) {
            return 2;
        }
        ExpectationContext expectations;
        testForcedLoaderUnavailable(expectations);
        testRealDeviceIfAvailable(expectations, options);
        return expectations.ok() ? 0 : 1;
    } catch (const std::exception& exception) {
        std::cerr << "Unexpected test exception: " << exception.what() << '\n';
        return 1;
    }
}
