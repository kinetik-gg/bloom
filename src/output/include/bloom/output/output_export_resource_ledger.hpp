#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <utility>

namespace bloom::runtime {
class MemoryBudgetLedger;
class GpuProcessingPhasePolicy;
class CancellationToken;
}
namespace bloom::render {
class GpuProcessingPhaseLease;
}

// docs/architecture/frame-output.md "Non-Blocking Execution": attempt admission retains the
// checked process/output products through approval. Publication claims independently retain their
// incremental scratch under that same ledger/root until each writer/verifier exits. The legacy
// 2 GiB job and 4 GiB service allowances remain defaults for unrooted callers; an injected
// processing root supplies sampled admission for rooted callers.
//
// bloom::output::ExportResourceLedgerV1 is the shared service-wide allowance (one instance owned by
// the application/test caller, analogous to bloom::host::PublicationCoordinator/
// bloom::platform::StagedArtifactCoordinator being shared singletons). bloom::output::
// ExportResourceReservationV1 retains an attempt's checked products until dismissal, supersession,
// cancellation, failure, or successful publication drops the last shared owner. Each publication
// obtains its own same-ledger/root scratch reservation, so concurrent claims add and sequential
// claims release. The facade can die while live reservations keep its accounting state alive.
namespace bloom::output {

using ExportCapacityRefreshV1 = std::function<void(const runtime::CancellationToken&)>;

inline constexpr std::uint64_t kOutputExportJobResidentAllowanceV1 = 2'147'483'648ULL;
inline constexpr std::uint64_t kOutputExportConcurrentResidentAllowanceV1 = 4'294'967'296ULL;

enum class ExportResourceAdmissionStatusV1 : std::uint8_t {
    Reserved,
    ArithmeticOverflow,
    JobAllowanceExceeded,
    ServiceAllowanceExceeded,
    CapacityRefreshFailed,
};

namespace detail {
struct ExportResourceLedgerState;
struct RootedExportReservationState;
}

class ExportResourceReservationV1;

// The shared service-wide accounting facade (holds the shared counter state); a caller keeps
// one instance alive for the lifetime of the application/test and passes it by reference to every
// attempt/job admission call.
//
// A rooted caller injects the processing policy; its new host claims are admitted and retained
// under that root until the reservation dies. An explicit constructor allowance still caps all
// calls. The implicit legacy service cap applies only to unrooted calls.
class ExportResourceLedgerV1 final {
  public:
    ExportResourceLedgerV1() noexcept;
    explicit ExportResourceLedgerV1(std::uint64_t concurrentAllowance) noexcept;
    // Compatibility constructor for callers with an isolated memory ledger. Export claims now
    // enter the injected processing root directly; this ledger no longer registers a second
    // cache/trim participant against process memory.
    explicit ExportResourceLedgerV1(runtime::MemoryBudgetLedger&) noexcept;
    ExportResourceLedgerV1(runtime::MemoryBudgetLedger&,
                           std::uint64_t concurrentAllowance) noexcept;
    ExportResourceLedgerV1(const ExportResourceLedgerV1&) = delete;
    ExportResourceLedgerV1& operator=(const ExportResourceLedgerV1&) = delete;
    ExportResourceLedgerV1(ExportResourceLedgerV1&&) = delete;
    ExportResourceLedgerV1& operator=(ExportResourceLedgerV1&&) = delete;
    ~ExportResourceLedgerV1() = default;

    // Reserves `bytes` against explicit caps and, when injected, the sampled processing root.
    // `alreadyRootedBytes` is only for verified published backing held by the same root; the
    // caller must prove that identity before passing it. Unrooted callers use the legacy implicit
    // per-job and service caps. Returns nullptr and a status on refusal with no charge.
    [[nodiscard]] std::shared_ptr<ExportResourceReservationV1>
    reserve(std::uint64_t bytes, ExportResourceAdmissionStatusV1& status,
            std::optional<std::uint64_t> jobAllowance = std::nullopt,
            std::shared_ptr<runtime::GpuProcessingPhasePolicy> processingPolicy = {},
            std::uint64_t alreadyRootedBytes = 0,
            ExportCapacityRefreshV1 capacityRefresh = {}) noexcept;

    [[nodiscard]] std::uint64_t chargedBytes() const noexcept;
    [[nodiscard]] std::uint64_t concurrentAllowance() const noexcept;

  private:
    friend class ExportResourceReservationV1;
    [[nodiscard]] std::shared_ptr<ExportResourceReservationV1>
    reserveWithFixedHostBacking(std::uint64_t bytes, ExportResourceAdmissionStatusV1& status,
                                std::optional<std::uint64_t> jobAllowance,
                                std::shared_ptr<runtime::GpuProcessingPhasePolicy> processingPolicy,
                                std::uint64_t alreadyRootedBytes,
                                ExportCapacityRefreshV1 capacityRefresh,
                                std::uint64_t fixedHostBackingBytes) noexcept;
    explicit ExportResourceLedgerV1(
        std::shared_ptr<detail::ExportResourceLedgerState> state) noexcept
        : state_(std::move(state)) {}

    std::shared_ptr<detail::ExportResourceLedgerState> state_;
};

class ExportResourceReservationV1 final {
  public:
    ~ExportResourceReservationV1();

    [[nodiscard]] std::uint64_t chargedBytes() const noexcept;
    [[nodiscard]] std::optional<std::uint64_t> jobAllowance() const noexcept {
        return jobAllowance_;
    }
    [[nodiscard]] std::uint64_t alreadyRootedBytes() const noexcept;
    [[nodiscard]] std::shared_ptr<runtime::GpuProcessingPhasePolicy>
    processingPolicy() const noexcept;
    // Optional retained worker-side sampler. A delayed publication calls it with that task's
    // cancellation token immediately before its scratch claim enters the root.
    [[nodiscard]] const ExportCapacityRefreshV1& capacityRefresh() const noexcept {
        return capacityRefresh_;
    }
    // Refresh a retained provider-owned root on the worker before a later expansion. Injected
    // application roots can omit the callback and must still present a fresh sampled root.
    [[nodiscard]] ExportResourceAdmissionStatusV1
    refreshCapacity(const runtime::CancellationToken& cancellation) const noexcept;
    // A publication owns its own incremental scratch claim. Concurrent publications of the same
    // attempt add their simultaneous claims; completed publications release them independently.
    // The parent's explicit per-job cap still applies to retained bytes plus this scratch.
    // Refresh exceptions produce CapacityRefreshFailed without charging scratch.
    [[nodiscard]] std::shared_ptr<ExportResourceReservationV1>
    reserveScratch(std::uint64_t additionalBytes, ExportResourceAdmissionStatusV1& status,
                   const runtime::CancellationToken& cancellation) const noexcept;

    // A retained host allocation gets its own fixed phase claim before construction. The
    // allocation can then transfer that claim to its real backing without an additive claim.
    // The caller must keep this reservation until every owner of the adopted backing retires.
    [[nodiscard]] std::shared_ptr<ExportResourceReservationV1>
    reserveOwnedHostBacking(std::uint64_t bytes, ExportResourceAdmissionStatusV1& status,
                            const runtime::CancellationToken& cancellation) const noexcept;
    [[nodiscard]] std::shared_ptr<render::GpuProcessingPhaseLease> ownedHostPhase() const noexcept;

    // Transactionally raises this reservation's own charge to `newTotalBytes` (never lowers it;
    // a `newTotalBytes` at or below the current charge is a no-op success -- "neither
    // double-charges shared retained storage"). Checks the same explicit/legacy caps as reserve()
    // and retains only the incremental delta under the injected root.
    [[nodiscard]] ExportResourceAdmissionStatusV1 expand(std::uint64_t newTotalBytes) noexcept;

  private:
    friend class ExportResourceLedgerV1;

    ExportResourceReservationV1(std::shared_ptr<detail::ExportResourceLedgerState> ledger,
                                std::uint64_t initialBytes,
                                std::optional<std::uint64_t> jobAllowance,
                                bool participatesInServiceCap,
                                std::unique_ptr<detail::RootedExportReservationState>,
                                ExportCapacityRefreshV1 capacityRefresh) noexcept;

    std::shared_ptr<detail::ExportResourceLedgerState> ledger_;
    mutable std::mutex mutex_;
    std::uint64_t chargedBytes_ = 0;
    std::optional<std::uint64_t> jobAllowance_;
    bool participatesInServiceCap_ = true;
    std::unique_ptr<detail::RootedExportReservationState> rooted_;
    ExportCapacityRefreshV1 capacityRefresh_;
};

} // namespace bloom::output
