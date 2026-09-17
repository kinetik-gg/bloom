#pragma once
#include <bloom/media/provider/protocol.hpp>
namespace bloom::media::fake {
[[nodiscard]] provider::Handshake handshake();
[[nodiscard]] provider::Payload call(const provider::CallRequest& request);
} // namespace bloom::media::fake
