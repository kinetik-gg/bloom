#include <bloom/runtime/qualified_display_processor_provider.hpp>

#include <bloom/color/ocio_builtin_registry.hpp>

#include <new>
#include <utility>

namespace {

using bloom::runtime::DiagnosticSeverity;
using bloom::runtime::TaskDiagnostic;

[[nodiscard]] TaskDiagnostic
diagnostic(std::string code, std::string summary, std::string detail = {},
           std::string suggestedAction = "Review the embedded Bloom Neutral display "
                                         "configuration and rebuild.") {
    return {.code = std::move(code),
            .severity = DiagnosticSeverity::Error,
            .summary = std::move(summary),
            .detail = std::move(detail),
            .suggestedAction = std::move(suggestedAction)};
}

[[nodiscard]] TaskDiagnostic
registryDiagnostic(const bloom::color::OcioBuiltInResolutionResult& result) {
    using bloom::color::OcioBuiltInInvalidReason;
    using bloom::color::OcioBuiltInRegistryOutcome;
    switch (result.outcome()) {
    case OcioBuiltInRegistryOutcome::Ready:
        break; // unreachable here; callers only invoke this on a non-Ready outcome.
    case OcioBuiltInRegistryOutcome::Changed:
        return diagnostic("bloom.runtime.qualified-display-processor.registry-changed",
                          "The embedded Bloom Neutral display configuration content changed");
    case OcioBuiltInRegistryOutcome::Missing:
        return diagnostic("bloom.runtime.qualified-display-processor.registry-missing",
                          "The Bloom Neutral display configuration locator could not be resolved");
    case OcioBuiltInRegistryOutcome::LocatorKindRequiresHelper:
        return diagnostic(
            "bloom.runtime.qualified-display-processor.registry-requires-helper",
            "The Bloom Neutral display configuration locator requires the supervised OCIO helper",
            "",
            "The supervised OCIO helper is not yet implemented; this locator kind is not "
            "reachable for the embedded built-in.");
    case OcioBuiltInRegistryOutcome::Invalid:
        switch (result.invalidReason()) {
        case OcioBuiltInInvalidReason::ParseFailed:
            return diagnostic("bloom.runtime.qualified-display-processor.registry-parse-failed",
                              "The embedded Bloom Neutral display configuration failed to parse");
        case OcioBuiltInInvalidReason::ValidateFailed:
            return diagnostic("bloom.runtime.qualified-display-processor.registry-validate-failed",
                              "The embedded Bloom Neutral display configuration failed validation");
        case OcioBuiltInInvalidReason::ProcessColorSpaceNotUniquelyMapped:
            return diagnostic(
                "bloom.runtime.qualified-display-processor.registry-process-space-ambiguous",
                "The embedded Bloom Neutral display configuration has no unambiguous process "
                "color space mapping");
        case OcioBuiltInInvalidReason::OutputColorSpaceNotUniquelyMapped:
            return diagnostic(
                "bloom.runtime.qualified-display-processor.registry-output-space-ambiguous",
                "The embedded Bloom Neutral display configuration has no unambiguous output "
                "color space mapping");
        case OcioBuiltInInvalidReason::DisplayViewNotUniquelyMapped:
            return diagnostic(
                "bloom.runtime.qualified-display-processor.registry-display-view-ambiguous",
                "The embedded Bloom Neutral display configuration has no unambiguous display/"
                "view mapping");
        case OcioBuiltInInvalidReason::None:
            break;
        }
        return diagnostic("bloom.runtime.qualified-display-processor.registry-invalid",
                          "The embedded Bloom Neutral display configuration is invalid");
    }
    return diagnostic("bloom.runtime.qualified-display-processor.registry-unknown",
                      "The Bloom Neutral display configuration could not be resolved");
}

[[nodiscard]] TaskDiagnostic buildDiagnostic(const bloom::color::OcioBuildProcessorError error) {
    using bloom::color::OcioBuildProcessorError;
    switch (error) {
    case OcioBuildProcessorError::GetProcessorFailed:
        return diagnostic("bloom.runtime.qualified-display-processor.build-get-processor-failed",
                          "The Bloom Neutral display processor could not be built");
    case OcioBuildProcessorError::GetCpuProcessorFailed:
        return diagnostic(
            "bloom.runtime.qualified-display-processor.build-get-cpu-processor-failed",
            "The Bloom Neutral CPU display processor could not be built");
    case OcioBuildProcessorError::IdentityConstructionFailed:
        return diagnostic(
            "bloom.runtime.qualified-display-processor.build-identity-construction-failed",
            "The Bloom Neutral display processor identity could not be constructed");
    case OcioBuildProcessorError::None:
        break;
    }
    return diagnostic("bloom.runtime.qualified-display-processor.build-unknown",
                      "The Bloom Neutral display processor could not be built");
}

} // namespace

namespace bloom::runtime {

QualifiedDisplayProcessorBuildResult::QualifiedDisplayProcessorBuildResult(
    std::shared_ptr<const color::PreparedCpuDisplayProcessorHandle> handle) noexcept
    : handle_(std::move(handle)) {}

QualifiedDisplayProcessorBuildResult::QualifiedDisplayProcessorBuildResult(
    TaskDiagnostic diagnostic) noexcept
    : diagnostic_(std::move(diagnostic)) {}

QualifiedDisplayProcessorBuildResult QualifiedDisplayProcessorBuildResult::ready(
    std::shared_ptr<const color::PreparedCpuDisplayProcessorHandle> handle) noexcept {
    return QualifiedDisplayProcessorBuildResult(std::move(handle));
}

QualifiedDisplayProcessorBuildResult
QualifiedDisplayProcessorBuildResult::failed(TaskDiagnostic diagnostic) noexcept {
    return QualifiedDisplayProcessorBuildResult(std::move(diagnostic));
}

QualifiedDisplayProcessorBuildResult buildBloomNeutralQualifiedDisplayProcessor() noexcept {
    try {
        auto resolution = color::resolveBloomNeutralV1BuiltIn(
            color::OcioConfigLocatorKind::BloomBuiltIn, color::kBloomNeutralV1ConfigUri,
            color::kBloomNeutralV1ConfigDigest);
        if (!resolution.ready()) {
            return QualifiedDisplayProcessorBuildResult::failed(registryDiagnostic(resolution));
        }
        auto resolved = std::move(resolution).takeResolved();
        if (!resolved.has_value()) {
            return QualifiedDisplayProcessorBuildResult::failed(
                diagnostic("bloom.runtime.qualified-display-processor.registry-no-product",
                           "The Bloom Neutral display configuration resolved with no product"));
        }
        auto built = color::buildBloomNeutralCpuDisplayProcessor(*resolved);
        if (!built) {
            return QualifiedDisplayProcessorBuildResult::failed(buildDiagnostic(built.error()));
        }
        auto handleValue = std::move(built).takeHandle();
        if (!handleValue.has_value()) {
            return QualifiedDisplayProcessorBuildResult::failed(
                diagnostic("bloom.runtime.qualified-display-processor.build-no-product",
                           "The Bloom Neutral display processor built with no product"));
        }
        auto shared = std::make_shared<const color::PreparedCpuDisplayProcessorHandle>(
            std::move(*handleValue));
        return QualifiedDisplayProcessorBuildResult::ready(std::move(shared));
    } catch (const std::bad_alloc&) {
        return QualifiedDisplayProcessorBuildResult::failed(
            diagnostic("bloom.runtime.qualified-display-processor.allocation-failure",
                       "The Bloom Neutral display processor could not be allocated"));
    } catch (...) {
        return QualifiedDisplayProcessorBuildResult::failed(
            diagnostic("bloom.runtime.qualified-display-processor.unexpected-failure",
                       "The Bloom Neutral display processor build failed unexpectedly"));
    }
}

QualifiedDisplayProcessorReadiness QualifiedDisplayProcessorProvider::readiness() const noexcept {
    std::lock_guard lock(mutex_);
    return readiness_;
}

std::shared_ptr<const color::PreparedCpuDisplayProcessorHandle>
QualifiedDisplayProcessorProvider::handle() const noexcept {
    std::lock_guard lock(mutex_);
    return handle_;
}

TaskDiagnostic QualifiedDisplayProcessorProvider::failureDiagnostic() const {
    std::lock_guard lock(mutex_);
    return failureDiagnostic_;
}

QualifiedDisplayProcessorSnapshot QualifiedDisplayProcessorProvider::snapshot() const {
    std::lock_guard lock(mutex_);
    return QualifiedDisplayProcessorSnapshot{
        .readiness = readiness_,
        .handle = handle_,
        .failureDiagnostic = failureDiagnostic_,
    };
}

void QualifiedDisplayProcessorProvider::publish(
    const QualifiedDisplayProcessorBuildResult& result) {
    std::lock_guard lock(mutex_);
    if (readiness_ != QualifiedDisplayProcessorReadiness::Pending) {
        return;
    }
    if (result.succeeded()) {
        handle_ = result.handle();
        readiness_ = QualifiedDisplayProcessorReadiness::Ready;
    } else {
        failureDiagnostic_ = result.diagnostic();
        readiness_ = QualifiedDisplayProcessorReadiness::Failed;
    }
}

} // namespace bloom::runtime
