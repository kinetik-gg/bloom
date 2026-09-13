#include <bloom/ui/composition_plan_cache.hpp>

#include <bloom/document/project.hpp>

#include <algorithm>
#include <utility>

namespace bloom::ui {
namespace {

[[nodiscard]] bool isRetainable(const runtime::SnapshotCompileStatus status) noexcept {
    switch (status) {
    case runtime::SnapshotCompileStatus::Compiled:
    case runtime::SnapshotCompileStatus::Unsupported:
        return true;
    case runtime::SnapshotCompileStatus::Cancelled:
    case runtime::SnapshotCompileStatus::Failed:
        return false;
    }
    return false;
}

} // namespace

CompiledPlanCache::CompiledPlanCache(const std::size_t capacity) noexcept
    : capacity_(std::max<std::size_t>(1, capacity)) {}

runtime::SnapshotCompileResult
CompiledPlanCache::compile(const runtime::SnapshotCompiler& compiler,
                           const runtime::SnapshotCompileRequest& request,
                           const runtime::CancellationToken& cancellation) {
    const auto projectId = request.snapshot.project().id();
    const auto revision = request.snapshot.revision();
    if (request.parameterOverride.has_value()) {
        // An overridden request's plan is not the revision's plan; it is this one gesture frame's.
        std::lock_guard lock(mutex_);
        ++statistics_.compiles;
        return compiler.compile(request, cancellation);
    }

    {
        std::lock_guard lock(mutex_);
        const auto position = std::ranges::find_if(entries_, [&](const Entry& entry) {
            return entry.projectId == projectId && entry.revision == revision &&
                   entry.compositionId == request.compositionId;
        });
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
    auto result = compiler.compile(request, cancellation);
    if (!isRetainable(result.status)) {
        return result;
    }

    std::lock_guard lock(mutex_);
    const auto existing = std::ranges::find_if(entries_, [&](const Entry& entry) {
        return entry.projectId == projectId && entry.revision == revision &&
               entry.compositionId == request.compositionId;
    });
    if (existing != entries_.end()) {
        return existing->result;
    }
    entries_.insert(entries_.begin(), Entry{.projectId = projectId,
                                            .compositionId = request.compositionId,
                                            .revision = revision,
                                            .result = result});
    if (entries_.size() > capacity_) {
        entries_.resize(capacity_);
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

} // namespace bloom::ui
