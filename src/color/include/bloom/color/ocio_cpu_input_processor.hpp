#pragma once

#include <array>
#include <bloom/color/ocio_builtin_registry.hpp>
#include <memory>
#include <span>

namespace bloom::color {
// Applies the inverse of the qualified display transform to straight RGBA32F samples.
// The alpha channel is preserved. No OCIO object crosses this boundary.
class CpuInputProcessor final {
  public:
    [[nodiscard]] static std::shared_ptr<const CpuInputProcessor>
    prepare(const ResolvedBloomNeutralConfig& config);
    [[nodiscard]] bool apply(std::span<std::array<float, 4>> pixels) const noexcept;
    [[nodiscard]] core::Sha256Digest configRevision() const noexcept { return revision_; }
    ~CpuInputProcessor();
    CpuInputProcessor(const CpuInputProcessor&) = delete;
    CpuInputProcessor& operator=(const CpuInputProcessor&) = delete;

  private:
    class Impl;
    CpuInputProcessor(std::unique_ptr<Impl> impl, core::Sha256Digest revision);
    std::unique_ptr<Impl> impl_;
    core::Sha256Digest revision_;
};
} // namespace bloom::color
