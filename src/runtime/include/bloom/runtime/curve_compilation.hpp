#pragma once

#include <bloom/document/animation.hpp>
#include <bloom/runtime/compiled_plan.hpp>

namespace bloom::runtime {

// Pure document-curve -> compiled-curve conversion (issue #86, task E1), extracted
// behavior-preserving from snapshot_compiler.cpp's own per-curve lowering
// (compileReachableCurves() in snapshot_compiler_lowering.ipp), which now calls these instead of
// duplicating the conversion. Two homes were possible -- animation_sampling.hpp (already public,
// already includes compiled_plan.hpp) or a dedicated header. This lives here, in
// bloom_runtime_compiler (the library that already owns document -> compiled-plan lowering),
// rather than in animation_sampling.hpp/bloom_runtime_evaluator: sampleAnimationCurve() consumes
// already-compiled curves and has no document-type dependency today, and pulling
// bloom::document::ScalarAnimationCurve/Vec2AnimationCurve into the evaluator library would blur
// that boundary. CompositionSession (src/ui) already links bloom_runtime_compiler privately for
// exactly this kind of use.
//
// No cancellation/checkpoint awareness: that is the compiler's own per-curve loop concern
// (compileReachableCurves() checks cancellation once before and after each curve, matching every
// other per-item loop in that file), not part of this pure, allocating conversion.
[[nodiscard]] CompiledScalarCurve
compileAnimationCurve(const document::ScalarAnimationCurve& curve);
[[nodiscard]] CompiledVec2Curve compileAnimationCurve(const document::Vec2AnimationCurve& curve);

} // namespace bloom::runtime
