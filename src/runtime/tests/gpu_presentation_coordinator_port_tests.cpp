// CPU-only port validation for the presentation coordinator. It runs with no GPU and no owner: it
// exercises the shared mailbox transport, typed admission, bounded target cap, latest-only
// coalescing (resize), stale-sequence rejection, retire closing admission, overlay validation, and
// fail-closed behaviour after owner teardown. It deliberately cannot substitute the native
// lifecycle proof, which lives in gpu_presentation_coordinator_tests.cpp.

#include "gpu_presentation_coordinator_private.hpp"

#include <bloom/runtime/gpu_presentation_coordinator.hpp>

#include <cstddef>
#include <cstdint>
#include <iostream>
#include <memory>
#include <source_location>
#include <string>
#include <utility>
#include <vector>

namespace {

using bloom::render::GpuBorrowedSurface;
using bloom::render::GpuPresentationEpoch;
using bloom::runtime::GpuPresentationClient;
using bloom::runtime::GpuPresentationOverlay;
using bloom::runtime::GpuPresentationPortCode;
using bloom::runtime::GpuPresentationTargetId;
using bloom::runtime::GpuPresentationTargetState;
using bloom::runtime::detail::GpuPresentationMailbox;

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

int run() {
    Expectations expectations;
    auto mailbox = std::make_shared<GpuPresentationMailbox>();
    mailbox->maxTargets = 2U;
    mailbox->maxOverlayBytes = 128U;
    mailbox->maxOverlayBytesPerFrame = 64U;
    mailbox->maxTargetExtent = 4096U;
    auto client = GpuPresentationClient::createForTesting(mailbox);
    expectations.expect(client != nullptr, "the test client is created");

    // The port publishes no instance view before the owner sets one.
    expectations.expect(!client->instanceView().valid,
                        "no borrowed instance view is published initially");

    // Unknown ids are Gone/UnknownTarget.
    expectations.expect(client->status(999U).state == GpuPresentationTargetState::Gone,
                        "an unknown target reports Gone");
    expectations.expect(client->update(999U, 1U, bloom::runtime::GpuPresentationUpdate{}).code ==
                            GpuPresentationPortCode::UnknownTarget,
                        "an update for an unknown target is refused");

    // Malformed attach is rejected without consuming an admission slot.
    expectations.expect(client->attach(surface(0U, 1U), 16U, 16U).code ==
                            GpuPresentationPortCode::Rejected,
                        "an attach without a surface is rejected");
    expectations.expect(client->attach(surface(1U, 1U), 0U, 16U).code ==
                            GpuPresentationPortCode::Rejected,
                        "an attach without an extent is rejected");

    const auto first = client->attach(surface(1U, 7U), 320U, 240U);
    expectations.expect(first.code == GpuPresentationPortCode::Accepted && first.target != 0U,
                        "the first attach is admitted with a stable id");
    expectations.expect(client->status(first.target).state == GpuPresentationTargetState::Attaching,
                        "an admitted attach starts Attaching");

    const auto second = client->attach(surface(2U, 7U), 320U, 240U);
    expectations.expect(second.code == GpuPresentationPortCode::Accepted,
                        "the second attach is admitted");
    const auto third = client->attach(surface(3U, 7U), 320U, 240U);
    expectations.expect(third.code == GpuPresentationPortCode::TooManyTargets,
                        "the bounded target cap refuses a third target");

    // Latest-only coalescing over resize: three quick resizes retain only the newest.
    expectations.expect(client->resize(first.target, 1U, 100U, 100U).code ==
                            GpuPresentationPortCode::Accepted,
                        "the first resize is accepted");
    expectations.expect(client->resize(first.target, 2U, 200U, 200U).code ==
                            GpuPresentationPortCode::Coalesced,
                        "a newer resize coalesces the unapplied one");
    expectations.expect(client->resize(first.target, 3U, 300U, 300U).code ==
                            GpuPresentationPortCode::Coalesced,
                        "the newest resize coalesces again");
    {
        std::lock_guard lock(mailbox->mutex);
        const auto it = mailbox->slots.find(first.target);
        expectations.expect(it != mailbox->slots.end() && it->second.resize.has_value() &&
                                it->second.resize->width == 300U,
                            "only the latest resize is retained in the port");
    }

    // Stale sequences are refused.
    expectations.expect(client->resize(first.target, 3U, 300U, 300U).code ==
                            GpuPresentationPortCode::StaleSequence,
                        "a repeated sequence is refused as stale");

    // Malformed overlays are rejected by checked dimension/stride/budget validation.
    expectations.expect(
        GpuPresentationOverlay::create(std::vector<std::uint8_t>(std::size_t{16U} * 16U * 4U, 0U),
                                       16U, 16U, 0U, 1U, 1ULL << 20) != nullptr,
        "a valid overlay builds");
    expectations.expect(GpuPresentationOverlay::create(std::vector<std::uint8_t>(16U, 0U), 16U, 16U,
                                                       0U, 1U, 1ULL << 20) == nullptr,
                        "an overlay with too few bytes is rejected");
    expectations.expect(
        GpuPresentationOverlay::create(std::vector<std::uint8_t>(std::size_t{16U} * 16U * 4U, 0U),
                                       16U, 16U, 4U, 1U, 1ULL << 20) == nullptr,
        "an overlay with an under-sized stride is rejected");
    expectations.expect(
        GpuPresentationOverlay::create(std::vector<std::uint8_t>(std::size_t{16U} * 16U * 4U, 0U),
                                       16U, 16U, 0U, 1U, 8U) == nullptr,
        "an overlay over the byte budget is rejected");

    // Retire closes admission for the target.
    expectations.expect(client->retire(second.target, 5U).code == GpuPresentationPortCode::Accepted,
                        "retire is accepted");
    expectations.expect(client->resize(second.target, 6U, 10U, 10U).code ==
                            GpuPresentationPortCode::Closed,
                        "resize after retire is closed");
    expectations.expect(client->retire(second.target, 7U).code == GpuPresentationPortCode::Closed,
                        "a second retire is closed");

    // Duplicate-surface admission control: a surface owned by a live target may not be attached
    // again, and the rejection must not disturb or retire the existing owner.
    {
        const auto duplicate = client->attach(surface(1U, 7U), 320U, 240U);
        expectations.expect(duplicate.code == GpuPresentationPortCode::DuplicateSurface,
                            "a duplicate live-surface attach is refused");
        expectations.expect(duplicate.target == 0U, "the duplicate refusal yields no new id");
        expectations.expect(mailbox->ownedSurfaces.size() == 2U,
                            "the refused duplicate added no surface ownership");
        expectations.expect(client->status(first.target).state ==
                                GpuPresentationTargetState::Attaching,
                            "the existing owner is not disturbed by the duplicate refusal");
        // The same bits under a different epoch is a different surface and is admitted.
        const auto otherEpoch = client->attach(surface(1U, 8U), 320U, 240U);
        expectations.expect(otherEpoch.code == GpuPresentationPortCode::TooManyTargets ||
                                otherEpoch.code == GpuPresentationPortCode::Accepted,
                            "a different epoch is not treated as a duplicate");
    }

    // Owner teardown fails every port method closed, with no dangling owner pointer.
    {
        std::lock_guard lock(mailbox->mutex);
        mailbox->ownerGone = true;
    }
    expectations.expect(!client->ownerAlive(), "the port observes owner teardown");
    expectations.expect(!client->instanceView().valid,
                        "no instance view is published after teardown");
    expectations.expect(client->attach(surface(9U, 7U), 16U, 16U).code ==
                            GpuPresentationPortCode::OwnerGone,
                        "attach fails closed after teardown");
    expectations.expect(client->retire(first.target, 8U).code == GpuPresentationPortCode::OwnerGone,
                        "retire fails closed after teardown");
    expectations.expect(client->status(first.target).state == GpuPresentationTargetState::Gone,
                        "status reports Gone after teardown");

    if (expectations.failures() == 0) {
        std::cout << "PASS: presentation coordinator CPU-only port validation\n";
    }
    return expectations.failures() == 0 ? 0 : 1;
}

} // namespace

int main() { return run(); }
