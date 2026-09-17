#pragma once

#include <bloom/runtime/operation_cache.hpp>
#include <bloom/ui/kit/tokens.hpp>
#include <bloom/ui/viewer_editor_probe.hpp>

#include <QString>
#include <bloom/ui/kit/surfaces.hpp>

class QLabel;
class QTimer;

namespace bloom::media::cache {
class MediaDiskCache;
struct MediaDiskCacheStatistics;
} // namespace bloom::media::cache

namespace bloom::ui {

class CompositionPreviewController;
class CompositionSession;
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
// PreviewActivity::Failed overrides to Error and shows the controller's own fail-closed diagnostic
// ("Any state other than Ready is fail-closed for qualified processing"): every Failed case this
// codebase produces reaches that state with an already-unqualified retained frame, or none at all.
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
                    runtime::OperationCache* operationCache = nullptr, QWidget* parent = nullptr);

    // A notice that clears itself after five seconds -- a rejected command, an export that
    // finished, a cancellation. It takes precedence over the persistent message while it lasts.
    void showTransientMessage(const QString& message);
    void clearTransientMessage();
    // A message that stays until it is replaced or cleared: "Saving…", "Opening…", export range
    // progress. Passing an empty string clears it.
    void setPersistentMessage(const QString& message);

    // Test/diagnostic surface only, mirroring ViewerEditor's own *ForTest precedent.
    [[nodiscard]] QString colorChipTextForTest() const;
    [[nodiscard]] QString previewStateTextForTest() const;
    [[nodiscard]] QString droppedFrameTextForTest() const;
    [[nodiscard]] QString cacheTextForTest() const;
    [[nodiscard]] QString mediaDiskCacheTextForTest() const;
    [[nodiscard]] QString messageTextForTest() const;
    [[nodiscard]] QString versionTextForTest() const;

  protected:
  private:
    void refreshPreviewCells();
    void refreshProbeCell(const ProbeReadout& readout);
    void refreshMediaDiskCacheCell();
    void refreshMessage();

    CompositionSession& session_;
    CompositionPreviewController* previewController_ = nullptr;
    runtime::OperationCache* operationCache_ = nullptr;
    media::cache::MediaDiskCache* mediaDiskCache_ = nullptr;
    QWidget* colorChip_ = nullptr;
    QLabel* previewState_ = nullptr;
    QLabel* droppedFrames_ = nullptr;
    QLabel* cache_ = nullptr;
    QLabel* mediaDiskCacheCell_ = nullptr;
    QLabel* message_ = nullptr;
    QLabel* version_ = nullptr;
    QLabel* probe_ = nullptr;
    QTimer* transientTimer_ = nullptr;
    QTimer* cacheRefreshTimer_ = nullptr;
    QTimer* mediaDiskCacheTimer_ = nullptr;
    QString transientMessage_;
    QString persistentMessage_;
};

} // namespace bloom::ui
