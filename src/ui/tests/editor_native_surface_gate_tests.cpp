// Focused CPU-only test for the asynchronous retire-before-mutation gate. It compiles against only
// the gate/interface headers and QtCore/QtWidgets; it needs no GPU, no Vulkan, and no frozen UI
// archive. The delayed fake proves the exact host semantics the EditorArea/WorkspaceHost gates rely
// on: synchronous CPU commit, deferred all-safe commit-once, all-or-nothing refusal that resumes
// already-retired survivors, duplicate rejection, stale-generation discard, and destroyed-receiver
// safety.

#include <bloom/ui/editor_native_surface.hpp>
#include <bloom/ui/native_surface_retirement.hpp>

#include <QCoreApplication>
#include <QObject>

#include <cstdint>
#include <cstdio>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace {

int failures = 0;

void check(bool condition, const char* what) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++failures;
    }
}

enum class Mode : std::uint8_t { ImmediateSafe, Delayed, Refuse };

struct FakeState final {
    bool live = true;
    Mode mode = Mode::Delayed;
    int prepareCalls = 0;
    int resumeCalls = 0;
    std::function<void()> ack;
};

class FakeSurface final : public QObject, public bloom::ui::EditorNativeSurface {
  public:
    explicit FakeSurface(std::shared_ptr<FakeState> state) : state_(std::move(state)) {}

    [[nodiscard]] bool hasLiveNativeTarget() const override { return state_->live; }

    PrepareOutcome prepareNativeSurfaceMutation(std::uint64_t generation,
                                                PrepareCallback completion) override {
        ++state_->prepareCalls;
        switch (state_->mode) {
        case Mode::Refuse:
            return PrepareOutcome::Refused;
        case Mode::ImmediateSafe:
            completion(generation, {true, {}});
            return PrepareOutcome::RetirePending;
        case Mode::Delayed:
            state_->ack = [completion, generation] { completion(generation, {true, {}}); };
            return PrepareOutcome::RetirePending;
        }
        return PrepareOutcome::Refused;
    }

    void resumeNativeSurfaceAfterMutation() override { ++state_->resumeCalls; }

    [[nodiscard]] std::string nativeSurfaceDiagnostic() const override { return "fake-surface"; }

  private:
    std::shared_ptr<FakeState> state_;
};

void cpuOnlyCommitsSynchronously() {
    auto state = std::make_shared<FakeState>();
    state->live = false;
    FakeSurface surface(state);
    bloom::ui::NativeSurfaceRetirementGate gate;
    int commits = 0;
    bool finished = false;
    bloom::ui::NativeSurfaceRetirementGate::Result synchronous;
    const auto status = gate.begin(
        {&surface}, [&commits] { ++commits; },
        [&finished](const bloom::ui::NativeSurfaceRetirementGate::Result&) { finished = true; }, {},
        &synchronous);
    check(status == bloom::ui::NativeSurfaceRetirementGate::StartStatus::CompletedSynchronously,
          "cpu-only begin is synchronous");
    check(commits == 1, "cpu-only commit ran once");
    check(finished, "cpu-only finish ran");
    check(synchronous.committed, "cpu-only synchronous result committed");
    check(!gate.isPending(), "cpu-only gate not pending");
}

void deferredAllSafeCommitsOnce() {
    auto state = std::make_shared<FakeState>();
    FakeSurface surface(state);
    bloom::ui::NativeSurfaceRetirementGate gate;
    int commits = 0;
    bool finished = false;
    bool committed = false;
    const auto status = gate.begin(
        {&surface}, [&commits] { ++commits; },
        [&finished, &committed](const bloom::ui::NativeSurfaceRetirementGate::Result& result) {
            finished = true;
            committed = result.committed;
        });
    check(status == bloom::ui::NativeSurfaceRetirementGate::StartStatus::Retiring,
          "deferred begin is retiring");
    check(gate.isPending(), "deferred gate pending before ack");
    check(commits == 0, "no commit before ack");
    check(state->prepareCalls == 1, "prepare called once");
    check(static_cast<bool>(state->ack), "ack captured");
    state->ack();
    check(commits == 1, "commit ran exactly once after ack");
    check(finished && committed, "deferred finish reports committed");
    check(state->resumeCalls == 1, "survivor resumed after commit");
    check(!gate.isPending(), "gate not pending after completion");
}

void refusalLeavesTreeAndResumesAlreadySafe() {
    auto safeState = std::make_shared<FakeState>();
    safeState->mode = Mode::ImmediateSafe;
    auto refuseState = std::make_shared<FakeState>();
    refuseState->mode = Mode::Refuse;
    FakeSurface safe(safeState);
    FakeSurface refuse(refuseState);
    bloom::ui::NativeSurfaceRetirementGate gate;
    int commits = 0;
    bool finished = false;
    bool committed = true;
    std::string diagnostic;
    const auto status = gate.begin(
        {&safe, &refuse}, [&commits] { ++commits; },
        [&finished, &committed, &diagnostic](
            const bloom::ui::NativeSurfaceRetirementGate::Result& result) {
            finished = true;
            committed = result.committed;
            diagnostic = result.diagnostic;
        });
    check(status == bloom::ui::NativeSurfaceRetirementGate::StartStatus::Retiring,
          "refusal path still starts");
    check(finished && !committed, "refusal finish reports not committed");
    check(commits == 0, "refusal leaves the tree uncommitted");
    check(safeState->resumeCalls == 1, "already-retired survivor resumed on refusal");
    check(refuseState->resumeCalls == 0, "refusing surface not resumed");
    check(!diagnostic.empty(), "refusal carries a diagnostic");
}

void duplicateRejectedWhilePending() {
    auto state = std::make_shared<FakeState>();
    FakeSurface surface(state);
    bloom::ui::NativeSurfaceRetirementGate gate;
    const auto first = gate.begin({&surface}, [] {}, [](const auto&) {});
    check(first == bloom::ui::NativeSurfaceRetirementGate::StartStatus::Retiring, "first accepted");
    int secondCommits = 0;
    const auto second = gate.begin(
        {&surface}, [&secondCommits] { ++secondCommits; }, [](const auto&) {});
    check(second == bloom::ui::NativeSurfaceRetirementGate::StartStatus::Refused,
          "duplicate rejected while pending");
    check(secondCommits == 0, "duplicate changed nothing");
    gate.abandon(false);
}

void staleGenerationIgnored() {
    auto state = std::make_shared<FakeState>();
    FakeSurface surface(state);
    bloom::ui::NativeSurfaceRetirementGate gate;
    int commits = 0;
    bool finished = false;
    (void)gate.begin(
        {&surface}, [&commits] { ++commits; },
        [&finished](const auto&) { finished = true; });
    auto lateAck = state->ack;
    check(static_cast<bool>(lateAck), "ack captured for stale test");
    gate.abandon(false);
    lateAck();
    check(commits == 0, "stale callback does not commit");
    check(!finished, "stale callback does not finish");
    check(!gate.isPending(), "gate not pending after abandon");
}

void destroyedReceiverNotResumed() {
    auto state = std::make_shared<FakeState>();
    auto surface = std::make_unique<FakeSurface>(state);
    bloom::ui::NativeSurfaceRetirementGate gate;
    int commits = 0;
    bool finished = false;
    bool committed = true;
    std::string diagnostic;
    (void)gate.begin(
        {surface.get()}, [&commits] { ++commits; },
        [&finished, &committed, &diagnostic](
            const bloom::ui::NativeSurfaceRetirementGate::Result& result) {
            finished = true;
            committed = result.committed;
            diagnostic = result.diagnostic;
        });
    auto lateAck = state->ack;
    // Destroy the receiver before its retirement callback arrives. Its safe-to-mutate claim is now
    // unprovable, so the gate must refuse the commit, not claim success.
    surface.reset();
    lateAck();
    check(finished, "destroyed receiver still completes the gate");
    check(!committed, "destroyed required receiver refuses the commit");
    check(commits == 0, "destroyed required receiver performs no tree mutation");
    check(!diagnostic.empty(), "destroyed receiver refusal carries a diagnostic");
    check(state->resumeCalls == 0, "destroyed receiver is never resumed");
}

void lateCallbackAfterGateDestroyedIsInert() {
    auto state = std::make_shared<FakeState>();
    auto surface = std::make_unique<FakeSurface>(state);
    std::function<void()> lateAck;
    {
        bloom::ui::NativeSurfaceRetirementGate gate;
        auto commits = std::make_shared<int>(0);
        bool finished = false;
        (void)gate.begin(
            {surface.get()}, [commits] { ++(*commits); },
            [&finished](const auto&) { finished = true; });
        lateAck = state->ack;
        check(static_cast<bool>(lateAck), "late ack captured before gate destruction");
        // The gate dies with the retirement still pending and the receiver still alive.
    }
    // Invoking the orphaned completion must not touch the freed gate, commit, or resume.
    lateAck();
    check(state->resumeCalls == 0, "late callback after gate destruction never resumes");
    check(state->prepareCalls == 1, "late callback after gate destruction does nothing else");
}

void reentrantBeginDuringCommitRefused() {
    auto state = std::make_shared<FakeState>();
    FakeSurface surface(state);
    bloom::ui::NativeSurfaceRetirementGate gate;
    bloom::ui::NativeSurfaceRetirementGate::StartStatus reentrant =
        bloom::ui::NativeSurfaceRetirementGate::StartStatus::CompletedSynchronously;
    int commits = 0;
    (void)gate.begin(
        {&surface}, [&] {
            ++commits;
            reentrant = gate.begin({&surface}, [] {}, [](const auto&) {});
        },
        [](const auto&) {});
    state->ack();
    check(commits == 1, "reentrant commit runs once");
    check(reentrant == bloom::ui::NativeSurfaceRetirementGate::StartStatus::Refused,
          "reentrant begin during commit is refused");
}

void abandonDoesNotReattachAfterLateAck() {
    auto state = std::make_shared<FakeState>();
    FakeSurface surface(state);
    bloom::ui::NativeSurfaceRetirementGate gate;
    (void)gate.begin({&surface}, [] {}, [](const auto&) {});
    auto lateAck = state->ack;
    gate.abandon(false);
    lateAck();
    check(state->resumeCalls == 0, "late ack after abandon never reattaches");
    check(!gate.isPending(), "abandon clears pending");
}

void abandonCanResumeAlreadyRetired() {
    auto safeState = std::make_shared<FakeState>();
    safeState->mode = Mode::ImmediateSafe;
    auto delayedState = std::make_shared<FakeState>();
    delayedState->mode = Mode::Delayed;
    FakeSurface safe(safeState);
    FakeSurface delayed(delayedState);
    bloom::ui::NativeSurfaceRetirementGate gate;
    (void)gate.begin({&safe, &delayed}, [] {}, [](const auto&) {});
    check(safeState->resumeCalls == 0, "no resume before abandon");
    gate.abandon(true);
    check(safeState->resumeCalls == 1, "abandon resumes an already-retired target");
    check(delayedState->resumeCalls == 0, "abandon does not resume an unanswered target");
}

} // namespace

int main(int argc, char** argv) {
    QCoreApplication application(argc, argv);
    cpuOnlyCommitsSynchronously();
    deferredAllSafeCommitsOnce();
    refusalLeavesTreeAndResumesAlreadySafe();
    duplicateRejectedWhilePending();
    staleGenerationIgnored();
    destroyedReceiverNotResumed();
    lateCallbackAfterGateDestroyedIsInert();
    reentrantBeginDuringCommitRefused();
    abandonDoesNotReattachAfterLateAck();
    abandonCanResumeAlreadyRetired();
    if (failures != 0) {
        std::fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    std::fprintf(stdout, "PASS: EditorNativeSurface retirement gate semantics\n");
    return 0;
}
