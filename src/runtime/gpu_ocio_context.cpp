#include <bloom/runtime/gpu_ocio_context.hpp>

#include <bloom/runtime/prepared_gpu_scene.hpp>

#include <mutex>
#include <optional>
#include <utility>

namespace bloom::runtime {
namespace {

[[nodiscard]] GpuOcioContextError
mapToolResolveError(const color::GpuShaderToolResolveError error) noexcept {
    switch (error) {
    case color::GpuShaderToolResolveError::Disabled:
        return GpuOcioContextError::ToolsUnavailable;
    case color::GpuShaderToolResolveError::InvalidPackage:
        return GpuOcioContextError::InvalidPackage;
    case color::GpuShaderToolResolveError::LimitsInvalid:
        return GpuOcioContextError::InvalidRequest;
    case color::GpuShaderToolResolveError::InvalidExecutable:
        return GpuOcioContextError::InvalidExecutable;
    case color::GpuShaderToolResolveError::ToolsMissing:
        return GpuOcioContextError::ToolsMissing;
    case color::GpuShaderToolResolveError::OutsideToolsDirectory:
        return GpuOcioContextError::OutsideToolsDirectory;
    case color::GpuShaderToolResolveError::DigestMismatch:
        return GpuOcioContextError::DigestMismatch;
    case color::GpuShaderToolResolveError::InventoryInvalid:
        return GpuOcioContextError::InventoryInvalid;
    case color::GpuShaderToolResolveError::InventoryMismatch:
        return GpuOcioContextError::InventoryMismatch;
    case color::GpuShaderToolResolveError::SourceStagedMismatch:
        return GpuOcioContextError::SourceStagedMismatch;
    case color::GpuShaderToolResolveError::IoFailure:
        return GpuOcioContextError::IoFailure;
    case color::GpuShaderToolResolveError::SizeLimit:
        return GpuOcioContextError::SizeLimit;
    case color::GpuShaderToolResolveError::Timeout:
        return GpuOcioContextError::Timeout;
    case color::GpuShaderToolResolveError::Cancelled:
        return GpuOcioContextError::Cancelled;
    }
    return GpuOcioContextError::InternalInvariant;
}

[[nodiscard]] GpuOcioContextResult contextFailure(const GpuOcioContextError error,
                                                  std::string diagnostic) {
    GpuOcioContextResult result;
    result.error = error;
    result.diagnostic = std::move(diagnostic);
    return result;
}

} // namespace

std::string_view gpuOcioContextErrorName(const GpuOcioContextError error) noexcept {
    switch (error) {
    case GpuOcioContextError::None:
        return "none";
    case GpuOcioContextError::InvalidRequest:
        return "invalid-request";
    case GpuOcioContextError::ToolsUnavailable:
        return "tools-unavailable";
    case GpuOcioContextError::InvalidExecutable:
        return "invalid-executable";
    case GpuOcioContextError::InvalidPackage:
        return "invalid-package";
    case GpuOcioContextError::ToolsMissing:
        return "tools-missing";
    case GpuOcioContextError::OutsideToolsDirectory:
        return "outside-tools-directory";
    case GpuOcioContextError::DigestMismatch:
        return "digest-mismatch";
    case GpuOcioContextError::InventoryInvalid:
        return "inventory-invalid";
    case GpuOcioContextError::InventoryMismatch:
        return "inventory-mismatch";
    case GpuOcioContextError::SourceStagedMismatch:
        return "source-staged-mismatch";
    case GpuOcioContextError::IoFailure:
        return "io-failure";
    case GpuOcioContextError::SizeLimit:
        return "size-limit";
    case GpuOcioContextError::Timeout:
        return "timeout";
    case GpuOcioContextError::Cancelled:
        return "cancelled";
    case GpuOcioContextError::CompileOptionsInvalid:
        return "compile-options-invalid";
    case GpuOcioContextError::InternalInvariant:
        return "internal-invariant";
    }
    return "unknown";
}

struct GpuOcioContextResolver::State final {
    explicit State(GpuOcioContextRequest requestValue) : request(std::move(requestValue)) {}

    // One lock guards the lazy qualification, the one-preparer invariant, and the cached context.
    // It is held across the blocking resolve so concurrent callers cannot create a second preparer.
    mutable std::mutex mutex;
    GpuOcioContextRequest request;
    color::GpuShaderToolResolver toolResolver;
    std::shared_ptr<GpuOcioProgramPreparer> preparer;
    std::shared_ptr<const GpuSceneOcioContext> context;
    GpuOcioContextCounters counters;
};

GpuOcioContextResolver::GpuOcioContextResolver()
    : state_(std::make_unique<State>(GpuOcioContextRequest{})) {}

GpuOcioContextResolver::GpuOcioContextResolver(GpuOcioContextRequest request)
    : state_(std::make_unique<State>(std::move(request))) {}

GpuOcioContextResolver::~GpuOcioContextResolver() = default;

GpuOcioContextResult GpuOcioContextResolver::resolve(const color::GpuShaderCancellation& cancel) {
    const std::lock_guard lock(state_->mutex);
    if (state_->context != nullptr) {
        ++state_->counters.cacheHits;
        GpuOcioContextResult result;
        result.context = state_->context;
        return result;
    }
    if (cancel && cancel()) {
        return contextFailure(GpuOcioContextError::Cancelled,
                              "resolve cancelled before tool qualification");
    }

    const auto resolved = state_->toolResolver.resolve(state_->request.applicationExecutable,
                                                       state_->request.toolPackage,
                                                       state_->request.toolLimits, cancel);
    if (!resolved.ok) {
        const auto code = resolved.failure ? mapToolResolveError(resolved.failure->code)
                                           : GpuOcioContextError::InternalInvariant;
        const std::string diagnostic =
            resolved.failure ? resolved.failure->diagnostic : std::string{"tool resolution failed"};
        return contextFailure(code, diagnostic);
    }

    GpuOcioCompileOptions options = state_->request.compileOptions;
    options.glslangValidatorPath = resolved.paths.glslangValidator;
    options.spirvValPath = resolved.paths.spirvVal;
    if (!validGpuOcioCompileOptions(options)) {
        return contextFailure(GpuOcioContextError::CompileOptionsInvalid,
                              "the compile options are invalid after the validated tool paths "
                              "were applied");
    }

    auto preparer = std::make_shared<GpuOcioProgramPreparer>(state_->request.preparerBudgets);
    auto context =
        std::make_shared<const GpuSceneOcioContext>(GpuSceneOcioContext{preparer, options});
    state_->preparer = std::move(preparer);
    state_->context = std::move(context);
    ++state_->counters.resolves;

    GpuOcioContextResult result;
    result.context = state_->context;
    return result;
}

std::shared_ptr<GpuOcioProgramPreparer> GpuOcioContextResolver::preparer() const {
    const std::lock_guard lock(state_->mutex);
    return state_->preparer;
}

GpuOcioContextCounters GpuOcioContextResolver::counters() const {
    const std::lock_guard lock(state_->mutex);
    return state_->counters;
}

} // namespace bloom::runtime
