#include "gpu_display_transfer_fault.hpp"
namespace bloom::render::display_transfer_detail {
void setTransferFaultForTest(const TransferFault fault) noexcept {
    transferFault().store(static_cast<std::uint8_t>(fault));
}
} // namespace bloom::render::display_transfer_detail
