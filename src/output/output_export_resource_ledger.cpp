#include <bloom/output/output_export_resource_ledger.hpp>
#include <bloom/render/processing_host_storage.hpp>
#include <bloom/runtime/cancellation.hpp>
#include <bloom/runtime/gpu_processing_phase_policy.hpp>

#include <limits>
#include <mutex>
#include <utility>
#include <vector>

namespace bloom::output::detail {

struct ExportResourceLedgerState final {
    explicit ExportResourceLedgerState(const std::uint64_t allowance, const bool explicitLimit)
        : concurrentAllowance(allowance), explicitConcurrentLimit(explicitLimit) {}

    std::mutex mutex;
    std::uint64_t concurrentAllowance = 0;
    bool explicitConcurrentLimit = false;
    std::uint64_t chargedBytes = 0;
    // The implicit legacy cap applies only to unrooted calls. An explicit constructor cap also
    // counts rooted calls; their actual headroom is admitted by the common processing root.
    std::uint64_t cappedBytes = 0;

    [[nodiscard]] bool chargeLocked(const std::uint64_t delta, const bool capped) noexcept {
        if (delta > std::numeric_limits<std::uint64_t>::max() - chargedBytes)
            return false;
        if (capped && (delta > std::numeric_limits<std::uint64_t>::max() - cappedBytes ||
                       cappedBytes + delta > concurrentAllowance))
            return false;
        chargedBytes += delta;
        if (capped)
            cappedBytes += delta;
        return true;
    }

    void releaseLocked(const std::uint64_t delta, const bool capped) noexcept {
        chargedBytes -= delta;
        if (capped)
            cappedBytes -= delta;
    }
};

struct RootedExportReservationState final {
    std::shared_ptr<runtime::GpuProcessingPhasePolicy> policy;
    std::shared_ptr<render::GpuProcessingPhaseLease> phase;
    std::vector<render::ProcessingHostStorageToken> claims;
    std::uint64_t alreadyRootedBytes = 0;
};

} // namespace bloom::output::detail

namespace bloom::output {

ExportResourceLedgerV1::ExportResourceLedgerV1() noexcept
    : state_(std::make_shared<detail::ExportResourceLedgerState>(
          kOutputExportConcurrentResidentAllowanceV1, false)) {}

ExportResourceLedgerV1::ExportResourceLedgerV1(const std::uint64_t concurrentAllowance) noexcept
    : state_(std::make_shared<detail::ExportResourceLedgerState>(concurrentAllowance, true)) {}

ExportResourceLedgerV1::ExportResourceLedgerV1(runtime::MemoryBudgetLedger&) noexcept
    : ExportResourceLedgerV1() {}

ExportResourceLedgerV1::ExportResourceLedgerV1(runtime::MemoryBudgetLedger&,
                                               const std::uint64_t concurrentAllowance) noexcept
    : ExportResourceLedgerV1(concurrentAllowance) {}

std::shared_ptr<ExportResourceReservationV1>
ExportResourceLedgerV1::reserve(const std::uint64_t bytes, ExportResourceAdmissionStatusV1& status,
                                const std::optional<std::uint64_t> jobAllowance,
                                std::shared_ptr<runtime::GpuProcessingPhasePolicy> processingPolicy,
                                const std::uint64_t alreadyRootedBytes,
                                ExportCapacityRefreshV1 capacityRefresh) noexcept {
    if (alreadyRootedBytes > bytes || (alreadyRootedBytes != 0 && !processingPolicy)) {
        status = ExportResourceAdmissionStatusV1::ArithmeticOverflow;
        return {};
    }
    const std::optional<std::uint64_t> effectiveJobAllowance =
        jobAllowance       ? jobAllowance
        : processingPolicy ? std::nullopt
                           : std::optional{kOutputExportJobResidentAllowanceV1};
    if (effectiveJobAllowance && bytes > *effectiveJobAllowance) {
        status = ExportResourceAdmissionStatusV1::JobAllowanceExceeded;
        return {};
    }

    const bool capped = !processingPolicy || state_->explicitConcurrentLimit;
    try {
        std::unique_ptr<detail::RootedExportReservationState> rooted;
        if (processingPolicy) {
            rooted = std::make_unique<detail::RootedExportReservationState>();
            rooted->policy = std::move(processingPolicy);
            rooted->alreadyRootedBytes = alreadyRootedBytes;
            auto phase =
                rooted->policy->reserve({.output = runtime::GpuProcessingPhaseOutput::FloatFinal,
                                         .pixels = 0,
                                         .metadataBytes = 0,
                                         .sourceFree = true,
                                         .native = false});
            if (phase.status != render::GpuProcessingPhaseAdmission::Accepted || !phase.lease) {
                status = ExportResourceAdmissionStatusV1::ServiceAllowanceExceeded;
                return {};
            }
            rooted->phase = std::move(phase.lease);
            const auto unowned = bytes - alreadyRootedBytes;
            if (unowned != 0) {
                auto claim = render::ProcessingHostStorageToken::reserve(rooted->phase, unowned);
                if (!claim.token) {
                    status = ExportResourceAdmissionStatusV1::ServiceAllowanceExceeded;
                    return {};
                }
                rooted->claims.push_back(std::move(*claim.token));
            }
        }
        // Build the shared owner before charging the ledger. If control-block allocation throws,
        // its raw-object cleanup owns zero charge and only releases the rooted phase/tokens.
        auto reservation = std::shared_ptr<ExportResourceReservationV1>(
            new ExportResourceReservationV1(state_, 0, effectiveJobAllowance, capped,
                                            std::move(rooted), std::move(capacityRefresh)));
        {
            const std::lock_guard lock(state_->mutex);
            if (!state_->chargeLocked(bytes, capped)) {
                status = ExportResourceAdmissionStatusV1::ServiceAllowanceExceeded;
                return {};
            }
            reservation->chargedBytes_ = bytes;
        }
        status = ExportResourceAdmissionStatusV1::Reserved;
        return reservation;
    } catch (const std::bad_alloc&) {
        status = ExportResourceAdmissionStatusV1::ServiceAllowanceExceeded;
        return {};
    }
}

std::uint64_t ExportResourceLedgerV1::chargedBytes() const noexcept {
    const std::lock_guard lock(state_->mutex);
    return state_->chargedBytes;
}

std::uint64_t ExportResourceLedgerV1::concurrentAllowance() const noexcept {
    const std::lock_guard lock(state_->mutex);
    return state_->concurrentAllowance;
}

ExportResourceReservationV1::ExportResourceReservationV1(
    std::shared_ptr<detail::ExportResourceLedgerState> ledger, const std::uint64_t initialBytes,
    const std::optional<std::uint64_t> jobAllowance, const bool participatesInServiceCap,
    std::unique_ptr<detail::RootedExportReservationState> rooted,
    ExportCapacityRefreshV1 capacityRefresh) noexcept
    : ledger_(std::move(ledger)), chargedBytes_(initialBytes), jobAllowance_(jobAllowance),
      participatesInServiceCap_(participatesInServiceCap), rooted_(std::move(rooted)),
      capacityRefresh_(std::move(capacityRefresh)) {}

ExportResourceReservationV1::~ExportResourceReservationV1() {
    // Hold the service charge until the rooted phase and every scratch claim have retired.
    rooted_.reset();
    if (ledger_) {
        const std::lock_guard lock(ledger_->mutex);
        ledger_->releaseLocked(chargedBytes_, participatesInServiceCap_);
    }
}

std::uint64_t ExportResourceReservationV1::chargedBytes() const noexcept {
    const std::lock_guard lock(mutex_);
    return chargedBytes_;
}

std::uint64_t ExportResourceReservationV1::alreadyRootedBytes() const noexcept {
    return rooted_ ? rooted_->alreadyRootedBytes : 0;
}

std::shared_ptr<runtime::GpuProcessingPhasePolicy>
ExportResourceReservationV1::processingPolicy() const noexcept {
    return rooted_ ? rooted_->policy : nullptr;
}

ExportResourceAdmissionStatusV1 ExportResourceReservationV1::refreshCapacity(
    const runtime::CancellationToken& cancellation) const noexcept {
    if (capacityRefresh_) {
        try {
            capacityRefresh_(cancellation);
        } catch (...) {
            return ExportResourceAdmissionStatusV1::CapacityRefreshFailed;
        }
    }
    return cancellation.isCancellationRequested()
               ? ExportResourceAdmissionStatusV1::CapacityRefreshFailed
               : ExportResourceAdmissionStatusV1::Reserved;
}

std::shared_ptr<ExportResourceReservationV1> ExportResourceReservationV1::reserveScratch(
    const std::uint64_t additionalBytes, ExportResourceAdmissionStatusV1& status,
    const runtime::CancellationToken& cancellation) const noexcept {
    status = refreshCapacity(cancellation);
    if (status != ExportResourceAdmissionStatusV1::Reserved)
        return {};
    const std::lock_guard reservationLock(mutex_);
    std::optional<std::uint64_t> remainingJobAllowance;
    if (jobAllowance_) {
        if (chargedBytes_ > *jobAllowance_ || additionalBytes > *jobAllowance_ - chargedBytes_) {
            status = ExportResourceAdmissionStatusV1::JobAllowanceExceeded;
            return {};
        }
        remainingJobAllowance = *jobAllowance_ - chargedBytes_;
    }
    ExportResourceLedgerV1 facade(ledger_);
    return facade.reserve(additionalBytes, status, remainingJobAllowance,
                          rooted_ ? rooted_->policy : nullptr);
}

ExportResourceAdmissionStatusV1
ExportResourceReservationV1::expand(const std::uint64_t newTotalBytes) noexcept {
    const std::lock_guard reservationLock(mutex_);
    if (newTotalBytes <= chargedBytes_)
        return ExportResourceAdmissionStatusV1::Reserved;
    if (jobAllowance_ && newTotalBytes > *jobAllowance_)
        return ExportResourceAdmissionStatusV1::JobAllowanceExceeded;
    const auto delta = newTotalBytes - chargedBytes_;
    std::optional<render::ProcessingHostStorageToken> claim;
    if (rooted_) {
        try {
            rooted_->claims.reserve(rooted_->claims.size() + 1);
        } catch (const std::bad_alloc&) {
            return ExportResourceAdmissionStatusV1::ServiceAllowanceExceeded;
        }
        auto admitted = render::ProcessingHostStorageToken::reserve(rooted_->phase, delta);
        if (!admitted.token)
            return ExportResourceAdmissionStatusV1::ServiceAllowanceExceeded;
        claim = std::move(*admitted.token);
    }
    {
        const std::lock_guard lock(ledger_->mutex);
        if (!ledger_->chargeLocked(delta, participatesInServiceCap_))
            return ExportResourceAdmissionStatusV1::ServiceAllowanceExceeded;
        chargedBytes_ = newTotalBytes;
    }
    if (claim)
        rooted_->claims.push_back(std::move(*claim));
    return ExportResourceAdmissionStatusV1::Reserved;
}

} // namespace bloom::output
