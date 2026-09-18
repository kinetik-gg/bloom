#pragma once

#include <bloom/platform/process_supervisor.hpp>
#include <filesystem>
#include <string>

namespace bloom::media::provider {

// The worker always receives its private library directory after an optional, verified Cisco
// directory. The latter shadows the shim only when the host has verified the exact Cisco bytes.
void configureFfmpegWorkerEnvironment(platform::ProcessOptions& options,
                                      const std::string& executable,
                                      std::filesystem::path verifiedOpenH264 = {});

} // namespace bloom::media::provider
