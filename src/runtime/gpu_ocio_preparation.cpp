#include <bloom/runtime/gpu_ocio_preparation.hpp>

#include <bloom/color/gpu_shader_compiler.hpp>
#include <bloom/color/ocio_gpu_program.hpp>
#include <bloom/runtime/gpu_ocio_wrapper.hpp>

#include <cstring>
#include <map>
#include <mutex>
#include <optional>
#include <span>
#include <utility>
#include <vector>

namespace bloom::runtime {
namespace {

constexpr std::string_view kPrepareKeyDomain = "BloomGpuOcioPrepareKey";

void appendText(std::vector<std::byte>& bytes, const std::string_view text) {
    const auto size = static_cast<std::uint64_t>(text.size());
    for (int shift = 56; shift >= 0; shift -= 8) {
        bytes.push_back(static_cast<std::byte>((size >> shift) & 0xFFU));
    }
    for (const char character : text) {
        bytes.push_back(static_cast<std::byte>(static_cast<unsigned char>(character)));
    }
}

void appendU64(std::vector<std::byte>& bytes, const std::uint64_t value) {
    for (int shift = 56; shift >= 0; shift -= 8) {
        bytes.push_back(static_cast<std::byte>((value >> shift) & 0xFFU));
    }
}

void appendF64(std::vector<std::byte>& bytes, const double value) {
    std::uint64_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    appendU64(bytes, bits);
}

[[nodiscard]] core::Sha256Digest computePrepareKey(const color::ResolvedBloomNeutralConfig& config,
                                                   const GpuOcioTransformSpec& transform,
                                                   const GpuOcioCommandGeometry geometry,
                                                   const GpuOcioCompileOptions& options) {
    std::vector<std::byte> bytes;
    for (const char character : kPrepareKeyDomain) {
        bytes.push_back(static_cast<std::byte>(static_cast<unsigned char>(character)));
    }
    bytes.push_back(std::byte{0});
    appendU64(bytes, 1);
    bytes.push_back(static_cast<std::byte>(transform.kind));
    appendText(bytes, transform.fromId);
    appendText(bytes, transform.toId);
    appendText(bytes, transform.display);
    appendText(bytes, transform.view);
    appendF64(bytes, transform.exposure);
    appendF64(bytes, transform.contrast);
    appendU64(bytes, geometry.width);
    appendU64(bytes, geometry.height);
    const auto revision = config.expectedRevision().bytes();
    for (const auto value : revision) {
        bytes.push_back(static_cast<std::byte>(value));
    }
    appendText(bytes, options.glslangValidatorPath);
    appendText(bytes, options.spirvValPath);
    appendText(bytes, options.targetEnvironment);
    const auto digest = core::Sha256Hasher::hash(bytes);
    return digest.has_value() ? *digest : core::Sha256Digest{};
}

[[nodiscard]] GpuOcioPreparationResult failure(const GpuOcioPreparationError error,
                                               std::string diagnostic = {}) {
    GpuOcioPreparationResult result;
    result.error = error;
    result.diagnostic = std::move(diagnostic);
    return result;
}

[[nodiscard]] GpuOcioPreparationError
mapProgramError(const render::OcioGpuProgramError error) noexcept {
    switch (error) {
    case render::OcioGpuProgramError::None:
        return GpuOcioPreparationError::None;
    case render::OcioGpuProgramError::InvalidRequest:
        return GpuOcioPreparationError::InvalidRequest;
    case render::OcioGpuProgramError::IdentityTransform:
        return GpuOcioPreparationError::IdentityTransform;
    default:
        return GpuOcioPreparationError::ExtractionFailed;
    }
}

struct Entry final {
    std::shared_ptr<const PreparedGpuOcioCommand> command;
    std::uint64_t bytes = 0;
    std::uint64_t serial = 0;
};

} // namespace

std::string_view gpuOcioPreparationErrorName(const GpuOcioPreparationError error) noexcept {
    switch (error) {
    case GpuOcioPreparationError::None:
        return "none";
    case GpuOcioPreparationError::InvalidRequest:
        return "invalid-request";
    case GpuOcioPreparationError::ExtractionFailed:
        return "extraction-failed";
    case GpuOcioPreparationError::IdentityTransform:
        return "identity-transform";
    case GpuOcioPreparationError::WrapperFailed:
        return "wrapper-failed";
    case GpuOcioPreparationError::CompileFailed:
        return "compile-failed";
    case GpuOcioPreparationError::CompileCancelled:
        return "compile-cancelled";
    case GpuOcioPreparationError::CommandInvalid:
        return "command-invalid";
    case GpuOcioPreparationError::OverBudget:
        return "over-budget";
    }
    return "unknown";
}

struct GpuOcioProgramPreparer::Impl final {
    explicit Impl(const GpuOcioPreparerBudgets& options)
        : budgets(options), compiler(options.compilerCacheEntries, options.compilerCacheBytes) {}

    GpuOcioPreparerBudgets budgets;
    color::GpuShaderCompiler compiler;
    mutable std::mutex mutex;
    std::map<core::Sha256Digest, Entry> cache;
    std::uint64_t totalBytes = 0;
    std::uint64_t serial = 0;
    GpuOcioPreparerCounters counters;
};

GpuOcioProgramPreparer::GpuOcioProgramPreparer(GpuOcioPreparerBudgets budgets)
    : impl_(std::make_unique<Impl>(budgets)) {}

GpuOcioProgramPreparer::~GpuOcioProgramPreparer() = default;

GpuOcioPreparerCounters GpuOcioProgramPreparer::counters() const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    GpuOcioPreparerCounters result = impl_->counters;
    result.cacheEntries = impl_->cache.size();
    result.cacheBytes = impl_->totalBytes;
    return result;
}

GpuOcioPreparationResult GpuOcioProgramPreparer::prepare(
    const color::ResolvedBloomNeutralConfig& config, const GpuOcioTransformSpec& transform,
    const GpuOcioCommandGeometry geometry, const GpuOcioCompileOptions& options,
    const GpuOcioCancellation& cancel) {
    if (options.glslangValidatorPath.empty() || options.spirvValPath.empty()) {
        return failure(GpuOcioPreparationError::InvalidRequest,
                       "explicit glslangValidator/spirv-val paths are required");
    }
    if (geometry.width == 0 || geometry.height == 0) {
        return failure(GpuOcioPreparationError::InvalidRequest, "the geometry is empty");
    }
    if (cancel && cancel()) {
        return failure(GpuOcioPreparationError::CompileCancelled, "cancelled before extraction");
    }
    const auto key = computePrepareKey(config, transform, geometry, options);
    if (key == core::Sha256Digest{}) {
        return failure(GpuOcioPreparationError::InvalidRequest, "the prepare key is empty");
    }
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        ++impl_->counters.preparations;
        const auto found = impl_->cache.find(key);
        if (found != impl_->cache.end()) {
            found->second.serial = ++impl_->serial;
            ++impl_->counters.cacheHits;
            GpuOcioPreparationResult result;
            result.command = found->second.command;
            return result;
        }
        ++impl_->counters.cacheMisses;
    }

    render::OcioGpuProgramResult extracted = [&]() -> render::OcioGpuProgramResult {
        switch (transform.kind) {
        case GpuOcioTransformKind::Cst:
            return color::buildOcioGpuProgramForCst(config, transform.fromId, transform.toId);
        case GpuOcioTransformKind::Display:
            return color::buildOcioGpuProgramForDisplay(config, transform.display, transform.view);
        case GpuOcioTransformKind::ExposureContrast:
            return color::buildOcioGpuProgramForExposureContrast(
                config, transform.fromId, transform.exposure, transform.contrast);
        }
        return render::OcioGpuProgramResult::failure(render::OcioGpuProgramError::InvalidRequest);
    }();
    if (!extracted.succeeded()) {
        const auto error = mapProgramError(extracted.error());
        return failure(error, "OCIO GPU program extraction failed");
    }
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        ++impl_->counters.extractions;
    }
    auto program = std::move(extracted).takeProgram();
    if (!program.has_value()) {
        return failure(GpuOcioPreparationError::ExtractionFailed, "the extracted program is empty");
    }
    const auto wrapper = buildGpuOcioWrapperGlsl(*program);
    if (!wrapper.succeeded()) {
        return failure(GpuOcioPreparationError::WrapperFailed,
                       std::string(gpuOcioWrapperErrorName(wrapper.error)));
    }
    if (cancel && cancel()) {
        return failure(GpuOcioPreparationError::CompileCancelled, "cancelled before compile");
    }

    color::GpuShaderCompileRequest request;
    request.computeShaderText = wrapper.source;
    request.entryPoint = wrapper.entryPoint;
    request.targetEnvironment = options.targetEnvironment;
    request.tools.glslangValidator = options.glslangValidatorPath;
    request.tools.spirvVal = options.spirvValPath;
    request.limits.maxSourceBytes = static_cast<std::size_t>(options.maxSourceBytes);
    request.limits.maxSpirvBytes = static_cast<std::size_t>(options.maxSpirvBytes);
    request.limits.maxDiagnosticBytes = static_cast<std::size_t>(options.maxDiagnosticBytes);
    request.limits.deadline = options.deadline;
    const auto compiled = impl_->compiler.compile(request, cancel);
    if (compiled.status == color::GpuShaderCompileStatus::Cancelled) {
        return failure(GpuOcioPreparationError::CompileCancelled, "the compile was cancelled");
    }
    if (compiled.status != color::GpuShaderCompileStatus::Compiled ||
        !compiled.artifact.has_value()) {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        ++impl_->counters.compileFailures;
        std::string diagnostic = "the wrapper did not compile";
        if (compiled.failure.has_value()) {
            diagnostic = compiled.failure->diagnostic;
        }
        return failure(GpuOcioPreparationError::CompileFailed, std::move(diagnostic));
    }
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        ++impl_->counters.compiles;
    }

    auto commandResult =
        PreparedGpuOcioCommand::prepare(std::move(*program), *compiled.artifact, geometry);
    if (!commandResult.hasValue()) {
        return failure(GpuOcioPreparationError::CommandInvalid,
                       std::string(gpuOcioCommandErrorName(commandResult.error)));
    }
    auto command = std::move(commandResult.command);

    const std::uint64_t bytes = command->retainedBytes();
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        if (impl_->budgets.maxEntries == 0 || impl_->budgets.maxBytes == 0 ||
            bytes > impl_->budgets.maxBytes) {
            return failure(GpuOcioPreparationError::OverBudget,
                           "the command does not fit the preparer byte budget");
        }
        while (!impl_->cache.empty() && (impl_->cache.size() >= impl_->budgets.maxEntries ||
                                         impl_->totalBytes > impl_->budgets.maxBytes - bytes)) {
            auto victim = impl_->cache.begin();
            for (auto candidate = impl_->cache.begin(); candidate != impl_->cache.end();
                 ++candidate) {
                if (candidate->second.serial < victim->second.serial) {
                    victim = candidate;
                }
            }
            impl_->totalBytes -= victim->second.bytes;
            impl_->cache.erase(victim);
            ++impl_->counters.evictions;
        }
        impl_->totalBytes += bytes;
        impl_->cache.emplace(key, Entry{command, bytes, ++impl_->serial});
    }
    GpuOcioPreparationResult result;
    result.command = std::move(command);
    return result;
}

} // namespace bloom::runtime
