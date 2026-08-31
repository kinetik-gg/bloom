#pragma once

// Private (non-installed) header shared only among bloom_color_ocio's own translation units. This
// is the sole place OpenColorIO's headers are included; nothing under include/bloom/color may
// name an OCIO type, per the "CPU Display Processor Boundary" contract ("OCIO classes,
// exceptions, pointers, and enums remain private"). Defines the out-of-line bodies of the opaque
// `Impl` nested classes forward-declared in the public headers, so the .cpp files that need real
// OCIO types (registry resolution, processor construction, display-frame application) share one
// consistent definition instead of three incompatible local ones.

#include <bloom/color/ocio_builtin_registry.hpp>
#include <bloom/color/ocio_cpu_display_processor.hpp>

#include <OpenColorIO/OpenColorIO.h>

#include <utility>

// OpenColorIO's own headers deliberately do not define this alias themselves (OpenColorIO.h's
// "namespace OCIO = OCIO_NAMESPACE;" appears only inside its usage-example doc comment); every
// consumer is expected to declare it. OCIO_NAMESPACE itself is a versioned macro
// (OpenColorABI.h), so this alias is the one place that versioning is absorbed.
namespace OCIO = OCIO_NAMESPACE;

namespace bloom::color {

class ResolvedBloomNeutralConfig::Impl final {
  public:
    explicit Impl(OCIO::ConstConfigRcPtr config) noexcept : config_(std::move(config)) {}

    [[nodiscard]] const OCIO::ConstConfigRcPtr& config() const& noexcept { return config_; }

  private:
    OCIO::ConstConfigRcPtr config_;
};

class PreparedCpuDisplayProcessorHandle::Impl final {
  public:
    explicit Impl(OCIO::ConstCPUProcessorRcPtr cpuProcessor) noexcept
        : cpuProcessor_(std::move(cpuProcessor)) {}

    [[nodiscard]] const OCIO::ConstCPUProcessorRcPtr& cpuProcessor() const& noexcept {
        return cpuProcessor_;
    }

  private:
    OCIO::ConstCPUProcessorRcPtr cpuProcessor_;
};

} // namespace bloom::color
