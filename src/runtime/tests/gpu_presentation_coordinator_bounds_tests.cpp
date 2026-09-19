// CPU-only ownership/bounds/forget validation for the presentation coordinator. It exercises the
// bounded process quarantine reservation store and the terminal forget lifecycle with no device and
// no owner thread. The real Wayland lifecycle proof (attach/present/resize/retire) lives in
// gpu_presentation_coordinator_tests.cpp; this test proves the safety gate itself.

#include "gpu_presentation_coordinator_private.hpp"
#include "gpu_presentation_coordinator_state.hpp"

#include <bloom/runtime/gpu_presentation_coordinator.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <iterator>
#include <memory>
#include <source_location>
#include <string>
#include <utility>

namespace {

using bloom::render::GpuBorrowedSurface;
using bloom::render::GpuPresentationEpoch;
using bloom::runtime::GpuPresentationClient;
using bloom::runtime::GpuPresentationCoordinatorOptions;
using bloom::runtime::GpuPresentationPortCode;
using bloom::runtime::GpuPresentationTargetId;
using bloom::runtime::GpuPresentationTargetState;
using bloom::runtime::detail::GpuPresentationMailbox;
using bloom::runtime::presentation_coordinator_detail::CoordinatorState;
using bloom::runtime::presentation_coordinator_detail::kMaxQuarantinedTargets;
using bloom::runtime::presentation_coordinator_detail::kNoQuarantineReservation;
using bloom::runtime::presentation_coordinator_detail::QuarantineReservation;

class Expectations final {
  public:
    void expect(const bool condition, const std::string& message,
                const std::source_location location = std::source_location::current()) {
        ++checks_;
        if (!condition) {
            ++failures_;
            std::cerr << "FAIL: " << message << " (" << location.file_name() << ':'
                      << location.line() << ")\n";
        }
    }
    [[nodiscard]] int failures() const noexcept { return failures_; }

  private:
    int checks_ = 0;
    int failures_ = 0;
};

[[nodiscard]] GpuBorrowedSurface surface(const std::uint64_t bits, const std::uint64_t epoch) {
    GpuBorrowedSurface value;
    value.surface_bits = bits;
    value.epoch = GpuPresentationEpoch{epoch};
    return value;
}

void testReservationBounds(Expectations& expectations) {
    using namespace bloom::runtime::presentation_coordinator_detail;

    resetQuarantineForTesting();
    expectations.expect(quarantineEnsureStorage(), "the fixed quarantine storage allocates");
    expectations.expect(!quarantineFuseLatched(), "the fuse starts clear");
    expectations.expect(quarantinedCount() == 0U, "no generation is retained initially");

    // Reserve the whole fixed capacity under the shared process lock.
    std::array<QuarantineReservation, kMaxQuarantinedTargets> reservations{};
    for (std::size_t index = 0; index < kMaxQuarantinedTargets; ++index) {
        expectations.expect(quarantineReserve(reservations[index]),
                            "a slot is reserved before native creation");
        expectations.expect(reservations[index] != kNoQuarantineReservation,
                            "a reserved slot has a valid token");
    }
    expectations.expect(quarantineCapReached(), "the bounded cap reports reached");

    // A reservation beyond the cap is refused and takes no ownership. That refusal is ordinary
    // back-pressure, NOT proof of an unretirable generation, so the irreversible fuse MUST stay
    // clear and no native state is created.
    QuarantineReservation overflow = kNoQuarantineReservation;
    expectations.expect(!quarantineReserve(overflow), "a reservation beyond the cap is refused");
    expectations.expect(overflow == kNoQuarantineReservation, "a refused reservation is invalid");
    expectations.expect(!quarantineFuseLatched(),
                        "temporary capacity pressure does not latch the irreversible fuse");

    // Capacity-recovery gate: releasing a healthy reservation lets a later reservation succeed,
    // still with no fuse latch. This is the normal "all slots reserved by healthy targets" case
    // that must not permanently poison future target creation.
    quarantineRelease(reservations[0]);
    reservations[0] = kNoQuarantineReservation;
    QuarantineReservation recovered = kNoQuarantineReservation;
    expectations.expect(quarantineReserve(recovered),
                        "a released healthy reservation recovers reservation capacity");
    expectations.expect(recovered != kNoQuarantineReservation && !quarantineFuseLatched(),
                        "capacity recovery leaves the fuse clear");
    quarantineRelease(recovered);
    quarantineRelease(recovered); // idempotent for an invalid token

    // Only an actual unproven retention latches the process fuse and makes a committed slot
    // irreversible.
    quarantineCommit(reservations[1], nullptr, nullptr);
    expectations.expect(quarantinedCount() == 1U,
                        "a committed generation is retained (ownership moved under one lock)");
    expectations.expect(quarantineFuseLatched(),
                        "an actual unproven retention latches the process fuse");
    expectations.expect(!quarantineCapReached(),
                        "releasing one reservation opened exactly one slot");
    quarantineRelease(reservations[1]); // committed -> intentionally irreversible
    expectations.expect(quarantinedCount() == 1U, "a committed slot can never be released");

    resetQuarantineForTesting();
    expectations.expect(quarantinedCount() == 0U && !quarantineFuseLatched(),
                        "the test reset clears the fixed store");
}

void testPortFuseAndRetainedBound(Expectations& expectations) {
    using namespace bloom::runtime::presentation_coordinator_detail;

    resetQuarantineForTesting();
    auto mailbox = std::make_shared<GpuPresentationMailbox>();
    mailbox->maxTargets = 1000U;
    mailbox->maxRetainedRecords = 4U;
    auto client = GpuPresentationClient::createForTesting(mailbox);
    expectations.expect(client != nullptr, "the bounds client is created");

    // The retained-record cap bounds snapshots/entries even before an adapter calls forget().
    GpuPresentationTargetId newest = 0U;
    for (int index = 0; index < 4; ++index) {
        const auto attached =
            client->attach(surface(static_cast<std::uint64_t>(index + 1), 7U), 16U, 16U);
        expectations.expect(attached.accepted(), "an attach under the record cap is admitted");
        newest = attached.target;
    }
    const auto overBound = client->attach(surface(50U, 7U), 16U, 16U);
    expectations.expect(overBound.code == GpuPresentationPortCode::TooManyTargets,
                        "the bounded retained-record cap refuses a new attach");

    expectations.expect(client->forget(9999U).code == GpuPresentationPortCode::UnknownTarget,
                        "forgetting an unknown record is refused");

    // Simulate the owner publishing a proven Retired acknowledgement.
    {
        std::lock_guard lock(mailbox->mutex);
        auto& snapshot = mailbox->snapshots[newest];
        snapshot.id = newest;
        snapshot.state = GpuPresentationTargetState::Retired;
        snapshot.surfaceSafeToDestroy = true;
    }
    expectations.expect(client->forget(newest).code == GpuPresentationPortCode::Accepted,
                        "a proven-terminal record can be forgotten");
    expectations.expect(client->status(newest).state == GpuPresentationTargetState::Gone,
                        "the forgotten record is no longer visible");
    {
        std::lock_guard lock(mailbox->mutex);
        expectations.expect(mailbox->forgetRequested.count(newest) == 1U,
                            "the owner is asked to erase the forgotten entry");
    }
    const auto readmitted = client->attach(surface(60U, 7U), 16U, 16U);
    expectations.expect(readmitted.accepted(), "freeing a record readmits an attach");

    // A live/unproven record may never be forgotten: its surface ownership stays retained.
    {
        std::lock_guard lock(mailbox->mutex);
        auto& snapshot = mailbox->snapshots[readmitted.target];
        snapshot.id = readmitted.target;
        snapshot.state = GpuPresentationTargetState::Quarantined;
        snapshot.surfaceSafeToDestroy = false;
    }
    expectations.expect(client->forget(readmitted.target).code ==
                            GpuPresentationPortCode::NotTerminal,
                        "an unproven record cannot be forgotten");
    expectations.expect(client->status(readmitted.target).state ==
                            GpuPresentationTargetState::Quarantined,
                        "the unproven record is retained");

    // A latched process fuse stops admission on the existing port, not only new coordinators.
    latchQuarantineFuse();
    expectations.expect(client->attach(surface(70U, 7U), 16U, 16U).code ==
                            GpuPresentationPortCode::QuarantineFused,
                        "a latched fuse refuses attach on an existing port");
    resetQuarantineForTesting();
}

void testOwnerForgetDrain(Expectations& expectations) {
    auto mailbox = std::make_shared<GpuPresentationMailbox>();
    constexpr GpuPresentationTargetId kProven = 42U;
    constexpr GpuPresentationTargetId kUnproven = 43U;
    {
        std::lock_guard lock(mailbox->mutex);
        mailbox->forgetRequested.insert(kProven);
        auto& snapshot = mailbox->snapshots[kProven];
        snapshot.id = kProven;
        snapshot.state = GpuPresentationTargetState::Retired;
        snapshot.surfaceSafeToDestroy = true;
    }

    CoordinatorState state(nullptr, nullptr, GpuPresentationCoordinatorOptions{});
    state.mailbox = mailbox;
    {
        auto& proven = state.entries[kProven];
        proven.id = kProven;
        proven.state = GpuPresentationTargetState::Retired;
        auto& unproven = state.entries[kUnproven];
        unproven.id = kUnproven;
        unproven.state = GpuPresentationTargetState::Quarantined;
    }
    state.ingestRequests();
    expectations.expect(state.entries.count(kProven) == 0U,
                        "the owner erases a proven forgotten entry");
    expectations.expect(state.entries.count(kUnproven) == 1U,
                        "the owner keeps an unproven entry and its surface ownership");

    // A Retired entry that still holds a quarantine reservation is not yet forgettable.
    constexpr GpuPresentationTargetId kReserved = 44U;
    {
        auto& reserved = state.entries[kReserved];
        reserved.id = kReserved;
        reserved.state = GpuPresentationTargetState::Retired;
        reserved.reservation = 0U;
    }
    {
        std::lock_guard lock(mailbox->mutex);
        mailbox->forgetRequested.insert(kReserved);
    }
    state.ingestRequests();
    expectations.expect(state.entries.count(kReserved) == 1U,
                        "a Retired entry with a live reservation is not forgotten");
}

void testThousandsForgetCycles(Expectations& expectations) {
    auto mailbox = std::make_shared<GpuPresentationMailbox>();
    mailbox->maxTargets = 8U;
    mailbox->maxRetainedRecords = 64U;
    auto client = GpuPresentationClient::createForTesting(mailbox);
    CoordinatorState state(nullptr, nullptr, GpuPresentationCoordinatorOptions{});
    state.mailbox = mailbox;

    std::uint64_t bits = 1000U;
    std::size_t peakSnapshots = 0;
    std::size_t peakEntries = 0;
    std::size_t peakSurfaces = 0;
    std::size_t peakAdmitted = 0;
    constexpr int kCycles = 5000;
    for (int cycle = 0; cycle < kCycles; ++cycle) {
        const auto attached = client->attach(surface(++bits, 7U), 16U, 16U);
        expectations.expect(attached.accepted(), "each attach in a forget cycle is admitted");
        auto& entry = state.entries[attached.target];
        entry.id = attached.target;
        entry.state = GpuPresentationTargetState::Retired;
        {
            std::lock_guard lock(mailbox->mutex);
            auto& snapshot = mailbox->snapshots[attached.target];
            snapshot.id = attached.target;
            snapshot.state = GpuPresentationTargetState::Retired;
            snapshot.surfaceSafeToDestroy = true;
            // Mirror the owner's proven-terminal releases.
            mailbox->slots.erase(attached.target);
            for (auto it = mailbox->ownedSurfaces.begin(); it != mailbox->ownedSurfaces.end();) {
                it = it->second == attached.target ? mailbox->ownedSurfaces.erase(it)
                                                   : std::next(it);
            }
            if (mailbox->admittedTargets > 0U) {
                --mailbox->admittedTargets;
            }
        }
        expectations.expect(client->forget(attached.target).code ==
                                GpuPresentationPortCode::Accepted,
                            "each proven-terminal record can be forgotten");
        state.ingestRequests();
        expectations.expect(state.entries.count(attached.target) == 0U,
                            "the owner erased the forgotten entry");
        if (mailbox->snapshots.size() > peakSnapshots) {
            peakSnapshots = mailbox->snapshots.size();
        }
        if (state.entries.size() > peakEntries) {
            peakEntries = state.entries.size();
        }
        if (mailbox->ownedSurfaces.size() > peakSurfaces) {
            peakSurfaces = mailbox->ownedSurfaces.size();
        }
        if (mailbox->admittedTargets > peakAdmitted) {
            peakAdmitted = mailbox->admittedTargets;
        }
    }
    expectations.expect(peakSnapshots <= mailbox->maxRetainedRecords,
                        "snapshots stayed within the bounded retained-record cap");
    expectations.expect(peakEntries <= 1U,
                        "owner entries stayed bounded across thousands of forget cycles");
    expectations.expect(peakSurfaces <= 1U,
                        "surface ownership stayed bounded across thousands of cycles");
    expectations.expect(peakAdmitted <= 1U,
                        "admission count stayed bounded across thousands of cycles");
}

void testResetRefusesRealCommittedGeneration(Expectations& expectations) {
    using namespace bloom::runtime::presentation_coordinator_detail;

    resetQuarantineForTesting(); // starts from a store that holds no real resources
    expectations.expect(quarantineEnsureStorage(), "storage exists for the reset guard");

    // Reserve and commit a generation whose retaining alias is non-null, exactly like a live lease
    // pin. The test-only reset MUST refuse to erase it rather than dropping the pin behind the
    // still-referencing native target.
    QuarantineReservation reservation = kNoQuarantineReservation;
    expectations.expect(quarantineReserve(reservation), "the guard reservation is taken");
    const auto* sentinel = reinterpret_cast<const bloom::render::GpuDisplayImage*>(0x1);
    std::shared_ptr<const bloom::render::GpuDisplayImage> realAlias(
        sentinel, [](const bloom::render::GpuDisplayImage*) noexcept {});
    quarantineCommit(reservation, nullptr, std::move(realAlias));
    expectations.expect(quarantinedCount() == 1U, "the real generation is retained");

    resetQuarantineForTesting();
    expectations.expect(quarantinedCount() == 1U,
                        "the test reset refuses to drop a committed generation with a live alias");
}

int run() {
    Expectations expectations;
    testReservationBounds(expectations);
    testPortFuseAndRetainedBound(expectations);
    testOwnerForgetDrain(expectations);
    testThousandsForgetCycles(expectations);
    testResetRefusesRealCommittedGeneration(expectations);
    if (expectations.failures() == 0) {
        std::cout << "PASS: presentation coordinator ownership/bounds/forget gates\n";
    }
    return expectations.failures() == 0 ? 0 : 1;
}

} // namespace

int main() { return run(); }
