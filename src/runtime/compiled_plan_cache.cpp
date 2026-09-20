#include <bloom/runtime/compiled_plan_cache.hpp>

#include <bloom/document/project.hpp>

#include <algorithm>
#include <cstddef>
#include <utility>

namespace bloom::runtime {
namespace {

[[nodiscard]] bool isRetainable(const SnapshotCompileStatus status) noexcept {
    switch (status) {
    case SnapshotCompileStatus::Compiled:
    case SnapshotCompileStatus::Unsupported:
        return true;
    case SnapshotCompileStatus::Cancelled:
    case SnapshotCompileStatus::Failed:
        return false;
    }
    return false;
}

} // namespace

CompiledPlanCache::CompiledPlanCache(const std::size_t capacity) noexcept
    : capacity_(std::max<std::size_t>(1, capacity)) {}

SnapshotCompileResult CompiledPlanCache::compile(const SnapshotCompiler& compiler,
                                                 const SnapshotCompileRequest& request,
                                                 const CancellationToken& cancellation) {
    const auto revision = request.snapshot.revision();
    // Provenance is the retained project-state object, not the numeric ids: a different Document
    // reuses ProjectId 1 / CompositionId 1 / the same revision numbers. Matching the address of
    // `project()` distinguishes two documents while still letting copies of one snapshot hit.
    const auto sameInput = [&request, revision](const Entry& entry) {
        return entry.compositionId == request.compositionId && entry.revision == revision &&
               &entry.snapshot.project() == &request.snapshot.project();
    };
    if (!request.parameterOverrides.empty()) {
        // An overridden request's plan is not the revision's plan; it is this one gesture frame's.
        {
            std::lock_guard lock(mutex_);
            ++statistics_.compiles;
        }
        return compiler.compile(request, cancellation, this);
    }

    {
        std::lock_guard lock(mutex_);
        const auto position = std::ranges::find_if(entries_, sameInput);
        if (position != entries_.end()) {
            ++statistics_.hits;
            std::rotate(entries_.begin(), position, position + 1);
            return entries_.front().result;
        }
        ++statistics_.compiles;
    }

    // Compiled OUTSIDE the lock: compilation is the expensive part, and holding the cache's lock
    // across it would make two compositions compiling at once wait on each other for no reason. Two
    // requests racing on the same key can therefore both compile, which costs one redundant
    // compilation and never produces a wrong plan -- the insert below keeps whichever arrives
    // first.
    auto result = compiler.compile(request, cancellation, this);
    if (!isRetainable(result.status)) {
        return result;
    }

    std::lock_guard lock(mutex_);
    const auto existing = std::ranges::find_if(entries_, sameInput);
    if (existing != entries_.end()) {
        return existing->result;
    }
    entries_.insert(entries_.begin(), Entry{.snapshot = request.snapshot,
                                            .compositionId = request.compositionId,
                                            .revision = revision,
                                            .result = result});
    if (entries_.size() > capacity_) {
        // Erase rather than resize: Snapshot is not default-constructible.
        entries_.erase(entries_.begin() + static_cast<std::ptrdiff_t>(capacity_), entries_.end());
    }
    return result;
}

CompiledPlanCache::Statistics CompiledPlanCache::statistics() const {
    std::lock_guard lock(mutex_);
    return statistics_;
}

std::size_t CompiledPlanCache::size() const {
    std::lock_guard lock(mutex_);
    return entries_.size();
}

void CompiledPlanCache::clear() {
    std::lock_guard lock(mutex_);
    entries_.clear();
}

} // namespace bloom::runtime
