// Focused Qt host fixture: drives the real EditorArea/WorkspaceHost retire-before-mutation gates
// with a delayed fake native-surface editor. It links the modified host TUs ahead of the frozen
// libbloom_ui.a, so it exercises the actual gate integration, not a copy. CPU-only workspace
// behavior is asserted unchanged. The real GPU adapter proof lives in the sibling
// gpu-viewer-adapter-prep slice; nothing here fakes a GPU.

#include <bloom/ui/editor_area.hpp>
#include <bloom/ui/editor_native_surface.hpp>
#include <bloom/ui/editor_registry.hpp>
#include <bloom/ui/native_surface_retirement.hpp>
#include <bloom/ui/workspace_host.hpp>

#include <QApplication>
#include <QLabel>
#include <QPointer>
#include <QString>
#include <QWidget>

#include <cstdint>
#include <cstdio>
#include <functional>
#include <memory>
#include <string>

namespace {

int failures = 0;

void check(bool condition, const char* what) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++failures;
    }
}

struct FakeState final {
    bool live = true;
    bool delayed = true;
    bool refuse = false;
    int prepareCalls = 0;
    int resumeCalls = 0;
    QPointer<QWidget> created;
    std::function<void()> ack;
};

class FakeNativeEditor final : public QWidget, public bloom::ui::EditorNativeSurface {
  public:
    explicit FakeNativeEditor(std::shared_ptr<FakeState> state, QWidget* parent = nullptr)
        : QWidget(parent), state_(std::move(state)) {}

    [[nodiscard]] bool hasLiveNativeTarget() const override { return state_->live; }

    PrepareOutcome prepareNativeSurfaceMutation(std::uint64_t generation,
                                                PrepareCallback completion) override {
        ++state_->prepareCalls;
        if (state_->refuse) {
            return PrepareOutcome::Refused;
        }
        if (state_->delayed) {
            state_->ack = [completion, generation] { completion(generation, {true, {}}); };
            return PrepareOutcome::RetirePending;
        }
        completion(generation, {true, {}});
        return PrepareOutcome::RetirePending;
    }

    void resumeNativeSurfaceAfterMutation() override { ++state_->resumeCalls; }

    [[nodiscard]] std::string nativeSurfaceDiagnostic() const override {
        return state_->refuse ? "fake-native refused" : "fake-native";
    }

  private:
    std::shared_ptr<FakeState> state_;
};

void registerEditors(bloom::ui::EditorRegistry& registry, const std::shared_ptr<FakeState>& state) {
    (void)registry.registerEditor(
        {.id = "cpu",
         .displayName = QStringLiteral("CPU"),
         .create = [](QWidget* parent) { return new QLabel(QStringLiteral("cpu"), parent); }});
    (void)registry.registerEditor(
        {.id = "native",
         .displayName = QStringLiteral("Native"),
         .create = [state](QWidget* parent) {
             auto* editor = new FakeNativeEditor(state, parent);
             state->created = editor;
             return editor;
         }});
}

void editorAreaReplacementIsDeferred() {
    auto state = std::make_shared<FakeState>();
    bloom::ui::EditorRegistry registry;
    registerEditors(registry, state);
    bloom::ui::EditorArea area(registry, "native");
    check(area.editorId() == "native", "area starts on native editor");
    QWidget* const fake = state->created.data();
    check(fake != nullptr, "native editor hosted");
    QWidget* const parentBefore = fake->parentWidget();

    const bool accepted = area.setEditorId("cpu");
    check(accepted, "setEditorId accepts a non-empty id");
    check(area.isEditorChangePending(), "replacement is pending while the surface retires");
    check(area.editorId() == "native", "selection truth unchanged before ack");
    check(state->created.data() == fake, "hosted widget unchanged before ack");
    check(fake->parentWidget() == parentBefore, "widget parent unchanged before ack");

    state->ack();
    check(!area.isEditorChangePending(), "replacement pending cleared after ack");
    check(area.editorId() == "cpu", "selection applied after ack");
    check(state->created.data() == nullptr, "replaced native editor destroyed after ack");
}

void editorAreaRefusalLeavesIntact() {
    auto state = std::make_shared<FakeState>();
    state->refuse = true;
    bloom::ui::EditorRegistry registry;
    registerEditors(registry, state);
    bloom::ui::EditorArea area(registry, "native");
    QWidget* const fake = state->created.data();
    check(fake != nullptr, "native editor hosted before refusal");

    (void)area.setEditorId("cpu");
    check(!area.isEditorChangePending(), "refusal completes without a pending change");
    check(area.editorId() == "native", "refusal leaves selection truth intact");
    check(state->created.data() == fake, "refusal leaves hosted widget intact");
    check(!area.lastNativeSurfaceDiagnostic().empty(), "refusal carries a diagnostic");
}

void workspaceSplitIsDeferred() {
    auto state = std::make_shared<FakeState>();
    bloom::ui::EditorRegistry registry;
    registerEditors(registry, state);
    bloom::ui::WorkspaceHost host(registry);
    host.resetToSingleArea("native");
    bloom::ui::EditorArea* area = host.activeArea();
    check(area != nullptr && area->editorId() == "native", "single native area created");
    QWidget* const fake = state->created.data();
    check(fake != nullptr, "native editor hosted in workspace");
    const int before = host.areaCount();
    const int resumeBefore = state->resumeCalls;

    bloom::ui::EditorArea* created = host.splitArea(*area, Qt::Horizontal, "cpu");
    check(created == nullptr, "deferred split claims no new pointer");
    check(host.isNativeSurfaceMutationPending(), "split is pending");
    check(host.areaCount() == before, "area count unchanged before split ack");
    check(state->created.data() == fake, "native widget unchanged before split ack");
    check(area->parentWidget() != nullptr, "area parent unchanged before split ack");

    state->ack();
    check(!host.isNativeSurfaceMutationPending(), "split pending cleared after ack");
    check(host.areaCount() == before + 1, "area count incremented after split ack");
    check(state->resumeCalls == resumeBefore + 1, "reparented survivor resumed after split");
}

void workspaceCloseIsDeferred() {
    auto state = std::make_shared<FakeState>();
    bloom::ui::EditorRegistry registry;
    registerEditors(registry, state);
    bloom::ui::WorkspaceHost host(registry);
    host.resetToSingleArea("native");
    bloom::ui::EditorArea* area = host.activeArea();
    check(area != nullptr, "native area present");
    (void)host.splitArea(*area, Qt::Horizontal, "cpu");
    state->ack(); // finish the split
    check(host.areaCount() == 2, "two areas after split");
    const int resumeBefore = state->resumeCalls;

    const bool closed = host.closeArea(*area);
    check(!closed, "deferred close reports not-yet-closed");
    check(host.isNativeSurfaceMutationPending(), "close is pending");
    check(host.areaCount() == 2, "area count unchanged before close ack");

    state->ack();
    check(!host.isNativeSurfaceMutationPending(), "close pending cleared after ack");
    check(host.areaCount() == 1, "area count decremented after close ack");
    check(state->resumeCalls == resumeBefore, "destroyed native editor is not resumed");
}

void workspaceRootRestoreIsDeferred() {
    auto state = std::make_shared<FakeState>();
    bloom::ui::EditorRegistry registry;
    registerEditors(registry, state);
    bloom::ui::WorkspaceHost host(registry);
    host.resetToSingleArea("native");
    QWidget* const fakeBefore = state->created.data();
    const int countBefore = host.areaCount();
    const QByteArray saved = host.saveLayoutState();

    const auto result = host.restoreLayoutState(saved);
    check(result == bloom::ui::WorkspaceLayoutRestoreResult::Deferred,
          "root restore defers on a live native surface");
    check(host.isNativeSurfaceMutationPending(), "restore is pending");
    check(host.areaCount() == countBefore, "area count unchanged before restore ack");
    check(state->created.data() == fakeBefore, "root tree unchanged before restore ack");

    state->ack();
    check(!host.isNativeSurfaceMutationPending(), "restore pending cleared after ack");
    check(host.areaCount() == countBefore, "restored tree has the same area count");
    check(state->created.data() != fakeBefore, "restored tree materializes a fresh native editor");
}

void shutdownRetirementLeavesTreeUntilAck() {
    auto state = std::make_shared<FakeState>();
    bloom::ui::EditorRegistry registry;
    registerEditors(registry, state);
    bloom::ui::WorkspaceHost host(registry);
    host.resetToSingleArea("native");
    QWidget* const fake = state->created.data();
    const int countBefore = host.areaCount();

    const auto surfaces = host.liveNativeSurfaces();
    check(surfaces.size() == 1, "shutdown enumerates the live native surface");

    bloom::ui::NativeSurfaceRetirementGate shutdownGate;
    bool finished = false;
    bool committed = false;
    bloom::ui::NativeSurfaceRetirementOptions options;
    options.resumeSurvivorsOnSuccess = false;
    const auto status = shutdownGate.begin(
        surfaces, [] {},
        [&finished, &committed](const bloom::ui::NativeSurfaceRetirementGate::Result& result) {
            finished = true;
            committed = result.committed;
        },
        options);
    check(status == bloom::ui::NativeSurfaceRetirementGate::StartStatus::Retiring,
          "shutdown retirement is deferred");
    check(host.areaCount() == countBefore, "tree unchanged before shutdown ack");
    check(state->created.data() == fake, "native widget unchanged before shutdown ack");

    state->ack();
    check(finished && committed, "shutdown retirement completes after ack");
    check(state->resumeCalls == 0, "shutdown does not resume survivors");
    check(host.areaCount() == countBefore, "shutdown retirement does not mutate the tree");
}

void cpuOnlyWorkspaceStaysSynchronous() {
    auto state = std::make_shared<FakeState>();
    bloom::ui::EditorRegistry registry;
    registerEditors(registry, state);
    bloom::ui::WorkspaceHost host(registry);
    host.resetToSingleArea("cpu");
    const int before = host.areaCount();
    bloom::ui::EditorArea* area = host.activeArea();
    check(area != nullptr && area->editorId() == "cpu", "cpu area active");
    bloom::ui::EditorArea* created = host.splitArea(*area, Qt::Horizontal, "cpu");
    check(created != nullptr, "cpu-only split returns the new area synchronously");
    check(!host.isNativeSurfaceMutationPending(), "cpu-only split never pends");
    check(host.areaCount() == before + 1, "cpu-only split applied synchronously");
    check(state->prepareCalls == 0, "cpu-only split asks no native surface");
}

} // namespace

int main(int argc, char** argv) {
    QApplication application(argc, argv);
    editorAreaReplacementIsDeferred();
    editorAreaRefusalLeavesIntact();
    workspaceSplitIsDeferred();
    workspaceCloseIsDeferred();
    workspaceRootRestoreIsDeferred();
    shutdownRetirementLeavesTreeUntilAck();
    cpuOnlyWorkspaceStaysSynchronous();
    if (failures != 0) {
        std::fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    std::fprintf(stdout, "PASS: host retire-before-mutation gate (EditorArea/WorkspaceHost)\n");
    return 0;
}
