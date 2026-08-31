#pragma once

#include <cstdint>
#include <memory>

// docs/architecture/frame-output.md "Non-Blocking Execution": "Attempt resource admission computes
// and reserves the checked retained bytes needed through the approval decision ... Approved-job
// admission transactionally expands that reservation to the checked peak across retained attempt
// products, prepared display/output pixels, encoder scratch, and verification chunks; it neither
// double-charges shared retained storage nor grants display mapping a hidden second allowance ...
// Admission reserves from the service allowance before work starts and releases on every terminal
// path." and the version 1 export-limits table's "one export job's aggregate Bloom-host resident
// allowance" (2 GiB) and "all concurrent export jobs' aggregate ... allowance" (4 GiB).
//
// bloom::output::ExportResourceLedgerV1 is the shared service-wide allowance (one instance owned by
// the application/test caller, analogous to bloom::host::PublicationCoordinator/
// bloom::platform::StagedArtifactCoordinator being shared singletons). bloom::output::
// ExportResourceReservationV1 is one export's own checked-bytes charge against it: created at
// attempt-admission time with the retained-product bytes, transactionally expand()ed at job-
// admission time to the checked peak, and released (in full) exactly once, on destruction --
// whichever terminal path (dismissal, supersession, cancellation, failure, or successful
// publication) drops the last shared_ptr to it. Reservation state lives in a heap block reached
// through a shared_ptr so a caller can hold a `const ExportResourceReservationV1&`/
// `std::shared_ptr<const ...>` (as OutputAnalysisAttemptV1 does) while still calling expand()
// through the pointee's own non-const method -- only the pointer, not the pointee, is const.
namespace bloom::output {

inline constexpr std::uint64_t kOutputExportJobResidentAllowanceV1 = 2'147'483'648ULL;
inline constexpr std::uint64_t kOutputExportConcurrentResidentAllowanceV1 = 4'294'967'296ULL;

enum class ExportResourceAdmissionStatusV1 : std::uint8_t {
    Reserved,
    ArithmeticOverflow,
    JobAllowanceExceeded,
    ServiceAllowanceExceeded,
};

namespace detail {
struct ExportResourceLedgerState;
}

class ExportResourceReservationV1;

// The shared service-wide allowance. Move-only (holds the shared counter state); a caller keeps
// one instance alive for the lifetime of the application/test and passes it by reference to every
// attempt/job admission call.
class ExportResourceLedgerV1 final {
  public:
    explicit ExportResourceLedgerV1(
        std::uint64_t concurrentAllowance = kOutputExportConcurrentResidentAllowanceV1) noexcept;
    ExportResourceLedgerV1(const ExportResourceLedgerV1&) = delete;
    ExportResourceLedgerV1& operator=(const ExportResourceLedgerV1&) = delete;
    ExportResourceLedgerV1(ExportResourceLedgerV1&&) = delete;
    ExportResourceLedgerV1& operator=(ExportResourceLedgerV1&&) = delete;
    ~ExportResourceLedgerV1() = default;

    // Reserves `bytes` against both the per-job cap (`kOutputExportJobResidentAllowanceV1` unless
    // overridden) and this ledger's shared concurrent cap. Returns a live reservation on success;
    // nullptr and a status on refusal (nothing is charged on refusal).
    [[nodiscard]] std::shared_ptr<ExportResourceReservationV1>
    reserve(std::uint64_t bytes, ExportResourceAdmissionStatusV1& status,
            std::uint64_t jobAllowance = kOutputExportJobResidentAllowanceV1) noexcept;

    [[nodiscard]] std::uint64_t chargedBytes() const noexcept;
    [[nodiscard]] std::uint64_t concurrentAllowance() const noexcept;

  private:
    friend class ExportResourceReservationV1;

    std::shared_ptr<detail::ExportResourceLedgerState> state_;
};

class ExportResourceReservationV1 final {
  public:
    ~ExportResourceReservationV1();

    [[nodiscard]] std::uint64_t chargedBytes() const noexcept { return chargedBytes_; }
    [[nodiscard]] std::uint64_t jobAllowance() const noexcept { return jobAllowance_; }

    // Transactionally raises this reservation's own charge to `newTotalBytes` (never lowers it;
    // a `newTotalBytes` at or below the current charge is a no-op success -- "neither
    // double-charges shared retained storage"). Checks the same per-job and shared-ledger caps the
    // initial reserve() did, charging (and refunding on refusal) only the incremental delta.
    [[nodiscard]] ExportResourceAdmissionStatusV1 expand(std::uint64_t newTotalBytes) noexcept;

  private:
    friend class ExportResourceLedgerV1;

    ExportResourceReservationV1(std::shared_ptr<detail::ExportResourceLedgerState> ledger,
                                std::uint64_t initialBytes, std::uint64_t jobAllowance) noexcept;

    std::shared_ptr<detail::ExportResourceLedgerState> ledger_;
    std::uint64_t chargedBytes_ = 0;
    std::uint64_t jobAllowance_ = 0;
};

} // namespace bloom::output
