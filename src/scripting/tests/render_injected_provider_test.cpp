// Proves that Render::run reuses an injected GpuExportProvider without retiring it, while a
// locally created provider is still retired with proof before the call returns. This is the
// contract the MCP server relies on to share ONE server-lifetime provider across still and video
// renders instead of re-bootstrapping (or prematurely retiring) a device per frame.

#include <bloom/document/node_definition_registry.hpp>
#include <bloom/host/gpu_export_provider.hpp>
#include <bloom/runtime/snapshot_compiler.hpp>
#include <bloom/runtime/task_scheduler.hpp>
#include <bloom/scripting/render.hpp>
#include <bloom/scripting/session.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <source_location>
#include <string>
#include <string_view>
#include <thread>
#include <unistd.h>

namespace {

namespace document = bloom::document;
namespace host = bloom::host;
namespace runtime = bloom::runtime;
namespace scripting = bloom::scripting;

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
    [[nodiscard]] int failures() const noexcept { return failures_; }

  private:
    int failures_ = 0;
};

class TempDirectory final {
  public:
    TempDirectory() {
        std::array<char, 64> pattern{};
        constexpr std::string_view prefix = "/tmp/bloom-render-injected-XXXXXX";
        std::ranges::copy(prefix, pattern.begin());
        if (const auto* result = ::mkdtemp(pattern.data()); result != nullptr) {
            path_ = result;
        }
    }
    TempDirectory(const TempDirectory&) = delete;
    TempDirectory& operator=(const TempDirectory&) = delete;
    ~TempDirectory() {
        if (!path_.empty()) {
            std::error_code error;
            std::filesystem::remove_all(path_, error);
        }
    }
    [[nodiscard]] bool isValid() const noexcept { return !path_.empty(); }
    [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }

  private:
    std::filesystem::path path_;
};

[[nodiscard]] runtime::GpuProcessFrameEvaluatorOptions
optionsFor(const std::filesystem::path& loader) {
    runtime::GpuProcessFrameEvaluatorOptions options;
    // A deliberately absent loader keeps a real owner worker alive (because enabled is true) but
    // never publishes a device, so retirementComplete() is only true after we retire it.
    options.enabled = true;
    options.loaderPath = loader;
    return options;
}

void testInjectedProviderIsReusedAndNotRetired(Expectations& expectations) {
    auto created = scripting::Session::createNew("Injected Provider", "Composition");
    expectations.expect(static_cast<bool>(created), "session is created");
    if (!created) {
        return;
    }
    auto session = std::move(created).takeSession();
    const auto snapshot = session->snapshot();
    expectations.expect(!snapshot.project().compositions().empty(),
                        "the default project has a composition");
    if (snapshot.project().compositions().empty()) {
        return;
    }
    TempDirectory directory;
    expectations.expect(directory.isValid(), "temp directory is available");
    if (!directory.isValid()) {
        return;
    }

    runtime::TaskScheduler scheduler;
    runtime::SnapshotCompiler compiler(document::builtInNodeDefinitions());
    auto provider =
        host::GpuExportProvider::create(optionsFor("/nonexistent/bloom/test/vulkan/loader.so"));
    provider->prepare(scheduler);
    const auto bootstrapDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
    while (!provider->prepared() && std::chrono::steady_clock::now() < bootstrapDeadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    expectations.expect(provider->prepared(), "injected provider bootstraps");
    const auto evaluator = provider->evaluator();
    expectations.expect(evaluator != nullptr,
                        "injected provider publishes an (unavailable) evaluator");
    expectations.expect(!provider->retirementComplete(),
                        "injected provider is live before the render");

    const auto compositionId = snapshot.project().compositions().front().id();
    for (int pass = 0; pass < 2; ++pass) {
        const auto target = directory.path() / ("injected-" + std::to_string(pass) + ".exr");
        const auto result = scripting::Render::run(
            *session, scheduler, compiler,
            {.composition = compositionId,
             .frame = std::uint64_t{0},
             .range = std::nullopt,
             .preset = bloom::output::OutputPresetV1::FlatExrRgba32fLinRec709SceneV1,
             .destination = target},
            {}, provider);
        expectations.expect(result.succeeded, "the injected-provider render publishes");
        expectations.expect(!provider->retirementComplete(),
                            "Render::run never retires an injected provider between stills");
        expectations.expect(provider->evaluator() != nullptr,
                            "the injected evaluator is still published after a still");
    }

    expectations.expect(provider->shutdownAndWait(std::chrono::seconds(10)),
                        "the owner can still retire the injected provider explicitly");
}

void testLocalProviderIsRetired(Expectations& expectations) {
    auto created = scripting::Session::createNew("Local Provider", "Composition");
    expectations.expect(static_cast<bool>(created), "local session is created");
    if (!created) {
        return;
    }
    auto session = std::move(created).takeSession();
    const auto snapshot = session->snapshot();
    if (snapshot.project().compositions().empty()) {
        return;
    }
    TempDirectory directory;
    if (!directory.isValid()) {
        return;
    }
    runtime::TaskScheduler scheduler;
    runtime::SnapshotCompiler compiler(document::builtInNodeDefinitions());
    const auto result = scripting::Render::run(
        *session, scheduler, compiler,
        {.composition = snapshot.project().compositions().front().id(),
         .frame = std::uint64_t{0},
         .range = std::nullopt,
         .preset = bloom::output::OutputPresetV1::FlatExrRgba32fLinRec709SceneV1,
         .destination = directory.path() / "local.exr"});
    expectations.expect(result.succeeded,
                        "a locally owned provider still renders and retires with proof");
}

} // namespace

int main() {
    Expectations expectations;
    try {
        testInjectedProviderIsReusedAndNotRetired(expectations);
        testLocalProviderIsRetired(expectations);
    } catch (const std::exception& error) {
        std::cerr << "unexpected exception: " << error.what() << '\n';
        return EXIT_FAILURE;
    }
    return expectations.failures() == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
