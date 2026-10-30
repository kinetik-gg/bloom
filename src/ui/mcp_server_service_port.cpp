#include <bloom/ui/mcp_server_service_port.hpp>

namespace bloom::ui {

McpServerServicePort::McpServerServicePort(mcp::McpServerService& service)
    : service_(service), subscription_(service.subscribe()) {}

McpServerServicePort::~McpServerServicePort() { subscription_.close(); }

std::shared_ptr<const mcp::McpServerSnapshot> McpServerServicePort::snapshot() const {
    return service_.snapshot();
}
mcp::McpServerEventBatch McpServerServicePort::events(const std::uint64_t afterSequence) const {
    return service_.events(afterSequence);
}
mcp::McpActivityBatch McpServerServicePort::activity(const std::uint64_t afterId) const {
    return service_.activity(afterId);
}
std::uint64_t McpServerServicePort::serial() const { return subscription_.serial(); }
std::optional<std::uint64_t>
McpServerServicePort::waitForChange(const std::uint64_t observed, const std::stop_token& stop) const {
    return subscription_.waitForChange(observed, stop);
}
void McpServerServicePort::pump() { service_.pump(); }
mcp::McpCommandResult McpServerServicePort::start() { return service_.start(); }
mcp::McpCommandResult McpServerServicePort::stop() { return service_.stop(); }
mcp::McpCommandResult McpServerServicePort::setPort(const std::uint32_t port) {
    return service_.setPort(port);
}
mcp::McpCommandResult McpServerServicePort::setAutoStart(const bool enabled) {
    return service_.setAutoStart(enabled);
}
mcp::McpCommandResult McpServerServicePort::regenerateToken() { return service_.regenerateToken(); }
mcp::McpTokenReveal McpServerServicePort::revealToken() { return service_.revealToken(); }
mcp::McpClientConfig
McpServerServicePort::clientConfig(const mcp::McpClientKind kind,
                                   const mcp::McpTokenDisclosure disclosure) const {
    return service_.clientConfig(kind, disclosure);
}

} // namespace bloom::ui
