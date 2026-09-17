#include <bloom/platform/process_supervisor.hpp>
#include <utility>
namespace bloom::platform {
struct ProcessSupervisor::State {};
ProcessSupervisor::ProcessSupervisor(std::unique_ptr<State> state) : state_(std::move(state)) {}
ProcessSupervisor::~ProcessSupervisor() = default;
ProcessResult<std::unique_ptr<ProcessSupervisor>> ProcessSupervisor::launch(const ProcessOptions&) {
    return ProcessFailure{ProcessError::Unavailable, 0};
}
ProcessResult<std::size_t> ProcessSupervisor::write(std::span<const std::byte>, ProcessDeadline,
                                                    const ProcessCancellation&) {
    return ProcessFailure{ProcessError::Unavailable, 0};
}
ProcessResult<std::size_t> ProcessSupervisor::read(std::span<std::byte>, ProcessDeadline,
                                                   const ProcessCancellation&) {
    return ProcessFailure{ProcessError::Unavailable, 0};
}
ProcessResult<int> ProcessSupervisor::finish(ProcessDeadline) {
    return ProcessFailure{ProcessError::Unavailable, 0};
}
void ProcessSupervisor::stop(std::span<const std::byte>) {}
std::int64_t ProcessSupervisor::processId() const { return -1; }
bool processWorkerBootstrap() { return false; }
} // namespace bloom::platform
