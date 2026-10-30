#pragma once

#include <bloom/runtime/operation_cache.hpp>
#include <bloom/ui/kit/tokens.hpp>
#include <bloom/ui/viewer_editor_probe.hpp>

#include <bloom/ui/compile_failure.hpp>

#include <QPointer>
#include <QString>

#include <optional>

#include <bloom/ui/kit/surfaces.hpp>
#include <cstddef>

class QLabel;
class QTimer;
class QToolButton;

namespace bloom::media::cache {
class MediaDiskCache;
struct MediaDiskCacheStatistics;
} // namespace bloom::media::cache

namespace bloom::ui::kit {
class KButton;
class KLabel;
class KPopupWindow;
}

namespace bloom::ui {

class CompositionPreviewController;
class CompositionSession;
class McpServerAdapter;
class ViewerEditor;
struct CompositionPreviewState;

// The application's one persistent reporting surface (task VIEW-1). Anything the artist needs to
// know that is NOT a property of the frame they are looking at belongs here rather than painted
// over the canvas: the colour-management state, how ready the preview is, how many frames playback
// dropped, what the cache holds, a transient notice, and the version of Bloom saying all of it.
//
// Not a QStatusBar. QMainWindow's status bar brings its own chrome, its own size grip, and its own
// item model, none of which the Kinetik language wants; this is a kit strip laid out under the
// workspace with the same Surface/hairline treatment every other Bloom bar has.

// The colour-state chip's text and palette role, resolved from the CURRENTLY DISPLAYED frame's own
// isOcioQualified() bit -- never from the request activity -- so a retained qualified frame keeps
// reading as qualified while a later, unrelated request is in flight. That is the "never a silent
// relabel" half of the contract in docs/architecture/color-management.md; the other half is that
// this chip is always on screen, which is what moving it from the viewer footer to the window
// status bar guarantees regardless of which panels an artist has open.
//
// A failed replacement reports an error separately while preserving a retained frame's provenance.
struct PreviewColorState final {
    QString text;
    kit::Color colorToken = kit::Color::Warn;
};

[[nodiscard]] PreviewColorState previewColorState(const CompositionPreviewState& preview);

// "Current frame ready", "Rendering current frame · Previous frame shown", and so on: the readiness
// sentence the viewer canvas used to paint as a chip over the pixels.
[[nodiscard]] QString previewActivityText(const CompositionPreviewState& preview);
[[nodiscard]] kit::Color previewActivityColor(const CompositionPreviewState& preview);

// The dropped-frame count, EMPTY unless a playback run is counting: outside a run this reports
// nothing rather than a stale or invented figure. Zero dropped frames during a run still reads
// "0 dropped", because silence would mean "not measured", which is a different statement.
[[nodiscard]] QString droppedFrameText(const CompositionPreviewController& previewController);

// What the preview cache is doing: a RAM preview run's own progress while one is caching, and
// otherwise how much the cache holds -- which is the only measured account of background cache fill
// that exists, since BackgroundPreviewController publishes no progress of its own. Empty when the
// cache is empty and nothing is caching.
[[nodiscard]] QString previewCacheText(const CompositionPreviewController& previewController);
[[nodiscard]] QString operationCacheText(const runtime::OperationCache& operationCache);

// The disk cache's own cell (task CACHE-2, docs/architecture/media-io.md "Disk cache"), separate
// from `previewCacheText()`'s in-memory RAM preview account above: "Disk 73% hit · 4.2 GB" once
// it has served at least one lookup, "Disk cache off" when disabled, and empty (nothing to report
// yet) before the first lookup. A hit rate is never invented from zero lookups.
[[nodiscard]] QString mediaDiskCacheStatusText(const media::cache::MediaDiskCacheStatistics& stats,
                                               bool enabled);

class WindowStatusBar final : public kit::KSurface {
    Q_OBJECT

  public:
    // `previewController` may be null (a window built without a preview pipeline, as several tests
    // do): the preview-derived cells then stay empty rather than claiming anything.
    // `mediaDiskCache` may also be null (disk cache disabled/unavailable for this session): the
    // disk-cache cell then reports "Disk cache off" instead of polling.
    // `operationCache` may also be null: the combined cache cell then shows only the RAM preview
    // account, with no operation-cache statistics appended.
    WindowStatusBar(CompositionSession& session, CompositionPreviewController* previewController,
                    media::cache::MediaDiskCache* mediaDiskCache = nullptr,
                    runtime::OperationCache* operationCache = nullptr, QWidget* parent = nullptr,
                    runtime::MemoryBudgetLedger& ledger = runtime::processMemoryBudgetLedger());

    // A notice that clears itself after five seconds -- a rejected command, an export that
    // finished, a cancellation. It takes precedence over the persistent message while it lasts.
    void showTransientMessage(const QString& message);
    void clearTransientMessage();
    // A message that stays until it is replaced or cleared: "Saving…", "Opening…", export range
    // progress. Passing an empty string clears it.
    void setPersistentMessage(const QString& message);
    // MainWindow owns selection among visible viewers. Cache/preview preparation remains shared.
    void setDisplayedViewer(ViewerEditor* viewer);

  public:
    // Injected memory samples and a monotonic clock keep pressure, recovery and notice tests
    // independent of the host. The legacy byte-only overload treats zero as unavailable.
    void pollMemoryPressureForTest(std::size_t availableBytes);
    void pollMemoryPressureForTest(runtime::MachineMemorySample sample,
                                   runtime::MemoryBudgetLedger::Clock::time_point now);
    [[nodiscard]] QString cacheToolTipForTest() const;
    [[nodiscard]] std::size_t memoryReserveBytesForTest() const noexcept {
        return memoryReserveBytes_;
    }

    // The MCP indicator (ADR 0034): shown only while the server runs or has failed, and a click asks
    // the adapter to reveal the MCP Server panel. Null removes it.
    void setMcpServer(McpServerAdapter* adapter);
    // The compile-failure disclosure (docs/ux/ui-grammar.md, "Compile failure details"): a small
    // button after the preview state, shown only while the preview failed to compile. It opens a
    // popup anchored to this bar. The ForTest hooks drive the same code the viewer's state does.
    void applyPreviewStateForTest(const CompositionPreviewState& preview);
    void openCompileFailureForTest() { openCompileFailure(); }
    [[nodiscard]] bool compileDetailsVisibleForTest() const;
    [[nodiscard]] QWidget* compileFailurePopupForTest() const;
    [[nodiscard]] QString mcpIndicatorTextForTest() const;
    [[nodiscard]] bool mcpIndicatorVisibleForTest() const;

    // Test/diagnostic surface only, mirroring ViewerEditor's own *ForTest precedent.
    [[nodiscard]] QString colorChipTextForTest() const;
    [[nodiscard]] QString previewStateTextForTest() const;
    [[nodiscard]] QString droppedFrameTextForTest() const;
    [[nodiscard]] QString cacheTextForTest() const;
    [[nodiscard]] QString mediaDiskCacheTextForTest() const;
    [[nodiscard]] QString videoTextForTest() const;
    [[nodiscard]] QString videoToolTipForTest() const;
    // Re-reads the video decode reports now (the bar otherwise follows its 250 ms cadence).
    void refreshVideoCellForTest() { refreshVideoCell(); }
    [[nodiscard]] QString messageTextForTest() const;
    [[nodiscard]] QString versionTextForTest() const;
    [[nodiscard]] ViewerEditor* displayedViewerForTest() const noexcept;

  protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

  private:
    void refreshPreviewCells();
    void refreshProbeCell(const ProbeReadout& readout);
    void refreshMediaDiskCacheCell();
    void refreshMessage();
    void applyCompileFailure(std::optional<CompileFailure> failure);
    void openCompileFailure();
    void refreshMcpIndicator();
    // Observes the application-owned worker snapshot. This surface never samples the OS.
    void observeMemoryBudget();
    void reportMemoryState(const runtime::MemoryBudgetState& state);
    void applyMemoryPressure(runtime::MachineMemorySample sample,
                             runtime::MemoryBudgetLedger::Clock::time_point now);
    void refreshMemoryToolTip();
    // The active composition's video sources: the latest decode route (software, VA-API or the
    // software fallback and its reason) and any assumed-colour note, from the runtime's decode
    // reports. It is status, not an evaluation diagnostic, so cached frames stay cacheable.
    void refreshVideoCell();

    runtime::MemoryBudgetLedger& memoryLedger_;
    CompositionSession& session_;
    CompositionPreviewController* previewController_ = nullptr;
    QPointer<ViewerEditor> displayedViewer_;
    QMetaObject::Connection displayedViewerConnection_;
    QMetaObject::Connection displayedViewerProbeConnection_;
    QMetaObject::Connection displayedViewerDestroyedConnection_;
    runtime::OperationCache* operationCache_ = nullptr;
    media::cache::MediaDiskCache* mediaDiskCache_ = nullptr;
    QWidget* colorChip_ = nullptr;
    kit::KLabel* previewState_ = nullptr;
    kit::KLabel* droppedFrames_ = nullptr;
    kit::KLabel* video_ = nullptr;
    QLabel* cache_ = nullptr;
    QLabel* mediaDiskCacheCell_ = nullptr;
    QLabel* message_ = nullptr;
    kit::KButton* mcpIndicator_ = nullptr;
    kit::KButton* compileDetails_ = nullptr;
    std::optional<CompileFailure> compileFailure_;
    QPointer<kit::KPopupWindow> compileFailurePopup_;
    QPointer<McpServerAdapter> mcpServer_;
    QLabel* version_ = nullptr;
    QLabel* probe_ = nullptr;
    QTimer* transientTimer_ = nullptr;
    QTimer* cacheRefreshTimer_ = nullptr;
    QTimer* mediaDiskCacheTimer_ = nullptr;
    QString transientMessage_;
    QString persistentMessage_;
    // max(8 GiB, 40% of physical) -- what the memory ledger left to the rest of the machine, and
    // therefore the line below which the machine is short of memory rather than merely busy.
    std::size_t memoryReserveBytes_ = 0;
    // When swap starts the same episode, its distinct notice follows the ordinary trim notice.
    bool pendingSwapNotice_ = false;
    std::uint64_t observedMemorySerial_ = 0;
    ProbeReadout probeReadout_{};
    bool probeFloatFormat_ = false;
};

} // namespace bloom::ui
