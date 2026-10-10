#pragma once
// Production-owned observation seam. Only the test-only library defines its setter; shipping
// targets cannot select a fault. A forced timeout never proves native retirement.
#include <atomic>
#include <cstdint>
namespace bloom::render::display_transfer_detail {
enum class TransferFault : std::uint8_t {
    None,
    ForceFenceTimeout,
    CommandSetup,
    Submit,
    Invalidate,
};
[[nodiscard]] std::atomic<std::uint8_t>& transferFault() noexcept;
void setTransferFaultForTest(TransferFault fault) noexcept;
} // namespace bloom::render::display_transfer_detail
