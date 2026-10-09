#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>

namespace bloom::render {
namespace detail {
struct PackedHostRetirementState;
struct PackedHostRetirementNode {
    PackedHostRetirementNode* next = nullptr;
    void (*destroy)(PackedHostRetirementNode*) noexcept = nullptr;
};
struct PackedHostRetirementTestAccess;
} // namespace detail

struct PackedHostRetirementSnapshot final {
    std::size_t allocations = 0;
    std::size_t queuedAllocations = 0;
    bool starting = false;
    bool started = false;
    bool closing = false;
    bool exitAcknowledged = false;
    bool collected = false;
};

// Explicit composition-root lifecycle owner. Creation is inert; prepare() starts the worker
// off the UI thread. The caller must retain this owner until shutdown, actual exit and collect.
// Destruction with a live worker is a lifecycle-contract violation, never an implicit UI join.
class PackedHostRetirementOwner final
    : public std::enable_shared_from_this<PackedHostRetirementOwner> {
  public:
    [[nodiscard]] static std::shared_ptr<PackedHostRetirementOwner> create();
    ~PackedHostRetirementOwner();
    PackedHostRetirementOwner(const PackedHostRetirementOwner&) = delete;
    PackedHostRetirementOwner& operator=(const PackedHostRetirementOwner&) = delete;
    // Worker-only. Failure leaves no worker/registration; a shutdown race refuses new work.
    [[nodiscard]] bool prepare() noexcept;
    void beginShutdown() noexcept;
    [[nodiscard]] bool exitAcknowledged() const noexcept;
    // Returns false unless shutdown and the at-thread-exit future are ready. The OS join is
    // permitted only off the active UI event loop (headless worker or post-eventloop teardown).
    // UI shutdown polls exitAcknowledged() and retains this owner for subsequent collection.
    [[nodiscard]] bool collect() noexcept;
    [[nodiscard]] PackedHostRetirementSnapshot snapshot() const noexcept;
    [[nodiscard]] std::shared_ptr<detail::PackedHostRetirementState> producerState() const noexcept;

  private:
    PackedHostRetirementOwner();
    struct Impl;
    std::unique_ptr<Impl> impl_;
    friend struct detail::PackedHostRetirementTestAccess;
};

namespace detail {
// Each admitted allocation already contains its queue node. Registration reserves no arbitrary
// frame slot; its exact node/header memory is admitted separately under the phase's host bytes.
[[nodiscard]] bool
preparePackedHostRetirement(const std::shared_ptr<PackedHostRetirementState>&) noexcept;
[[nodiscard]] bool
packedHostRetirementAvailable(const std::shared_ptr<PackedHostRetirementState>&) noexcept;
[[nodiscard]] bool
registerPackedHostAllocation(const std::shared_ptr<PackedHostRetirementState>&) noexcept;
void releasePackedHostRegistration(const std::shared_ptr<PackedHostRetirementState>&) noexcept;
void enqueuePackedHostRetirement(const std::shared_ptr<PackedHostRetirementState>&,
                                 PackedHostRetirementNode*) noexcept;
} // namespace detail
} // namespace bloom::render
