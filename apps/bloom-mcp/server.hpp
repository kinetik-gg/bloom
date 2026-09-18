#pragma once

#include "json.hpp"
#include <bloom/scripting/facade.hpp>

#include <atomic>
#include <deque>
#include <mutex>
#include <optional>

namespace bloom::mcp {

class Server final {
  public:
    explicit Server(std::unique_ptr<scripting::Session> session);
    [[nodiscard]] std::optional<std::string> handle(std::string_view message);
    [[nodiscard]] bool interceptCancellation(std::string_view message);
    [[nodiscard]] static std::string error(yyjson_val* id, int code, std::string_view message);

  private:
    class ActiveRequest;
    std::mutex activeMutex_;
    std::string activeId_;
    std::atomic_bool cancellationRequested_ = false;
    [[nodiscard]] yyjson_mut_val* call(Json& out, std::string_view tool, yyjson_val* arguments);
    [[nodiscard]] yyjson_mut_val* query(Json& out, yyjson_val* arguments);
    [[nodiscard]] yyjson_mut_val* transact(Json& out, yyjson_val* arguments);
    [[nodiscard]] yyjson_mut_val* render(Json& out, yyjson_val* arguments, bool compositionExport);
    [[nodiscard]] yyjson_mut_val* events(Json& out, yyjson_val* arguments);
    [[nodiscard]] yyjson_mut_val* tools(Json& out);
    std::unique_ptr<scripting::Session> session_;
    scripting::Facade facade_;
    runtime::TaskScheduler scheduler_;
    std::deque<std::pair<std::uint64_t, commands::CommandEvent>> events_;
    std::uint64_t sequence_ = 0;
    scripting::Subscription subscription_;
    bool initialized_ = false;
    bool ready_ = false;
};

[[nodiscard]] yyjson_mut_val* commandResult(Json& out, const commands::CommandResult& result);
[[nodiscard]] yyjson_mut_val* parameterRecord(Json& out, const document::ParameterRecord& value);
[[nodiscard]] std::string digestFile(const std::filesystem::path& path);

} // namespace bloom::mcp
