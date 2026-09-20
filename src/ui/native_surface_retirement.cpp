#include <bloom/ui/native_surface_retirement.hpp>

#include <QWidget>

#include <utility>

namespace bloom::ui {

EditorNativeSurface* editorNativeSurface(QWidget* editorWidget) noexcept {
    if (editorWidget == nullptr) {
        return nullptr;
    }
    return dynamic_cast<EditorNativeSurface*>(editorWidget);
}

NativeSurfaceRetirementGate::~NativeSurfaceRetirementGate() {
    // Lifetime first: any completion still holding a weak_ptr must be able to prove the gate is
    // gone and return without dereferencing `this`. A generation counter alone cannot express
    // lifetime.
    if (lifetime_) {
        lifetime_->alive = false;
        lifetime_.reset();
    }
    ++generation_;
    pending_ = false;
}

NativeSurfaceRetirementGate::StartStatus
NativeSurfaceRetirementGate::begin(const std::vector<EditorNativeSurface*>& targets, Commit commit,
                                   Finish finish, NativeSurfaceRetirementOptions options,
                                   Result* synchronousResult) {
    if (pending_ || completing_) {
        if (synchronousResult != nullptr) {
            *synchronousResult = {false, "a native-surface retirement is already pending"};
        }
        return StartStatus::Refused;
    }

    entries_.clear();
    commit_ = std::move(commit);
    finish_ = std::move(finish);
    options_ = std::move(options);
    outstanding_ = 0;
    failed_ = false;
    failureDiagnostic_.clear();
    ++generation_;

    for (EditorNativeSurface* target : targets) {
        if (target == nullptr) {
            continue;
        }
        bool duplicate = false;
        for (const auto& existing : entries_) {
            if (existing.surface == target) {
                duplicate = true;
                break;
            }
        }
        if (duplicate) {
            continue;
        }
        Entry entry;
        entry.surface = target;
        // Every editor widget is a QObject; a non-widget implementation is treated as already gone.
        entry.guard = dynamic_cast<QObject*>(target);
        entries_.push_back(entry);
    }

    std::size_t liveTargets = 0;
    for (auto& entry : entries_) {
        if (entry.guard.isNull()) {
            // A required receiver that is not a live QObject can never be proven retired. Refuse
            // rather than silently claiming a mutation was safe.
            entry.answered = true;
            failed_ = true;
            if (failureDiagnostic_.empty()) {
                failureDiagnostic_ = "native surface receiver is not a live object";
            }
            continue;
        }
        if (entry.surface->hasLiveNativeTarget()) {
            ++liveTargets;
        } else {
            // Nothing live to retire; already safe for this generation.
            entry.answered = true;
            entry.retired = true;
        }
    }

    if (liveTargets == 0) {
        // CPU-only subtree (the common case): preserve the host's existing synchronous semantics.
        // A destroyed/non-QObject receiver refuses instead of committing.
        completing_ = true;
        if (!failed_ && commit_) {
            commit_();
        }
        Result result;
        result.committed = !failed_;
        result.diagnostic = failureDiagnostic_;
        pending_ = false;
        auto finishFn = std::move(finish_);
        commit_ = nullptr;
        entries_.clear();
        if (synchronousResult != nullptr) {
            *synchronousResult = result;
        }
        if (finishFn) {
            finishFn(result);
        }
        completing_ = false;
        return StartStatus::CompletedSynchronously;
    }

    pending_ = true;
    // `starting_` keeps maybeComplete() from committing while later targets are still being asked.
    starting_ = true;
    const std::uint64_t generation = generation_;
    const std::weak_ptr<Lifetime> weakLifetime = lifetime_;
    for (std::size_t index = 0; index < entries_.size(); ++index) {
        Entry& entry = entries_[index];
        if (entry.answered) {
            continue;
        }
        ++outstanding_;
        const auto outcome = entry.surface->prepareNativeSurfaceMutation(
            generation, [weakLifetime, this, generation,
                         index](std::uint64_t callbackGeneration,
                                const EditorNativeSurface::PrepareResult& result) {
                if (callbackGeneration != generation) {
                    return;
                }
                // Prove the gate is still alive BEFORE dereferencing `this`; a late completion
                // after gate destruction is a no-op.
                const auto lifetime = weakLifetime.lock();
                if (!lifetime || !lifetime->alive) {
                    return;
                }
                onPrepare(generation, index, result);
            });
        if (outcome == EditorNativeSurface::PrepareOutcome::Refused) {
            onPrepare(generation, index,
                      {false, "editor refused to start native-surface retirement"});
        } else if (outcome == EditorNativeSurface::PrepareOutcome::NoLiveTarget) {
            onPrepare(generation, index, {true, {}});
        }
        // RetirePending: the callback arrives (possibly inline) and onPrepare() handles it.
    }
    starting_ = false;
    maybeComplete();
    return StartStatus::Retiring;
}

void NativeSurfaceRetirementGate::onPrepare(std::uint64_t generation, std::size_t index,
                                            const EditorNativeSurface::PrepareResult& result) {
    if (!pending_ || generation != generation_ || index >= entries_.size()) {
        return; // stale generation or duplicate completion
    }
    Entry& entry = entries_[index];
    if (entry.answered) {
        return;
    }
    entry.answered = true;
    if (entry.guard.isNull()) {
        // The required receiver died while its retirement was in flight. Its safe-to-mutate claim
        // is unprovable, so refuse rather than claim the tree mutation succeeded.
        failed_ = true;
        if (failureDiagnostic_.empty()) {
            failureDiagnostic_ = "native surface receiver destroyed before retirement completed";
        }
    } else if (result.safeToMutate) {
        entry.retired = true;
    } else {
        failed_ = true;
        if (failureDiagnostic_.empty()) {
            failureDiagnostic_ = result.diagnostic.empty()
                                     ? std::string("native surface retained (unproven)")
                                     : result.diagnostic;
        }
    }
    if (outstanding_ > 0) {
        --outstanding_;
    }
    maybeComplete();
}

void NativeSurfaceRetirementGate::maybeComplete() {
    if (!pending_ || starting_ || outstanding_ > 0 || completing_) {
        return;
    }
    pending_ = false;
    // `completing_` forbids a reentrant begin() from the commit/resume/finish callbacks, which
    // would otherwise clear entries_ mid-iteration.
    completing_ = true;

    if (failed_) {
        // All-or-nothing abort: no commit, but every target that genuinely retired is put back
        // into service so the untouched tree keeps working.
        resumeSurvivors();
        Result result;
        result.committed = false;
        result.diagnostic = failureDiagnostic_.empty()
                                ? std::string("native surface retirement refused")
                                : failureDiagnostic_;
        auto finish = std::move(finish_);
        commit_ = nullptr;
        entries_.clear();
        if (finish) {
            finish(result);
        }
        completing_ = false;
        return;
    }

    if (commit_) {
        commit_();
    }
    if (options_.resumeSurvivorsOnSuccess) {
        resumeSurvivors();
    }
    Result result;
    result.committed = true;
    auto finish = std::move(finish_);
    commit_ = nullptr;
    entries_.clear();
    if (finish) {
        finish(result);
    }
    completing_ = false;
}

void NativeSurfaceRetirementGate::abandon(bool resumeRetired) {
    if (!pending_) {
        return;
    }
    // Invalidate the generation before any resume so late callbacks are stale and ignored.
    ++generation_;
    pending_ = false;
    if (resumeRetired) {
        resumeSurvivors();
    }
    entries_.clear();
    commit_ = nullptr;
    finish_ = nullptr;
}

void NativeSurfaceRetirementGate::resumeSurvivors() {
    for (auto& entry : entries_) {
        if (!entry.retired || entry.guard.isNull()) {
            continue; // never retired, or the receiver was destroyed by the commit
        }
        if (options_.shouldResume && !options_.shouldResume(entry.surface)) {
            continue; // commit detached this target (e.g. deleteLater'd outgoing subtree)
        }
        entry.surface->resumeNativeSurfaceAfterMutation();
    }
}

} // namespace bloom::ui
