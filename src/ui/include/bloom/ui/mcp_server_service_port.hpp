#pragma once

// The desktop McpServerPort: a thin forwarding layer over the real mcp::McpServerService. Owner
// thread only for everything except snapshot/events/activity/serial/waitForChange.

#include <bloom/ui/mcp_server_adapter.hpp>

namespace bloom::ui {

class McpServerServicePort final : public McpServerPort {
  public:
    // `service` is borrowed and outlives the port.
    explicit McpServerServicePort(mcp::McpServerService& service);
    ~McpServerServicePort() override;

    [[nodiscard]] std::shared_ptr<const mcp::McpServerSnapshot> snapshot() const override;
    [[nodiscard]] mcp::McpServerEventBatch events(std::uint64_t afterSequence) const override;
    [[nodiscard]] mcp::McpActivityBatch activity(std::uint64_t afterId) const override;
    [[nodiscard]] std::uint64_t serial() const override;
    [[nodiscard]] std::optional<std::uint64_t>
    waitForChange(std::uint64_t observed, const std::stop_token& stop) const override;
    void pump() override;
    mcp::McpCommandResult start() override;
    mcp::McpCommandResult stop() override;
    mcp::McpCommandResult setPort(std::uint32_t port) override;
    mcp::McpCommandResult setAutoStart(bool enabled) override;
    mcp::McpCommandResult regenerateToken() override;
    [[nodiscard]] mcp::McpTokenReveal revealToken() override;
    [[nodiscard]] mcp::McpClientConfig
    clientConfig(mcp::McpClientKind kind, mcp::McpTokenDisclosure disclosure) const override;

  private:
    mcp::McpServerService& service_;
    mcp::McpServerSubscription subscription_;
};

} // namespace bloom::ui
