#include <bloom/output/output_export_resource_ledger.hpp>

#include <limits>
#include <mutex>
#include <utility>

namespace bloom::output::detail {

struct ExportResourceLedgerState final {
    explicit ExportResourceLedgerState(const std::uint64_t allowance) noexcept
        : concurrentAllowance(allowance) {}

    std::mutex mutex;
    std::uint64_t concurrentAllowance = 0;
    std::uint64_t chargedBytes = 0;

    // Returns true and charges `delta` on success. Checked against overflow and both caps.
    [[nodiscard]] bool chargeLocked(const std::uint64_t delta) noexcept {
        if (delta > std::numeric_limits<std::uint64_t>::max() - chargedBytes) {
            return false;
        }
        if (chargedBytes + delta > concurrentAllowance) {
            return false;
        }
        chargedBytes += delta;
        return true;
    }

    void releaseLocked(const std::uint64_t delta) noexcept { chargedBytes -= delta; }
};

} // namespace bloom::output::detail

namespace bloom::output {

ExportResourceLedgerV1::ExportResourceLedgerV1(const std::uint64_t concurrentAllowance) noexcept
    : state_(std::make_shared<detail::ExportResourceLedgerState>(concurrentAllowance)) {}

std::shared_ptr<ExportResourceReservationV1>
ExportResourceLedgerV1::reserve(const std::uint64_t bytes, ExportResourceAdmissionStatusV1& status,
                                const std::uint64_t jobAllowance) noexcept {
    if (bytes > jobAllowance) {
        status = ExportResourceAdmissionStatusV1::JobAllowanceExceeded;
        return nullptr;
    }
    {
        std::lock_guard lock(state_->mutex);
        if (!state_->chargeLocked(bytes)) {
            status = ExportResourceAdmissionStatusV1::ServiceAllowanceExceeded;
            return nullptr;
        }
    }
    status = ExportResourceAdmissionStatusV1::Reserved;
    try {
        return std::shared_ptr<ExportResourceReservationV1>(
            new ExportResourceReservationV1(state_, bytes, jobAllowance));
    } catch (const std::bad_alloc&) {
        std::lock_guard lock(state_->mutex);
        state_->releaseLocked(bytes);
        status = ExportResourceAdmissionStatusV1::ServiceAllowanceExceeded;
        return nullptr;
    }
}

std::uint64_t ExportResourceLedgerV1::chargedBytes() const noexcept {
    std::lock_guard lock(state_->mutex);
    return state_->chargedBytes;
}

std::uint64_t ExportResourceLedgerV1::concurrentAllowance() const noexcept {
    return state_->concurrentAllowance;
}

ExportResourceReservationV1::ExportResourceReservationV1(
    std::shared_ptr<detail::ExportResourceLedgerState> ledger, const std::uint64_t initialBytes,
    const std::uint64_t jobAllowance) noexcept
    : ledger_(std::move(ledger)), chargedBytes_(initialBytes), jobAllowance_(jobAllowance) {}

ExportResourceReservationV1::~ExportResourceReservationV1() {
    if (ledger_ == nullptr) {
        return;
    }
    std::lock_guard lock(ledger_->mutex);
    ledger_->releaseLocked(chargedBytes_);
}

ExportResourceAdmissionStatusV1
ExportResourceReservationV1::expand(const std::uint64_t newTotalBytes) noexcept {
    if (newTotalBytes <= chargedBytes_) {
        return ExportResourceAdmissionStatusV1::Reserved;
    }
    if (newTotalBytes > jobAllowance_) {
        return ExportResourceAdmissionStatusV1::JobAllowanceExceeded;
    }
    const auto delta = newTotalBytes - chargedBytes_;
    std::lock_guard lock(ledger_->mutex);
    if (!ledger_->chargeLocked(delta)) {
        return ExportResourceAdmissionStatusV1::ServiceAllowanceExceeded;
    }
    chargedBytes_ = newTotalBytes;
    return ExportResourceAdmissionStatusV1::Reserved;
}

} // namespace bloom::output
