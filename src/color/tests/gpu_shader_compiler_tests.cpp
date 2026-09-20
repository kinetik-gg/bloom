#include <bloom/color/gpu_shader_compiler.hpp>
#include <bloom/core/sha256.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <vector>

namespace {

class Expectations final {
  public:
    void expect(const bool condition, const std::string_view message) {
        if (condition)
            return;
        ++failures_;
        std::cerr << "FAILED: " << message << '\n';
    }
    [[nodiscard]] int failures() const noexcept { return failures_; }

  private:
    int failures_ = 0;
};

using namespace std::chrono_literals;
using bloom::color::GpuShaderCompileError;
using bloom::color::GpuShaderCompileLimits;
using bloom::color::GpuShaderCompiler;
using bloom::color::GpuShaderCompileRequest;
using bloom::color::GpuShaderCompileStatus;

constexpr std::string_view kComputeShader = R"(#version 450
layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;
layout(set = 0, binding = 0, std140) uniform Params { vec4 scale; vec4 bias; } params;
layout(set = 0, binding = 1, std430) buffer Data { vec4 values[]; } data;
layout(set = 0, binding = 2, rgba32f) uniform image2D targetImage;
void main() {
    ivec2 coord = ivec2(gl_GlobalInvocationID.xy);
    uint index = gl_GlobalInvocationID.y * 8u + gl_GlobalInvocationID.x;
    vec4 value = data.values[index] * params.scale + params.bias;
    imageStore(targetImage, coord, value);
}
)";

std::span<const std::byte> asBytes(const std::uint8_t* data, const std::size_t size) {
    return std::as_bytes(std::span(data, size));
}

GpuShaderCompileRequest baseRequest(const std::string& glslang, const std::string& spirvVal) {
    GpuShaderCompileRequest request;
    request.computeShaderText = std::string(kComputeShader);
    request.tools.glslangValidator = glslang;
    request.tools.spirvVal = spirvVal;
    return request;
}

// A distinct, valid real shader: the workgroup size changes the emitted SPIR-V digest.
GpuShaderCompileRequest sizedRequest(const std::string& glslang, const std::string& spirvVal,
                                     const int localSize) {
    auto request = baseRequest(glslang, spirvVal);
    const std::string needle = "local_size_x = 8";
    const auto position = request.computeShaderText.find(needle);
    if (position != std::string::npos)
        request.computeShaderText.replace(position, needle.size(),
                                          "local_size_x = " + std::to_string(localSize));
    return request;
}

class FixtureDirectory final {
  public:
    explicit FixtureDirectory(const std::string& helper) {
        std::error_code error;
        const auto base = std::filesystem::temp_directory_path(error);
        if (error)
            return;
        const auto seed = std::chrono::steady_clock::now().time_since_epoch().count();
        for (int attempt = 0; attempt < 64 && path_.empty(); ++attempt) {
            auto candidate = base / ("bloom-gpu-shader-test-" + std::to_string(seed) + "-" +
                                     std::to_string(attempt));
            if (std::filesystem::create_directory(candidate, error) && !error)
                path_ = std::move(candidate);
        }
        helper_ = helper;
    }
    ~FixtureDirectory() {
        if (!path_.empty()) {
            std::error_code error;
            std::filesystem::remove_all(path_, error);
        }
    }
    [[nodiscard]] std::string tool(const std::string& name) {
        const auto link = path_ / name;
        std::error_code error;
        std::filesystem::create_symlink(helper_, link, error);
        return link.string();
    }

  private:
    std::filesystem::path path_;
    std::string helper_;
};

bool spirvMagic(const std::vector<std::uint8_t>& bytes) {
    return bytes.size() >= 4 && bytes[0] == 0x03 && bytes[1] == 0x02 && bytes[2] == 0x23 &&
           bytes[3] == 0x07;
}

void testRealCompileAndCache(Expectations& expectations, const std::string& glslang,
                             const std::string& spirvVal) {
    GpuShaderCompiler compiler;
    const auto request = baseRequest(glslang, spirvVal);
    const auto first = compiler.compile(request);
    expectations.expect(first.status == GpuShaderCompileStatus::Compiled &&
                            first.artifact.has_value(),
                        "a realistic multi-descriptor compute shader compiles and validates");
    if (first.artifact) {
        expectations.expect(spirvMagic(first.artifact->spirv), "output carries the SPIR-V magic");
        expectations.expect(first.artifact->spirv.size() % 4 == 0, "output is whole words");
        expectations.expect(first.artifact->entryPoint == "main", "entry point is recorded");
        expectations.expect(!first.artifact->compiler.version.empty(),
                            "compiler provenance records a version");
        expectations.expect(!first.artifact->validator.version.empty(),
                            "validator provenance records a version");
        const auto digest = bloom::core::Sha256Hasher::hash(
            asBytes(first.artifact->spirv.data(), first.artifact->spirv.size()));
        expectations.expect(digest.has_value() && *digest == first.artifact->spirvDigest,
                            "artifact digest is the exact SPIR-V hash");
        const auto sourceDigest = bloom::core::Sha256Hasher::hash(
            asBytes(reinterpret_cast<const std::uint8_t*>(request.computeShaderText.data()),
                    request.computeShaderText.size()));
        expectations.expect(sourceDigest.has_value() &&
                                first.artifact->sourceDigest == *sourceDigest,
                            "artifact carries the exact source digest");
        expectations.expect(first.artifact->sourceDigest != bloom::core::Sha256Digest{},
                            "artifact source provenance is non-zero");
    }
    const auto second = compiler.compile(request);
    expectations.expect(second.status == GpuShaderCompileStatus::Compiled,
                        "repeat compile still succeeds");
    expectations.expect(second.artifact.has_value() && first.artifact.has_value() &&
                            second.artifact->sourceDigest == first.artifact->sourceDigest,
                        "a compiler cache hit preserves the exact source digest");
    expectations.expect(compiler.stats().cacheHits == 1 && compiler.stats().cacheMisses == 1,
                        "identical request identity is a stable cache hit");
    auto changed = request;
    changed.computeShaderText += "\n";
    const auto changedResult = compiler.compile(changed);
    expectations.expect(changedResult.status == GpuShaderCompileStatus::Compiled,
                        "changed source still compiles");
    expectations.expect(changedResult.artifact.has_value() && first.artifact.has_value() &&
                            changedResult.artifact->sourceDigest != first.artifact->sourceDigest,
                        "a changed source carries a distinct source digest");
    expectations.expect(compiler.stats().cacheMisses == 2, "changed source misses the cache");
    auto retargeted = request;
    retargeted.targetEnvironment = "vulkan1.1";
    expectations.expect(compiler.compile(retargeted).status == GpuShaderCompileStatus::Compiled,
                        "alternate target still compiles");
    expectations.expect(compiler.stats().cacheMisses == 3, "changed target misses the cache");
}

void testStaticRejections(Expectations& expectations, const std::string& glslang,
                          const std::string& spirvVal) {
    GpuShaderCompiler compiler;
    const auto code = [](const bloom::color::GpuShaderCompileResult& result) {
        return result.failure ? std::optional<GpuShaderCompileError>(result.failure->code)
                              : std::nullopt;
    };
    auto request = baseRequest(glslang, spirvVal);
    request.computeShaderText.clear();
    expectations.expect(code(compiler.compile(request)) == GpuShaderCompileError::MalformedSource,
                        "empty source is rejected as malformed");
    request = baseRequest(glslang, spirvVal);
    request.limits.maxSourceBytes = 32;
    expectations.expect(code(compiler.compile(request)) == GpuShaderCompileError::SourceTooLarge,
                        "over-budget source is rejected before any tool runs");
    request = baseRequest(glslang, spirvVal);
    request.targetEnvironment = "vulkan9.9";
    expectations.expect(code(compiler.compile(request)) == GpuShaderCompileError::UnsupportedTarget,
                        "unknown target environment is rejected");
    request = baseRequest(glslang, spirvVal);
    request.entryPoint = "absent_entry_point";
    expectations.expect(code(compiler.compile(request)) == GpuShaderCompileError::MissingEntryPoint,
                        "a missing compute entry point is rejected");
    request = baseRequest("/nonexistent/glslangValidator", spirvVal);
    expectations.expect(code(compiler.compile(request)) == GpuShaderCompileError::InvalidTool,
                        "a nonexistent tool path is rejected");
    request = baseRequest("/bin/true", spirvVal);
    expectations.expect(code(compiler.compile(request)) == GpuShaderCompileError::ToolCrashed,
                        "a wrong tool that writes no SPIR-V is rejected");
    request = baseRequest(glslang, spirvVal);
    const auto beforeCancelled = compiler.compile(request, []() { return true; });
    expectations.expect(beforeCancelled.status == GpuShaderCompileStatus::Cancelled,
                        "cancellation before launch is honoured");
}

void testDiagnosticCeilings(Expectations& expectations, const std::string& glslang,
                            const std::string& spirvVal) {
    GpuShaderCompiler compiler;
    std::string noisy = "#version 450\nvoid main() {\n";
    for (int line = 0; line < 400; ++line)
        noisy += "missing" + std::to_string(line) + ";\n";
    noisy += "}\n";
    auto request = baseRequest(glslang, spirvVal);
    request.computeShaderText = noisy;
    request.limits.maxDiagnosticBytes = 256;
    const auto result = compiler.compile(request);
    expectations.expect(result.status == GpuShaderCompileStatus::Failed && result.failure &&
                            result.failure->code == GpuShaderCompileError::MalformedSource,
                        "a noisy malformed shader is rejected");
    if (result.failure)
        expectations.expect(result.failure->toolDiagnostic.size() <= 256,
                            "captured tool diagnostics stay inside the byte ceiling");
}

void testFakeToolFailures(Expectations& expectations, FixtureDirectory& fixtures,
                          const std::string& glslang, const std::string& spirvVal) {
    GpuShaderCompiler compiler;
    const auto code = [](const bloom::color::GpuShaderCompileResult& result) {
        return result.failure ? std::optional<GpuShaderCompileError>(result.failure->code)
                              : std::nullopt;
    };
    auto request = baseRequest(fixtures.tool("badspv"), spirvVal);
    expectations.expect(code(compiler.compile(request)) == GpuShaderCompileError::MalformedSource,
                        "structurally malformed SPIR-V is rejected before validation");
    request = baseRequest(fixtures.tool("badver"), spirvVal);
    expectations.expect(code(compiler.compile(request)) == GpuShaderCompileError::WrongSpirvVersion,
                        "SPIR-V with a version above the target is rejected");
    request = baseRequest(glslang, fixtures.tool("valfail"));
    const auto invalid = compiler.compile(request);
    expectations.expect(invalid.status == GpuShaderCompileStatus::Failed && invalid.failure &&
                            invalid.failure->code == GpuShaderCompileError::ValidationFailed,
                        "a failing spirv-val is a typed validation failure");
    request = baseRequest(fixtures.tool("hang"), spirvVal);
    request.limits.deadline = 250ms;
    const auto start = std::chrono::steady_clock::now();
    const auto timedOut = compiler.compile(request);
    const auto elapsed = std::chrono::steady_clock::now() - start;
    expectations.expect(timedOut.status == GpuShaderCompileStatus::Failed && timedOut.failure &&
                            timedOut.failure->code == GpuShaderCompileError::Timeout,
                        "a hung child hits the compile deadline");
    expectations.expect(elapsed < 5s, "a hung child is killed and reaped promptly");
    request = baseRequest(fixtures.tool("hang"), spirvVal);
    const auto cancelStart = std::chrono::steady_clock::now();
    const auto cancelled = compiler.compile(request, [cancelStart]() {
        return std::chrono::steady_clock::now() - cancelStart > 150ms;
    });
    expectations.expect(cancelled.status == GpuShaderCompileStatus::Cancelled,
                        "cancellation during a hung child terminates the compile");
}

std::optional<GpuShaderCompileError> errorCode(const bloom::color::GpuShaderCompileResult& result) {
    return result.failure ? std::optional<GpuShaderCompileError>(result.failure->code)
                          : std::nullopt;
}

void testSamePathReplacement(Expectations& expectations, const std::string& glslang,
                             const std::string& spirvVal) {
    std::error_code error;
    const auto base = std::filesystem::temp_directory_path(error);
    const auto directory =
        base / ("bloom-gpu-shader-replace-" +
                std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    if (error || !std::filesystem::create_directory(directory, error) || error) {
        expectations.expect(false, "replacement fixture directory is available");
        return;
    }
    const auto tool = directory / "glslangValidator";
    std::filesystem::copy_file(glslang, tool, std::filesystem::copy_options::overwrite_existing,
                               error);
    expectations.expect(!error, "copied tool is created");
    GpuShaderCompiler compiler;
    const auto request = baseRequest(tool.string(), spirvVal);
    expectations.expect(compiler.compile(request).status == GpuShaderCompileStatus::Compiled,
                        "copied tool compiles");
    expectations.expect(compiler.compile(request).status == GpuShaderCompileStatus::Compiled,
                        "same-path repeat compiles");
    expectations.expect(compiler.stats().cacheHits == 1 && compiler.stats().cacheMisses == 1,
                        "identical tool bytes are a stable cache hit");
    {
        std::ofstream append(tool, std::ios::binary | std::ios::app);
        append.put('\0');
        expectations.expect(static_cast<bool>(append), "tool bytes are replaced at the same path");
    }
    const auto replaced = compiler.compile(request);
    expectations.expect(compiler.stats().cacheMisses == 2,
                        "same-path replacement invalidates the cached tool identity");
    expectations.expect(replaced.status == GpuShaderCompileStatus::Compiled,
                        "a re-validated replacement still compiles");
    std::filesystem::remove_all(directory, error);
}

void testCachedBudgetAndHardCeilings(Expectations& expectations, const std::string& glslang,
                                     const std::string& spirvVal) {
    GpuShaderCompiler compiler;
    const auto request = baseRequest(glslang, spirvVal);
    const auto baseline = compiler.compile(request);
    expectations.expect(baseline.status == GpuShaderCompileStatus::Compiled && baseline.artifact,
                        "baseline compile produces SPIR-V");
    const std::size_t produced = baseline.artifact ? baseline.artifact->spirv.size() : 0;
    auto tighter = request;
    tighter.limits.maxSpirvBytes = produced > 4 ? 4 : 1;
    expectations.expect(errorCode(compiler.compile(tighter)) ==
                            GpuShaderCompileError::OutputTooLarge,
                        "a cache hit is re-checked against a tighter SPIR-V ceiling");
    auto raised = request;
    raised.limits.maxSourceBytes = std::size_t{128} * 1024u * 1024u;
    expectations.expect(errorCode(compiler.compile(raised)) == GpuShaderCompileError::LimitsInvalid,
                        "a caller cannot raise a limit above the hard ceiling");
    auto zeroDeadline = request;
    zeroDeadline.limits.deadline = std::chrono::milliseconds::zero();
    expectations.expect(errorCode(compiler.compile(zeroDeadline)) ==
                            GpuShaderCompileError::LimitsInvalid,
                        "a non-positive deadline is rejected");

    GpuShaderCompiler byteBounded(4, 1); // one-byte artifact budget cannot retain any real SPIR-V
    expectations.expect(byteBounded.compile(request).status == GpuShaderCompileStatus::Compiled,
                        "an artifact larger than the cache byte budget still compiles");
    expectations.expect(byteBounded.compile(request).status == GpuShaderCompileStatus::Compiled,
                        "the oversized artifact is recompiled");
    const auto bounded = byteBounded.stats();
    expectations.expect(bounded.cacheHits == 0 && bounded.cacheMisses == 2,
                        "the byte budget bounds what the cache actually retains");
    expectations.expect(bounded.cacheEntries == 0 && bounded.cacheBytes == 0,
                        "an oversized artifact contributes no entry or byte accounting");
}

void testConcurrentSameKey(Expectations& expectations, const std::string& glslang,
                           const std::string& spirvVal) {
    GpuShaderCompiler compiler;
    const auto request = baseRequest(glslang, spirvVal);
    std::atomic<int> compiled{0};
    std::mutex failuresMutex;
    struct AttemptFailure {
        GpuShaderCompileStatus status = GpuShaderCompileStatus::Failed;
        std::optional<GpuShaderCompileError> code;
        std::string diagnostic;
        int toolExitStatus = 0;
    };
    std::vector<AttemptFailure> failures;
    std::vector<std::thread> workers;
    workers.reserve(4);
    for (int worker = 0; worker < 4; ++worker) {
        workers.emplace_back([&compiler, &request, &compiled, &failuresMutex, &failures]() {
            for (int attempt = 0; attempt < 2; ++attempt) {
                const auto result = compiler.compile(request);
                if (result.status == GpuShaderCompileStatus::Compiled) {
                    ++compiled;
                    continue;
                }
                AttemptFailure failure;
                failure.status = result.status;
                if (result.failure) {
                    failure.code = result.failure->code;
                    failure.diagnostic = result.failure->diagnostic;
                    failure.toolExitStatus = result.failure->toolExitStatus;
                }
                const std::lock_guard lock(failuresMutex);
                failures.push_back(std::move(failure));
            }
        });
    }
    for (auto& worker : workers)
        worker.join();
    expectations.expect(compiled.load() == 8, "concurrent identical compiles all succeed");
    if (compiled.load() != 8) {
        const std::lock_guard lock(failuresMutex);
        std::cerr << "concurrent failures: compiled=" << compiled.load()
                  << " failed=" << failures.size() << '\n';
        for (const auto& failure : failures) {
            std::cerr << "  status=" << static_cast<int>(failure.status) << " code=";
            if (failure.code)
                std::cerr << static_cast<int>(*failure.code);
            else
                std::cerr << "none";
            std::cerr << " exit=" << failure.toolExitStatus << " diagnostic=" << failure.diagnostic
                      << '\n';
        }
    }
    GpuShaderCompiler reference;
    const auto single = reference.compile(request);
    expectations.expect(single.status == GpuShaderCompileStatus::Compiled && single.artifact,
                        "reference compile succeeds");
    const auto stats = compiler.stats();
    expectations.expect(stats.cacheEntries == 1,
                        "concurrent duplicate keys retain exactly one cache entry");
    if (single.artifact)
        expectations.expect(stats.cacheBytes == single.artifact->spirv.size(),
                            "concurrent duplicate keys do not inflate byte accounting");
}

void testSameSizeSameMtimeReplacement(Expectations& expectations, const std::string& glslang,
                                      const std::string& spirvVal) {
    std::error_code error;
    const auto base = std::filesystem::temp_directory_path(error);
    const auto directory =
        base / ("bloom-gpu-shader-mtime-" +
                std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    if (error || !std::filesystem::create_directory(directory, error) || error) {
        expectations.expect(false, "same-size fixture directory is available");
        return;
    }
    const auto tool = directory / "glslangValidator";
    std::filesystem::copy_file(glslang, tool, std::filesystem::copy_options::overwrite_existing,
                               error);
    const auto size = std::filesystem::file_size(tool, error);
    const auto writeTime = std::filesystem::last_write_time(tool, error);
    expectations.expect(!error && size > 0, "same-size fixture tool is available");
    GpuShaderCompiler compiler;
    const auto request = baseRequest(tool.string(), spirvVal);
    expectations.expect(compiler.compile(request).status == GpuShaderCompileStatus::Compiled,
                        "baseline tool compiles");
    expectations.expect(compiler.stats().cacheMisses == 1, "baseline is a cache miss");
    {
        std::fstream io(tool, std::ios::in | std::ios::out | std::ios::binary);
        io.seekg(static_cast<std::streamoff>(size / 2));
        char original = 0;
        io.read(&original, 1);
        io.clear();
        io.seekp(static_cast<std::streamoff>(size / 2));
        const char changed = static_cast<char>(original ^ 0x5A);
        io.write(&changed, 1);
    }
    std::filesystem::last_write_time(tool, writeTime, error);
    expectations.expect(!error, "same-size replacement preserves the original mtime");
    (void)compiler.compile(request);
    expectations.expect(compiler.stats().cacheMisses == 2,
                        "same-size same-mtime byte change is content-reverified, not trusted");
    std::filesystem::remove_all(directory, error);
}

void testDiagnosticsAndEntryPointNul(Expectations& expectations, const std::string& glslang,
                                     const std::string& spirvVal) {
    GpuShaderCompiler compiler;
    auto malformed = baseRequest(glslang, spirvVal);
    malformed.computeShaderText = "#version 450\nvoid main() { this_is_not_glsl_v1; }\n";
    const auto result = compiler.compile(malformed);
    expectations.expect(errorCode(result) == GpuShaderCompileError::MalformedSource,
                        "a real malformed shader is rejected by glslang");
    if (result.failure) {
        expectations.expect(!result.failure->diagnostic.empty(),
                            "a human diagnostic is always present");
        expectations.expect(!result.failure->toolDiagnostic.empty(),
                            "the real glslang stdout diagnostic is preserved");
        expectations.expect(result.failure->toolExitStatus != 0,
                            "the real glslang exit status is preserved");
    }
    auto embedded = baseRequest(glslang, spirvVal);
    embedded.entryPoint = "main";
    embedded.entryPoint.push_back('\0');
    expectations.expect(errorCode(compiler.compile(embedded)) ==
                            GpuShaderCompileError::MissingEntryPoint,
                        "an entry point with an embedded NUL is rejected");
}

void testCacheEviction(Expectations& expectations, const std::string& glslang,
                       const std::string& spirvVal) {
    const auto requestA = sizedRequest(glslang, spirvVal, 4);
    const auto requestB = sizedRequest(glslang, spirvVal, 8);
    const auto requestC = sizedRequest(glslang, spirvVal, 16);
    GpuShaderCompiler probe;
    const auto probeA = probe.compile(requestA);
    const auto probeB = probe.compile(requestB);
    const auto probeC = probe.compile(requestC);
    expectations.expect(probeA.status == GpuShaderCompileStatus::Compiled && probeA.artifact &&
                            probeB.artifact && probeC.artifact,
                        "three distinct real shaders all compile");
    if (!probeA.artifact || !probeB.artifact || !probeC.artifact)
        return;
    const std::uint64_t maxBytes =
        std::max({probeA.artifact->spirv.size(), probeB.artifact->spirv.size(),
                  probeC.artifact->spirv.size()});

    {
        GpuShaderCompiler capacityOne(1);
        const auto a = capacityOne.compile(requestA);
        expectations.expect(a.status == GpuShaderCompileStatus::Compiled && a.artifact,
                            "capacity-1: first insert compiles");
        if (!a.artifact)
            return;
        auto stats = capacityOne.stats();
        expectations.expect(stats.cacheEntries == 1 && stats.cacheBytes == a.artifact->spirv.size(),
                            "capacity-1: exact entry/byte accounting after first insert");
        (void)capacityOne.compile(requestA);
        expectations.expect(capacityOne.stats().cacheHits == 1,
                            "capacity-1: revisit of the first shader is a hit");
        const auto b = capacityOne.compile(requestB);
        expectations.expect(b.status == GpuShaderCompileStatus::Compiled && b.artifact,
                            "capacity-1: second insert compiles");
        if (!b.artifact)
            return;
        stats = capacityOne.stats();
        expectations.expect(stats.cacheEntries == 1 && stats.cacheBytes == b.artifact->spirv.size(),
                            "capacity-1: second insert evicts the first with exact bytes");
        const auto revisitA = capacityOne.compile(requestA);
        expectations.expect(revisitA.status == GpuShaderCompileStatus::Compiled &&
                                revisitA.artifact,
                            "capacity-1: evicted first shader recompiles");
        if (!revisitA.artifact)
            return;
        expectations.expect(capacityOne.stats().cacheMisses == 3,
                            "capacity-1: evicted first shader is a miss");
        stats = capacityOne.stats();
        expectations.expect(stats.cacheEntries == 1 &&
                                stats.cacheBytes == revisitA.artifact->spirv.size(),
                            "capacity-1: exact accounting after re-insert");
        (void)capacityOne.compile(requestC);
        expectations.expect(capacityOne.stats().cacheEntries == 1,
                            "capacity-1: third insert keeps one entry");
    }

    {
        GpuShaderCompiler byteBudget(8, static_cast<std::size_t>(maxBytes));
        const auto a = byteBudget.compile(requestA);
        expectations.expect(a.status == GpuShaderCompileStatus::Compiled && a.artifact,
                            "byte-budget: first insert compiles");
        if (!a.artifact)
            return;
        auto stats = byteBudget.stats();
        expectations.expect(stats.cacheEntries == 1 && stats.cacheBytes == a.artifact->spirv.size(),
                            "byte-budget: exact accounting after first insert");
        const auto b = byteBudget.compile(requestB);
        expectations.expect(b.status == GpuShaderCompileStatus::Compiled && b.artifact,
                            "byte-budget: second insert compiles");
        if (!b.artifact)
            return;
        stats = byteBudget.stats();
        expectations.expect(stats.cacheEntries == 1 && stats.cacheBytes == b.artifact->spirv.size(),
                            "byte-budget: second insert evicts the first with exact bytes");
        const auto revisitA = byteBudget.compile(requestA);
        expectations.expect(revisitA.status == GpuShaderCompileStatus::Compiled &&
                                revisitA.artifact,
                            "byte-budget: evicted first shader recompiles");
        if (!revisitA.artifact)
            return;
        expectations.expect(byteBudget.stats().cacheMisses == 3,
                            "byte-budget: evicted first shader is a miss");
        stats = byteBudget.stats();
        expectations.expect(stats.cacheEntries == 1 &&
                                stats.cacheBytes == revisitA.artifact->spirv.size(),
                            "byte-budget: exact accounting after re-insert");
        (void)byteBudget.compile(requestC);
        stats = byteBudget.stats();
        expectations.expect(stats.cacheEntries == 1 && stats.cacheBytes <= maxBytes,
                            "byte-budget: third insert stays inside the byte budget");
    }
}

} // namespace

int main() {
    Expectations expectations;
    const char* glslang = std::getenv("BLOOM_GLSLANG_VALIDATOR");
    const char* spirvVal = std::getenv("BLOOM_SPIRV_VAL");
    const char* helper = BLOOM_GPU_SHADER_TEST_HELPER;
    if (glslang == nullptr || spirvVal == nullptr) {
        std::cerr << "SKIP: qualified glslangValidator/spirv-val paths not provided\n";
        return 77;
    }
    FixtureDirectory fixtures(helper);
    testRealCompileAndCache(expectations, glslang, spirvVal);
    testStaticRejections(expectations, glslang, spirvVal);
    testDiagnosticCeilings(expectations, glslang, spirvVal);
    testFakeToolFailures(expectations, fixtures, glslang, spirvVal);
    testSamePathReplacement(expectations, glslang, spirvVal);
    testSameSizeSameMtimeReplacement(expectations, glslang, spirvVal);
    testCachedBudgetAndHardCeilings(expectations, glslang, spirvVal);
    testConcurrentSameKey(expectations, glslang, spirvVal);
    testDiagnosticsAndEntryPointNul(expectations, glslang, spirvVal);
    testCacheEviction(expectations, glslang, spirvVal);
    return expectations.failures() == 0 ? 0 : 1;
}
