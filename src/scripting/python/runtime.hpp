#pragma once

#include <bloom/runtime/task_scheduler.hpp>
#include <bloom/scripting/session.hpp>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <string_view>

namespace bloom::scripting::python {

class NativeHost;
struct PythonContext {
    std::uint64_t project = 0, composition = 0;
    std::vector<std::uint64_t> selection;
    core::RationalTime time;
};

struct ExecutionResult final {
    std::string output;
    bool succeeded = false;
    bool incomplete = false;
};

// One isolated interpreter owned by the application's scripting thread. Native work releases
// the GIL; authoring operations may be queued by the host adapter.
class EmbeddedPython final {
  public:
    explicit EmbeddedPython(const std::filesystem::path& executable = {});
    ~EmbeddedPython();
    EmbeddedPython(const EmbeddedPython&) = delete;
    EmbeddedPython& operator=(const EmbeddedPython&) = delete;

    [[nodiscard]] ExecutionResult execute(std::string_view source, bool interactive = false);
    [[nodiscard]] int runFile(const std::filesystem::path& path);
    [[nodiscard]] int interactive();
    void bind(std::shared_ptr<NativeHost> host);
    void bindLive(std::shared_ptr<Session> session, std::function<PythonContext()> context,
                  std::function<bool()> cancelled);
    void requestCancellation();
    [[nodiscard]] std::vector<runtime::TaskSnapshot> tasks() const;

  private:
    struct State;
    std::unique_ptr<State> state_;
};

} // namespace bloom::scripting::python
