#pragma once

#include <array>
#include <bloom/core/sha256.hpp>
#include <filesystem>
#include <functional>
#include <memory>
#include <span>
#include <string_view>
#include <vector>

namespace bloom::color {
namespace detail {
class LutReservation;
}
inline constexpr std::size_t kMaximumLutBytes = std::size_t{64} * 1024U * 1024U;
inline constexpr std::uint32_t kMaximumLut3dEdge = 129;
enum class LutInterpolation : std::uint32_t { Linear, Tetrahedral, Best };
enum class LutDirection : std::uint32_t { Forward, Inverse };
enum class LutError : std::uint32_t {
    None,
    MissingFile,
    UnsupportedFormat,
    FileTooLarge,
    EdgeTooLarge,
    MalformedFile,
    ChangedFile,
    HelperUnavailable,
    HelperProtocolViolation,
    HelperMemoryLimit,
    HelperDeadline,
    HelperCancelled,
    HelperTerminated,
    TransformBuildFailed,
    InvalidPixel,
};
[[nodiscard]] std::string_view lutErrorName(LutError error) noexcept;
[[nodiscard]] bool isLutExtension(const std::filesystem::path& path);
struct LutFile final {
    std::shared_ptr<const detail::LutReservation> reservation;
    std::vector<std::byte> bytes;
    core::Sha256Digest digest;
    std::uint32_t format = 0;
    LutError error = LutError::None;
};
// Bounded, cancellable structural preflight and digest over the same opened file. No OCIO parsing.
[[nodiscard]] LutFile readLutFile(const std::filesystem::path& path,
                                  const std::function<bool()>& cancellation = {});

class CpuFileTransformProcessor final {
  public:
    struct Result;
    [[nodiscard]] static Result prepare(const LutFile& file, LutInterpolation interpolation,
                                        LutDirection direction,
                                        const std::function<bool()>& cancellation = {});
    [[nodiscard]] LutError apply(std::span<std::array<float, 4>> pixels,
                                 const std::function<bool()>& cancellation = {}) const;
    [[nodiscard]] bool isAvailable() const noexcept;
    [[nodiscard]] bool isIdentity() const noexcept;
    ~CpuFileTransformProcessor();
    CpuFileTransformProcessor(const CpuFileTransformProcessor&) = delete;
    CpuFileTransformProcessor& operator=(const CpuFileTransformProcessor&) = delete;

  private:
    class Impl;
    explicit CpuFileTransformProcessor(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};
struct CpuFileTransformProcessor::Result final {
    std::shared_ptr<const CpuFileTransformProcessor> processor;
    LutError error = LutError::None;
};
} // namespace bloom::color
