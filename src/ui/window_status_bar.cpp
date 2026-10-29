#include <bloom/ui/kit/controls.hpp>
#include <bloom/ui/window_status_bar.hpp>
#include <algorithm>
#include <cmath>
#include <memory>

#include <bloom/media/cache/media_disk_cache.hpp>
#include <bloom/runtime/memory_budget_ledger.hpp>
#include <bloom/runtime/operation_cache.hpp>
#include <bloom/ui/composition_preview_controller.hpp>
#include <bloom/ui/composition_session.hpp>
#include <bloom/ui/kit/painting.hpp>
#include <bloom/ui/kit/tokens.hpp>
#include <bloom/ui/preview_frame_cache.hpp>
#include <bloom/ui/viewer_editor.hpp>

#include <QCoreApplication>
#include <QFontMetrics>
#include <QHBoxLayout>
#include <QLabel>
#include <QMouseEvent>
#include <QPaintEvent>
#include <QPainter>
#include <QTimer>

namespace bloom::ui {
namespace {

// A notice is gone five seconds after it appears. Long enough to read a sentence, short enough that
// a stale notice never sits beside live state and gets read as current.
constexpr int kTransientMessageMs = 5'000;

// Mebibytes, one decimal place: the cache budget is set in whole MiB, so a finer reading would
// imply a precision the setting itself does not have.
// Bytes with the unit that keeps the number readable: MB below one gibibyte, GB from there on,
// one decimal either way, so a machine-sized budget reads "45.3 GB" rather than "46370.2 MB".
[[nodiscard]] QString formatBytes(const std::size_t bytes) {
    constexpr double kMebibyte = 1024.0 * 1024.0;
    constexpr double kGibibyte = kMebibyte * 1024.0;
    const auto value = static_cast<double>(bytes);
    return value >= kGibibyte ? WindowStatusBar::tr("%1 GB").arg(value / kGibibyte, 0, 'f', 1)
                              : WindowStatusBar::tr("%1 MB").arg(value / kMebibyte, 0, 'f', 1);
}

class StatusColorChip final : public kit::KLabel {
  public:
    explicit StatusColorChip(QWidget* parent) : KLabel(QString{}, parent, kit::TypeRole::UiSmall) {
        setObjectName("windowStatusBarColorChip");
    }
    void setState(const PreviewColorState& state) {
        setText(state.text);
        setAccessibleName(state.text);
        setToolTip(state.text);
        setTone(state.colorToken);
    }
};

kit::KLabel* makeCell(const QString& objectName, const kit::TypeRole role, const kit::Color ink,
                      QWidget* parent) {
    auto* label = new kit::KLabel(parent);
    label->setObjectName(objectName);
    label->setTypeRole(role);
    label->setTone(ink);
    return label;
}

} // namespace

PreviewColorState previewColorState(const CompositionPreviewState& preview) {
    if (preview.activity == PreviewActivity::Failed &&
        (!preview.viewerDisplayState || !preview.frame)) {
        return {preview.message.isEmpty() ? WindowStatusBar::tr("Color state unavailable")
                                          : preview.message,
                kit::Color::Error};
    }
    const auto bufferView =
        preview.frame != nullptr ? preview.frame->displayBufferView() : std::nullopt;
    if (preview.frame != nullptr) {
        const auto provider = preview.frame->provenance().provider;
        // The resident arm keeps no CPU display buffer by construction (its pixels never crossed
        // the host boundary), so `bufferView` can never classify it and the reference fallback
        // below would falsely report the CPU "Reference (unqualified)" state. Read the immutable
        // provenance instead. A valid admission lease or an exact known owner-applied descriptor
        // identifies GPU pixels. Lease invalidation alone does not erase that applied metadata;
        // uncorrelated/blank displays remain unavailable and can never become CPU reference.
        if (provider == runtime::PreviewDisplayProvider::GpuResident) {
            const auto& resident = preview.frame->residentFrame();
            if (preview.ownerAppliedDisplay ||
                (resident != nullptr && resident->isDisplayValid())) {
                return {WindowStatusBar::tr("GPU resident"), kit::Color::Ok};
            }
            return {WindowStatusBar::tr("Color state unavailable"), kit::Color::Warn};
        }
        // GPU provenance is reported honestly on the existing color chip: a display-only frame the
        // fixed GPU Neutral operation produced says so, and the qualification report it carries is
        // PreviewOnly (RGB tolerance), so this never claims reference parity. CPU reference and
        // qualified-CPU frames keep their exact existing text.
        if (provider == runtime::PreviewDisplayProvider::GpuNeutral) {
            return {WindowStatusBar::tr("GPU display"), kit::Color::Ok};
        }
        if (provider == runtime::PreviewDisplayProvider::GpuPreparedPacked) {
            return {WindowStatusBar::tr("GPU-prepared / CPU-painted"), kit::Color::Ok};
        }
    }
    if (bufferView.has_value() && bufferView->isOcioQualified) {
        return {WindowStatusBar::tr("Qualified · Bloom Neutral"), kit::Color::Ok};
    }
    return {preview.frame ? WindowStatusBar::tr("Reference (unqualified)")
                          : WindowStatusBar::tr("No displayed frame"),
            kit::Color::Warn};
}

QString previewActivityText(const CompositionPreviewState& preview) {
    if (preview.viewerDisplayState && !preview.viewerStatusText.isEmpty())
        return preview.viewerStatusText;
    if (preview.viewerDisplayState && !preview.message.isEmpty())
        return preview.message;
    if (!preview.message.isEmpty() && (preview.activity == PreviewActivity::Failed ||
                                       preview.freshness != FrameFreshness::Current))
        return preview.message;
    const bool stale = preview.freshness == FrameFreshness::Stale;
    switch (preview.activity) {
    case PreviewActivity::Rendering:
        return stale ? WindowStatusBar::tr("Rendering current frame · Previous frame shown")
                     : WindowStatusBar::tr("Rendering current frame");
    case PreviewActivity::Ready:
        if (!preview.diagnostics.empty() && !preview.diagnostics.front().summary.empty())
            return QString::fromStdString(preview.diagnostics.front().summary);
        return WindowStatusBar::tr("Current frame ready");
    case PreviewActivity::Unsupported:
        return stale ? WindowStatusBar::tr("Preview unsupported · Previous frame shown")
                     : WindowStatusBar::tr("Preview unsupported");
    case PreviewActivity::Cancelled:
        return stale ? WindowStatusBar::tr("Preview cancelled · Previous frame shown")
                     : WindowStatusBar::tr("Preview cancelled");
    case PreviewActivity::Failed:
        return stale ? WindowStatusBar::tr("Preview failed · Previous frame shown")
                     : WindowStatusBar::tr("Preview failed");
    }
    return WindowStatusBar::tr("Preview unavailable");
}

kit::Color previewActivityColor(const CompositionPreviewState& preview) {
    switch (preview.activity) {
    case PreviewActivity::Rendering:
        return kit::Color::Accent;
    case PreviewActivity::Ready:
        return kit::Color::Ok;
    case PreviewActivity::Unsupported:
        return kit::Color::Warn;
    case PreviewActivity::Cancelled:
        return kit::Color::Muted;
    case PreviewActivity::Failed:
        return kit::Color::Error;
    }
    return kit::Color::Muted;
}

QString droppedFrameText(const CompositionPreviewController& previewController) {
    if (!previewController.isCountingDroppedFrames()) {
        return {};
    }
    return WindowStatusBar::tr("%1 dropped").arg(previewController.droppedFrameCount());
}

QString previewCacheText(const CompositionPreviewController& previewController) {
    const auto& progress = previewController.ramPreviewProgress();
    if (progress.has_value()) {
        return WindowStatusBar::tr("Caching %1/%2")
            .arg(progress->cachedFrames)
            .arg(progress->totalFrames);
    }
    // No run is caching. What is left to report is what the cache actually holds -- which is also
    // the only honest account of BACKGROUND cache fill available: BackgroundPreviewController
    // publishes no progress signal, so a "filling in the background" claim would be invented.
    const auto& cache = previewController.frameCache();
    if (cache.size() == 0) {
        return {};
    }
    return WindowStatusBar::tr("Cache %1 frames · %2 / %3")
        .arg(cache.size())
        .arg(formatBytes(cache.residentBytes()), formatBytes(cache.byteBudget()));
}

QString operationCacheText(const runtime::OperationCache& operationCache) {
    const auto statistics = operationCache.statistics();
    if (statistics.hits == 0 && statistics.misses == 0 && operationCache.retainedBytes() == 0)
        return {};
    return WindowStatusBar::tr("Ops %1 hits · %2 misses · %3 / %4")
        .arg(statistics.hits)
        .arg(statistics.misses)
        .arg(formatBytes(operationCache.retainedBytes()), formatBytes(operationCache.byteBudget()));
}

// CACHE-2 (docs/architecture/media-io.md "Disk cache"): kept as its own free function, separate
// hunk and separate cell from previewCacheText() above -- the RAM preview cell reports the
// in-memory frame cache, this one the on-disk decoded-frame store, and the two are independent
// budgets an artist can reason about separately.
QString mediaDiskCacheStatusText(const media::cache::MediaDiskCacheStatistics& stats,
                                 const bool enabled) {
    if (!enabled) {
        return WindowStatusBar::tr("Disk cache off");
    }
    const auto total = stats.hits + stats.misses;
    if (total == 0 && stats.entryCount == 0) {
        return {};
    }
    return WindowStatusBar::tr("Disk %1% hit · %2")
        .arg(static_cast<int>(std::lround(stats.hitRate() * 100.0)))
        .arg(formatBytes(stats.storedBytes));
}

WindowStatusBar::WindowStatusBar(CompositionSession& session,
                                 CompositionPreviewController* const previewController,
                                 media::cache::MediaDiskCache* const mediaDiskCache,
                                 runtime::OperationCache* const operationCache, QWidget* parent,
                                 runtime::MemoryBudgetLedger& ledger)
    : kit::KSurface(parent), memoryLedger_(ledger), session_(session),
      previewController_(previewController), operationCache_(operationCache),
      mediaDiskCache_(mediaDiskCache) {
    setObjectName(QStringLiteral("windowStatusBar"));
    setAccessibleName(tr("Application status"));
    setFixedHeight(kit::px(kit::Size::Control));

    auto* layout = new QHBoxLayout(this);
    layout->setContentsMargins(kit::px(kit::Spacing::S), 0, kit::px(kit::Spacing::S), 0);
    layout->setSpacing(kit::px(kit::Spacing::M));

    auto* chip = new StatusColorChip(this);
    colorChip_ = chip;
    previewState_ = makeCell(QStringLiteral("windowStatusBarPreviewState"), kit::TypeRole::UiSmall,
                             kit::Color::Muted, this);
    droppedFrames_ = makeCell(QStringLiteral("windowStatusBarDroppedFrames"),
                              kit::TypeRole::UiSmall, kit::Color::Warn, this);
    video_ = makeCell(QStringLiteral("windowStatusBarVideo"), kit::TypeRole::UiSmall,
                      kit::Color::Muted, this);
    video_->setAccessibleName(tr("Video decode"));
    video_->hide();
    cache_ = makeCell(QStringLiteral("windowStatusBarCache"), kit::TypeRole::UiSmall,
                      kit::Color::Muted, this);
    // CACHE-2: the on-disk decoded-frame cache's own cell, kept as its own makeCell() call rather
    // than folded into `cache_` above so the two budgets stay independently readable.
    mediaDiskCacheCell_ = makeCell(QStringLiteral("windowStatusBarMediaDiskCache"),
                                   kit::TypeRole::UiSmall, kit::Color::Muted, this);
    probe_ = makeCell(QStringLiteral("windowStatusBarProbe"), kit::TypeRole::UiSmall,
                      kit::Color::Muted, this);
    probe_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
    probe_->installEventFilter(this);
    probe_->hide();
    message_ = makeCell(QStringLiteral("windowStatusBarMessage"), kit::TypeRole::UiSmall,
                        kit::Color::Foreground, this);
    message_->setAccessibleName(tr("Status message"));
    version_ = makeCell(QStringLiteral("windowStatusBarVersion"), kit::TypeRole::UiSmall,
                        kit::Color::Faint, this);
    version_->setAccessibleName(tr("Bloom version"));
    version_->setText(QCoreApplication::applicationVersion());

    layout->addWidget(version_);
    layout->addWidget(colorChip_);
    layout->addWidget(previewState_);
    layout->addWidget(droppedFrames_);
    layout->addWidget(video_);
    layout->addWidget(cache_);
    // CACHE-2: placed right beside the RAM preview cache cell it complements.
    layout->addWidget(mediaDiskCacheCell_);
    layout->addWidget(probe_, 1);
    layout->addWidget(message_, 1);
    exportCancel_ = new kit::KMenuButton(this);
    exportCancel_->setObjectName(QStringLiteral("windowStatusBarCancelExport"));
    exportCancel_->setText(tr("Cancel"));
    exportCancel_->setAccessibleName(tr("Cancel export"));
    exportCancel_->setToolTip(tr("Cancel export"));
    exportCancel_->hide();
    layout->addWidget(exportCancel_);
    connect(exportCancel_, &QToolButton::clicked, this, &WindowStatusBar::cancelExportRequested);

    transientTimer_ = new QTimer(this);
    transientTimer_->setSingleShot(true);
    transientTimer_->setInterval(kTransientMessageMs);
    connect(transientTimer_, &QTimer::timeout, this, &WindowStatusBar::clearTransientMessage);

    // CACHE-2: the disk cache has no Qt signal of its own (it is a Qt-free module shared with the
    // runtime evaluator), so a light poll is what keeps this cell honest without threading a
    // notification channel through a module that otherwise never depends on Qt. Five seconds
    // matches this bar's other non-urgent cadence (the transient-message lifetime above); a hit
    // rate does not need sub-second freshness.
    refreshMediaDiskCacheCell();
    mediaDiskCacheTimer_ = new QTimer(this);
    mediaDiskCacheTimer_->setInterval(5'000);
    connect(mediaDiskCacheTimer_, &QTimer::timeout, this,
            &WindowStatusBar::refreshMediaDiskCacheCell);
    // One application-owned worker sampler supplies every window; the status surface only
    // observes the cached state on its existing coarse reporting cadence.
    connect(mediaDiskCacheTimer_, &QTimer::timeout, this, &WindowStatusBar::observeMemoryBudget);
    mediaDiskCacheTimer_->start();
    memoryReserveBytes_ = memoryLedger_.reserveByteBudget();

    // A refused command is a notice, not a dialog: the artist asked for something the document
    // could not do, the command did not run, and the reason belongs where every other notice is.
    connect(&session_, &CompositionSession::commandRejected, this,
            [this](const QString& reason) { showTransientMessage(reason); });
    // A blank project (no active composition) has no colour or preview truth to report, so the
    // chip/cells switch to a neutral status the moment a composition is created or removed.
    connect(&session_, &CompositionSession::compositionChanged, this,
            &WindowStatusBar::refreshPreviewCells);

    if (operationCache_ != nullptr) {
        cacheRefreshTimer_ = new QTimer(this);
        cacheRefreshTimer_->setInterval(250);
        connect(cacheRefreshTimer_, &QTimer::timeout, this, &WindowStatusBar::refreshPreviewCells);
        cacheRefreshTimer_->start();
    }

    if (previewController_ != nullptr) {
        connect(previewController_, &CompositionPreviewController::stateChanged, this,
                &WindowStatusBar::refreshPreviewCells);
        connect(previewController_, &CompositionPreviewController::droppedFrameCountChanged, this,
                &WindowStatusBar::refreshPreviewCells);
        connect(previewController_, &CompositionPreviewController::ramPreviewProgressChanged, this,
                &WindowStatusBar::refreshPreviewCells);
        connect(&previewController_->frameCache(), &PreviewFrameCache::contentsChanged, this,
                &WindowStatusBar::refreshPreviewCells);
        connect(&previewController_->frameCache(), &PreviewFrameCache::byteBudgetChanged, this,
                &WindowStatusBar::refreshPreviewCells);
    }
    if (operationCache_ != nullptr) {
        cache_->setAccessibleName(tr("Cache statistics"));
    }
    refreshMemoryToolTip();
    refreshPreviewCells();
    refreshMessage();
}

void WindowStatusBar::observeMemoryBudget() {
    const auto state = memoryLedger_.state();
    memoryReserveBytes_ = memoryLedger_.reserveByteBudget();
    if (state.sampleSerial != observedMemorySerial_) {
        observedMemorySerial_ = state.sampleSerial;
        reportMemoryState(state);
    } else {
        refreshMemoryToolTip();
    }
}

void WindowStatusBar::pollMemoryPressureForTest(const std::size_t availableBytes) {
    runtime::MachineMemorySample sample;
    if (availableBytes != 0)
        sample.availableBytes = availableBytes;
    applyMemoryPressure(sample, runtime::MemoryBudgetLedger::Clock::now());
}

void WindowStatusBar::pollMemoryPressureForTest(
    runtime::MachineMemorySample sample, runtime::MemoryBudgetLedger::Clock::time_point now) {
    applyMemoryPressure(sample, now);
}

void WindowStatusBar::applyMemoryPressure(runtime::MachineMemorySample sample,
                                          runtime::MemoryBudgetLedger::Clock::time_point now) {
    reportMemoryState(memoryLedger_.poll(sample, now));
}

void WindowStatusBar::reportMemoryState(const runtime::MemoryBudgetState& state) {
    if (state.memoryNotice)
        showTransientMessage(tr("Cache budget reduced by system memory limit"));
    if (state.swapNotice) {
        if (state.memoryNotice)
            pendingSwapNotice_ = true;
        else
            showTransientMessage(tr("Swap usage is increasing"));
    }
    refreshMemoryToolTip();
    refreshPreviewCells();
}

void WindowStatusBar::refreshMemoryToolTip() {
    const auto state = memoryLedger_.state();
    const auto available = state.machine.availableBytes ? formatBytes(*state.machine.availableBytes)
                                                        : tr("Unavailable");
    QString reason;
    if (state.awaitingProcessSample) {
        reason = tr("Checking system memory limits…");
    } else if (state.processCacheCeilingBytes) {
        reason = tr("A system memory limit of %1 applies to combined cache budgets.")
                     .arg(formatBytes(*state.processCacheCeilingBytes));
        if (!state.processSampleFresh)
            reason +=
                tr(" The last verified limit remains in effect until it can be checked again.");
    } else if (state.process.status == runtime::ProcessMemoryStatus::NoFiniteVisibleLimit) {
        reason = tr("No system memory limit was found.");
    } else {
        reason = tr("System memory limits are unavailable on this system.");
    }
    QString current;
    if (previewController_ != nullptr)
        current =
            tr("RAM preview: %1").arg(formatBytes(previewController_->frameCache().byteBudget()));
    if (operationCache_ != nullptr) {
        if (!current.isEmpty())
            current += tr(" · ");
        current += tr("Operation cache: %1").arg(formatBytes(operationCache_->byteBudget()));
    }
    if (current.isEmpty())
        current = tr("No active caches");
    auto tooltip = tr("RAM preview and operation-cache statistics.\n"
                      "Current cache budget: %1\n"
                      "Requested cache total: %2 · Available memory: %3\n%4")
                       .arg(current, formatBytes(state.configuredBytes), available, reason);
    if (previewController_ != nullptr) {
        if (const auto& progress = previewController_->ramPreviewProgress(); progress) {
            tooltip += tr("\n\n%1 retained of %2 requested\n%3 prepared this run\n"
                          "Longest cached span: %4 frames")
                           .arg(progress->cachedFrames)
                           .arg(progress->totalFrames)
                           .arg(progress->preparedFrames)
                           .arg(progress->playableFrames);
        }
    }
    cache_->setToolTip(tooltip);
}

QString WindowStatusBar::cacheToolTipForTest() const { return cache_->toolTip(); }

QString WindowStatusBar::videoTextForTest() const { return video_->text(); }
QString WindowStatusBar::videoToolTipForTest() const { return video_->toolTip(); }

void WindowStatusBar::refreshVideoCell() {
    const auto* composition = session_.composition();
    std::vector<runtime::VideoSourceReport> reports;
    if (operationCache_ != nullptr && composition != nullptr)
        reports = operationCache_->videoSourceReports();
    // Only the sources the active composition's Video nodes read.
    const auto& project = session_.snapshot().project();
    std::vector<std::pair<std::string, QString>> sources;
    if (composition != nullptr) {
        for (const auto& node : composition->graph().nodes()) {
            if (node.typeId != "bloom.video-source")
                continue;
            for (const auto& binding : node.parameters) {
                if (binding.role != "asset")
                    continue;
                const auto stored = session_.constantStringValue(binding.parameterId);
                bool numeric = false;
                const auto raw = stored ? stored->toULongLong(&numeric) : 0;
                if (!numeric)
                    continue;
                const auto* asset = project.findAsset(
                    document::AssetId::fromRaw(static_cast<std::uint64_t>(raw)));
                if (asset == nullptr)
                    continue;
                const auto digest = asset->contentDigest.toLowercaseHex();
                sources.emplace_back(std::string(digest.begin(), digest.end()),
                                     QString::fromStdString(asset->name));
            }
        }
    }
    QString text;
    QString note;
    QStringList lines;
    for (const auto& report : reports) {
        const auto found = std::ranges::find(sources, report.assetDigest,
                                             &std::pair<std::string, QString>::first);
        if (found == sources.end())
            continue;
        const auto decode = QString::fromStdString(report.decode);
        if (text.isEmpty())
            text = tr("Video: %1").arg(decode);
        auto line = tr("%1: %2 decode").arg(found->second, decode);
        if (!report.colourNote.empty()) {
            const auto colour = QString::fromStdString(report.colourNote);
            if (note.isEmpty())
                note = colour;
            line += tr(" · ") + colour;
        }
        lines.push_back(line);
    }
    if (!note.isEmpty())
        text += tr(" · ") + note;
    video_->setText(text);
    video_->setToolTip(lines.join(QLatin1Char('\n')));
    video_->setTone(note.isEmpty() ? kit::Color::Muted : kit::Color::Warn);
    video_->setVisible(!text.isEmpty());
}

void WindowStatusBar::refreshPreviewCells() {
    refreshMemoryToolTip();
    refreshVideoCell();
    if (!displayedViewer_)
        refreshProbeCell({});
    if (previewController_ == nullptr) {
        static_cast<StatusColorChip*>(colorChip_)
            ->setState({tr("Color state unavailable"), kit::Color::Warn});
        previewState_->clear();
        droppedFrames_->clear();
        cache_->setText(operationCache_ == nullptr ? QString{}
                                                   : operationCacheText(*operationCache_));
        return;
    }
    if (session_.composition() == nullptr) {
        // No active composition: there is no colour or preview truth to report, and the ordinary
        // "Reference (unqualified)" / "Preview unsupported" cells would read as a failure of a
        // project that simply has not been authored yet. Show the neutral no-composition status and
        // omit the preview/colour badges until a composition exists.
        static_cast<StatusColorChip*>(colorChip_)
            ->setState({tr("No composition"), kit::Color::Muted});
        previewState_->clear();
        droppedFrames_->clear();
        cache_->setText(operationCache_ == nullptr ? QString{}
                                                   : operationCacheText(*operationCache_));
        return;
    }
    const auto preview =
        displayedViewer_ ? displayedViewer_->displayedPreviewState() : CompositionPreviewState{};
    static_cast<StatusColorChip*>(colorChip_)
        ->setState(displayedViewer_
                       ? previewColorState(preview)
                       : PreviewColorState{tr("No visible viewer"), kit::Color::Muted});

    previewState_->setText(displayedViewer_ ? previewActivityText(preview) : QString{});
    previewState_->setTone(preview.activity == PreviewActivity::Failed ? kit::Color::Error
                                                                       : kit::Color::Muted);
    previewState_->setToolTip(displayedViewer_ ? preview.message : QString{});

    const QString dropped = droppedFrameText(*previewController_);
    droppedFrames_->setText(dropped);
    droppedFrames_->setTone(previewController_->droppedFrameCount() == 0 ? kit::Color::Muted
                                                                         : kit::Color::Warn);

    const auto previewText = previewCacheText(*previewController_);
    const auto operationText =
        operationCache_ == nullptr ? QString{} : operationCacheText(*operationCache_);
    cache_->setText(previewText.isEmpty()     ? operationText
                    : operationText.isEmpty() ? previewText
                                              : previewText + tr(" · ") + operationText);
}

void WindowStatusBar::setDisplayedViewer(ViewerEditor* viewer) {
    if (displayedViewer_ != viewer) {
        QObject::disconnect(displayedViewerConnection_);
        QObject::disconnect(displayedViewerProbeConnection_);
        QObject::disconnect(displayedViewerDestroyedConnection_);
        displayedViewer_ = viewer;
        refreshProbeCell({});
        if (viewer) {
            displayedViewerConnection_ = connect(viewer, &ViewerEditor::displayedStateChanged, this,
                                                 &WindowStatusBar::refreshPreviewCells);
            displayedViewerProbeConnection_ = connect(viewer, &ViewerEditor::probeChanged, this,
                                                      &WindowStatusBar::refreshProbeCell);
            displayedViewerDestroyedConnection_ =
                connect(viewer, &QObject::destroyed, this, [this] {
                    refreshProbeCell({});
                    refreshPreviewCells();
                });
        }
    }
    refreshPreviewCells();
}

// CACHE-2: independent of refreshPreviewCells() above -- the disk cache is unrelated to the
// preview pipeline and stays live even in a window built without one (previewController_ null).
void WindowStatusBar::refreshMediaDiskCacheCell() {
    mediaDiskCacheCell_->setText(mediaDiskCacheStatusText(
        mediaDiskCache_ != nullptr ? mediaDiskCache_->statistics()
                                   : media::cache::MediaDiskCacheStatistics{},
        mediaDiskCache_ != nullptr && mediaDiskCache_->enabled()));
}

void WindowStatusBar::showTransientMessage(const QString& message) {
    transientMessage_ = message;
    refreshMessage();
    if (message.isEmpty()) {
        transientTimer_->stop();
        return;
    }
    transientTimer_->start();
}

void WindowStatusBar::clearTransientMessage() {
    transientTimer_->stop();
    transientMessage_.clear();
    refreshMessage();
    if (pendingSwapNotice_) {
        pendingSwapNotice_ = false;
        showTransientMessage(tr("Swap usage is increasing"));
    }
}

void WindowStatusBar::setPersistentMessage(const QString& message) {
    persistentMessage_ = message;
    refreshMessage();
}

void WindowStatusBar::refreshMessage() {
    message_->setText(transientMessage_.isEmpty() ? persistentMessage_ : transientMessage_);
}

QString WindowStatusBar::colorChipTextForTest() const {
    return static_cast<const StatusColorChip*>(colorChip_)->text();
}

QString WindowStatusBar::previewStateTextForTest() const { return previewState_->text(); }

QString WindowStatusBar::droppedFrameTextForTest() const { return droppedFrames_->text(); }

QString WindowStatusBar::cacheTextForTest() const { return cache_->text(); }

QString WindowStatusBar::mediaDiskCacheTextForTest() const { return mediaDiskCacheCell_->text(); }

void WindowStatusBar::setExportActive(bool active) { exportCancel_->setVisible(active); }

QString WindowStatusBar::messageTextForTest() const { return message_->text(); }

QString WindowStatusBar::versionTextForTest() const { return version_->text(); }

ViewerEditor* WindowStatusBar::displayedViewerForTest() const noexcept { return displayedViewer_; }

void WindowStatusBar::refreshProbeCell(const ProbeReadout& readout) {
    probeReadout_ = readout;
    if (!readout.valid) {
        static_cast<kit::KLabel*>(probe_)->setElidedText({});
        probe_->setAccessibleName({});
        probe_->hide();
        return;
    }
    const auto rgba = [](core::Color4d value) {
        return QStringLiteral("%1 %2 %3 %4")
            .arg(value.red, 0, 'g', 7)
            .arg(value.green, 0, 'g', 7)
            .arg(value.blue, 0, 'g', 7)
            .arg(value.alpha, 0, 'g', 7);
    };
    const auto rgb = [](core::Color4d value) {
        return QStringLiteral("%1 %2 %3")
            .arg(value.red, 0, 'g', 7)
            .arg(value.green, 0, 'g', 7)
            .arg(value.blue, 0, 'g', 7);
    };
    const auto working = readout.working                ? rgba(*readout.working)
                         : readout.pending              ? tr("Sampling…")
                         : readout.diagnostic.isEmpty() ? tr("Unavailable")
                                                        : readout.diagnostic;
    const auto display = probeFloatFormat_ ? (readout.displayLinear ? rgb(*readout.displayLinear)
                                                                    : tr("Unavailable"))
                                           : QStringLiteral("%1 %2 %3")
                                                 .arg(readout.displayEncoded.red)
                                                 .arg(readout.displayEncoded.green)
                                                 .arg(readout.displayEncoded.blue);
    const auto text = tr("(%1, %2)  W: %3 · D: %4")
                          .arg(readout.coordinate.x, 0, 'f', 2)
                          .arg(readout.coordinate.y, 0, 'f', 2)
                          .arg(working, display);
    probe_->show();
    static_cast<kit::KLabel*>(probe_)->setElidedText(text);
    probe_->setAccessibleName(text);
    probe_->setToolTip(tr("Working space: %1\nDisplay: %2\nCtrl-click to show %3 values")
                           .arg(readout.workingColorSpaceId, readout.displayName,
                                probeFloatFormat_ ? tr("float pre-encode") : tr("8-bit encoded")));
}

bool WindowStatusBar::eventFilter(QObject* watched, QEvent* event) {
    if (watched == probe_ && event->type() == QEvent::MouseButtonPress) {
        const auto* mouse = static_cast<const QMouseEvent*>(event);
        if (mouse->button() == Qt::LeftButton && mouse->modifiers().testFlag(Qt::ControlModifier)) {
            probeFloatFormat_ = !probeFloatFormat_;
            refreshProbeCell(probeReadout_);
            return true;
        }
    }
    return KSurface::eventFilter(watched, event);
}

} // namespace bloom::ui
