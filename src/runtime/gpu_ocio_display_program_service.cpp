// The bounded multi-config general-display program service. It consumes the ONE shared
// GpuOcioContextResolver (never resolving tools or owning a second preparer itself): the first
// prepare() resolves the shared context and reuses its single GpuOcioProgramPreparer for every
// config. Per-config and CPU-oracle resolutions are cached in bounded LRU caches keyed by the exact
// config/working identity and the display/view; the immutable resolved objects are pinned by
// shared_ptr while cached. The command program cache is the shared preparer's own bounded cache.

#include <bloom/runtime/gpu_ocio_display_arm.hpp>

#include <bloom/color/ocio_builtin_registry.hpp>
#include <bloom/color/ocio_cpu_display_processor.hpp>
#include <bloom/runtime/gpu_ocio_context.hpp>
#include <bloom/runtime/prepared_gpu_scene.hpp>

#include <algorithm>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace bloom::runtime {
namespace {

struct DisplayConfigKey final {
    color::OcioConfigLocatorKind locatorKind = color::OcioConfigLocatorKind::BloomBuiltIn;
    std::string locatorValue;
    core::Sha256Digest expectedRevision{};
    std::string workingColorSpaceId;

    friend bool operator==(const DisplayConfigKey&, const DisplayConfigKey&) = default;
};

struct ConfigEntry final {
    DisplayConfigKey key;
    std::shared_ptr<const color::ResolvedBloomNeutralConfig> config;
    std::uint64_t serial = 0;
};

struct OracleEntry final {
    DisplayConfigKey key;
    std::string display;
    std::string view;
    std::shared_ptr<const color::PreparedCpuDisplayProcessorHandle> oracle;
    std::uint64_t serial = 0;
};

} // namespace

struct GpuDisplayProgramService::Impl final {
    Impl(std::shared_ptr<GpuOcioContextResolver> resolverValue, const std::size_t maxConfigsValue,
         const std::size_t maxOraclesValue)
        : resolver(std::move(resolverValue)),
          maxConfigs(std::max<std::size_t>(maxConfigsValue, 1)),
          maxOracles(std::max<std::size_t>(maxOraclesValue, 1)) {}

    std::shared_ptr<GpuOcioContextResolver> resolver;
    std::size_t maxConfigs;
    std::size_t maxOracles;
    mutable std::mutex mutex;
    mutable std::uint64_t serial = 0;
    mutable std::vector<ConfigEntry> configs;
    mutable std::vector<OracleEntry> oracles;
};

GpuDisplayProgramService::GpuDisplayProgramService(
    std::shared_ptr<GpuOcioContextResolver> resolver, const std::size_t maxConfigs,
    const std::size_t maxOracles)
    : impl_(std::make_unique<Impl>(std::move(resolver), maxConfigs, maxOracles)) {}

GpuDisplayProgramService::~GpuDisplayProgramService() = default;

GpuDisplayProgramResult
GpuDisplayProgramService::prepare(const GpuDisplayColorBinding& binding, const std::uint32_t width,
                                  const std::uint32_t height, const ViewAdjust viewAdjust,
                                  const GpuOcioCancellation& cancel) const {
    GpuDisplayProgramResult result;
    if (width == 0 || height == 0 || !viewAdjust.valid() || binding.locatorValue.empty() ||
        binding.expectedRevision == core::Sha256Digest{}) {
        result.error = GpuDisplayProgramError::InvalidRequest;
        result.diagnostic = "the general display binding request is invalid";
        return result;
    }
    if (cancel && cancel()) {
        result.error = GpuDisplayProgramError::Cancelled;
        result.diagnostic = "the general display preparation was cancelled before resolution";
        return result;
    }
    if (impl_->resolver == nullptr) {
        result.error = GpuDisplayProgramError::ConfigUnavailable;
        result.diagnostic = "no shared GPU OCIO context resolver is configured";
        return result;
    }

    // Resolve the one shared context (idempotent after the first success). Its single preparer is
    // reused for every config and its compile options carry the validated executable-relative tools.
    const auto contextResult = impl_->resolver->resolve(cancel);
    if (!contextResult.hasValue()) {
        if (contextResult.error == GpuOcioContextError::Cancelled) {
            result.error = GpuDisplayProgramError::Cancelled;
            result.diagnostic = "the shared GPU OCIO context resolution was cancelled";
            return result;
        }
        result.error = GpuDisplayProgramError::ConfigUnavailable;
        result.diagnostic = contextResult.diagnostic.empty()
                                ? std::string("the shared GPU OCIO context is unavailable")
                                : contextResult.diagnostic;
        return result;
    }
    const GpuSceneOcioContext& context = *contextResult.context;
    if (context.preparer == nullptr) {
        result.error = GpuDisplayProgramError::ConfigUnavailable;
        result.diagnostic = "the shared GPU OCIO context has no prepared compiler";
        return result;
    }

    const DisplayConfigKey key{binding.locatorKind, binding.locatorValue, binding.expectedRevision,
                               binding.workingColorSpaceId};
    std::shared_ptr<const color::ResolvedBloomNeutralConfig> config;
    std::shared_ptr<const color::PreparedCpuDisplayProcessorHandle> oracle;
    {
        std::lock_guard lock(impl_->mutex);
        // Config lookup (bounded LRU).
        for (auto& entry : impl_->configs) {
            if (entry.key == key) {
                entry.serial = ++impl_->serial;
                config = entry.config;
                break;
            }
        }
        if (config == nullptr) {
            auto resolution =
                color::resolveOcioBuiltIn(binding.locatorKind, binding.locatorValue,
                                          binding.expectedRevision, binding.workingColorSpaceId);
            if (!resolution.ready()) {
                result.error = GpuDisplayProgramError::ConfigUnavailable;
                result.diagnostic = "the requested OCIO config/working space did not resolve";
                return result;
            }
            auto resolved = std::move(resolution).takeResolved();
            if (!resolved.has_value()) {
                result.error = GpuDisplayProgramError::ConfigUnavailable;
                result.diagnostic = "the requested OCIO config/working space did not resolve";
                return result;
            }
            if (resolved->expectedRevision() != binding.expectedRevision ||
                (!binding.workingColorSpaceId.empty() &&
                 resolved->processColorSpaceId() != binding.workingColorSpaceId)) {
                result.error = GpuDisplayProgramError::BindingMismatch;
                result.diagnostic = "the resolved config does not match the requested binding";
                return result;
            }
            config = std::make_shared<const color::ResolvedBloomNeutralConfig>(std::move(*resolved));
            if (impl_->configs.size() >= impl_->maxConfigs) {
                const auto victim = std::min_element(
                    impl_->configs.begin(), impl_->configs.end(),
                    [](const ConfigEntry& a, const ConfigEntry& b) { return a.serial < b.serial; });
                impl_->configs.erase(victim);
            }
            impl_->configs.push_back(ConfigEntry{key, config, ++impl_->serial});
        }

        // Oracle lookup (bounded LRU); the CPU display processor is built for the same config pair.
        const std::string resolvedDisplay =
            binding.display.empty() ? std::string(config->displayName()) : binding.display;
        const std::string resolvedView =
            binding.view.empty() ? std::string(config->viewName()) : binding.view;
        for (auto& entry : impl_->oracles) {
            if (entry.key == key && entry.display == resolvedDisplay && entry.view == resolvedView) {
                entry.serial = ++impl_->serial;
                oracle = entry.oracle;
                break;
            }
        }
        if (oracle == nullptr) {
            auto built =
                color::buildCpuDisplayProcessorForView(*config, resolvedDisplay, resolvedView);
            auto handle = std::move(built).takeHandle();
            if (!handle.has_value()) {
                result.error = GpuDisplayProgramError::OracleUnavailable;
                result.diagnostic = "the CPU display oracle could not be prepared for the pair";
                return result;
            }
            oracle = std::make_shared<const color::PreparedCpuDisplayProcessorHandle>(
                std::move(*handle));
            if (impl_->oracles.size() >= impl_->maxOracles) {
                const auto victim = std::min_element(
                    impl_->oracles.begin(), impl_->oracles.end(),
                    [](const OracleEntry& a, const OracleEntry& b) { return a.serial < b.serial; });
                impl_->oracles.erase(victim);
            }
            impl_->oracles.push_back(
                OracleEntry{key, resolvedDisplay, resolvedView, oracle, ++impl_->serial});
        }
    }

    // The Display transform is extracted from the config's own working space. The spec's
    // working-id is set from the resolved config so the SHARED preparer's command cache keys a
    // changed working space distinctly (Display extraction itself uses only display/view).
    GpuOcioTransformSpec spec;
    spec.kind = GpuOcioTransformKind::Display;
    spec.display = binding.display.empty() ? std::string(config->displayName()) : binding.display;
    spec.view = binding.view.empty() ? std::string(config->viewName()) : binding.view;
    spec.workingSpaceId = std::string(config->processColorSpaceId());
    spec.viewAdjust = viewAdjust;

    const auto prepared =
        context.preparer->prepare(*config, spec, GpuOcioCommandGeometry{width, height},
                                  context.compileOptions, cancel);
    if (!prepared.hasValue()) {
        result.error = prepared.error == GpuOcioPreparationError::CompileCancelled
                           ? GpuDisplayProgramError::Cancelled
                           : GpuDisplayProgramError::TransformUnavailable;
        result.diagnostic = prepared.diagnostic.empty()
                                ? std::string("the OCIO display transform could not be prepared")
                                : prepared.diagnostic;
        return result;
    }

    result.program.command = prepared.command;
    result.program.cpuOracle = std::move(oracle);
    result.program.binding.locatorKind = binding.locatorKind;
    result.program.binding.locatorValue = binding.locatorValue;
    result.program.binding.expectedRevision = config->expectedRevision();
    result.program.binding.workingColorSpaceId = std::string(config->processColorSpaceId());
    result.program.binding.display = spec.display;
    result.program.binding.view = spec.view;
    result.program.viewAdjust = viewAdjust;
    result.program.width = width;
    result.program.height = height;
    result.error = GpuDisplayProgramError::None;
    return result;
}

GpuOcioPreparerCounters GpuDisplayProgramService::counters() const {
    std::lock_guard lock(impl_->mutex);
    GpuOcioPreparerCounters total;
    if (impl_->resolver != nullptr) {
        // The shared preparer's counters are the authoritative cold/warm measurement; expose them
        // through this service so a caller sees the same numbers the actual route compiled with.
        const auto context = impl_->resolver->preparer();
        if (context != nullptr) {
            total = context->counters();
        }
    }
    total.cacheEntries = impl_->configs.size() + impl_->oracles.size();
    return total;
}

} // namespace bloom::runtime
