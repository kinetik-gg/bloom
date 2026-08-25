#include "staged_artifact_platform.hpp"

namespace bloom::platform::detail {

PlatformCoordinatorResult
createPlatformStagedArtifactCoordinator(const StagedArtifactConfig&) noexcept {
    return PlatformCoordinatorResult(StagedArtifactError::UnsupportedPlatform);
}

} // namespace bloom::platform::detail
