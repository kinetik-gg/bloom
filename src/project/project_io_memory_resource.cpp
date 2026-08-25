#include <bloom/project/project_io_memory_resource.hpp>

#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <memory_resource>
#include <new>
#include <optional>
#include <utility>

namespace bloom::project {

struct ProjectIoMemoryResource::AllocationHeader final {
    AllocationHeader(void* allocation, const std::size_t allocationBytes,
                     const std::size_t allocationAlignment,
                     ProjectIoMemoryReservation memoryReservation) noexcept
        : base(allocation), bytes(allocationBytes), alignment(allocationAlignment),
          reservation(std::move(memoryReservation)) {}

    void* base = nullptr;
    std::size_t bytes = 0;
    std::size_t alignment = 0;
    ProjectIoMemoryReservation reservation;
};

ProjectIoMemoryResource::ProjectIoMemoryResource(ProjectIoOperationMemory operation,
                                                 std::pmr::memory_resource* const upstream) noexcept
    : operation_(std::move(operation)), upstream_(upstream) {}

ProjectIoPmrAllocationResult
ProjectIoMemoryResource::checkedAllocate(const std::size_t bytes,
                                         const std::size_t alignment) noexcept {
    if (upstream_ == nullptr) {
        return ProjectIoPmrAllocationResult::failure(ProjectIoPmrAllocationError::InvalidResource);
    }
    if (!std::has_single_bit(alignment)) {
        return ProjectIoPmrAllocationResult::failure(ProjectIoPmrAllocationError::InvalidAlignment);
    }

    constexpr auto headerPointerBytes = sizeof(AllocationHeader*);
    constexpr auto fixedPrefixBytes = sizeof(AllocationHeader) + headerPointerBytes;
    const auto alignmentMask = alignment - 1;
    if (fixedPrefixBytes > SIZE_MAX - alignmentMask) {
        return ProjectIoPmrAllocationResult::failure(ProjectIoPmrAllocationError::SizeOverflow);
    }
    const auto userOffset = (fixedPrefixBytes + alignmentMask) & ~alignmentMask;
    const auto payloadBytes = std::max<std::size_t>(bytes, 1);
    if (payloadBytes > SIZE_MAX - userOffset) {
        return ProjectIoPmrAllocationResult::failure(ProjectIoPmrAllocationError::SizeOverflow);
    }
    const auto upstreamBytes = userOffset + payloadBytes;
    if (!std::in_range<std::uint64_t>(upstreamBytes)) {
        return ProjectIoPmrAllocationResult::failure(ProjectIoPmrAllocationError::SizeOverflow);
    }
    const auto chargedBytes = static_cast<std::uint64_t>(upstreamBytes);

    std::optional<ProjectIoMemoryReservation> reservation;
    try {
        auto reservationResult = operation_.reserve(chargedBytes);
        if (!reservationResult) {
            return ProjectIoPmrAllocationResult::failure(
                ProjectIoPmrAllocationError::BudgetRejected, reservationResult.error());
        }
        reservation.emplace(std::move(reservationResult).takeReservation());
    } catch (...) {
        return ProjectIoPmrAllocationResult::failure(
            ProjectIoPmrAllocationError::AccountingFailure);
    }

    const auto upstreamAlignment = std::max(alignment, alignof(AllocationHeader));
    void* base = nullptr;
    try {
        base = upstream_->allocate(upstreamBytes, upstreamAlignment);
    } catch (...) {
        return ProjectIoPmrAllocationResult::failure(
            ProjectIoPmrAllocationError::UpstreamAllocationFailed);
    }

    auto* header = std::construct_at(static_cast<AllocationHeader*>(base), base, upstreamBytes,
                                     upstreamAlignment, std::move(*reservation));
    auto* userPointer = static_cast<std::byte*>(base) + userOffset;
    std::memcpy(userPointer - headerPointerBytes, static_cast<const void*>(&header),
                headerPointerBytes);
    return ProjectIoPmrAllocationResult::success(userPointer, chargedBytes);
}

void* ProjectIoMemoryResource::do_allocate(const std::size_t bytes, const std::size_t alignment) {
    const auto result = checkedAllocate(bytes, alignment);
    if (!result) {
        throw std::bad_alloc();
    }
    return result.pointer();
}

void ProjectIoMemoryResource::do_deallocate(void* const pointer, const std::size_t,
                                            const std::size_t) {
    constexpr auto headerPointerBytes = sizeof(AllocationHeader*);
    auto* userPointer = static_cast<std::byte*>(pointer);
    AllocationHeader* header = nullptr;
    std::memcpy(static_cast<void*>(&header), userPointer - headerPointerBytes, headerPointerBytes);

    void* const base = header->base;
    const auto bytes = header->bytes;
    const auto alignment = header->alignment;
    auto reservation = std::move(header->reservation);
    std::destroy_at(header);
    upstream_->deallocate(base, bytes, alignment);
}

bool ProjectIoMemoryResource::do_is_equal(const std::pmr::memory_resource& other) const noexcept {
    return this == &other;
}

} // namespace bloom::project
