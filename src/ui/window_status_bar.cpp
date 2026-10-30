#include <bloom/ui/mcp_server_text.hpp>

#include "window_status_bar_segments.hpp"

#include <bloom/ui/kit/button.hpp>
#include <bloom/ui/kit/progress_bar.hpp>
#include <bloom/ui/kit/controls.hpp>
#include <bloom/ui/kit/popup_window.hpp>
#include <bloom/ui/compile_failure.hpp>
#include <bloom/ui/mcp_server_adapter.hpp>
#include <bloom/ui/window_status_bar.hpp>
#include <algorithm>
#include <array>
#include <cmath>
#include <memory>

#include <bloom/host/project_color_choice.hpp>
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

using statusbar::formatBytes;

} // namespace

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

    buildSegments();

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
    // cells switch to a neutral status the moment a composition is created or removed.
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
    refreshMemoryToolTip();
    refreshPreviewCells();
    refreshMessage();
    refreshMcpIndicator();
}

// The bar is a row of labelled segments: a short Muted label in the UI font, then its value in the
// mono face at a fixed width, so a changing number never moves its neighbours. Detailed counters
// live in each segment's tooltip, never inline.
void WindowStatusBar::buildSegments() {
    using statusbar::ClickableValue;
    using statusbar::Segment;
    using statusbar::ValueLabel;
    auto* layout = new QHBoxLayout(this);
    layout->setContentsMargins(kit::px(kit::Spacing::S), 0, kit::px(kit::Spacing::S), 0);
    layout->setSpacing(kit::px(kit::Spacing::M));
    const auto separator = [this, layout] {
        auto* line = new kit::KSurface(this, kit::Color::Border);
        line->setObjectName(QStringLiteral("windowStatusBarSeparator"));
        line->setFixedSize(kit::px(kit::Size::Hairline), kit::px(kit::Size::StatusSeparatorHeight));
        layout->addWidget(line);
        separators_.push_back(line);
    };

    // Viewer: how the shown frame was produced and presented.
    viewerSegment_ = new Segment(QStringLiteral("windowStatusBarViewer"), this);
    auto* viewerTitle = viewerSegment_->addLabel(tr("Viewer"));
    viewerTitle->setTypeRole(kit::TypeRole::UiSmallStrong);
    viewerSegment_->addGap();
    viewerSegment_->addLabel(tr("Render"));
    renderPath_ = new ValueLabel(QStringLiteral("windowStatusBarColorChip"), QStringLiteral("GPU"),
                                 kit::Color::Muted, viewerSegment_);
    viewerSegment_->add(renderPath_);
    viewerSegment_->addGap();
    viewerSegment_->addLabel(tr("Exposure"));
    exposure_ = new ValueLabel(QStringLiteral("windowStatusBarExposure"), QStringLiteral("-00.00"),
                               kit::Color::Foreground, viewerSegment_);
    viewerSegment_->add(exposure_);
    viewerSegment_->addGap();
    viewerSegment_->addLabel(tr("Channel"));
    channel_ = new ValueLabel(QStringLiteral("windowStatusBarChannel"), QStringLiteral("RGBA"),
                              kit::Color::Foreground, viewerSegment_);
    viewerSegment_->add(channel_);
    viewerSegment_->addGap();
    viewerSegment_->addLabel(tr("Dropped"));
    droppedFrames_ = new ValueLabel(QStringLiteral("windowStatusBarDroppedFrames"),
                                    QStringLiteral("0000"), kit::Color::Muted, viewerSegment_);
    viewerSegment_->add(droppedFrames_);
    // Video decode: only while the composition reads video sources.
    videoGroup_ = new Segment(QStringLiteral("windowStatusBarVideoGroup"), viewerSegment_);
    auto* videoGroup = static_cast<Segment*>(videoGroup_);
    videoGroup->addGap();
    videoGroup->addLabel(tr("Decode"));
    video_ = new ValueLabel(QStringLiteral("windowStatusBarVideo"), QString{}, kit::Color::Muted,
                            videoGroup);
    video_->setAccessibleName(tr("Video decode"));
    videoGroup->add(video_);
    videoGroup_->hide();
    viewerSegment_->add(videoGroup_);
    viewerSegment_->setSegmentToolTip(
        tr("The Viewer's frame: whether the GPU or the CPU produced it, the exposure and channel "
           "it is shown with, and the frames playback dropped."));
    layout->addWidget(viewerSegment_);
    separator();

    // Activity: the current job or progress, or idle. The compile-failure Details button lives here.
    activitySegment_ = new Segment(QStringLiteral("windowStatusBarActivity"), this);
    activitySegment_->addLabel(tr("Activity"));
    activityText_ = new kit::KLabel(QString{}, this, kit::TypeRole::UiSmall);
    activityText_->setObjectName(QStringLiteral("windowStatusBarMessage"));
    activityText_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
    activityText_->setMinimumWidth(kit::px(kit::Size::StatusActivityMinWidth) / 2);
    activityText_->setAccessibleName(tr("Status message"));
    activityProgress_ = new kit::KProgressBar(activitySegment_);
    activityProgress_->setObjectName(QStringLiteral("windowStatusBarActivityProgress"));
    activityProgress_->setFixedWidth(kit::px(kit::Size::ViewerResolutionWidth));
    activityProgress_->hide();
    compileDetails_ = new kit::KButton(tr("Details"), this);
    compileDetails_->setObjectName(QStringLiteral("windowStatusBarCompileDetails"));
    compileDetails_->setVariant(kit::KButton::Variant::Secondary);
    compileDetails_->setControlSize(kit::KButton::ControlSize::Compact);
    compileDetails_->setAccessibleName(tr("Why the composition cannot render"));
    compileDetails_->hide();
    connect(compileDetails_, &QAbstractButton::clicked, this, [this] { openCompileFailure(); });
    activitySegment_->add(activityProgress_);
    activitySegment_->add(activityText_, 1);
    activitySegment_->add(compileDetails_);
    layout->addWidget(activitySegment_, 1);

    // Pixel: the Viewer's colour readout under the pointer; empty and hidden until it has one.
    probeSegment_ = new Segment(QStringLiteral("windowStatusBarPixel"), this);
    probeSegment_->addLabel(tr("Pixel"));
    probe_ = new kit::KLabel(QString{}, this, kit::TypeRole::Value);
    probe_->setObjectName(QStringLiteral("windowStatusBarProbe"));
    probe_->setTone(kit::Color::Muted);
    probe_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
    probe_->setMinimumWidth(kit::px(kit::Size::ViewerDisplayViewWidth));
    probe_->installEventFilter(this);
    probeSegment_->add(probe_);
    probeSegment_->hide();
    layout->addWidget(probeSegment_, 2);
    separator();

    // RAM: what the caches hold, against what they may hold; the frame count of the preview cache.
    memorySegment_ = new Segment(QStringLiteral("windowStatusBarMemory"), this);
    memorySegment_->addLabel(tr("RAM"));
    memory_ = new ValueLabel(QStringLiteral("windowStatusBarCache"),
                             QStringLiteral("000.0/000.0 GB"), kit::Color::Foreground,
                             memorySegment_);
    memory_->setAccessibleName(tr("Cache statistics"));
    memorySegment_->add(memory_);
    memorySegment_->addGap();
    memorySegment_->addLabel(tr("Cache"));
    memoryFrames_ = new ValueLabel(QStringLiteral("windowStatusBarCacheFrames"),
                                   QStringLiteral("00000 f"), kit::Color::Foreground,
                                   memorySegment_);
    memorySegment_->add(memoryFrames_);
    layout->addWidget(memorySegment_);
    separator();

    diskSegment_ = new Segment(QStringLiteral("windowStatusBarDisk"), this);
    diskSegment_->addLabel(tr("Disk cache"));
    mediaDiskCacheCell_ = new ValueLabel(QStringLiteral("windowStatusBarMediaDiskCache"),
                                         QStringLiteral("000% hit · 0000.0 MB"),
                                         kit::Color::Foreground, diskSegment_);
    diskSegment_->add(mediaDiskCacheCell_);
    layout->addWidget(diskSegment_);

    // MCP: text, only while the server runs (or has failed); a click opens the MCP Server panel.
    mcpSegment_ = new Segment(QStringLiteral("windowStatusBarMcpSegment"), this);
    mcpIndicator_ = new ClickableValue(QStringLiteral("windowStatusBarMcp"),
                                       QStringLiteral(":00000"), kit::Color::Muted, mcpSegment_);
    mcpIndicator_->setAccessibleName(tr("MCP server"));
    mcpIndicator_->setCursor(Qt::PointingHandCursor);
    mcpIndicator_->setFocusPolicy(Qt::TabFocus);
    mcpSegment_->addLabel(tr("MCP"));
    mcpSegment_->add(mcpIndicator_);
    mcpSegment_->hide();
    layout->addSpacing(kit::px(kit::Spacing::S));
    layout->addWidget(mcpSegment_);

    versionSegment_ = new Segment(QStringLiteral("windowStatusBarVersionSegment"), this);
    auto* versionLabel = versionSegment_->addLabel(tr("Version"));
    versionLabel->setTone(kit::Color::Faint);
    version_ = new ValueLabel(QStringLiteral("windowStatusBarVersion"), QString{},
                              kit::Color::Faint, versionSegment_);
    version_->setAccessibleName(tr("Bloom version"));
    version_->setText(QCoreApplication::applicationVersion());
    versionSegment_->add(version_);
    versionSegment_->setSegmentToolTip(tr("The version of Bloom that is running."));
    layout->addSpacing(kit::px(kit::Spacing::S));
    layout->addWidget(versionSegment_);

    diskSegment_->setSegmentToolTip(tr("The on-disk cache of decoded frames."));
    memorySegment_->setSegmentToolTip(tr("The memory the caches hold."));
}

QSize WindowStatusBar::minimumSizeHint() const {
    return {0, kit::px(kit::Size::Control)};
}

void WindowStatusBar::resizeEvent(QResizeEvent* event) {
    KSurface::resizeEvent(event);
    applyNarrowLayout();
}

// Whole segments collapse by priority (never their labels): the version first, then MCP, the disk
// cache, the pixel readout and last the RAM segment. Activity keeps its minimum width.
void WindowStatusBar::applyNarrowLayout() {
    const int gap = kit::px(kit::Spacing::M) + kit::px(kit::Size::Hairline);
    const int margins = kit::px(kit::Spacing::S) + kit::px(kit::Spacing::S);
    struct Entry {
        QWidget* widget;
        bool wanted;
    };
    const std::array<Entry, 5> byPriority{{{versionSegment_, true},
                                           {mcpSegment_, mcpWanted_},
                                           {diskSegment_, true},
                                           {probeSegment_, probeReadout_.valid},
                                           {memorySegment_, true}}};
    int needed = margins + viewerSegment_->sizeHint().width() + gap +
                 kit::px(kit::Size::StatusActivityMinWidth);
    std::array<int, 5> widths{};
    for (std::size_t index = 0; index < byPriority.size(); ++index) {
        if (byPriority[index].wanted) {
            widths[index] = byPriority[index].widget->sizeHint().width() + gap;
            needed += widths[index];
        }
    }
    std::array<bool, 5> shown{};
    for (std::size_t index = 0; index < byPriority.size(); ++index)
        shown[index] = byPriority[index].wanted;
    for (std::size_t index = 0; index < byPriority.size() && needed > width(); ++index) {
        if (!shown[index])
            continue;
        shown[index] = false;
        needed -= widths[index];
    }
    for (std::size_t index = 0; index < byPriority.size(); ++index)
        byPriority[index].widget->setVisible(shown[index]);
    // A separator stays only between two shown groups: hide the ones next to a collapsed segment.
    if (separators_.size() >= 3) {
        separators_[1]->setVisible(shown[4]);
        separators_[2]->setVisible(shown[4] && shown[2]);
    }
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
    auto tooltip = tr("The RAM the preview and operation caches hold, against their budgets.\n"
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
    // The detailed counters live here, not in the bar.
    if (previewController_ != nullptr) {
        const auto& frames = previewController_->frameCache();
        tooltip += tr("\n\nPreview cache: %1 frames · %2 of %3")
                       .arg(frames.size())
                       .arg(formatBytes(frames.residentBytes()), formatBytes(frames.byteBudget()));
    }
    if (operationCache_ != nullptr) {
        const auto statistics = operationCache_->statistics();
        tooltip += tr("\nOperation cache: %1 hits · %2 misses · %3 of %4")
                       .arg(statistics.hits)
                       .arg(statistics.misses)
                       .arg(formatBytes(operationCache_->retainedBytes()),
                            formatBytes(operationCache_->byteBudget()));
    }
    memoryToolTip_ = tooltip;
    memorySegment_->setSegmentToolTip(tooltip);
    memory_->setToolTip(tooltip);
}

QString WindowStatusBar::cacheToolTipForTest() const { return memoryToolTip_; }

QString WindowStatusBar::videoTextForTest() const { return videoText_; }
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
    videoText_ = text;
    // The value beside the "Decode" label is the route alone ("software"); the note rides after it.
    auto value = text;
    value.remove(0, std::min<qsizetype>(value.size(), tr("Video: ").size()));
    video_->setText(value);
    video_->setToolTip(lines.join(QLatin1Char('\n')));
    video_->setTone(note.isEmpty() ? kit::Color::Muted : kit::Color::Warn);
    videoGroup_->setVisible(!text.isEmpty());
}

void WindowStatusBar::setPreviewNotice(const CompositionPreviewState& preview) {
    previewStateText_ = previewActivityText(preview);
    previewNotice_.clear();
    previewNoticeTone_ = kit::Color::Muted;
    const bool stale = preview.freshness == FrameFreshness::Stale;
    switch (preview.activity) {
    case PreviewActivity::Rendering:
        previewNotice_ = stale ? tr("Rendering frame · previous frame shown") : tr("Rendering frame");
        previewNoticeTone_ = kit::Color::Accent;
        break;
    case PreviewActivity::Failed:
        previewNotice_ = previewStateText_;
        previewNoticeTone_ = kit::Color::Error;
        break;
    case PreviewActivity::Unsupported:
        previewNotice_ = previewStateText_;
        previewNoticeTone_ = kit::Color::Warn;
        break;
    case PreviewActivity::Cancelled:
        previewNotice_ = previewStateText_;
        break;
    case PreviewActivity::Ready:
        break;
    }
}

void WindowStatusBar::setViewerCells(const PreviewColorState& state,
                                     const CompositionPreviewState* preview) {
    colorStateText_ = state.text;
    renderPath_->setText(state.pathLabel.isEmpty() ? tr("—") : state.pathLabel);
    renderPath_->setTone(state.colorToken);
    renderPath_->setAccessibleName(state.text);
    renderPath_->setToolTip(tr("Render: how the shown frame was produced. %1.").arg(state.text));
    const bool shown = preview != nullptr && preview->viewerReadout.valid;
    const auto dim = shown && preview->viewerReadout.current ? kit::Color::Foreground
                                                             : kit::Color::Muted;
    exposure_->setText(shown ? QString::number(preview->viewerReadout.exposure, 'f', 2) : tr("—"));
    exposure_->setTone(dim);
    exposure_->setToolTip(
        shown ? tr("Exposure: the view adjustment the Viewer shows, in stops. %1")
                    .arg(previewStateText_)
              : tr("Exposure: the view adjustment the Viewer shows, in stops. No frame is shown."));
    channel_->setText(shown ? preview->viewerReadout.channel : tr("—"));
    channel_->setTone(dim);
    channel_->setToolTip(tr("Channel: which channels of the frame the Viewer shows."));
}

void WindowStatusBar::refreshActivity() {
    QString text = transientMessage_.isEmpty() ? persistentMessage_ : transientMessage_;
    auto tone = kit::Color::Foreground;
    bool progress = false;
    QString tip = tr("What Bloom is doing now: a saved or exported file, an import, a RAM preview "
                     "run, or a problem with the preview.");
    if (text.isEmpty() && previewController_ != nullptr) {
        if (const auto& run = previewController_->ramPreviewProgress(); run.has_value()) {
            text = tr("Caching frames for RAM preview · %1 of %2")
                       .arg(run->cachedFrames)
                       .arg(run->totalFrames);
            activityProgress_->setProgress(run->cachedFrames, run->totalFrames);
            progress = true;
            tone = kit::Color::Accent;
        }
    }
    if (text.isEmpty() && !previewNotice_.isEmpty()) {
        text = previewNotice_;
        tone = previewNoticeTone_;
        tip = tr("The preview's state. %1").arg(previewStateText_);
    }
    const bool idle = text.isEmpty();
    if (idle) {
        text = tr("Idle");
        tone = kit::Color::Faint;
    }
    activityProgress_->setVisible(progress);
    activityText_->setTone(tone);
    activityText_->setElidedText(text);
    activityText_->setAccessibleName(idle ? tr("Status message") : text);
    activitySegment_->setSegmentToolTip(idle ? tip : tip + QLatin1Char('\n') + text);
}

void WindowStatusBar::refreshMemoryCell() {
    std::size_t resident = 0;
    std::size_t budget = 0;
    bool any = false;
    if (previewController_ != nullptr) {
        resident += previewController_->frameCache().residentBytes();
        budget += previewController_->frameCache().byteBudget();
        any = true;
    }
    if (operationCache_ != nullptr) {
        resident += operationCache_->retainedBytes();
        budget += operationCache_->byteBudget();
        any = true;
    }
    constexpr double kGibibyte = 1024.0 * 1024.0 * 1024.0;
    constexpr double kMebibyte = 1024.0 * 1024.0;
    QString value = tr("—");
    if (any && budget != 0) {
        const bool gigabytes = static_cast<double>(budget) >= kGibibyte;
        const double unit = gigabytes ? kGibibyte : kMebibyte;
        value = QStringLiteral("%1/%2 %3")
                    .arg(static_cast<double>(resident) / unit, 0, 'f', 1)
                    .arg(static_cast<double>(budget) / unit, 0, 'f', 1)
                    .arg(gigabytes ? tr("GB") : tr("MB"));
    }
    memory_->setText(value);
    memoryFrames_->setText(previewController_ != nullptr
                               ? tr("%1 f").arg(previewController_->frameCache().size())
                               : tr("—"));
}

void WindowStatusBar::refreshPreviewCells() {
    refreshMemoryToolTip();
    refreshVideoCell();
    refreshMemoryCell();
    if (!displayedViewer_)
        refreshProbeCell({});
    const auto dropped = [this](const bool counting) {
        droppedFrames_->setText(counting ? QString::number(previewController_->droppedFrameCount())
                                         : tr("—"));
        droppedFrames_->setTone(counting && previewController_->droppedFrameCount() != 0
                                    ? kit::Color::Warn
                                    : kit::Color::Muted);
        droppedFrames_->setToolTip(
            tr("Dropped: frames playback skipped to keep up. A dash means no playback is being "
               "measured."));
    };
    if (previewController_ == nullptr || session_.composition() == nullptr) {
        // No preview pipeline or no active composition: there is no colour or preview truth to
        // report, and the ordinary "Reference (unqualified)" / "Preview unsupported" cells would read
        // as a failure of a project that simply has not been authored yet.
        setViewerCells(previewController_ == nullptr
                           ? PreviewColorState{tr("Color state unavailable"), kit::Color::Warn,
                                               tr("None")}
                           : PreviewColorState{tr("No composition"), kit::Color::Muted, tr("None")},
                       nullptr);
        previewStateText_.clear();
        previewNotice_.clear();
        applyCompileFailure(std::nullopt);
        droppedFrames_->setTone(kit::Color::Muted);
        dropped(false);
        refreshActivity();
        return;
    }
    const auto preview =
        displayedViewer_ ? displayedViewer_->displayedPreviewState() : CompositionPreviewState{};
    setPreviewNotice(preview);
    if (!displayedViewer_) {
        previewStateText_.clear();
        previewNotice_.clear();
    }
    setViewerCells(displayedViewer_
                       ? previewColorState(preview)
                       : PreviewColorState{tr("No visible viewer"), kit::Color::Muted, tr("None")},
                   displayedViewer_ ? &preview : nullptr);
    applyCompileFailure(displayedViewer_ ? compileFailureFromPreview(preview) : std::nullopt);
    dropped(previewController_->isCountingDroppedFrames());
    refreshActivity();
}

void WindowStatusBar::applyCompileFailure(std::optional<CompileFailure> failure) {
    compileFailure_ = std::move(failure);
    compileDetails_->setVisible(compileFailure_.has_value());
    if (!compileFailure_ && compileFailurePopup_ != nullptr)
        compileFailurePopup_->close();
}

void WindowStatusBar::openCompileFailure() {
    if (!compileFailure_)
        return;
    if (compileFailurePopup_ != nullptr)
        compileFailurePopup_->close();
    auto* panel = new CompileFailurePanel(*compileFailure_, session_);
    auto* popup = new kit::KPopupWindow(nullptr, kit::Elevation::Popup);
    popup->setObjectName(QStringLiteral("windowStatusBarCompileFailurePopup"));
    popup->setAttribute(Qt::WA_DeleteOnClose);
    popup->setContent(panel);
    connect(panel, &CompileFailurePanel::dismissed, popup, &QWidget::close);
    compileFailurePopup_ = popup;
    const int border = std::max(1, static_cast<int>(std::lround(kit::kHairlineWidth)));
    panel->resize(panel->preferredSize());
    popup->openFor(*this, panel->preferredSize().grownBy(QMargins(border, border, border, border)));
}

void WindowStatusBar::applyPreviewStateForTest(const CompositionPreviewState& preview) {
    setPreviewNotice(preview);
    refreshActivity();
    applyCompileFailure(compileFailureFromPreview(preview));
}

bool WindowStatusBar::compileDetailsVisibleForTest() const { return !compileDetails_->isHidden(); }

QWidget* WindowStatusBar::compileFailurePopupForTest() const { return compileFailurePopup_; }

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
    const auto stats = mediaDiskCache_ != nullptr ? mediaDiskCache_->statistics()
                                                  : media::cache::MediaDiskCacheStatistics{};
    const bool enabled = mediaDiskCache_ != nullptr && mediaDiskCache_->enabled();
    diskStatusText_ = mediaDiskCacheStatusText(stats, enabled);
    QString value = tr("—");
    QString detail = tr("The disk cache has not served a lookup yet.");
    if (!enabled) {
        value = tr("off");
        detail = tr("The disk cache is switched off in Preferences.");
    } else if (!diskStatusText_.isEmpty()) {
        value = tr("%1% hit · %2")
                    .arg(static_cast<int>(std::lround(stats.hitRate() * 100.0)))
                    .arg(formatBytes(stats.storedBytes));
        detail = tr("%1 hits · %2 misses · %3 entries · %4 stored")
                     .arg(stats.hits)
                     .arg(stats.misses)
                     .arg(stats.entryCount)
                     .arg(formatBytes(stats.storedBytes));
    }
    mediaDiskCacheCell_->setText(value);
    diskSegment_->setSegmentToolTip(tr("The on-disk cache of decoded frames: how often a lookup "
                                       "found its frame, and how much it stores.\n%1")
                                        .arg(detail));
    mediaDiskCacheCell_->setToolTip(diskSegment_->toolTip());
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
    refreshActivity();
}

// The full colour-state sentence ("Qualified · Bloom Neutral", "GPU-prepared / CPU-painted"): the
// bar shows a plain GPU or CPU, and this is the detail its tooltip carries.
QString WindowStatusBar::colorChipTextForTest() const { return colorStateText_; }

// The preview's full status sentence ("Shown EV 0 · RGBA · Current"), which the Viewer segment's
// tooltips carry; empty while no viewer's state is displayed.
QString WindowStatusBar::previewStateTextForTest() const { return previewStateText_; }

QString WindowStatusBar::droppedFrameTextForTest() const { return droppedFrames_->text(); }

QString WindowStatusBar::mediaDiskCacheTextForTest() const { return diskStatusText_; }

QString WindowStatusBar::messageTextForTest() const {
    return transientMessage_.isEmpty() ? persistentMessage_ : transientMessage_;
}

QString WindowStatusBar::renderPathTextForTest() const { return renderPath_->text(); }
QString WindowStatusBar::exposureTextForTest() const { return exposure_->text(); }
QString WindowStatusBar::channelTextForTest() const { return channel_->text(); }
QString WindowStatusBar::activityTextForTest() const { return activityText_->text(); }
QString WindowStatusBar::memoryTextForTest() const {
    return tr("RAM %1 · cache %2").arg(memory_->text(), memoryFrames_->text());
}
QString WindowStatusBar::diskTextForTest() const { return mediaDiskCacheCell_->text(); }
QString WindowStatusBar::diskToolTipForTest() const { return diskSegment_->toolTip(); }

QStringList WindowStatusBar::segmentLabelsForTest() const {
    QStringList rows;
    for (const auto* segment : {viewerSegment_, activitySegment_, probeSegment_, memorySegment_,
                                diskSegment_, mcpSegment_, versionSegment_}) {
        QStringList labels;
        for (const auto* label : segment->findChildren<kit::KLabel*>())
            if (label->tone() == kit::Color::Muted && label->objectName().isEmpty())
                labels << label->text();
            else if (label->tone() == kit::Color::Faint && label->objectName().isEmpty())
                labels << label->text();
        rows << labels.join(QLatin1Char('|'));
    }
    return rows;
}

bool WindowStatusBar::segmentVisibleForTest(const QString& objectName) const {
    const auto* widget = findChild<QWidget*>(objectName);
    return widget != nullptr && !widget->isHidden();
}

QString WindowStatusBar::versionTextForTest() const { return version_->text(); }

ViewerEditor* WindowStatusBar::displayedViewerForTest() const noexcept { return displayedViewer_; }

void WindowStatusBar::refreshProbeCell(const ProbeReadout& readout) {
    probeReadout_ = readout;
    if (!readout.valid) {
        probe_->setElidedText({});
        probe_->setAccessibleName({});
        probeSegment_->hide();
        applyNarrowLayout();
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
    probe_->setElidedText(text);
    probe_->setAccessibleName(text);
    probe_->setToolTip(tr("Working space: %1\nDisplay: %2\nCtrl-click to show %3 values")
                           .arg(QString::fromUtf8(host::workingColorSpaceLabel(
                                    readout.workingColorSpaceId.toStdString())),
                                readout.displayName,
                                probeFloatFormat_ ? tr("float pre-encode") : tr("8-bit encoded")));
    probeSegment_->setVisible(true);
    applyNarrowLayout();
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

namespace bloom::ui {

void WindowStatusBar::setMcpServer(McpServerAdapter* const adapter) {
    if (mcpServer_ == adapter)
        return;
    if (mcpServer_ != nullptr)
        disconnect(mcpServer_, nullptr, this, nullptr);
    mcpServer_ = adapter;
    mcpIndicator_->onClick = [this] {
        if (mcpServer_ != nullptr)
            mcpServer_->requestReveal();
    };
    if (adapter != nullptr)
        connect(adapter, &McpServerAdapter::changed, this, &WindowStatusBar::refreshMcpIndicator);
    refreshMcpIndicator();
}

void WindowStatusBar::refreshMcpIndicator() {
    const auto snapshot = mcpServer_ != nullptr ? mcpServer_->snapshot() : nullptr;
    const bool running = snapshot && snapshot->state == mcp::McpServerState::Running;
    const bool failed = snapshot && !running && snapshot->failure.has_value();
    mcpWanted_ = running || failed;
    if (running) {
        const auto port = snapshot->endpoint ? snapshot->endpoint->port : snapshot->preferences.port;
        mcpIndicator_->setText(tr(":%1").arg(port));
        mcpIndicator_->setTone(kit::Color::Muted);
        mcpIndicator_->setToolTip(tr("MCP: the server is running; click to open the MCP Server "
                                     "panel.\n%1")
                                      .arg(mcppanel::statusText(*snapshot)));
    } else if (failed) {
        mcpIndicator_->setText(tr("failed"));
        mcpIndicator_->setTone(kit::Color::Error);
        mcpIndicator_->setToolTip(tr("MCP: the server stopped; click to open the MCP Server "
                                     "panel.\n%1")
                                      .arg(mcppanel::statusText(*snapshot)));
    }
    mcpSegment_->setVisible(mcpWanted_);
    applyNarrowLayout();
}

QString WindowStatusBar::mcpIndicatorTextForTest() const {
    return tr("MCP %1").arg(mcpIndicator_->text());
}
bool WindowStatusBar::mcpIndicatorVisibleForTest() const { return !mcpSegment_->isHidden(); }
void WindowStatusBar::clickMcpIndicatorForTest() {
    if (mcpIndicator_->onClick)
        mcpIndicator_->onClick();
}

} // namespace bloom::ui
