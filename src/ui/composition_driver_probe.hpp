#pragma once
#include <bloom/runtime/snapshot_compiler.hpp>
#include <span>
namespace bloom::ui {
// A private, worker-only evaluation copy. Original node/parameter IDs and authored values stay
// intact; ordinary built-in image probes make detached value branches reachable to the compiler.
[[nodiscard]] runtime::SnapshotCompileResult
compileDriverProbe(const document::Snapshot& snapshot, document::CompositionId composition,
                   std::span<const document::ParameterId> parameters,
                   const runtime::CancellationToken& cancellation);
} // namespace bloom::ui
