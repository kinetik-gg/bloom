#pragma once

#include <bloom/project/project_io_memory.hpp>

#include <cstddef>
#include <cstdint>
#include <memory_resource>
#include <type_traits>

namespace bloom::project {

enum class ProjectIoPmrAllocationError : std::uint8_t {
    None,
    InvalidResource,
    InvalidAlignment,
    SizeOverflow,
    BudgetRejected,
    AccountingFailure,
    UpstreamAllocationFailed,
};

class [[nodiscard]] ProjectIoPmrAllocationResult final {
  public:
    [[nodiscard]] explicit constexpr operator bool() const noexcept {
        return error_ == ProjectIoPmrAllocationError::None;
    }
    [[nodiscard]] constexpr void* pointer() const noexcept { return pointer_; }
    [[nodiscard]] constexpr std::uint64_t chargedBytes() const noexcept { return chargedBytes_; }
    [[nodiscard]] constexpr ProjectIoPmrAllocationError error() const noexcept { return error_; }
    [[nodiscard]] constexpr ProjectIoMemoryError budgetError() const noexcept {
        return budgetError_;
    }

  private:
    friend class ProjectIoMemoryResource;

    [[nodiscard]] static constexpr ProjectIoPmrAllocationResult
    success(void* pointer, const std::uint64_t chargedBytes) noexcept {
        return ProjectIoPmrAllocationResult(pointer, chargedBytes);
    }
    [[nodiscard]] static constexpr ProjectIoPmrAllocationResult
    failure(const ProjectIoPmrAllocationError error,
            const ProjectIoMemoryError budgetError = ProjectIoMemoryError::None) noexcept {
        return ProjectIoPmrAllocationResult(error, budgetError);
    }

    constexpr ProjectIoPmrAllocationResult(void* pointer, const std::uint64_t chargedBytes) noexcept
        : pointer_(pointer), chargedBytes_(chargedBytes) {}
    constexpr ProjectIoPmrAllocationResult(const ProjectIoPmrAllocationError error,
                                           const ProjectIoMemoryError budgetError) noexcept
        : error_(error), budgetError_(budgetError) {}

    void* pointer_ = nullptr;
    std::uint64_t chargedBytes_ = 0;
    ProjectIoPmrAllocationError error_ = ProjectIoPmrAllocationError::None;
    ProjectIoMemoryError budgetError_ = ProjectIoMemoryError::None;
};

// Charges complete upstream blocks, including the in-band accounting header and alignment padding,
// before asking the upstream resource to allocate. The adapter and its conforming upstream
// resource (non-null on success, otherwise throwing) must outlive every allocation, as required by
// the std::pmr::memory_resource contract.
class ProjectIoMemoryResource final : public std::pmr::memory_resource {
  public:
    explicit ProjectIoMemoryResource(
        ProjectIoOperationMemory operation,
        std::pmr::memory_resource* upstream = std::pmr::get_default_resource()) noexcept;

    ProjectIoMemoryResource(const ProjectIoMemoryResource&) = delete;
    ProjectIoMemoryResource& operator=(const ProjectIoMemoryResource&) = delete;
    ProjectIoMemoryResource(ProjectIoMemoryResource&&) = delete;
    ProjectIoMemoryResource& operator=(ProjectIoMemoryResource&&) = delete;

    // PMR's allocate() surface necessarily reports every failure as std::bad_alloc. Direct codec
    // paths may use this noexcept companion to retain exact budget-rejection diagnostics.
    [[nodiscard]] ProjectIoPmrAllocationResult
    checkedAllocate(std::size_t bytes, std::size_t alignment = alignof(std::max_align_t)) noexcept;

    [[nodiscard]] std::pmr::memory_resource* upstreamResource() const noexcept { return upstream_; }

  private:
    struct AllocationHeader;

    [[nodiscard]] void* do_allocate(std::size_t bytes, std::size_t alignment) override;
    void do_deallocate(void* pointer, std::size_t bytes, std::size_t alignment) override;
    [[nodiscard]] bool do_is_equal(const std::pmr::memory_resource& other) const noexcept override;

    ProjectIoOperationMemory operation_;
    std::pmr::memory_resource* upstream_ = nullptr;
};

static_assert(std::is_trivially_copyable_v<ProjectIoPmrAllocationResult>);

} // namespace bloom::project
