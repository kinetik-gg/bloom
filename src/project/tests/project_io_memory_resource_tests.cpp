#include <bloom/project/project_io_memory_resource.hpp>

#include <atomic>
#include <barrier>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <functional>
#include <iostream>
#include <limits>
#include <memory>
#include <memory_resource>
#include <new>
#include <stdexcept>
#include <string_view>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

namespace {

class Expectations final {
  public:
    void expect(const bool condition, const std::string_view message) {
        if (condition) {
            return;
        }
        ++failures_;
        std::cerr << "FAILED: " << message << '\n';
    }

    [[nodiscard]] int failures() const noexcept { return failures_; }

  private:
    int failures_ = 0;
};

enum class UpstreamFailure : std::uint8_t {
    None,
    BadAlloc,
    OtherException,
    DeallocateException,
};

class TrackingMemoryResource final : public std::pmr::memory_resource {
  public:
    explicit TrackingMemoryResource(const UpstreamFailure failure = UpstreamFailure::None) noexcept
        : failure_(failure) {}

    [[nodiscard]] std::size_t allocationCalls() const noexcept { return allocationCalls_; }
    [[nodiscard]] std::size_t liveAllocations() const noexcept { return liveAllocations_; }
    [[nodiscard]] std::size_t lastBytes() const noexcept { return lastBytes_; }
    [[nodiscard]] std::size_t lastAlignment() const noexcept { return lastAlignment_; }

  private:
    [[nodiscard]] void* do_allocate(const std::size_t bytes, const std::size_t alignment) override {
        ++allocationCalls_;
        lastBytes_ = bytes;
        lastAlignment_ = alignment;
        if (failure_ == UpstreamFailure::BadAlloc) {
            throw std::bad_alloc();
        }
        if (failure_ == UpstreamFailure::OtherException) {
            throw std::runtime_error("injected upstream allocation failure");
        }
        void* const pointer = ::operator new(bytes, std::align_val_t(alignment));
        ++liveAllocations_;
        return pointer;
    }

    void do_deallocate(void* const pointer, const std::size_t,
                       const std::size_t alignment) override {
        ::operator delete(pointer, std::align_val_t(alignment));
        --liveAllocations_;
        if (failure_ == UpstreamFailure::DeallocateException) {
            throw std::runtime_error("injected upstream deallocation failure");
        }
    }

    [[nodiscard]] bool do_is_equal(const std::pmr::memory_resource& other) const noexcept override {
        return this == &other;
    }

    UpstreamFailure failure_ = UpstreamFailure::None;
    std::size_t allocationCalls_ = 0;
    std::size_t liveAllocations_ = 0;
    std::size_t lastBytes_ = 0;
    std::size_t lastAlignment_ = 0;
};

using bloom::project::ProjectIoMemoryError;
using bloom::project::ProjectIoMemoryResource;
using bloom::project::ProjectIoPmrAllocationError;

static_assert(!std::is_copy_constructible_v<ProjectIoMemoryResource>);
static_assert(!std::is_move_constructible_v<ProjectIoMemoryResource>);

[[nodiscard]] bloom::project::ProjectIoMemoryCoordinator
makeCoordinator(const std::uint64_t limit) {
    auto result = bloom::project::ProjectIoMemoryCoordinator::create(limit);
    if (!result.has_value()) {
        throw std::runtime_error("failed to create test memory coordinator");
    }
    return std::move(*result);
}

[[nodiscard]] bloom::project::ProjectIoOperationMemory
makeOperation(const bloom::project::ProjectIoMemoryCoordinator& coordinator,
              const std::uint64_t limit, const std::uint64_t singleAllocationLimit) {
    auto result = coordinator.createOperation(limit, singleAllocationLimit);
    if (!result.has_value()) {
        throw std::runtime_error("failed to create test operation memory");
    }
    return std::move(*result);
}

[[nodiscard]] std::uint64_t measureCharge(Expectations& expectations, const std::size_t bytes,
                                          const std::size_t alignment) {
    auto coordinator = makeCoordinator(1'000'000);
    auto operation = makeOperation(coordinator, 1'000'000, 1'000'000);
    ProjectIoMemoryResource resource(operation);
    const auto result = resource.checkedAllocate(bytes, alignment);
    expectations.expect(result && result.pointer() != nullptr && result.chargedBytes() >= bytes,
                        "the charge-measurement allocation succeeds");
    const auto charge = result.chargedBytes();
    if (result) {
        resource.deallocate(result.pointer(), bytes, alignment);
    }
    return charge;
}

void testExactLimitsAndTypedErrors(Expectations& expectations) {
    constexpr std::size_t bytes = 17;
    constexpr std::size_t alignment = 32;
    const auto charge = measureCharge(expectations, bytes, alignment);
    if (charge == 0) {
        return;
    }

    auto coordinator = makeCoordinator(charge);
    auto firstOperation = makeOperation(coordinator, charge, charge);
    auto secondOperation = makeOperation(coordinator, charge, charge);
    ProjectIoMemoryResource first(firstOperation);
    ProjectIoMemoryResource second(secondOperation);

    const auto exact = first.checkedAllocate(bytes, alignment);
    expectations.expect(exact && exact.chargedBytes() == charge &&
                            firstOperation.snapshot().currentBytes == charge &&
                            coordinator.snapshot().currentBytes == charge,
                        "the exact single, operation, and shared allocation ceiling succeeds");
    if (!exact) {
        return;
    }

    const auto singleRejected = first.checkedAllocate(bytes + 1, alignment);
    expectations.expect(
        !singleRejected && singleRejected.error() == ProjectIoPmrAllocationError::BudgetRejected &&
            singleRejected.budgetError() == ProjectIoMemoryError::SingleAllocationLimitExceeded,
        "a padded block above the single-allocation ceiling stays typed");

    const auto operationRejected = first.checkedAllocate(0, 1);
    expectations.expect(
        !operationRejected &&
            operationRejected.error() == ProjectIoPmrAllocationError::BudgetRejected &&
            operationRejected.budgetError() == ProjectIoMemoryError::OperationLimitExceeded,
        "another in-limit block is rejected by the full operation budget");

    const auto sharedRejected = second.checkedAllocate(bytes, alignment);
    expectations.expect(
        !sharedRejected && sharedRejected.error() == ProjectIoPmrAllocationError::BudgetRejected &&
            sharedRejected.budgetError() == ProjectIoMemoryError::SharedLimitExceeded,
        "a second operation receives an exact shared-budget rejection");

    first.deallocate(exact.pointer(), bytes, alignment);
    const auto admittedAfterRelease = second.checkedAllocate(bytes, alignment);
    expectations.expect(admittedAfterRelease && coordinator.snapshot().currentBytes == charge,
                        "deallocation releases the charge exactly once for another operation");
    if (!admittedAfterRelease) {
        return;
    }
    second.deallocate(admittedAfterRelease.pointer(), bytes, alignment);
    expectations.expect(coordinator.snapshot().currentBytes == 0 &&
                            coordinator.snapshot().peakBytes == charge,
                        "exact-limit accounting returns to zero and preserves its peak");
}

void testAlignmentAndOverflow(Expectations& expectations) {
    auto coordinator = makeCoordinator(1'000'000);
    auto operation = makeOperation(coordinator, 1'000'000, 1'000'000);
    TrackingMemoryResource upstream;
    ProjectIoMemoryResource resource(operation, &upstream);

    constexpr std::size_t alignment = 4096;
    constexpr std::size_t bytes = 13;
    const auto overAligned = resource.checkedAllocate(bytes, alignment);
    if (!overAligned) {
        expectations.expect(false, "the over-aligned allocation succeeds");
        return;
    }
    const auto address = reinterpret_cast<std::uintptr_t>(overAligned.pointer());
    expectations.expect(overAligned && address % alignment == 0 &&
                            upstream.lastAlignment() >= alignment &&
                            upstream.lastBytes() == overAligned.chargedBytes() &&
                            operation.snapshot().currentBytes == overAligned.chargedBytes(),
                        "an over-aligned allocation charges its complete padded upstream block");
    resource.deallocate(overAligned.pointer(), bytes, alignment);
    expectations.expect(upstream.liveAllocations() == 0 && operation.snapshot().currentBytes == 0,
                        "an over-aligned round trip releases upstream storage and accounting");

    const auto zeroBytes = resource.checkedAllocate(0, 64);
    expectations.expect(zeroBytes && zeroBytes.chargedBytes() > 0 &&
                            operation.snapshot().currentBytes == zeroBytes.chargedBytes(),
                        "a zero-byte PMR request still charges its real upstream block");
    if (zeroBytes) {
        resource.deallocate(zeroBytes.pointer(), 0, 64);
    }

    const auto callsBeforeInvalid = upstream.allocationCalls();
    const auto zeroAlignment = resource.checkedAllocate(1, 0);
    const auto nonPowerOfTwo = resource.checkedAllocate(1, 3);
    expectations.expect(
        !zeroAlignment && zeroAlignment.error() == ProjectIoPmrAllocationError::InvalidAlignment &&
            !nonPowerOfTwo &&
            nonPowerOfTwo.error() == ProjectIoPmrAllocationError::InvalidAlignment &&
            upstream.allocationCalls() == callsBeforeInvalid,
        "zero and non-power-of-two alignments fail before budget or upstream work");

    const auto sizeOverflow = resource.checkedAllocate(std::numeric_limits<std::size_t>::max(), 1);
    constexpr auto highestAlignment = std::size_t{1}
                                      << (std::numeric_limits<std::size_t>::digits - 1);
    const auto alignmentOverflow = resource.checkedAllocate(highestAlignment, highestAlignment);
    expectations.expect(
        !sizeOverflow && sizeOverflow.error() == ProjectIoPmrAllocationError::SizeOverflow &&
            !alignmentOverflow &&
            alignmentOverflow.error() == ProjectIoPmrAllocationError::SizeOverflow &&
            operation.snapshot().currentBytes == 0,
        "size and alignment arithmetic overflow without allocation or charge");
}

void testUpstreamFailures(Expectations& expectations) {
    auto coordinator = makeCoordinator(100'000);
    auto operation = makeOperation(coordinator, 100'000, 100'000);
    TrackingMemoryResource badAllocUpstream(UpstreamFailure::BadAlloc);
    ProjectIoMemoryResource checkedResource(operation, &badAllocUpstream);

    const auto checked = checkedResource.checkedAllocate(64, 64);
    expectations.expect(
        !checked && checked.error() == ProjectIoPmrAllocationError::UpstreamAllocationFailed &&
            checked.budgetError() == ProjectIoMemoryError::None &&
            operation.snapshot().currentBytes == 0 && coordinator.snapshot().currentBytes == 0,
        "an upstream bad_alloc is distinct from budget rejection and leaks no charge");

    TrackingMemoryResource otherUpstream(UpstreamFailure::OtherException);
    ProjectIoMemoryResource pmrResource(operation, &otherUpstream);
    bool mappedToBadAlloc = false;
    try {
        static_cast<void>(pmrResource.allocate(64, 64));
    } catch (const std::bad_alloc&) {
        mappedToBadAlloc = true;
    }
    expectations.expect(mappedToBadAlloc && operation.snapshot().currentBytes == 0,
                        "the standard PMR surface maps an arbitrary upstream throw to bad_alloc");

    TrackingMemoryResource deallocateUpstream(UpstreamFailure::DeallocateException);
    ProjectIoMemoryResource deallocateResource(operation, &deallocateUpstream);
    const auto allocation = deallocateResource.checkedAllocate(64, 64);
    if (!allocation) {
        expectations.expect(false, "the throwing-deallocation fixture allocates successfully");
        return;
    }
    bool deallocationPropagated = false;
    try {
        deallocateResource.deallocate(allocation.pointer(), 64, 64);
    } catch (const std::runtime_error&) {
        deallocationPropagated = true;
    }
    expectations.expect(deallocationPropagated && deallocateUpstream.liveAllocations() == 0 &&
                            operation.snapshot().currentBytes == 0,
                        "a throwing upstream deallocation still releases its charge exactly once");
}

void testOperationLifetimeAndPmrMoves(Expectations& expectations) {
    auto coordinator = makeCoordinator(100'000);
    auto operation = std::make_unique<bloom::project::ProjectIoOperationMemory>(
        makeOperation(coordinator, 100'000, 100'000));
    ProjectIoMemoryResource resource(*operation);
    operation.reset();

    {
        std::pmr::vector<std::uint64_t> values(&resource);
        for (std::uint64_t value = 0; value < 128; ++value) {
            values.push_back(value);
        }
        std::pmr::vector<std::uint64_t> moved(std::move(values));
        expectations.expect(moved.size() == 128 && moved.front() == 0 && moved.back() == 127 &&
                                coordinator.snapshot().currentBytes > 0,
                            "a moved PMR container retains data and one live accounted allocation");
    }
    expectations.expect(coordinator.snapshot().currentBytes == 0 &&
                            coordinator.snapshot().peakBytes > 0,
                        "the adapter retains operation state after wrapper destruction and "
                        "releases on container destruction");
}

void testConcurrentSharedAdmission(Expectations& expectations) {
    constexpr std::size_t bytes = 64;
    constexpr std::size_t alignment = 64;
    const auto charge = measureCharge(expectations, bytes, alignment);
    if (charge == 0) {
        return;
    }

    auto coordinator = makeCoordinator(charge);
    auto firstOperation = makeOperation(coordinator, charge, charge);
    auto secondOperation = makeOperation(coordinator, charge, charge);
    ProjectIoMemoryResource first(firstOperation);
    ProjectIoMemoryResource second(secondOperation);
    std::barrier gate(2);
    std::atomic<int> successes = 0;
    std::atomic<int> sharedRejections = 0;

    const auto worker = [&](ProjectIoMemoryResource& resource) {
        const auto result = resource.checkedAllocate(bytes, alignment);
        if (result) {
            ++successes;
        } else if (result.error() == ProjectIoPmrAllocationError::BudgetRejected &&
                   result.budgetError() == ProjectIoMemoryError::SharedLimitExceeded) {
            ++sharedRejections;
        }
        gate.arrive_and_wait();
        if (result) {
            resource.deallocate(result.pointer(), bytes, alignment);
        }
    };

    std::thread firstThread(worker, std::ref(first));
    std::thread secondThread(worker, std::ref(second));
    firstThread.join();
    secondThread.join();
    expectations.expect(
        successes == 1 && sharedRejections == 1 && coordinator.snapshot().currentBytes == 0 &&
            coordinator.snapshot().peakBytes == charge,
        "concurrent operations atomically admit one exact shared-budget allocation");
}

void testInvalidResource(Expectations& expectations) {
    auto coordinator = makeCoordinator(1000);
    auto operation = makeOperation(coordinator, 1000, 1000);
    ProjectIoMemoryResource resource(operation, nullptr);
    const auto result = resource.checkedAllocate(1);
    expectations.expect(!result && result.error() == ProjectIoPmrAllocationError::InvalidResource &&
                            operation.snapshot().currentBytes == 0,
                        "a null upstream resource fails before accounting");
}

} // namespace

int main() {
    Expectations expectations;
    try {
        testExactLimitsAndTypedErrors(expectations);
        testAlignmentAndOverflow(expectations);
        testUpstreamFailures(expectations);
        testOperationLifetimeAndPmrMoves(expectations);
        testConcurrentSharedAdmission(expectations);
        testInvalidResource(expectations);
    } catch (const std::exception& error) {
        std::cerr << "FAILED: test fixture exception: " << error.what() << '\n';
        return 1;
    } catch (...) {
        std::cerr << "FAILED: unknown test fixture exception\n";
        return 1;
    }
    return expectations.failures() == 0 ? 0 : 1;
}
