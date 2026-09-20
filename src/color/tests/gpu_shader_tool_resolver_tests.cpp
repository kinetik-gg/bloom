#include <bloom/color/gpu_shader_compiler.hpp>
#include <bloom/color/gpu_shader_tool_resolver.hpp>
#include <bloom/core/sha256.hpp>

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace {

using namespace std::chrono_literals;
namespace fs = std::filesystem;
using bloom::color::GpuShaderCompiler;
using bloom::color::GpuShaderCompileRequest;
using bloom::color::GpuShaderCompileStatus;
using bloom::color::GpuShaderToolPackage;
using bloom::color::GpuShaderToolResolveError;
using bloom::color::GpuShaderToolResolveLimits;
using bloom::color::GpuShaderToolResolver;

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

std::string sha256Text(const bloom::core::Sha256Digest& digest) {
    const auto text = digest.toLowercaseHex();
    return "sha256:" + std::string(text.data(), text.size());
}

bloom::core::Sha256Digest hashFile(const fs::path& path) {
    std::ifstream input(path, std::ios::binary);
    std::vector<char> bytes{std::istreambuf_iterator<char>(input),
                            std::istreambuf_iterator<char>()};
    return *bloom::core::Sha256Hasher::hash(std::as_bytes(std::span(bytes)));
}

bloom::core::Sha256Digest digestOf(std::string_view text) {
    return *bloom::core::Sha256Hasher::hash(std::as_bytes(std::span(text.data(), text.size())));
}

std::string inventoryText(const bool relocated, const std::string& glslangStaged,
                          const std::string& spirvStaged, const std::string& glslangSource,
                          const std::string& spirvSource) {
    std::string text = "{\"format\":\"org.kinetik.bloom.gpu-shader-tools.inventory\","
                       "\"version\":1,\"layout\":\"executable-relative\","
                       "\"directory\":\"bloom-gpu-tools\",\"relocated\":";
    text += relocated ? "true" : "false";
    text += ",\"tools\":[{\"name\":\"glslangValidator\",\"file\":\"glslangValidator\","
            "\"component\":\"glslang\",\"componentVersion\":\"16.4.0\","
            "\"source\":\"bin/glslangValidator\",\"sourceSha256\":\"" +
            glslangSource + "\",\"stagedSha256\":\"" + glslangStaged +
            "\",\"runtimeLibraries\":[]},{\"name\":\"spirv-val\",\"file\":\"spirv-val\","
            "\"component\":\"spirv-tools\",\"componentVersion\":\"b707790\","
            "\"source\":\"bin/spirv-val\",\"sourceSha256\":\"" +
            spirvSource + "\",\"stagedSha256\":\"" + spirvStaged +
            "\",\"runtimeLibraries\":[]}],\"licenses\":[\"licenses/glslang-LICENSE.txt\"]}";
    return text;
}

class Fixture final {
  public:
    Fixture(const std::string& glslang, const std::string& spirvVal) {
        std::error_code error;
        const auto base = fs::temp_directory_path(error);
        for (int attempt = 0; attempt < 64 && root_.empty(); ++attempt) {
            auto candidate =
                base /
                ("bloom-gpu-tools-resolve-" +
                 std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "-" +
                 std::to_string(attempt));
            if (fs::create_directory(candidate, error) && !error)
                root_ = std::move(candidate);
        }
        if (root_.empty())
            return;
        fs::create_directories(bin(), error);
        {
            std::ofstream app(bin() / "app", std::ios::binary);
            app << "fake application executable\n";
        }
        fs::create_directory(tools(), error);
        fs::copy_file(glslang, tools() / "glslangValidator", fs::copy_options::overwrite_existing,
                      error);
        fs::copy_file(spirvVal, tools() / "spirv-val", fs::copy_options::overwrite_existing, error);
        glslangDigest_ = hashFile(tools() / "glslangValidator");
        spirvDigest_ = hashFile(tools() / "spirv-val");
        writeInventory(false, sha256Text(glslangDigest_), sha256Text(spirvDigest_),
                       sha256Text(glslangDigest_), sha256Text(spirvDigest_));
    }
    ~Fixture() {
        if (!root_.empty()) {
            std::error_code error;
            fs::remove_all(root_, error);
        }
    }
    Fixture(const Fixture&) = delete;
    Fixture& operator=(const Fixture&) = delete;

    [[nodiscard]] bool ready() const { return !root_.empty(); }
    [[nodiscard]] fs::path bin() const { return root_ / "bin"; }
    [[nodiscard]] fs::path tools() const { return bin() / "bloom-gpu-tools"; }
    [[nodiscard]] fs::path app() const { return bin() / "app"; }
    [[nodiscard]] bloom::core::Sha256Digest glslangDigest() const { return glslangDigest_; }
    [[nodiscard]] bloom::core::Sha256Digest spirvDigest() const { return spirvDigest_; }

    void writeInventory(const bool relocated, const std::string& glslangStaged,
                        const std::string& spirvStaged, const std::string& glslangSource,
                        const std::string& spirvSource) const {
        std::ofstream out(tools() / "inventory.json", std::ios::binary | std::ios::trunc);
        out << inventoryText(relocated, glslangStaged, spirvStaged, glslangSource, spirvSource);
    }
    void writeRawInventory(const std::string& text) const {
        std::ofstream out(tools() / "inventory.json", std::ios::binary | std::ios::trunc);
        out << text;
    }

  private:
    fs::path root_;
    bloom::core::Sha256Digest glslangDigest_{};
    bloom::core::Sha256Digest spirvDigest_{};
};

GpuShaderToolPackage packageFor(const Fixture& fixture) {
    GpuShaderToolPackage package;
    package.toolsDirectory = "bloom-gpu-tools";
    package.inventoryName = "inventory.json";
    package.glslangValidatorName = "glslangValidator";
    package.spirvValName = "spirv-val";
    package.glslangStagedDigest = fixture.glslangDigest();
    package.spirvValStagedDigest = fixture.spirvDigest();
    return package;
}

GpuShaderToolPackage relocatedPackage() {
    GpuShaderToolPackage package;
    package.toolsDirectory = "bloom-gpu-tools";
    package.inventoryName = "inventory.json";
    package.glslangValidatorName = "glslangValidator";
    package.spirvValName = "spirv-val";
    package.relocated = true;
    return package;
}

std::optional<GpuShaderToolResolveError> codeOf(const bloom::color::GpuShaderToolResolveResult& r) {
    return r.failure ? std::optional<GpuShaderToolResolveError>(r.failure->code) : std::nullopt;
}

void testValidStagedToolsCompile(Expectations& expectations, const std::string& glslang,
                                 const std::string& spirvVal) {
    Fixture fixture(glslang, spirvVal);
    GpuShaderToolResolver resolver;
    const auto resolved = resolver.resolve(fixture.app(), packageFor(fixture));
    expectations.expect(resolved.ok, "valid staged tools resolve");
    expectations.expect(
        resolved.paths.glslangValidator.ends_with("bloom-gpu-tools/glslangValidator"),
        "resolved glslang path is inside the owned tools directory");
    expectations.expect(resolved.paths.spirvVal.ends_with("bloom-gpu-tools/spirv-val"),
                        "resolved spirv path is inside the owned tools directory");
    (void)resolver.resolve(fixture.app(), packageFor(fixture));
    expectations.expect(resolver.stats().cacheHits == 1 && resolver.stats().resolves == 1,
                        "startup qualification is hashed once and cached");
    if (!resolved.ok)
        return;
    ::setenv("PATH", "", 1);
    ::setenv("LD_LIBRARY_PATH", "", 1);
    GpuShaderCompiler compiler;
    GpuShaderCompileRequest request;
    request.computeShaderText = std::string(kComputeShader);
    request.tools = resolved.paths;
    const auto compiled = compiler.compile(request);
    ::unsetenv("PATH");
    ::unsetenv("LD_LIBRARY_PATH");
    expectations.expect(compiled.status == GpuShaderCompileStatus::Compiled && compiled.artifact,
                        "resolved tools compile+validate with a scrubbed PATH");
}

void testMissingTool(Expectations& expectations, const std::string& glslang,
                     const std::string& spirvVal) {
    Fixture fixture(glslang, spirvVal);
    std::error_code error;
    fs::remove(fixture.tools() / "glslangValidator", error);
    GpuShaderToolResolver resolver;
    expectations.expect(codeOf(resolver.resolve(fixture.app(), packageFor(fixture))) ==
                            GpuShaderToolResolveError::ToolsMissing,
                        "a missing staged tool fails typed");
}

void testWrongDigest(Expectations& expectations, const std::string& glslang,
                     const std::string& spirvVal) {
    Fixture fixture(glslang, spirvVal);
    auto package = packageFor(fixture);
    package.glslangStagedDigest = digestOf("wrong bytes");
    GpuShaderToolResolver resolver;
    expectations.expect(codeOf(resolver.resolve(fixture.app(), package)) ==
                            GpuShaderToolResolveError::DigestMismatch,
                        "a wrong staged digest fails typed");
}

void testSymlinkOutside(Expectations& expectations, const std::string& glslang,
                        const std::string& spirvVal) {
    Fixture fixture(glslang, spirvVal);
    std::error_code error;
    fs::create_directory(fixture.bin() / "outside", error);
    fs::copy_file(fixture.tools() / "spirv-val", fixture.bin() / "outside" / "glslangValidator",
                  fs::copy_options::overwrite_existing, error);
    fs::remove(fixture.tools() / "glslangValidator", error);
    fs::create_symlink(fixture.bin() / "outside" / "glslangValidator",
                       fixture.tools() / "glslangValidator", error);
    GpuShaderToolResolver resolver;
    expectations.expect(codeOf(resolver.resolve(fixture.app(), packageFor(fixture))) ==
                            GpuShaderToolResolveError::OutsideToolsDirectory,
                        "a symlink escaping the owned directory fails typed");
}

void testInvalidInventory(Expectations& expectations, const std::string& glslang,
                          const std::string& spirvVal) {
    Fixture fixture(glslang, spirvVal);
    fixture.writeRawInventory("not json at all");
    GpuShaderToolResolver resolver;
    expectations.expect(codeOf(resolver.resolve(fixture.app(), packageFor(fixture))) ==
                            GpuShaderToolResolveError::InventoryInvalid,
                        "a malformed inventory fails typed");
}

void testSourceStagedMismatch(Expectations& expectations, const std::string& glslang,
                              const std::string& spirvVal) {
    Fixture fixture(glslang, spirvVal);
    const std::string source = sha256Text(digestOf("source identity differs"));
    fixture.writeInventory(false, sha256Text(fixture.glslangDigest()),
                           sha256Text(fixture.spirvDigest()), source,
                           sha256Text(fixture.spirvDigest()));
    GpuShaderToolResolver resolver;
    expectations.expect(codeOf(resolver.resolve(fixture.app(), packageFor(fixture))) ==
                            GpuShaderToolResolveError::SourceStagedMismatch,
                        "non-relocated source != staged is rejected");
}

void testDescriptorAndCancellation(Expectations& expectations, const std::string& glslang,
                                   const std::string& spirvVal) {
    Fixture fixture(glslang, spirvVal);
    GpuShaderToolResolver resolver;
    expectations.expect(codeOf(resolver.resolve(fixture.app(), GpuShaderToolPackage{})) ==
                            GpuShaderToolResolveError::Disabled,
                        "an empty descriptor is a typed disabled result");
    expectations.expect(codeOf(resolver.resolve("relative/app", packageFor(fixture))) ==
                            GpuShaderToolResolveError::InvalidExecutable,
                        "a relative executable path is rejected");
    expectations.expect(
        codeOf(resolver.resolve(fixture.app(), packageFor(fixture), {}, []() { return true; })) ==
            GpuShaderToolResolveError::Cancelled,
        "cancellation before hashing is honoured");
}

void testCancelledRetry(Expectations& expectations, const std::string& glslang,
                        const std::string& spirvVal) {
    Fixture fixture(glslang, spirvVal);
    GpuShaderToolResolver resolver;
    int cancelCalls = 0;
    const auto cancelled = resolver.resolve(fixture.app(), packageFor(fixture), {},
                                            [&cancelCalls]() { return ++cancelCalls >= 3; });
    expectations.expect(codeOf(cancelled) == GpuShaderToolResolveError::Cancelled,
                        "cancellation during hashing is reported");
    expectations.expect(resolver.stats().resolves == 1 && resolver.stats().cacheHits == 0,
                        "a transient cancellation is not cached");
    const auto retried = resolver.resolve(fixture.app(), packageFor(fixture));
    expectations.expect(retried.ok, "an uncancelled retry on the same resolver succeeds");
    expectations.expect(resolver.stats().resolves == 2 && resolver.stats().cacheHits == 0,
                        "the retry performed a real resolution, not a poisoned cache hit");
    const auto warm = resolver.resolve(fixture.app(), packageFor(fixture));
    expectations.expect(warm.ok && resolver.stats().cacheHits == 1,
                        "a subsequent call is a warm cache hit");
}

void testWarmLimits(Expectations& expectations, const std::string& glslang,
                    const std::string& spirvVal) {
    Fixture fixture(glslang, spirvVal);
    GpuShaderToolResolver resolver;
    expectations.expect(resolver.resolve(fixture.app(), packageFor(fixture)).ok,
                        "baseline qualification succeeds");

    auto tightTool = packageFor(fixture);
    GpuShaderToolResolveLimits toolLimits;
    toolLimits.maxToolBytes = 16;
    expectations.expect(codeOf(resolver.resolve(fixture.app(), tightTool, toolLimits)) ==
                            GpuShaderToolResolveError::SizeLimit,
                        "a warm hit honors a tightened tool byte ceiling");
    expectations.expect(resolver.stats().resolves == 1 && resolver.stats().cacheHits == 0,
                        "a refused warm hit performs no rehash");

    GpuShaderToolResolveLimits inventoryLimits;
    inventoryLimits.maxInventoryBytes = 1;
    expectations.expect(codeOf(resolver.resolve(fixture.app(), tightTool, inventoryLimits)) ==
                            GpuShaderToolResolveError::SizeLimit,
                        "a warm hit honors a tightened inventory byte ceiling");
    expectations.expect(resolver.stats().resolves == 1,
                        "the inventory refusal also does not rehash");

    GpuShaderToolResolveLimits nonsensical;
    nonsensical.maxToolBytes = 0;
    expectations.expect(codeOf(resolver.resolve(fixture.app(), tightTool, nonsensical)) ==
                            GpuShaderToolResolveError::LimitsInvalid,
                        "zero tool ceiling is rejected upfront");
    GpuShaderToolResolveLimits zeroDeadline;
    zeroDeadline.deadline = std::chrono::milliseconds::zero();
    expectations.expect(codeOf(resolver.resolve(fixture.app(), tightTool, zeroDeadline)) ==
                            GpuShaderToolResolveError::LimitsInvalid,
                        "a non-positive deadline is rejected upfront");

    const auto restored = resolver.resolve(fixture.app(), packageFor(fixture));
    expectations.expect(restored.ok && resolver.stats().cacheHits == 1 &&
                            resolver.stats().resolves == 1,
                        "restoring the limits returns the warm success without rehashing");
}

void testInventorySymlinkEscape(Expectations& expectations, const std::string& glslang,
                                const std::string& spirvVal) {
    Fixture fixture(glslang, spirvVal);
    std::error_code error;
    fs::create_directory(fixture.bin() / "outside", error);
    std::ofstream(fixture.bin() / "outside" / "inventory.json") << inventoryText(
        false, sha256Text(fixture.glslangDigest()), sha256Text(fixture.spirvDigest()),
        sha256Text(fixture.glslangDigest()), sha256Text(fixture.spirvDigest()));
    fs::remove(fixture.tools() / "inventory.json", error);
    fs::create_symlink(fixture.bin() / "outside" / "inventory.json",
                       fixture.tools() / "inventory.json", error);
    GpuShaderToolResolver resolver;
    expectations.expect(codeOf(resolver.resolve(fixture.app(), packageFor(fixture))) ==
                            GpuShaderToolResolveError::OutsideToolsDirectory,
                        "an inventory symlink escaping the owned directory fails typed");
}

void testRelocatedInventory(Expectations& expectations, const std::string& glslang,
                            const std::string& spirvVal) {
    Fixture fixture(glslang, spirvVal);
    fixture.writeInventory(true, sha256Text(fixture.glslangDigest()),
                           sha256Text(fixture.spirvDigest()),
                           sha256Text(digestOf("original glslang source")),
                           sha256Text(digestOf("original spirv source")));
    GpuShaderToolResolver resolver;
    const auto resolved = resolver.resolve(fixture.app(), relocatedPackage());
    expectations.expect(resolved.ok, "a relocated package validates through its bounded inventory");
    if (!resolved.ok)
        return;
    GpuShaderCompiler compiler;
    GpuShaderCompileRequest request;
    request.computeShaderText = std::string(kComputeShader);
    request.tools = resolved.paths;
    expectations.expect(compiler.compile(request).status == GpuShaderCompileStatus::Compiled,
                        "relocated tools compile+validate");
}

} // namespace

int main() {
    const char* glslang = std::getenv("BLOOM_GLSLANG_VALIDATOR");
    const char* spirvVal = std::getenv("BLOOM_SPIRV_VAL");
    if (glslang == nullptr || spirvVal == nullptr) {
        std::cerr << "SKIP: qualified glslangValidator/spirv-val paths not provided\n";
        return 77;
    }
    Expectations expectations;
    testValidStagedToolsCompile(expectations, glslang, spirvVal);
    testCancelledRetry(expectations, glslang, spirvVal);
    testWarmLimits(expectations, glslang, spirvVal);
    testInventorySymlinkEscape(expectations, glslang, spirvVal);
    testRelocatedInventory(expectations, glslang, spirvVal);
    testMissingTool(expectations, glslang, spirvVal);
    testWrongDigest(expectations, glslang, spirvVal);
    testSymlinkOutside(expectations, glslang, spirvVal);
    testInvalidInventory(expectations, glslang, spirvVal);
    testSourceStagedMismatch(expectations, glslang, spirvVal);
    testDescriptorAndCancellation(expectations, glslang, spirvVal);
    return expectations.failures() == 0 ? 0 : 1;
}
