#pragma once

#include <bloom/ui/editor_native_surface.hpp>
#include <bloom/ui/native_surface_retirement.hpp>

#include <QObject>
#include <QTimer>

#include <functional>
#include <string>
#include <vector>

class QEvent;

namespace bloom::ui {

class CompositionPreviewController;
class TaskUiBridge;

class ApplicationShutdownCoordinator final : public QObject {
    Q_OBJECT

  public:
    ApplicationShutdownCoordinator(CompositionPreviewController& previewController,
                                   TaskUiBridge& taskUiBridge, QObject* parent = nullptr);

    // Optional. Must be set before beginShutdown(). The function returns every live native surface
    // in the application's workspace; an empty function (the CPU-only / no-native default) means
    // the surface half of the shutdown contract is already satisfied and behavior is unchanged. A
    // plain function seam (not a second QObject base) keeps the frozen moc vtable intact.
    using NativeSurfaceSource = std::function<std::vector<EditorNativeSurface*>()>;
    void setNativeSurfaceSource(NativeSurfaceSource source);

    [[nodiscard]] bool isShuttingDown() const noexcept;
    [[nodiscard]] bool nativeSurfaceRetirementSatisfied() const noexcept;
    [[nodiscard]] const std::string& nativeSurfaceRefusalDiagnostic() const noexcept;

  public slots:
    void beginShutdown();

  signals:
    void shutdownStarted();
    // Emitted only once BOTH runtime task quiescence AND native-surface retirement have been
    // observed. A retained/unproven surface keeps this un-emitted on purpose.
    void shutdownQuiescent();

  protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

  private:
    // FORMAL AMENDMENT 1 (S1-C): no shape in application_shutdown_tests.cpp reproduced a genuine
    // stuck shutdown (a solid layer's preview delivered then close, close while a preview is
    // in flight, close while playback is armed, close right after a completed frame export --
    // every one reaches shutdownQuiescent well within 2s). Per S1-C, this is a diagnostic, not a
    // force-quit fallback: if shutdownQuiescent still has not arrived 5s after beginShutdown(),
    // log the task bridge's outstanding task snapshots once so a real future occurrence is
    // self-explaining instead of silently unresponsive.
    void logStillShuttingDownDiagnostic() const;
    void beginNativeSurfaceRetirement();
    void publishQuiescenceIfReady();

    CompositionPreviewController& previewController_;
    TaskUiBridge& taskUiBridge_;
    NativeSurfaceSource nativeSurfaceSource_;
    NativeSurfaceRetirementGate surfaceRetirementGate_;
    std::string nativeSurfaceRefusalDiagnostic_;
    QTimer stuckShutdownDiagnosticTimer_;
    bool shuttingDown_ = false;
    bool taskQuiescence_ = false;
    bool surfaceRetirementComplete_ = false;
    bool quiescencePublished_ = false;
};

} // namespace bloom::ui
