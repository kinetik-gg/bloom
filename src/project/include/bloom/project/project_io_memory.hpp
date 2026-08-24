#pragma once

#include <cstdint>
#include <memory>
#include <optional>

namespace bloom::project {

inline constexpr std::uint64_t kDefaultProjectIoOperationResidentBytes = 1ULL << 30U;
inline constexpr std::uint64_t kDefaultProjectIoSharedResidentBytes = 2ULL << 30U;
inline constexpr std::uint64_t kDefaultProjectIoSingleAllocationBytes = 256ULL << 20U;

enum class ProjectIoMemoryError : std::uint8_t {
    None,
    InvalidLimit,
    SingleAllocationLimitExceeded,
    OperationLimitExceeded,
    SharedLimitExceeded,
};

struct ProjectIoMemorySnapshot final {
    std::uint64_t currentBytes = 0;
    std::uint64_t peakBytes = 0;
    std::uint64_t limitBytes = 0;

    friend bool operator==(const ProjectIoMemorySnapshot&,
                           const ProjectIoMemorySnapshot&) = default;
};

struct ProjectIoOperationMemorySnapshot final {
    std::uint64_t currentBytes = 0;
    std::uint64_t peakBytes = 0;
    std::uint64_t limitBytes = 0;
    std::uint64_t singleAllocationLimitBytes = 0;

    friend bool operator==(const ProjectIoOperationMemorySnapshot&,
                           const ProjectIoOperationMemorySnapshot&) = default;
};

namespace detail {
struct ProjectIoSharedMemoryState;
struct ProjectIoOperationMemoryState;
} // namespace detail

class ProjectIoMemoryCoordinator;
class ProjectIoOperationMemory;

class ProjectIoMemoryReservation final {
  public:
    ProjectIoMemoryReservation(const ProjectIoMemoryReservation&) = delete;
    ProjectIoMemoryReservation& operator=(const ProjectIoMemoryReservation&) = delete;
    ProjectIoMemoryReservation(ProjectIoMemoryReservation&& other) noexcept;
    ProjectIoMemoryReservation& operator=(ProjectIoMemoryReservation&& other) noexcept;
    ~ProjectIoMemoryReservation();

    [[nodiscard]] bool isValid() const noexcept;
    [[nodiscard]] std::uint64_t bytes() const noexcept { return bytes_; }
    void reset() noexcept;

  private:
    friend class ProjectIoOperationMemory;
    friend class ProjectIoMemoryReservationResult;

    ProjectIoMemoryReservation() noexcept = default;
    ProjectIoMemoryReservation(std::shared_ptr<detail::ProjectIoOperationMemoryState> operation,
                               std::uint64_t bytes) noexcept;

    std::shared_ptr<detail::ProjectIoOperationMemoryState> operation_;
    std::uint64_t bytes_ = 0;
};

class [[nodiscard]] ProjectIoMemoryReservationResult final {
  public:
    [[nodiscard]] explicit operator bool() const noexcept {
        return error_ == ProjectIoMemoryError::None;
    }
    [[nodiscard]] ProjectIoMemoryError error() const noexcept { return error_; }
    [[nodiscard]] ProjectIoMemoryReservation takeReservation() && noexcept;

  private:
    friend class ProjectIoOperationMemory;

    explicit ProjectIoMemoryReservationResult(ProjectIoMemoryError error) noexcept;
    explicit ProjectIoMemoryReservationResult(ProjectIoMemoryReservation reservation) noexcept;

    ProjectIoMemoryError error_ = ProjectIoMemoryError::InvalidLimit;
    ProjectIoMemoryReservation reservation_;
};

class ProjectIoOperationMemory final {
  public:
    [[nodiscard]] ProjectIoMemoryReservationResult reserve(std::uint64_t bytes) const;
    [[nodiscard]] ProjectIoOperationMemorySnapshot snapshot() const;

  private:
    friend class ProjectIoMemoryCoordinator;

    explicit ProjectIoOperationMemory(
        std::shared_ptr<detail::ProjectIoOperationMemoryState> state) noexcept;

    std::shared_ptr<detail::ProjectIoOperationMemoryState> state_;
};

class ProjectIoMemoryCoordinator final {
  public:
    [[nodiscard]] static std::optional<ProjectIoMemoryCoordinator>
    create(std::uint64_t limitBytes = kDefaultProjectIoSharedResidentBytes) noexcept;

    [[nodiscard]] std::optional<ProjectIoOperationMemory>
    createOperation(std::uint64_t limitBytes = kDefaultProjectIoOperationResidentBytes,
                    std::uint64_t singleAllocationLimitBytes =
                        kDefaultProjectIoSingleAllocationBytes) const noexcept;

    [[nodiscard]] ProjectIoMemorySnapshot snapshot() const;

  private:
    explicit ProjectIoMemoryCoordinator(
        std::shared_ptr<detail::ProjectIoSharedMemoryState> state) noexcept;

    std::shared_ptr<detail::ProjectIoSharedMemoryState> state_;
};

} // namespace bloom::project
