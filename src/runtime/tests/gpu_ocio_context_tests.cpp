// Focused acceptance for the shared runtime OCIO context resolver
// (bloom/runtime/gpu_ocio_context.hpp).
//
// The packaged-tool vectors use the REAL tools staged by
// cmake/BloomGpuShaderTools.cmake beside THIS test executable (bloom_package_gpu_shader_tools):
// the package descriptor is built from the same BLOOM_GPU_TOOLS_* compile definitions the desktop,
// CLI, and MCP targets receive. There are no fake tool paths; the resolved adapter is additionally
// proven to compile and validate a tiny shader. The typed-refusal vectors (empty package,
// cancellation, missing tools, malformed inventory) run without any ambient tool discovery, and a
// build without qualified tools exercises the CPU-fallback refusal directly.

#include <bloom/color/gpu_shader_compiler.hpp>
#include <bloom/color/gpu_shader_tool_resolver.hpp>
#include <bloom/core/sha256.hpp>
#include <bloom/runtime/gpu_ocio_context.hpp>
#include <bloom/runtime/prepared_gpu_scene.hpp>

#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <vector>

namespace {

namespace fs = std::filesystem;
using bloom::color::GpuShaderCompiler;
using bloom::color::GpuShaderCompileRequest;
using bloom::color::GpuShaderCompileStatus;
using bloom::color::GpuShaderToolPackage;
using bloom::runtime::GpuOcioContextError;
using bloom::runtime::GpuOcioContextRequest;
using bloom::runtime::GpuOcioContextResolver;
using bloom::runtime::GpuSceneOcioContext;

class Expectations final {
  public:
    void expect(const bool condition, const std::string_view message) {
        if (!condition) {
            ++failures_;
            std::cerr << "FAILED: " << message << '\n';
        }
    }
    [[nodiscard]] int failures() const noexcept { return failures_; }

  private:
    int failures_ = 0;
};

constexpr std::string_view kComputeShader = R"(#version 450
layout(local_size_x = 4, local_size_y = 1, local_size_z = 1) in;
void main() { }
)";

#if defined(_WIN32)
void setEnvironment(const char* name, const char* value) { _putenv_s(name, value ? value : ""); }
void clearEnvironment(const char* name) { _putenv_s(name, ""); }
#else
void setEnvironment(const char* name, const char* value) { ::setenv(name, value ? value : "", 1); }
void clearEnvironment(const char* name) { ::unsetenv(name); }
#endif

std::optional<bloom::core::Sha256Digest> parsePinnedDigest(const std::string_view text) {
    constexpr std::string_view prefix = "sha256:";
    if (text.size() != prefix.size() + bloom::core::kSha256HexCharacters ||
        text.substr(0, prefix.size()) != prefix) {
        return std::nullopt;
    }
    return bloom::core::Sha256Digest::fromLowercaseHex(text.substr(prefix.size()));
}

// A package descriptor that is structurally complete but has no real tools; used for the
// request-validation vectors that must not touch the filesystem.
GpuShaderToolPackage syntheticRelocatedPackage() {
    GpuShaderToolPackage package;
    package.toolsDirectory = "bloom-gpu-tools";
    package.inventoryName = "inventory.json";
    package.glslangValidatorName = "glslangValidator";
    package.spirvValName = "spirv-val";
    package.relocated = true;
    return package;
}

void testInertConstruction(Expectations& expectations) {
    GpuOcioContextResolver resolver(GpuOcioContextRequest{
        fs::path{"/nonexistent/bloom-test-executable"}, syntheticRelocatedPackage(), {}, {}});
    expectations.expect(resolver.preparer() == nullptr,
                        "construction exposes no preparer before resolve");
    expectations.expect(resolver.counters().resolves == 0 && resolver.counters().cacheHits == 0,
                        "construction performs no resolve work");
}

void testEmptyRequestIsTypedUnavailable(Expectations& expectations) {
    GpuOcioContextResolver defaultResolver;
    const auto defaultResult = defaultResolver.resolve();
    expectations.expect(!defaultResult.hasValue() &&
                            defaultResult.error == GpuOcioContextError::ToolsUnavailable,
                        "a default request is a typed ToolsUnavailable refusal");
    expectations.expect(defaultResolver.preparer() == nullptr,
                        "an unavailable resolve publishes no preparer");

    GpuOcioContextResolver resolver(
        GpuOcioContextRequest{fs::path{"relative/app"}, GpuShaderToolPackage{}, {}, {}});
    const auto result = resolver.resolve();
    expectations.expect(!result.hasValue() && result.error == GpuOcioContextError::ToolsUnavailable,
                        "an empty package is a typed ToolsUnavailable refusal");
}

void testInvalidExecutableIsTyped(Expectations& expectations) {
    GpuOcioContextResolver resolver(
        GpuOcioContextRequest{fs::path{"relative/app"}, syntheticRelocatedPackage(), {}, {}});
    const auto result = resolver.resolve();
    expectations.expect(!result.hasValue() &&
                            result.error == GpuOcioContextError::InvalidExecutable,
                        "a relative application executable is rejected before tool work");
}

void testCancellationBeforeWorkIsTypedAndNotCached(Expectations& expectations) {
    GpuOcioContextResolver resolver;
    const auto cancelled = resolver.resolve([] { return true; });
    expectations.expect(!cancelled.hasValue() && cancelled.error == GpuOcioContextError::Cancelled,
                        "cancellation before tool work is a typed refusal");
    expectations.expect(resolver.counters().resolves == 0 && resolver.counters().cacheHits == 0,
                        "a cancelled resolve caches nothing");
    const auto retried = resolver.resolve();
    expectations.expect(!retried.hasValue() &&
                            retried.error == GpuOcioContextError::ToolsUnavailable,
                        "an uncancelled retry performs a real resolve, not a poisoned cache hit");
}

#if defined(BLOOM_GPU_TOOLS_AVAILABLE) && BLOOM_GPU_TOOLS_AVAILABLE

GpuShaderToolPackage packagedToolsDescriptor() {
    GpuShaderToolPackage package;
    package.toolsDirectory = BLOOM_GPU_TOOLS_DIR;
    package.inventoryName = BLOOM_GPU_TOOLS_INVENTORY_NAME;
    package.glslangValidatorName = BLOOM_GPU_TOOLS_GLSLANG_NAME;
    package.spirvValName = BLOOM_GPU_TOOLS_SPIRV_VAL_NAME;
#if defined(BLOOM_GPU_TOOLS_RELOCATED)
    package.relocated = BLOOM_GPU_TOOLS_RELOCATED != 0;
#endif
#if defined(BLOOM_GPU_TOOLS_BUNDLE_RELATIVE)
    package.bundleRelative = true;
#endif
#if defined(BLOOM_GPU_TOOLS_GLSLANG_STAGED_SHA256)
    package.glslangStagedDigest = parsePinnedDigest(BLOOM_GPU_TOOLS_GLSLANG_STAGED_SHA256);
#endif
#if defined(BLOOM_GPU_TOOLS_SPIRV_VAL_STAGED_SHA256)
    package.spirvValStagedDigest = parsePinnedDigest(BLOOM_GPU_TOOLS_SPIRV_VAL_STAGED_SHA256);
#endif
    return package;
}

fs::path testExecutable() { return fs::path{BLOOM_GPU_OCIO_CONTEXT_TEST_EXECUTABLE}; }

GpuOcioContextRequest packagedRequest() {
    return GpuOcioContextRequest{testExecutable(), packagedToolsDescriptor(), {}, {}};
}

void testResolveSharesOneContextAndPreparer(Expectations& expectations) {
    GpuOcioContextResolver resolver{packagedRequest()};
    const auto first = resolver.resolve();
    expectations.expect(first.hasValue(), "the packaged tools resolve to a context");
    if (!first.hasValue())
        return;
    expectations.expect(first.context->preparer != nullptr,
                        "the resolved context carries a live preparer");
    expectations.expect(resolver.preparer() == first.context->preparer,
                        "the resolver exposes the same shared preparer");

    const fs::path glslang{first.context->compileOptions.glslangValidatorPath};
    const fs::path spirvVal{first.context->compileOptions.spirvValPath};
    expectations.expect(glslang.is_absolute() && spirvVal.is_absolute(),
                        "the resolved tool paths are absolute");
    expectations.expect(glslang.filename().string() == BLOOM_GPU_TOOLS_GLSLANG_NAME &&
                            spirvVal.filename().string() == BLOOM_GPU_TOOLS_SPIRV_VAL_NAME,
                        "the resolved tool paths name the packaged tools");
    expectations.expect(glslang.parent_path().filename().string() == BLOOM_GPU_TOOLS_DIR &&
                            spirvVal.parent_path().filename().string() == BLOOM_GPU_TOOLS_DIR,
                        "the resolved tools live in the executable-relative packaged directory");
    expectations.expect(fs::is_regular_file(glslang) && fs::is_regular_file(spirvVal),
                        "the resolved tools name real staged artifacts");

    const auto second = resolver.resolve();
    expectations.expect(second.context == first.context,
                        "a second resolve returns the identical immutable context");
    expectations.expect(resolver.counters().resolves == 1 && resolver.counters().cacheHits == 1,
                        "the packaged tools are qualified exactly once");
}

void testResolvedToolsCompileWithScrubbedPath(Expectations& expectations) {
    GpuOcioContextResolver resolver{packagedRequest()};
    const auto result = resolver.resolve();
    if (!result.hasValue()) {
        expectations.expect(false, "the packaged tools resolve before the compile check");
        return;
    }
    setEnvironment("PATH", "");
    setEnvironment("LD_LIBRARY_PATH", "");
    GpuShaderCompiler compiler;
    GpuShaderCompileRequest request;
    request.computeShaderText = std::string(kComputeShader);
    request.tools.glslangValidator = result.context->compileOptions.glslangValidatorPath;
    request.tools.spirvVal = result.context->compileOptions.spirvValPath;
    const auto compiled = compiler.compile(request);
    clearEnvironment("PATH");
    clearEnvironment("LD_LIBRARY_PATH");
    expectations.expect(compiled.status == GpuShaderCompileStatus::Compiled && compiled.artifact,
                        "the resolved packaged tools compile+validate with a scrubbed PATH");
}

void testCancellationDuringHashingIsNotCached(Expectations& expectations) {
    GpuOcioContextResolver resolver{packagedRequest()};
    int cancelCalls = 0;
    const auto cancelled = resolver.resolve([&cancelCalls]() { return ++cancelCalls >= 3; });
    expectations.expect(cancelled.error == GpuOcioContextError::Cancelled,
                        "cancellation during tool hashing is a typed refusal");
    expectations.expect(resolver.counters().resolves == 0 && resolver.counters().cacheHits == 0,
                        "a transient cancellation is not cached");
    const auto retried = resolver.resolve();
    expectations.expect(retried.hasValue(), "an uncancelled retry on the same resolver succeeds");
    expectations.expect(resolver.counters().resolves == 1 && resolver.counters().cacheHits == 0,
                        "the retry performed a real qualification");
    const auto warm = resolver.resolve();
    expectations.expect(warm.context == retried.context && resolver.counters().cacheHits == 1,
                        "a subsequent call is a warm cache hit of the same context");
}

void testConcurrentReuseSharesOnePreparer(Expectations& expectations) {
    GpuOcioContextResolver resolver{packagedRequest()};
    constexpr int kThreads = 8;
    std::vector<std::shared_ptr<const GpuSceneOcioContext>> contexts(kThreads);
    std::vector<std::thread> threads;
    threads.reserve(kThreads);
    for (int index = 0; index < kThreads; ++index) {
        threads.emplace_back([&resolver, &contexts, index]() {
            contexts[static_cast<std::size_t>(index)] = resolver.resolve().context;
        });
    }
    for (auto& thread : threads)
        thread.join();
    bool allPresent = true;
    for (const auto& context : contexts)
        allPresent = allPresent && context != nullptr;
    expectations.expect(allPresent, "every concurrent resolve returned a context");
    bool allIdentical = contexts.front() != nullptr;
    for (const auto& context : contexts)
        allIdentical = allIdentical && context == contexts.front();
    expectations.expect(allIdentical, "every concurrent resolve shared one context");
    expectations.expect(resolver.counters().resolves == 1 &&
                            resolver.counters().cacheHits == kThreads - 1,
                        "a concurrent burst qualifies the tools once and shares one preparer");
}

class SiblingToolsDirectory final {
  public:
    explicit SiblingToolsDirectory(const std::string& name)
        : path_(testExecutable().parent_path() / name) {
        std::error_code error;
        fs::remove_all(path_, error);
        const auto source = testExecutable().parent_path() / BLOOM_GPU_TOOLS_DIR;
        fs::copy(source, path_, fs::copy_options::recursive | fs::copy_options::overwrite_existing,
                 error);
    }
    ~SiblingToolsDirectory() {
        std::error_code error;
        fs::remove_all(path_, error);
    }
    SiblingToolsDirectory(const SiblingToolsDirectory&) = delete;
    SiblingToolsDirectory& operator=(const SiblingToolsDirectory&) = delete;

    [[nodiscard]] const fs::path& path() const { return path_; }

  private:
    fs::path path_;
};

void testMissingToolsIsTyped(Expectations& expectations) {
    auto package = packagedToolsDescriptor();
    package.toolsDirectory = "bloom-gpu-tools-absent-for-context-test";
    GpuOcioContextResolver resolver(GpuOcioContextRequest{testExecutable(), package, {}, {}});
    const auto result = resolver.resolve();
    expectations.expect(!result.hasValue() && result.error == GpuOcioContextError::ToolsMissing,
                        "an absent packaged tools directory is a typed ToolsMissing refusal");
}

void testMalformedInventoryIsTyped(Expectations& expectations) {
    const SiblingToolsDirectory directory{"bloom-gpu-tools-context-malformed"};
    std::ofstream(directory.path() / BLOOM_GPU_TOOLS_INVENTORY_NAME,
                  std::ios::binary | std::ios::trunc)
        << "this is not the packaged inventory";
    auto package = packagedToolsDescriptor();
    package.toolsDirectory = directory.path().filename().string();
    GpuOcioContextResolver resolver(GpuOcioContextRequest{testExecutable(), package, {}, {}});
    const auto result = resolver.resolve();
    expectations.expect(!result.hasValue() && result.error == GpuOcioContextError::InventoryInvalid,
                        "a malformed packaged inventory is a typed refusal");
    expectations.expect(resolver.preparer() == nullptr,
                        "a malformed inventory publishes no preparer");
}

#endif // BLOOM_GPU_TOOLS_AVAILABLE

} // namespace

int main() {
    Expectations expectations;
    testInertConstruction(expectations);
    testEmptyRequestIsTypedUnavailable(expectations);
    testInvalidExecutableIsTyped(expectations);
    testCancellationBeforeWorkIsTypedAndNotCached(expectations);

#if defined(BLOOM_GPU_TOOLS_AVAILABLE) && BLOOM_GPU_TOOLS_AVAILABLE
    testResolveSharesOneContextAndPreparer(expectations);
    testResolvedToolsCompileWithScrubbedPath(expectations);
    testCancellationDuringHashingIsNotCached(expectations);
    testConcurrentReuseSharesOnePreparer(expectations);
    testMissingToolsIsTyped(expectations);
    testMalformedInventoryIsTyped(expectations);
#else
    std::cerr << "NOTE: packaged GPU shader tools unavailable; only typed-refusal vectors ran\n";
#endif
    return expectations.failures() == 0 ? 0 : 1;
}
