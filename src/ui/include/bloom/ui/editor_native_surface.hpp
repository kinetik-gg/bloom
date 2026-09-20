#pragma once

// Optional, typed lifecycle interface for an editor widget that owns a live native GPU
// presentation surface. This mirrors the existing EditorChromeProvider discovery pattern: the host
// probes the hosted editor widget with one dynamic_cast and, on failure, uses the unchanged CPU
// path. There is deliberately no dynamic property, global registry, service locator, or event bus.
//
// Why this exists (measured, not assumed -- see the sibling lifecycle review's Qt probe):
//   * QWidget::createWindowContainer owns the QWindow; destroying the container frees the
//     VkSurfaceKHR synchronously with no owner proof.
//   * Reparenting the container OR an ancestor silently recreates the VkSurfaceKHR with no
//     SurfaceAboutToBeDestroyed event. hide()/show() alone is surface-stable.
// Therefore every host mutation that reparents or destroys the widget tree (editor replacement,
// split, close/collapse, root replace/restore) must first ask the editor to retire its native
// target and wait for a real SafeToMutate. This interface is that ask.
//
// Implementations are QWidgets (every editor widget is), so a host may guard the non-blocking
// callback with a QPointer<QObject>. The future ViewerEditor implements this through
// ViewerGpuPresenter::prepareForMutation(); see the README's adapter hook section.

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

class QWidget;

namespace bloom::ui {

class EditorNativeSurface {
  public:
    // Outcome of asking the editor to start retiring its native target.
    enum class PrepareOutcome : std::uint8_t {
        // No native target is live right now; the host may mutate synchronously.
        NoLiveTarget,
        // Retirement started; `completion` fires later with the same generation (never before this
        // call returns in a way the host cannot handle).
        RetirePending,
        // A retirement is already in flight for this editor, or the target is quarantined/unproven
        // and cannot be released. The host must not mutate anything.
        Refused,
    };

    struct PrepareResult final {
        // True only when the surface is genuinely Retired and may be reparented/destroyed.
        bool safeToMutate = false;
        std::string diagnostic;
    };

    // `generation` is the host's monotonic token, echoed so a stale completion can be discarded.
    using PrepareCallback = std::function<void(std::uint64_t generation, const PrepareResult&)>;

    virtual ~EditorNativeSurface() = default;

    [[nodiscard]] virtual bool hasLiveNativeTarget() const = 0;

    // Non-blocking. Must invoke `completion` exactly once with the same `generation`. A second call
    // while a retirement is pending must return Refused.
    virtual PrepareOutcome prepareNativeSurfaceMutation(std::uint64_t generation,
                                                        PrepareCallback completion) = 0;

    // Called after the host has finished its mutation attempt (committed OR aborted) for every
    // target that retired and survived, so it can re-attach with a fresh target. Never called for a
    // destroyed editor. Implementations must tolerate being resumed in an unchanged tree (the
    // all-or-nothing abort path).
    virtual void resumeNativeSurfaceAfterMutation() = 0;

    // Human-readable state for an honest shutdown refusal diagnostic.
    [[nodiscard]] virtual std::string nativeSurfaceDiagnostic() const = 0;
};

// The single shared probe: returns the optional lifecycle interface for a hosted editor widget, or
// nullptr for the CPU-only editors (the common case). Defined in native_surface_retirement.cpp so
// QWidget is complete at the cast.
[[nodiscard]] EditorNativeSurface* editorNativeSurface(QWidget* editorWidget) noexcept;

} // namespace bloom::ui
