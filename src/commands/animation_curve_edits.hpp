#pragma once

// Batch keyframe edits shared by the animation operations: stage whole copies of the addressed
// curves, edit them, then publish them all or nothing (final keys normalized to canonical Linear).

#include <bloom/commands/animation_operations.hpp>
#include <bloom/document/project.hpp>

#include <map>
#include <vector>

namespace bloom::commands::detail {

using CurveEdits = std::map<document::AnimationCurveId, document::AnimationCurveRecord>;

// Copies every curve the addresses name into `edits`, refusing a duplicate or missing key.
[[nodiscard]] OperationResult stageKeys(const document::Composition& composition,
                                        const std::vector<KeyframeAddress>& keys,
                                        CurveEdits& edits);
// Publishes the staged curves atomically; NoChange when every staged curve is unchanged.
[[nodiscard]] OperationResult publishCurves(document::Composition& composition, CurveEdits edits);

} // namespace bloom::commands::detail
