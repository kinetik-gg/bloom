#pragma once

// Host-side coordinator for the asynchronous retire-before-mutation protocol. It accepts the set
// of editor native surfaces in the subtree a mutation will touch and guarantees, all-or-nothing:
//   * CPU-only subtrees (no live target) commit synchronously inside begin();
//   * if every live target reports SafeToMutate, the commit runs exactly once and every surviving
//     target is resumed;
//   * if any target refuses, the commit never runs, already-retired targets are resumed, and the
//     tree is left untouched.
//
// It is a plain class (not a QObject): callers pass std::function callbacks, so no moc is involved
// and no new signals/slots are added to the Q_OBJECT host classes. Duplicate surfaces are collapsed,
// destroyed receivers are detected with QPointer, and a generation counter discards stale callbacks.
//
// Lifetime: every prepare completion holds a std::weak_ptr to a small per-gate control block. The
// callback must lock it BEFORE it dereferences the gate; destruction invalidates and releases the
// block, so a completion that arrives after the gate is gone is a no-op instead of a
// use-after-free. A generation counter alone cannot express object lifetime.

#include <bloom/ui/editor_native_surface.hpp>

#include <QPointer>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

class QObject;

namespace bloom::ui {

// Kept at namespace scope (not nested) so a designated initializer can be used at call sites
// without tripping the "default member initializer in a default argument" rule.
struct NativeSurfaceRetirementOptions final {
    // For ordinary workspace mutations, survivors re-attach after the commit. At application
    // shutdown nothing survives, so this is false.
    bool resumeSurvivorsOnSuccess = true;
    // Optional discriminator evaluated after the commit. An empty function resumes every retired
    // target that is still alive. WorkspaceHost supplies one that only resumes targets still
    // attached to the live root, so a deleteLater'd outgoing subtree (whose parent is already
    // nullptr) is never resumed.
    std::function<bool(EditorNativeSurface*)> shouldResume;
};

class NativeSurfaceRetirementGate final {
  public:
    struct Result final {
        // True only if the commit actually ran.
        bool committed = false;
        std::string diagnostic;
    };

    enum class StartStatus : std::uint8_t {
        // No live target: the commit and finish already ran before begin() returned.
        CompletedSynchronously,
        // Deferred: `finish` will be called once after the last callback (or abandoned).
        Retiring,
        // A request is already pending. The new request changed nothing.
        Refused,
    };

    using Commit = std::function<void()>;
    using Finish = std::function<void(const Result&)>;

    NativeSurfaceRetirementGate() = default;
    ~NativeSurfaceRetirementGate();

    NativeSurfaceRetirementGate(const NativeSurfaceRetirementGate&) = delete;
    NativeSurfaceRetirementGate& operator=(const NativeSurfaceRetirementGate&) = delete;
    NativeSurfaceRetirementGate(NativeSurfaceRetirementGate&&) = delete;
    NativeSurfaceRetirementGate& operator=(NativeSurfaceRetirementGate&&) = delete;

    [[nodiscard]] bool isPending() const noexcept { return pending_; }
    [[nodiscard]] std::uint64_t generation() const noexcept { return generation_; }

    // Starts a retirement. Null entries are ignored and duplicates collapsed. `finish` runs exactly
    // once (inline for the synchronous case) unless abandon() is used. When the caller needs the
    // synchronous result it passes `synchronousResult`.
    StartStatus begin(std::vector<EditorNativeSurface*> targets, Commit commit, Finish finish,
                      NativeSurfaceRetirementOptions options = {},
                      Result* synchronousResult = nullptr);

    // Abandons a pending request without committing and without invoking finish. Any target that had
    // already retired and is still alive is resumed only when `resumeRetired` is true. Intended for
    // host teardown/overwrite; late callbacks from the abandoned generation are ignored.
    void abandon(bool resumeRetired = true);

  private:
    struct Entry final {
        EditorNativeSurface* surface = nullptr;
        QPointer<QObject> guard;
        bool retired = false;
        bool answered = false;
    };

    // Per-gate lifetime control block. Callbacks hold weak_ptr; the destructor flips `alive` and
    // drops the strong reference so a late completion can prove the gate is gone before touching it.
    struct Lifetime final {
        bool alive = true;
    };

    void onPrepare(std::uint64_t generation, std::size_t index,
                   const EditorNativeSurface::PrepareResult& result);
    void maybeComplete();
    void resumeSurvivors();

    std::vector<Entry> entries_;
    Commit commit_;
    Finish finish_;
    NativeSurfaceRetirementOptions options_;
    std::shared_ptr<Lifetime> lifetime_ = std::make_shared<Lifetime>();
    std::uint64_t generation_ = 0;
    std::size_t outstanding_ = 0;
    bool pending_ = false;
    bool starting_ = false;
    bool completing_ = false;
    bool failed_ = false;
    std::string failureDiagnostic_;
};

} // namespace bloom::ui
