// The lazy multi-config general-display program service. See the header for the contract. This
// translation unit owns the per-config preparer cache; the shader-tool resolution and the OCIO
// config/working-space resolution both happen here on the first prepare() call (off the UI thread),
// never at construction. The per-config GpuDisplayProgramPreparer itself lives in
// gpu_ocio_display_arm.cpp and is shared by this service.

#include <bloom/runtime/gpu_ocio_display_arm.hpp>

#include <bloom/color/ocio_builtin_registry.hpp>

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

} // namespace

struct GpuDisplayProgramService::Impl final {
    Impl(CompileOptionsProvider providerValue, GpuOcioPreparerBudgets preparerBudgets)
        : provider(std::move(providerValue)), budgets(preparerBudgets) {}

    CompileOptionsProvider provider;
    GpuOcioPreparerBudgets budgets;
    mutable std::mutex mutex;
    mutable bool optionsResolved = false;
    mutable GpuOcioCompileOptions options;
    // Tiny set of immutable per-config preparers keyed by the exact config/working identity. The
    // number of distinct project configs is bounded in practice; a linear scan avoids requiring an
    // ordering on the digest.
    mutable std::vector<std::pair<DisplayConfigKey, std::shared_ptr<GpuDisplayProgramPreparer>>>
        configs;
};

GpuDisplayProgramService::GpuDisplayProgramService(CompileOptionsProvider optionsProvider,
                                                   GpuOcioPreparerBudgets budgets)
    : impl_(std::make_unique<Impl>(std::move(optionsProvider), budgets)) {}

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

    std::shared_ptr<GpuDisplayProgramPreparer> preparer;
    {
        std::lock_guard lock(impl_->mutex);
        if (!impl_->optionsResolved) {
            impl_->options = impl_->provider ? impl_->provider() : GpuOcioCompileOptions{};
            impl_->optionsResolved = true;
        }
        if (impl_->options.glslangValidatorPath.empty() || impl_->options.spirvValPath.empty()) {
            result.error = GpuDisplayProgramError::ConfigUnavailable;
            result.diagnostic = "the qualified GPU shader tools are unavailable";
            return result;
        }
        const DisplayConfigKey key{binding.locatorKind, binding.locatorValue,
                                   binding.expectedRevision, binding.workingColorSpaceId};
        for (const auto& entry : impl_->configs) {
            if (entry.first == key) {
                preparer = entry.second;
                break;
            }
        }
        if (preparer == nullptr) {
            auto resolution = color::resolveOcioBuiltIn(binding.locatorKind, binding.locatorValue,
                                                        binding.expectedRevision,
                                                        binding.workingColorSpaceId);
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
            const bool revisionMatches = resolved->expectedRevision() == binding.expectedRevision;
            const bool workingMatches = binding.workingColorSpaceId.empty() ||
                                        resolved->processColorSpaceId() ==
                                            binding.workingColorSpaceId;
            if (!revisionMatches || !workingMatches) {
                result.error = GpuDisplayProgramError::BindingMismatch;
                result.diagnostic = "the resolved config does not match the requested binding";
                return result;
            }
            preparer =
                std::make_shared<GpuDisplayProgramPreparer>(std::move(*resolved), impl_->options);
            impl_->configs.emplace_back(key, preparer);
        }
    }

    auto prepared =
        preparer->prepare(binding.display, binding.view, width, height, viewAdjust, cancel);
    if (prepared.hasValue()) {
        prepared.program.binding.locatorKind = binding.locatorKind;
        prepared.program.binding.locatorValue = binding.locatorValue;
    }
    return prepared;
}

GpuOcioPreparerCounters GpuDisplayProgramService::counters() const {
    GpuOcioPreparerCounters total;
    std::lock_guard lock(impl_->mutex);
    for (const auto& entry : impl_->configs) {
        const auto counters = entry.second->counters();
        total.preparations += counters.preparations;
        total.cacheHits += counters.cacheHits;
        total.cacheMisses += counters.cacheMisses;
        total.extractions += counters.extractions;
        total.compiles += counters.compiles;
        total.compileFailures += counters.compileFailures;
        total.evictions += counters.evictions;
        total.cacheEntries += counters.cacheEntries;
        total.cacheBytes += counters.cacheBytes;
    }
    return total;
}

} // namespace bloom::runtime
