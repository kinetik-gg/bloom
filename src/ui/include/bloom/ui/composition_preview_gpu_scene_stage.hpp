#pragma once

#include <bloom/runtime/prepared_gpu_scene.hpp>
#include <bloom/runtime/preview_gpu_scene_stage.hpp>
#include <bloom/runtime/qualified_display_processor_provider.hpp>
#include <bloom/runtime/snapshot_compiler.hpp>
#include <bloom/ui/composition_plan_cache.hpp>

// UI-owned factory for the GPU-scene stage. It lives in bloom::ui because the compiled-plan cache
// is a UI-owned handle (CompiledPlanCacheHandle), exactly as makeCompositionPreviewCpuStage does;
// the function it returns is a pure runtime type with no Qt or UI dependency.
//
// `builder` is CONSTRUCTED BY THE CALLER and injected. That single object owns the coverage cache
// and (once the media slice lands) the evaluator-shared media context: a media-capable builder
// substitutes its decoder through its own constructor, so this stage never invents an alternative
// decoder, never touches media I/O itself, and cannot drift from the evaluator's semantics. The
// builder reference must outlive the returned function, exactly as the compiler and provider must.
namespace bloom::ui {

// Compile -> prepare the immutable GPU scene -> select a CPU display processor, reproducing
// makeCompositionPreviewCpuStage's compile/validation/selection half: the same request validation,
// the same fail-closed check for a permanently failed qualified provider, the same
// Unsupported/Cancelled/Failed returns, the same Pending-reference window, the same
// ACES/non-default-view/viewAdjust selection, and the same plan identity through `planCache`.
//
// A compiled graph outside the prepared GPU subset is returned as
// PreviewGpuSceneStageStatus::UnsupportedGpuSubset (a succeeded outcome) so the caller takes the
// full original CPU path; a semantic compile rejection is the distinct
// PreviewGpuSceneStageStatus::Unsupported.
[[nodiscard]] runtime::PreviewGpuSceneStageFunction makeCompositionPreviewGpuSceneStage(
    const runtime::SnapshotCompiler& compiler, const runtime::CpuGpuSceneBuilder& builder,
    const runtime::QualifiedDisplayProcessorProvider& qualifiedProcessorProvider,
    CompiledPlanCacheHandle planCache = nullptr);

} // namespace bloom::ui
