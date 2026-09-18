#pragma once

#include <array>
#include <bloom/color/ocio_builtin_registry.hpp>
#include <memory>
#include <span>
#include <string>
#include <string_view>

namespace bloom::color {

// A general CPU colour-space transform between non-data spaces in the exact resolved config.
// The config resolver validates the project working space; explicit graph transforms may target
// nonlinear spaces such as ACEScct. Data
// spaces, missing IDs, unsupported floating-point environments, and OCIO failures are explicit
// typed failures; callers must not silently fall back to a transfer-function guess.
enum class OcioColorSpaceProcessorError : std::uint8_t {
    None,
    MissingInputColorSpace,
    MissingWorkingColorSpace,
    InputColorSpaceIsData,
    WorkingColorSpaceInvalid,
    TransformBuildFailed,
    CpuProcessorUnavailable,
    UnsupportedFloatingPointEnvironment,
    InvalidColorSpaceId,
};

class CpuColorSpaceProcessor;

class [[nodiscard]] OcioColorSpaceProcessorResult final {
  public:
    OcioColorSpaceProcessorResult(OcioColorSpaceProcessorResult&&) noexcept = default;
    OcioColorSpaceProcessorResult& operator=(OcioColorSpaceProcessorResult&&) noexcept = default;
    OcioColorSpaceProcessorResult(const OcioColorSpaceProcessorResult&) = delete;
    OcioColorSpaceProcessorResult& operator=(const OcioColorSpaceProcessorResult&) = delete;
    ~OcioColorSpaceProcessorResult() = default;

    [[nodiscard]] bool succeeded() const noexcept { return processor_ != nullptr; }
    [[nodiscard]] explicit operator bool() const noexcept { return succeeded(); }
    [[nodiscard]] OcioColorSpaceProcessorError error() const noexcept { return error_; }
    [[nodiscard]] const CpuColorSpaceProcessor* processor() const& noexcept {
        return processor_.get();
    }
    [[nodiscard]] const CpuColorSpaceProcessor* processor() const&& = delete;
    [[nodiscard]] std::shared_ptr<const CpuColorSpaceProcessor> share() const& {
        return processor_;
    }
    [[nodiscard]] std::shared_ptr<const CpuColorSpaceProcessor> share() const&& = delete;
    [[nodiscard]] std::shared_ptr<const CpuColorSpaceProcessor> takeProcessor() && noexcept {
        return std::move(processor_);
    }

  private:
    friend class CpuColorSpaceProcessor;
    explicit OcioColorSpaceProcessorResult(std::shared_ptr<const CpuColorSpaceProcessor> processor,
                                           OcioColorSpaceProcessorError error) noexcept
        : processor_(std::move(processor)), error_(error) {}

    std::shared_ptr<const CpuColorSpaceProcessor> processor_;
    OcioColorSpaceProcessorError error_ = OcioColorSpaceProcessorError::None;
};

class CpuColorSpaceProcessor final {
  public:
    [[nodiscard]] static OcioColorSpaceProcessorResult
    prepare(const ResolvedBloomNeutralConfig& config, std::string_view fromId,
            std::string_view toWorkingSpaceId) noexcept;
    [[nodiscard]] bool isIdentity() const noexcept { return identity_; }
    [[nodiscard]] bool apply(std::span<std::array<float, 4>> pixels) const noexcept;
    [[nodiscard]] const core::Sha256Digest& configRevision() const& noexcept { return revision_; }
    [[nodiscard]] const core::Sha256Digest& configRevision() const&& = delete;
    [[nodiscard]] std::string_view fromId() const& noexcept { return fromId_; }
    [[nodiscard]] std::string_view fromId() const&& = delete;
    [[nodiscard]] std::string_view workingSpaceId() const& noexcept { return workingSpaceId_; }
    [[nodiscard]] std::string_view workingSpaceId() const&& = delete;

    ~CpuColorSpaceProcessor();
    CpuColorSpaceProcessor(const CpuColorSpaceProcessor&) = delete;
    CpuColorSpaceProcessor& operator=(const CpuColorSpaceProcessor&) = delete;

  private:
    class Impl;
    CpuColorSpaceProcessor(std::unique_ptr<Impl> impl, core::Sha256Digest revision,
                           std::string fromId, std::string workingSpaceId, bool identity) noexcept;

    std::unique_ptr<Impl> impl_;
    core::Sha256Digest revision_;
    std::string fromId_;
    std::string workingSpaceId_;
    bool identity_ = false;
};

} // namespace bloom::color
