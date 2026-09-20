#include <bloom/color/gpu_shader_compiler.hpp>

#include "gpu_shader_compiler_process.hpp"

#include <bloom/platform/process_supervisor.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <deque>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <optional>
#include <span>
#include <string_view>
#include <system_error>
#include <unordered_map>
#include <utility>

namespace bloom::color {
namespace {

using Clock = std::chrono::steady_clock;
using platform::ProcessDeadline;
using platform::ProcessError;
using platform::ProcessFailure;
using platform::ProcessOptions;
using platform::ProcessSupervisor;

constexpr std::uint32_t kSpirvMagic = 0x07230203u;
constexpr std::uint16_t kOpEntryPoint = 15u;
constexpr std::uint32_t kExecutionModelGLCompute = 5u;

constexpr std::size_t kExecutableByteCeiling = 1u << 30;

// Hard ceilings. A request may lower a limit but never raise it above these; the check runs before
// any allocation, child launch, or read so an oversized caller value cannot become a real budget.
constexpr std::size_t kHardMaxSourceBytes = 64u * 1024u * 1024u;
constexpr std::size_t kHardMaxSpirvBytes = 256u * 1024u * 1024u;
constexpr std::size_t kHardMaxDiagnosticBytes = 1u * 1024u * 1024u;
constexpr std::uint64_t kHardMinAddressSpaceBytes = 64ull * 1024ull * 1024ull;
constexpr std::uint64_t kHardMaxAddressSpaceBytes = 4ull * 1024ull * 1024ull * 1024ull;
constexpr std::uint32_t kHardMinOpenFiles = 8;
constexpr std::uint32_t kHardMaxOpenFiles = 4096;
constexpr std::chrono::milliseconds kHardMaxDeadline{120000};
constexpr std::size_t kMaxToolCacheEntries = 32;

std::optional<GpuShaderCompileError> validateLimits(const GpuShaderCompileLimits& limits) {
    if (limits.maxSourceBytes == 0 || limits.maxSourceBytes > kHardMaxSourceBytes)
        return GpuShaderCompileError::LimitsInvalid;
    if (limits.maxSpirvBytes == 0 || limits.maxSpirvBytes > kHardMaxSpirvBytes)
        return GpuShaderCompileError::LimitsInvalid;
    if (limits.maxDiagnosticBytes == 0 || limits.maxDiagnosticBytes > kHardMaxDiagnosticBytes)
        return GpuShaderCompileError::LimitsInvalid;
    if (limits.deadline <= std::chrono::milliseconds::zero() || limits.deadline > kHardMaxDeadline)
        return GpuShaderCompileError::LimitsInvalid;
    if (limits.addressSpaceBytes < kHardMinAddressSpaceBytes ||
        limits.addressSpaceBytes > kHardMaxAddressSpaceBytes)
        return GpuShaderCompileError::LimitsInvalid;
    if (limits.openFiles < kHardMinOpenFiles || limits.openFiles > kHardMaxOpenFiles)
        return GpuShaderCompileError::LimitsInvalid;
    return std::nullopt;
}

using gpu_shader_detail::runProcess;
using gpu_shader_detail::ToolRun;

std::optional<int> maxSpirvMinor(std::string_view target) {
    if (target == "vulkan1.0")
        return 0;
    if (target == "vulkan1.1")
        return 3;
    if (target == "vulkan1.1spv1.4")
        return 4;
    if (target == "vulkan1.2")
        return 5;
    if (target == "vulkan1.3")
        return 6;
    if (target == "vulkan1.4")
        return 6;
    return std::nullopt;
}

enum class SpirvCheck {
    Ok,
    Structure,
    Version,
    EntryPoint,
};

std::uint32_t wordAt(const std::vector<std::uint8_t>& bytes, std::size_t index) {
    return static_cast<std::uint32_t>(bytes[index]) |
           (static_cast<std::uint32_t>(bytes[index + 1]) << 8) |
           (static_cast<std::uint32_t>(bytes[index + 2]) << 16) |
           (static_cast<std::uint32_t>(bytes[index + 3]) << 24);
}

SpirvCheck checkSpirv(const std::vector<std::uint8_t>& bytes, std::string_view entryPoint,
                      int allowedMinor) {
    if (bytes.size() < 20 || bytes.size() % 4 != 0)
        return SpirvCheck::Structure;
    if (wordAt(bytes, 0) != kSpirvMagic)
        return SpirvCheck::Structure;
    const std::uint32_t version = wordAt(bytes, 4);
    const int major = static_cast<int>((version >> 16) & 0xffu);
    const int minor = static_cast<int>((version >> 8) & 0xffu);
    if (major != 1 || minor > allowedMinor)
        return SpirvCheck::Version;
    const std::size_t words = bytes.size() / 4;
    bool found = false;
    for (std::size_t index = 5; index < words;) {
        const std::uint32_t instruction = wordAt(bytes, index * 4);
        const std::size_t declared = instruction >> 16;
        if (declared == 0 || index + declared > words)
            return SpirvCheck::Structure;
        if ((instruction & 0xffffu) == kOpEntryPoint && declared >= 3) {
            const std::uint32_t model = wordAt(bytes, (index + 1) * 4);
            std::string name;
            const std::size_t begin = (index + 3) * 4;
            const std::size_t end = (index + declared) * 4;
            for (std::size_t offset = begin; offset < end; ++offset) {
                if (bytes[offset] == 0)
                    break;
                name.push_back(static_cast<char>(bytes[offset]));
            }
            if (model == kExecutionModelGLCompute && name == entryPoint)
                found = true;
        }
        index += declared;
    }
    return found ? SpirvCheck::Ok : SpirvCheck::EntryPoint;
}

std::optional<std::vector<std::uint8_t>> readWholeFile(const std::string& path,
                                                       std::size_t ceiling) {
    std::error_code error;
    const auto size = std::filesystem::file_size(path, error);
    if (error || size > ceiling)
        return std::nullopt;
    std::ifstream input(path, std::ios::binary);
    if (!input)
        return std::nullopt;
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
    input.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (!input)
        return std::nullopt;
    return bytes;
}

class TempDirectory final {
  public:
    explicit TempDirectory(std::filesystem::path path) : path_(std::move(path)) {}
    ~TempDirectory() {
        std::error_code error;
        std::filesystem::remove_all(path_, error);
    }
    TempDirectory(const TempDirectory&) = delete;
    TempDirectory& operator=(const TempDirectory&) = delete;
    TempDirectory(TempDirectory&&) noexcept = default;
    TempDirectory& operator=(TempDirectory&&) noexcept = default;
    const std::filesystem::path& path() const { return path_; }

  private:
    std::filesystem::path path_;
};

std::optional<TempDirectory> makeTempDirectory() {
    std::error_code error;
    const auto base = std::filesystem::temp_directory_path(error);
    if (error)
        return std::nullopt;
    const auto seed = Clock::now().time_since_epoch().count();
    for (int attempt = 0; attempt < 128; ++attempt) {
        auto candidate =
            base / ("bloom-gpu-shader-" + std::to_string(seed) + "-" + std::to_string(attempt));
        if (std::filesystem::create_directory(candidate, error) && !error)
            return TempDirectory(std::move(candidate));
    }
    return std::nullopt;
}

std::string hex(const core::Sha256Digest& digest) {
    const auto text = digest.toLowercaseHex();
    return std::string(text.data(), text.size());
}

std::string trimFirstLine(const std::vector<std::uint8_t>& bytes) {
    std::string text(bytes.begin(), bytes.end());
    const auto newline = text.find('\n');
    if (newline != std::string::npos)
        text.resize(newline);
    while (!text.empty() && (text.back() == '\r' || text.back() == ' '))
        text.pop_back();
    return text;
}

GpuShaderCompileResult failed(GpuShaderCompileError code, std::string diagnostic,
                              const ToolRun* run = nullptr) {
    GpuShaderCompileResult result;
    result.status = GpuShaderCompileStatus::Failed;
    GpuShaderCompileFailure failure;
    failure.code = code;
    failure.diagnostic = std::move(diagnostic);
    if (run != nullptr) {
        failure.toolDiagnostic = run->output;
        failure.toolExitStatus = run->exitStatus;
    }
    result.failure = std::move(failure);
    return result;
}

GpuShaderCompileResult cancelled() {
    GpuShaderCompileResult result;
    result.status = GpuShaderCompileStatus::Cancelled;
    result.failure =
        GpuShaderCompileFailure{GpuShaderCompileError::Cancelled, "compile cancelled", {}, 0};
    return result;
}

std::optional<GpuShaderCompileError> mapProcessError(ProcessError error) {
    switch (error) {
    case ProcessError::Unavailable:
        return GpuShaderCompileError::Unavailable;
    case ProcessError::Spawn:
        return GpuShaderCompileError::SpawnFailure;
    case ProcessError::ResourceLimit:
        return GpuShaderCompileError::SpawnFailure;
    case ProcessError::Io:
        return GpuShaderCompileError::IoFailure;
    case ProcessError::Crashed:
        return GpuShaderCompileError::ToolCrashed;
    case ProcessError::Timeout:
        return GpuShaderCompileError::Timeout;
    case ProcessError::Cancelled:
        return std::nullopt; // Reported as the dedicated Cancelled status.
    }
    return GpuShaderCompileError::IoFailure;
}

GpuShaderCompileResult fromToolFailure(const ToolRun& run, GpuShaderCompileError fallback,
                                       std::string stage) {
    if (run.failure && run.failure->code == ProcessError::Cancelled)
        return cancelled();
    if (run.failure) {
        const auto mapped = mapProcessError(run.failure->code);
        return failed(mapped.value_or(fallback), std::move(stage), &run);
    }
    return failed(fallback, std::move(stage), &run);
}

} // namespace

struct GpuShaderCompiler::State {
    explicit State(std::size_t entries, std::size_t bytes)
        : maxCacheEntries(entries == 0 ? 1 : entries), maxCacheBytes(bytes == 0 ? 1 : bytes) {}
    std::mutex mutex;
    std::size_t maxCacheEntries;
    std::size_t maxCacheBytes;
    std::size_t cacheBytes = 0;
    std::unordered_map<std::string, render::CompiledGpuShader> cache;
    std::deque<std::string> insertionOrder;
    std::unordered_map<std::string, render::GpuShaderToolIdentity> tools;
    std::deque<std::string> toolOrder;
    GpuShaderCompilerStats stats;
};

GpuShaderCompiler::GpuShaderCompiler(std::size_t maxCacheEntries, std::size_t maxCacheBytes)
    : state_(std::make_unique<State>(maxCacheEntries, maxCacheBytes)) {}

GpuShaderCompiler::~GpuShaderCompiler() = default;

GpuShaderCompiler::ToolResolution
GpuShaderCompiler::resolveTool(const std::string& role, const std::string& path,
                               const GpuShaderCompileLimits& limits, GpuShaderDeadline deadline,
                               const GpuShaderCancellation& cancel) {
    ToolResolution result;
    if (cancel && cancel()) {
        result.error = GpuShaderCompileError::Cancelled;
        return result;
    }
    if (Clock::now() >= deadline) {
        result.error = GpuShaderCompileError::Timeout;
        return result;
    }
    const std::filesystem::path toolPath(path);
    if (path.empty() || !toolPath.is_absolute() || path.find('\0') != std::string::npos) {
        result.error = GpuShaderCompileError::InvalidTool;
        return result;
    }
    std::error_code error;
    if (!std::filesystem::is_regular_file(toolPath, error) || error) {
        result.error = GpuShaderCompileError::InvalidTool;
        return result;
    }
    const std::string cacheKey = role + '|' + path;
    // Content verification, not stat metadata: a same-size, same-mtime replacement must still be
    // detected. Bounded and cancellable between chunks.
    const auto hashed =
        gpu_shader_detail::hashFileBounded(path, kExecutableByteCeiling, deadline, cancel);
    if (hashed.error) {
        result.error = *hashed.error;
        return result;
    }
    {
        const std::lock_guard lock(state_->mutex);
        const auto found = state_->tools.find(cacheKey);
        if (found != state_->tools.end() && found->second.executableDigest == *hashed.digest) {
            result.identity = found->second;
            return result;
        }
    }
    render::GpuShaderToolIdentity identity;
    identity.role = role;
    identity.path = path;
    identity.executableDigest = *hashed.digest;
    const ToolRun version = runProcess(path, {"--version"}, limits.addressSpaceBytes,
                                       limits.openFiles, deadline, cancel, 4096);
    if (version.failure && version.failure->code == ProcessError::Cancelled) {
        result.error = GpuShaderCompileError::Cancelled;
        return result;
    }
    if (version.failure && version.failure->code == ProcessError::Timeout) {
        result.error = GpuShaderCompileError::Timeout;
        return result;
    }
    if (!version.failure && version.exitStatus == 0)
        identity.version = trimFirstLine(version.output);
    {
        const std::lock_guard lock(state_->mutex);
        const auto found = state_->tools.find(cacheKey);
        if (found != state_->tools.end()) {
            found->second = identity;
        } else {
            if (state_->tools.size() >= kMaxToolCacheEntries && !state_->toolOrder.empty()) {
                state_->tools.erase(state_->toolOrder.front());
                state_->toolOrder.pop_front();
            }
            state_->tools.emplace(cacheKey, identity);
            state_->toolOrder.push_back(cacheKey);
        }
    }
    result.identity = identity;
    return result;
}

std::optional<render::CompiledGpuShader> GpuShaderCompiler::lookupCache(const std::string& key) {
    const std::lock_guard lock(state_->mutex);
    const auto found = state_->cache.find(key);
    if (found == state_->cache.end()) {
        ++state_->stats.cacheMisses;
        return std::nullopt;
    }
    ++state_->stats.cacheHits;
    return found->second;
}

void GpuShaderCompiler::insertCache(std::string key, const render::CompiledGpuShader& artifact) {
    const std::size_t bytes = artifact.spirv.size();
    const std::lock_guard lock(state_->mutex);
    if (bytes > state_->maxCacheBytes)
        return; // Larger than the whole byte budget: returned to the caller, never retained.
    // A concurrent compile may have inserted this exact identity first. Do not double-count bytes
    // or push a duplicate queue entry that would later evict a newer artifact.
    if (state_->cache.contains(key))
        return;
    while (!state_->insertionOrder.empty() &&
           (state_->cache.size() >= state_->maxCacheEntries ||
            bytes > state_->maxCacheBytes - state_->cacheBytes)) {
        const auto victim = state_->insertionOrder.front();
        state_->insertionOrder.pop_front();
        const auto found = state_->cache.find(victim);
        if (found != state_->cache.end()) {
            state_->cacheBytes -= found->second.spirv.size();
            state_->cache.erase(found);
        }
    }
    // One owned key for the map, one for the eviction queue. Account bytes only after the map
    // insertion actually succeeded; if the queue push fails, roll the map back so limits stay true.
    const auto inserted = state_->cache.emplace(key, artifact);
    try {
        state_->insertionOrder.push_back(key);
    } catch (...) {
        state_->cache.erase(inserted.first);
        throw;
    }
    state_->cacheBytes += bytes;
}

GpuShaderCompilerStats GpuShaderCompiler::stats() const {
    const std::lock_guard lock(state_->mutex);
    GpuShaderCompilerStats snapshot = state_->stats;
    snapshot.cacheEntries = state_->cache.size();
    snapshot.cacheBytes = state_->cacheBytes;
    return snapshot;
}

GpuShaderCompileResult GpuShaderCompiler::compile(const GpuShaderCompileRequest& request,
                                                  const GpuShaderCancellation& cancel) {
    const auto& limits = request.limits;
    if (const auto limitsError = validateLimits(limits))
        return failed(*limitsError, "request limits exceed the hard ceilings");
    if (request.computeShaderText.empty())
        return failed(GpuShaderCompileError::MalformedSource, "compute shader text is empty");
    if (request.computeShaderText.size() > limits.maxSourceBytes)
        return failed(GpuShaderCompileError::SourceTooLarge,
                      "compute shader text exceeds byte ceiling");
    if (request.entryPoint.empty() || request.entryPoint.find('\0') != std::string::npos)
        return failed(GpuShaderCompileError::MissingEntryPoint,
                      "entry point name is empty or contains an embedded NUL");
    if (request.targetEnvironment.find('\0') != std::string::npos)
        return failed(GpuShaderCompileError::UnsupportedTarget,
                      "target environment contains an embedded NUL");
    const auto allowedMinor = maxSpirvMinor(request.targetEnvironment);
    if (!allowedMinor)
        return failed(GpuShaderCompileError::UnsupportedTarget, "unsupported target environment");
    const GpuShaderDeadline deadline = Clock::now() + limits.deadline;
    if (cancel && cancel())
        return cancelled();
    if (Clock::now() >= deadline)
        return failed(GpuShaderCompileError::Timeout, "compile deadline already elapsed");

    const auto compiler =
        resolveTool("glslangValidator", request.tools.glslangValidator, limits, deadline, cancel);
    if (compiler.error) {
        if (*compiler.error == GpuShaderCompileError::Cancelled)
            return cancelled();
        return failed(*compiler.error, "glslangValidator path is not a qualified executable");
    }
    const auto validator =
        resolveTool("spirv-val", request.tools.spirvVal, limits, deadline, cancel);
    if (validator.error) {
        if (*validator.error == GpuShaderCompileError::Cancelled)
            return cancelled();
        return failed(*validator.error, "spirv-val path is not a qualified executable");
    }

    const auto sourceDigest = core::Sha256Hasher::hash(std::as_bytes(
        std::span<const char>(request.computeShaderText.data(), request.computeShaderText.size())));
    if (!sourceDigest)
        return failed(GpuShaderCompileError::IoFailure, "source digest unavailable");
    const std::string key =
        hex(*sourceDigest) + '\n' + request.entryPoint + '\n' + request.targetEnvironment + '\n' +
        hex(compiler.identity->executableDigest) + '\n' + hex(validator.identity->executableDigest);
    if (auto cached = lookupCache(key)) {
        if (cached->spirv.size() > limits.maxSpirvBytes)
            return failed(GpuShaderCompileError::OutputTooLarge,
                          "cached SPIR-V exceeds the request byte ceiling");
        if (cancel && cancel())
            return cancelled();
        if (Clock::now() >= deadline)
            return failed(GpuShaderCompileError::Timeout,
                          "compile deadline elapsed before cache publication");
        render::CompiledGpuShader cachedArtifact = *cached;
        // The cache key is the source digest, so the cached artifact's provenance is exact; set it
        // explicitly so a consumer can always bind the returned artifact to its source.
        cachedArtifact.sourceDigest = *sourceDigest;
        return GpuShaderCompileResult{GpuShaderCompileStatus::Compiled, std::move(cachedArtifact),
                                      std::nullopt};
    }

    auto temp = makeTempDirectory();
    if (!temp)
        return failed(GpuShaderCompileError::IoFailure, "private temporary directory unavailable");
    const auto sourcePath = temp->path() / "input.comp";
    const auto spirvPath = temp->path() / "output.spv";
    {
        std::ofstream output(sourcePath, std::ios::binary | std::ios::trunc);
        if (!output)
            return failed(GpuShaderCompileError::IoFailure, "cannot stage shader source");
        output.write(request.computeShaderText.data(),
                     static_cast<std::streamsize>(request.computeShaderText.size()));
        if (!output)
            return failed(GpuShaderCompileError::IoFailure, "cannot stage shader source");
    }

    ToolRun compiled = runProcess(
        compiler.identity->path,
        {"-V", "-S", "comp", "--entry-point", request.entryPoint, "--target-env",
         request.targetEnvironment, "-o", spirvPath.string(), sourcePath.string()},
        limits.addressSpaceBytes, limits.openFiles, deadline, cancel, limits.maxDiagnosticBytes);
    if (compiled.failure || compiled.exitStatus != 0)
        return fromToolFailure(compiled, GpuShaderCompileError::MalformedSource,
                               "glslangValidator rejected the shader");

    std::error_code error;
    const auto spirvSize = std::filesystem::file_size(spirvPath, error);
    if (error)
        return failed(GpuShaderCompileError::ToolCrashed, "compiler produced no SPIR-V output",
                      &compiled);
    if (spirvSize > limits.maxSpirvBytes)
        return failed(GpuShaderCompileError::OutputTooLarge, "SPIR-V output exceeds byte ceiling");
    auto spirv = readWholeFile(spirvPath.string(), limits.maxSpirvBytes);
    if (!spirv)
        return failed(GpuShaderCompileError::IoFailure, "cannot read SPIR-V output", &compiled);
    if (spirv->empty())
        return failed(GpuShaderCompileError::ToolCrashed, "compiler produced empty SPIR-V output",
                      &compiled);

    switch (checkSpirv(*spirv, request.entryPoint, *allowedMinor)) {
    case SpirvCheck::Ok:
        break;
    case SpirvCheck::Structure:
        return failed(GpuShaderCompileError::MalformedSource,
                      "SPIR-V output is structurally malformed", &compiled);
    case SpirvCheck::Version:
        return failed(GpuShaderCompileError::WrongSpirvVersion,
                      "SPIR-V version exceeds the target environment", &compiled);
    case SpirvCheck::EntryPoint:
        return failed(GpuShaderCompileError::MissingEntryPoint,
                      "SPIR-V has no matching compute entry point", &compiled);
    }

    if (cancel && cancel())
        return cancelled();
    if (Clock::now() >= deadline)
        return failed(GpuShaderCompileError::Timeout, "compile deadline elapsed before validation");
    ToolRun validated = runProcess(
        validator.identity->path, {"--target-env", request.targetEnvironment, spirvPath.string()},
        limits.addressSpaceBytes, limits.openFiles, deadline, cancel, limits.maxDiagnosticBytes);
    if (validated.failure || validated.exitStatus != 0)
        return fromToolFailure(validated, GpuShaderCompileError::ValidationFailed,
                               "spirv-val rejected the SPIR-V module");
    if (cancel && cancel())
        return cancelled();
    if (Clock::now() >= deadline)
        return failed(GpuShaderCompileError::Timeout,
                      "compile deadline elapsed before publication");

    render::CompiledGpuShader artifact;
    artifact.spirv = std::move(*spirv);
    const auto artifactDigest = core::Sha256Hasher::hash(
        std::as_bytes(std::span<const std::uint8_t>(artifact.spirv.data(), artifact.spirv.size())));
    if (!artifactDigest)
        return failed(GpuShaderCompileError::IoFailure, "artifact digest unavailable");
    artifact.spirvDigest = *artifactDigest;
    artifact.sourceDigest = *sourceDigest;
    artifact.entryPoint = request.entryPoint;
    artifact.targetEnvironment = request.targetEnvironment;
    artifact.stage = render::GpuShaderStage::Compute;
    artifact.compiler = *compiler.identity;
    artifact.validator = *validator.identity;

    // Final publication guards: nothing reaches the cache or the caller after a cancel or expiry.
    if (cancel && cancel())
        return cancelled();
    if (Clock::now() >= deadline)
        return failed(GpuShaderCompileError::Timeout,
                      "compile deadline elapsed immediately before publication");
    {
        const std::lock_guard lock(state_->mutex);
        ++state_->stats.compiles;
    }
    insertCache(key, artifact);
    return GpuShaderCompileResult{GpuShaderCompileStatus::Compiled, std::move(artifact),
                                  std::nullopt};
}

} // namespace bloom::color
