#include <bloom/ui/window_status_bar.hpp>

#include <bloom/ui/composition_preview_controller.hpp>
#include <bloom/ui/composition_session.hpp>
#include <bloom/ui/kit/painting.hpp>
#include <bloom/ui/kit/tokens.hpp>
#include <bloom/ui/preview_frame_cache.hpp>

#include <QCoreApplication>
#include <QFontMetrics>
#include <QHBoxLayout>
#include <QLabel>
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
[[nodiscard]] QString formatMebibytes(const std::size_t bytes) {
    constexpr double kMebibyte = 1024.0 * 1024.0;
    return QStringLiteral("%1").arg(static_cast<double>(bytes) / kMebibyte, 0, 'f', 1);
}

// The colour-state pill: a rounded, token-coloured fill with the state's own text. Its own widget
// rather than a QLabel with a stylesheet so the token colour is read at paint time and follows a
// theme change without a restyle pass.
class StatusColorChip final : public QWidget {
  public:
    explicit StatusColorChip(QWidget* parent) : QWidget(parent) {
        setObjectName(QStringLiteral("windowStatusBarColorChip"));
        setFixedHeight(kit::px(kit::Size::ControlCompact));
    }

    void setState(const PreviewColorState& state) {
        if (state_.text == state.text && state_.colorToken == state.colorToken) {
            return;
        }
        state_ = state;
        setAccessibleName(state_.text);
        setToolTip(state_.text);
        updateGeometry();
        update();
    }

    [[nodiscard]] QString text() const { return state_.text; }

    [[nodiscard]] QSize sizeHint() const override {
        const QFontMetrics metrics(kit::font(kit::TypeRole::Ui));
        return {metrics.horizontalAdvance(state_.text) + 2 * kit::px(kit::Spacing::S),
                kit::px(kit::Size::ControlCompact)};
    }

  protected:
    void paintEvent(QPaintEvent*) override {
        if (state_.text.isEmpty()) {
            return;
        }
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, true);
        painter.setFont(kit::font(kit::TypeRole::Ui));
        const QColor color = kit::color(state_.colorToken);
        const QRectF pill = QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5);
        painter.setPen(QPen(color.lighter(125), 1.0));
        painter.setBrush(kit::withOpacity(color, 0.855));
        painter.drawRoundedRect(pill, kit::radiusPx(kit::Radius::Small, height()),
                                kit::radiusPx(kit::Radius::Small, height()));
        painter.setPen(kit::color(kit::Color::Foreground));
        const qreal inset = kit::px(kit::Spacing::S);
        painter.drawText(pill.adjusted(inset, 0.0, -inset, 0.0), Qt::AlignVCenter | Qt::AlignLeft,
                         painter.fontMetrics().elidedText(
                             state_.text, Qt::ElideRight,
                             static_cast<int>(std::max<qreal>(0.0, pill.width() - 2 * inset))));
    }

  private:
    PreviewColorState state_{};
};

QLabel* makeCell(const QString& objectName, const kit::TypeRole role, const kit::Color ink,
                 QWidget* parent) {
    auto* label = new QLabel(parent);
    label->setObjectName(objectName);
    label->setFont(kit::font(role));
    QPalette palette = label->palette();
    palette.setColor(QPalette::WindowText, kit::color(ink));
    label->setPalette(palette);
    return label;
}

} // namespace

PreviewColorState previewColorState(const CompositionPreviewState& preview) {
    if (preview.activity == PreviewActivity::Failed) {
        return {preview.message.isEmpty() ? WindowStatusBar::tr("Color state unavailable")
                                          : preview.message,
                kit::Color::Error};
    }
    const auto bufferView =
        preview.frame != nullptr ? preview.frame->displayBufferView() : std::nullopt;
    if (bufferView.has_value() && bufferView->isOcioQualified) {
        return {WindowStatusBar::tr("Qualified · Bloom Neutral"), kit::Color::Ok};
    }
    return {WindowStatusBar::tr("Reference (unqualified)"), kit::Color::Warn};
}

QString previewActivityText(const CompositionPreviewState& preview) {
    const bool stale = preview.freshness == FrameFreshness::Stale;
    switch (preview.activity) {
    case PreviewActivity::Rendering:
        return stale ? WindowStatusBar::tr("Rendering current frame · Previous frame shown")
                     : WindowStatusBar::tr("Rendering current frame");
    case PreviewActivity::Ready:
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
    return WindowStatusBar::tr("Cache %1 frames · %2/%3 MB")
        .arg(cache.size())
        .arg(formatMebibytes(cache.residentBytes()), formatMebibytes(cache.byteBudget()));
}

WindowStatusBar::WindowStatusBar(CompositionSession& session,
                                 CompositionPreviewController* const previewController,
                                 QWidget* parent)
    : QWidget(parent), session_(session), previewController_(previewController) {
    setObjectName(QStringLiteral("windowStatusBar"));
    setAccessibleName(tr("Application status"));
    setFixedHeight(kit::px(kit::Size::Control));

    auto* layout = new QHBoxLayout(this);
    layout->setContentsMargins(kit::px(kit::Spacing::S), 0, kit::px(kit::Spacing::S), 0);
    layout->setSpacing(kit::px(kit::Spacing::M));

    auto* chip = new StatusColorChip(this);
    colorChip_ = chip;
    previewState_ = makeCell(QStringLiteral("windowStatusBarPreviewState"), kit::TypeRole::Ui,
                             kit::Color::Muted, this);
    droppedFrames_ = makeCell(QStringLiteral("windowStatusBarDroppedFrames"), kit::TypeRole::Value,
                              kit::Color::Warn, this);
    cache_ = makeCell(QStringLiteral("windowStatusBarCache"), kit::TypeRole::Value,
                      kit::Color::Accent, this);
    message_ = makeCell(QStringLiteral("windowStatusBarMessage"), kit::TypeRole::Ui,
                        kit::Color::Foreground, this);
    message_->setAccessibleName(tr("Status message"));
    version_ = makeCell(QStringLiteral("windowStatusBarVersion"), kit::TypeRole::UiSmall,
                        kit::Color::Faint, this);
    version_->setAccessibleName(tr("Bloom version"));
    version_->setText(QCoreApplication::applicationVersion());

    layout->addWidget(colorChip_);
    layout->addWidget(previewState_);
    layout->addWidget(droppedFrames_);
    layout->addWidget(cache_);
    layout->addWidget(message_, 1);
    layout->addWidget(version_, 0, Qt::AlignRight);

    transientTimer_ = new QTimer(this);
    transientTimer_->setSingleShot(true);
    transientTimer_->setInterval(kTransientMessageMs);
    connect(transientTimer_, &QTimer::timeout, this, &WindowStatusBar::clearTransientMessage);

    // A refused command is a notice, not a dialog: the artist asked for something the document
    // could not do, the command did not run, and the reason belongs where every other notice is.
    connect(&session_, &CompositionSession::commandRejected, this,
            [this](const QString& reason) { showTransientMessage(reason); });

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
    refreshPreviewCells();
    refreshMessage();
}

void WindowStatusBar::refreshPreviewCells() {
    if (previewController_ == nullptr) {
        return;
    }
    const auto& preview = previewController_->state();
    static_cast<StatusColorChip*>(colorChip_)->setState(previewColorState(preview));

    previewState_->setText(previewActivityText(preview));
    QPalette statePalette = previewState_->palette();
    statePalette.setColor(QPalette::WindowText, kit::color(previewActivityColor(preview)));
    previewState_->setPalette(statePalette);

    const QString dropped = droppedFrameText(*previewController_);
    droppedFrames_->setText(dropped);
    QPalette droppedPalette = droppedFrames_->palette();
    droppedPalette.setColor(QPalette::WindowText, previewController_->droppedFrameCount() == 0
                                                      ? kit::color(kit::Color::Muted)
                                                      : kit::color(kit::Color::Warn));
    droppedFrames_->setPalette(droppedPalette);

    cache_->setText(previewCacheText(*previewController_));
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

QString WindowStatusBar::messageTextForTest() const { return message_->text(); }

QString WindowStatusBar::versionTextForTest() const { return version_->text(); }

void WindowStatusBar::paintEvent(QPaintEvent* event) {
    Q_UNUSED(event)
    QPainter painter(this);
    painter.fillRect(rect(), kit::color(kit::Color::Surface));
    kit::applyHairlinePen(painter, kit::color(kit::Color::Border));
    painter.drawLine(QPointF(0.0, 0.5), QPointF(static_cast<qreal>(width()), 0.5));
}

} // namespace bloom::ui
