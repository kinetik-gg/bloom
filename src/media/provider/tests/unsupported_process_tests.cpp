#include <bloom/platform/process_supervisor.hpp>
int main() {
    const auto result = bloom::platform::ProcessSupervisor::launch({});
    const auto* failure = std::get_if<bloom::platform::ProcessFailure>(&result);
    return failure && failure->code == bloom::platform::ProcessError::Unavailable &&
                   !bloom::platform::processWorkerBootstrap()
               ? 0
               : 1;
}
