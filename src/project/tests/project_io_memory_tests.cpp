#include <bloom/project/project_io_memory.hpp>

#include <cstdint>
#include <iostream>
#include <limits>
#include <string_view>

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

using bloom::project::ProjectIoMemoryError;

void testLimitValidation(Expectations& expectations) {
    expectations.expect(!bloom::project::ProjectIoMemoryCoordinator::create(0).has_value(),
                        "a zero shared limit is invalid");
    auto coordinator = bloom::project::ProjectIoMemoryCoordinator::create(1000);
    expectations.expect(coordinator.has_value(), "a positive shared limit is valid");
    if (!coordinator.has_value()) {
        return;
    }
    expectations.expect(!coordinator->createOperation(0, 1).has_value(),
                        "a zero operation limit is invalid");
    expectations.expect(!coordinator->createOperation(1001, 1).has_value(),
                        "an operation cannot exceed the shared limit");
    expectations.expect(!coordinator->createOperation(500, 0).has_value(),
                        "a zero single-allocation limit is invalid");
    expectations.expect(!coordinator->createOperation(500, 501).has_value(),
                        "a single allocation cannot exceed its operation limit");
}

void testReservationAccounting(Expectations& expectations) {
    auto coordinatorResult = bloom::project::ProjectIoMemoryCoordinator::create(1000);
    if (!coordinatorResult.has_value()) {
        expectations.expect(false, "the accounting fixture creates a coordinator");
        return;
    }
    auto coordinator = std::move(*coordinatorResult);
    auto operationResult = coordinator.createOperation(800, 600);
    if (!operationResult.has_value()) {
        expectations.expect(false, "the accounting fixture creates an operation budget");
        return;
    }
    auto operation = std::move(*operationResult);
    auto firstResult = operation.reserve(600);
    expectations.expect(static_cast<bool>(firstResult),
                        "the exact single-allocation limit reserves");
    auto first = std::move(firstResult).takeReservation();
    expectations.expect(first.isValid() && first.bytes() == 600,
                        "a successful result transfers one live reservation");
    expectations.expect(operation.snapshot().currentBytes == 600 &&
                            coordinator.snapshot().currentBytes == 600,
                        "operation and shared accounting advance together");

    const auto tooLarge = operation.reserve(601);
    expectations.expect(!tooLarge &&
                            tooLarge.error() == ProjectIoMemoryError::SingleAllocationLimitExceeded,
                        "the single-allocation ceiling fails before aggregate accounting");
    const auto operationFull = operation.reserve(201);
    expectations.expect(!operationFull &&
                            operationFull.error() == ProjectIoMemoryError::OperationLimitExceeded,
                        "an operation cannot exceed its resident ceiling");

    auto exactResult = operation.reserve(200);
    auto exact = std::move(exactResult).takeReservation();
    expectations.expect(operation.snapshot().currentBytes == 800 &&
                            operation.snapshot().peakBytes == 800,
                        "the exact operation ceiling is accepted and recorded as peak");
    exact.reset();
    expectations.expect(operation.snapshot().currentBytes == 600,
                        "explicit release returns the charge exactly once");
    first.reset();
    expectations.expect(operation.snapshot().currentBytes == 0 &&
                            coordinator.snapshot().currentBytes == 0 &&
                            coordinator.snapshot().peakBytes == 800,
                        "RAII release preserves high-water evidence");
}

void testSharedAdmissionAndLifetime(Expectations& expectations) {
    auto coordinator = bloom::project::ProjectIoMemoryCoordinator::create(1000);
    if (!coordinator.has_value()) {
        expectations.expect(false, "the shared-admission fixture creates a coordinator");
        return;
    }
    auto firstOperation = coordinator->createOperation(800, 800);
    auto secondOperation = coordinator->createOperation(800, 800);
    if (!firstOperation.has_value() || !secondOperation.has_value()) {
        expectations.expect(false, "the shared-admission fixture creates two operation budgets");
        return;
    }
    auto firstResult = firstOperation->reserve(700);
    auto first = std::move(firstResult).takeReservation();

    const auto rejected = secondOperation->reserve(301);
    expectations.expect(!rejected && rejected.error() == ProjectIoMemoryError::SharedLimitExceeded,
                        "concurrent operations cannot overcommit shared resident memory");
    auto acceptedResult = secondOperation->reserve(300);
    auto accepted = std::move(acceptedResult).takeReservation();
    expectations.expect(accepted.isValid() && coordinator->snapshot().currentBytes == 1000,
                        "the exact shared ceiling is accepted");

    coordinator.reset();
    firstOperation.reset();
    secondOperation.reset();
    accepted.reset();
    first.reset();
    expectations.expect(true, "reservations retain accounting state beyond wrapper lifetimes");
}

void testMoveAndOverflowBoundaries(Expectations& expectations) {
    auto coordinatorResult = bloom::project::ProjectIoMemoryCoordinator::create(1000);
    if (!coordinatorResult.has_value()) {
        expectations.expect(false, "the move fixture creates a coordinator");
        return;
    }
    auto coordinator = std::move(*coordinatorResult);
    auto operationResult = coordinator.createOperation(900, 900);
    if (!operationResult.has_value()) {
        expectations.expect(false, "the move fixture creates an operation budget");
        return;
    }
    auto operation = std::move(*operationResult);
    auto result = operation.reserve(400);
    auto original = std::move(result).takeReservation();
    auto moved = std::move(original);
    expectations.expect(
        !original.isValid() && // NOLINT(bugprone-use-after-move,clang-analyzer-cplusplus.Move)
            moved.isValid() && coordinator.snapshot().currentBytes == 400,
        "moving a reservation transfers without duplicating its charge");

    const auto enormous = operation.reserve(std::numeric_limits<std::uint64_t>::max());
    expectations.expect(
        !enormous && enormous.error() == ProjectIoMemoryError::SingleAllocationLimitExceeded &&
            coordinator.snapshot().currentBytes == 400,
        "an enormous request fails without wrapped accounting");
    moved.reset();
}

} // namespace

int main() {
    Expectations expectations;
    testLimitValidation(expectations);
    testReservationAccounting(expectations);
    testSharedAdmissionAndLifetime(expectations);
    testMoveAndOverflowBoundaries(expectations);
    return expectations.failures() == 0 ? 0 : 1;
}
