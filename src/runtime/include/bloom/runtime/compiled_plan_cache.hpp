#pragma once

#include <bloom/runtime/snapshot_compiler.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>

namespace bloom::runtime {

inline constexpr std::size_t kDefaultCompiledPlanCacheCapacity = 4;

// One compiled plan per document revision, kept so that playing or scrubbing a composition compiles
// nothing at all.
//
// This is sound because a compiled plan is TIME-INDEPENDENT: every animated parameter is a curve
// index the evaluator samples at the request time, so two requests that differ only in time compile
// to the same plan -- the same reason docs/architecture/animation-and-time.md gives for compiling a
// sequence export's range once. The key is therefore the document identity the plan was built from
// and nothing about the request.
//
// That identity is the RETAINED snapshot itself, not the numeric (project, composition, revision)
// tuple: every New/Open document deliberately reuses ProjectId 1, CompositionId 1 and the same
// revision numbers, so those numbers are not document-instance provenance. Each entry keeps the
// immutable Snapshot it was compiled from, and a request matches only when it carries the exact
// same retained project-state object (the address of `Snapshot::project()`, stable while the
// snapshot is retained) plus the same composition and revision. Copies of one snapshot -- the
// normal case, since every request copies the session's snapshot -- still share that state object
// and hit; a different Document with colliding numbers cannot. Holding the Snapshot also prevents a
// freed state object's address from being reused by a later document. Conservative misses are
// acceptable.
//
// A request carrying an interactive parameter override does NOT come through here: the override is
// lowered into the plan as a constant for that one request, so its plan is not the revision's plan.
// Caching it under the revision would hand a dragged value to every later frame.
//
// A cancelled compilation is never retained, and neither is a failed one -- a failure's diagnostics
// belong to the request that asked, and re-deriving them costs what it cost the first time. Only a
// Compiled or Unsupported outcome is retained, both of which are a property of the revision.
//
// Entries are held most-recently-used first and the oldest is dropped past the capacity, so undo
// and redo across a few revisions still hit while a long editing session cannot grow the cache.
// Thread-safe: the preview pipeline runs on task workers.
class CompiledPlanCache final {
  public:
    struct Statistics final {
        // How many times a plan was actually compiled through this cache, and how many requests
        // were answered from it. A preview that keeps compiling at one revision shows up here as
        // compiles that should have been hits.
        std::uint64_t compiles = 0;
        std::uint64_t hits = 0;

        friend bool operator==(const Statistics&, const Statistics&) = default;
    };

    explicit CompiledPlanCache(std::size_t capacity = kDefaultCompiledPlanCacheCapacity) noexcept;

    // The cached plan for `request`'s revision, compiling it exactly once per revision. `request`
    // must carry no parameter override (see the class comment); one that does is compiled directly
    // and never retained.
    [[nodiscard]] SnapshotCompileResult compile(const SnapshotCompiler& compiler,
                                                const SnapshotCompileRequest& request,
                                                const CancellationToken& cancellation);

    [[nodiscard]] Statistics statistics() const;
    [[nodiscard]] std::size_t size() const;
    void clear();

  private:
    struct Entry final {
        // The real immutable input, retained as the provenance owner. `project()` lives inside the
        // snapshot's shared document state, so its address identifies that exact state while this
        // entry holds it and cannot be recycled by a later document.
        document::Snapshot snapshot;
        document::CompositionId compositionId;
        document::Revision revision;
        SnapshotCompileResult result;
    };

    mutable std::mutex mutex_;
    std::vector<Entry> entries_;
    std::size_t capacity_;
    Statistics statistics_;
};

using CompiledPlanCacheHandle = std::shared_ptr<CompiledPlanCache>;

} // namespace bloom::runtime
