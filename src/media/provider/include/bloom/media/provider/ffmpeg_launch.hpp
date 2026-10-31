#pragma once

#include <bloom/platform/process_supervisor.hpp>
#include <filesystem>
#include <string>

namespace bloom::media::provider {

// The worker receives its private library directory. Every codec it uses is inside those
// libraries; nothing is loaded from a user or system location.
void configureFfmpegWorkerEnvironment(platform::ProcessOptions& options,
                                      const std::string& executable);

} // namespace bloom::media::provider
