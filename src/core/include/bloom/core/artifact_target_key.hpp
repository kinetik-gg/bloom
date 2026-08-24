#pragma once

#include <bloom/core/id.hpp>

namespace bloom::core {

struct ArtifactTargetKeyTag;

// Opaque process-local identity produced by platform target preflight. It deliberately carries
// neither a display path nor filesystem comparison semantics across module boundaries.
using ArtifactTargetKey = Id<ArtifactTargetKeyTag>;

} // namespace bloom::core
