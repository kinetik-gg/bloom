#pragma once

#include <bloom/runtime/snapshot_compiler.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>

namespace bloom::ui {

inline constexpr std::size_t kDefaultCompiledPlanCacheCapacity = 4;

// One compiled plan per document revision, kept so that playing or scrubbing a composition compiles
// nothing at all.
//
// This is sound because a compiled plan is TIME-INDEPENDENT: every animated parameter is a curve
// index the evaluator samples at the request time, so two requests that differ only in time compile
// to the same plan -- the same reason docs/architecture/animation-and-time.md gives for compiling a
// sequence export's range once. The key is therefore the document identity the plan was built from
// (project, composition, revision) and nothing about the request.
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
    [[nodiscard]] runtime::SnapshotCompileResult
    compile(const runtime::SnapshotCompiler& compiler,
            const runtime::SnapshotCompileRequest& request,
            const runtime::CancellationToken& cancellation);

    [[nodiscard]] Statistics statistics() const;
    [[nodiscard]] std::size_t size() const;
    void clear();

  private:
    struct Entry final {
        document::ProjectId projectId;
        document::CompositionId compositionId;
        document::Revision revision;
        runtime::SnapshotCompileResult result;
    };

    mutable std::mutex mutex_;
    std::vector<Entry> entries_;
    std::size_t capacity_;
    Statistics statistics_;
};

using CompiledPlanCacheHandle = std::shared_ptr<CompiledPlanCache>;

} // namespace bloom::ui
