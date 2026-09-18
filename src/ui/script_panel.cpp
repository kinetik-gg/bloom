#include "script_panel.hpp"
#ifdef BLOOM_BUILD_PYTHON
#include "script_panel_runtime.hpp"
#endif

#include <QScrollArea>
#include <QScrollBar>
#include <QShortcut>
#include <QSignalBlocker>
#include <QVBoxLayout>
#include <bloom/ui/kit/button.hpp>
#include <bloom/ui/kit/controls.hpp>
#include <bloom/ui/kit/dropdown.hpp>
#include <bloom/ui/kit/tokens.hpp>

namespace bloom::ui {
ScriptPanel::ScriptPanel(std::shared_ptr<ScriptPanelRuntime> runtime, QWidget* parent)
    : QWidget(parent), runtime_(std::move(runtime)) {
    setObjectName(QStringLiteral("scriptPanel"));
    chrome_.header.objectName = QStringLiteral("scriptHeader");
    chrome_.header.owner = this;
    chrome_.footer.objectName = QStringLiteral("scriptFooter");
    chrome_.footer.owner = this;
    auto* runButton = new kit::KButton(QStringLiteral("Run"), this);
    runButton->setToolTip(QStringLiteral("Run Python (Ctrl+Enter)"));
    auto* cancelButton = new kit::KButton(QStringLiteral("Cancel"), this);
    cancelButton->setEnabled(false);
    chrome_.header.addWidget(runButton);
    chrome_.header.addWidget(cancelButton);
    status_ = new kit::KLabel(QStringLiteral("Ready"), this, kit::TypeRole::UiSmall);
    chrome_.footer.addWidget(status_);

    auto* layout = new QVBoxLayout(this);
    const int gap = kit::px(kit::Spacing::S);
    layout->setContentsMargins(gap, gap, gap, gap);
    layout->setSpacing(gap);
    history_ = new kit::KDropdown(this);
    history_->setObjectName(QStringLiteral("scriptHistory"));
    history_->addItem(QStringLiteral("History"));
    history_->setAccessibleName(QStringLiteral("Script history"));
    layout->addWidget(history_);
    auto* scroll = new QScrollArea(this);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    output_ = new kit::KLabel(QStringLiteral("Python • bloom is pre-imported\n"
                                             "Use bloom.tasks for long work."),
                              scroll, kit::TypeRole::Value);
    output_->setObjectName(QStringLiteral("scriptOutput"));
    output_->setTextFormat(Qt::PlainText);
    output_->setWordWrap(true);
    // KLabel defaults to one control row; the transcript needs its full text height.
    output_->setMinimumHeight(0);
    output_->setMaximumHeight(QWIDGETSIZE_MAX);
    output_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    output_->setAlignment(Qt::AlignTop | Qt::AlignLeft);
    output_->setTextInteractionFlags(Qt::TextSelectableByMouse | Qt::TextSelectableByKeyboard);
    scroll->setWidget(output_);
    layout->addWidget(scroll, 1);
    input_ = new kit::KLineEdit(this);
    input_->setObjectName(QStringLiteral("scriptInput"));
    input_->setAccessibleName(QStringLiteral("Python input"));
    input_->setPlaceholderText(QStringLiteral("Python · Ctrl+Enter to run"));
    input_->setMaxLength(65536);
    layout->addWidget(input_);
    auto* shortcut = new QShortcut(QKeySequence(Qt::CTRL | Qt::Key_Return), this);
    shortcut->setContext(Qt::WidgetWithChildrenShortcut);
    connect(shortcut, &QShortcut::activated, this, &ScriptPanel::run);
    auto* keypad = new QShortcut(QKeySequence(Qt::CTRL | Qt::Key_Enter), this);
    keypad->setContext(Qt::WidgetWithChildrenShortcut);
    connect(keypad, &QShortcut::activated, this, &ScriptPanel::run);
    connect(runButton, &kit::KButton::clicked, this, &ScriptPanel::run);
    connect(history_, &kit::KDropdown::currentIndexChanged, this, [this](int index) {
        if (index > 0 && index <= sources_.size()) {
            input_->setText(sources_[index - 1]);
            input_->setFocus();
        }
    });
    connect(scroll->verticalScrollBar(), &QScrollBar::rangeChanged, this,
            [scroll](int, int maximum) { scroll->verticalScrollBar()->setValue(maximum); });
#ifdef BLOOM_BUILD_PYTHON
    if (runtime_) {
        connect(runtime_.get(), &ScriptPanelRuntime::outputReady, this, &ScriptPanel::append);
        connect(runtime_.get(), &ScriptPanelRuntime::activityChanged, this,
                [this, cancelButton](const QString& message, bool active) {
                    status_->setText(message);
                    cancelButton->setEnabled(active);
                });
        connect(cancelButton, &kit::KButton::clicked, runtime_.get(), &ScriptPanelRuntime::cancel);
        return;
    }
#endif
    input_->setEnabled(false);
    runButton->setEnabled(false);
    status_->setText(QStringLiteral("Python unavailable"));
    output_->setText(
        QStringLiteral("The Script editor requires a Python-enabled build and a live project "
                       "host (ADR 0022)."));
}
void ScriptPanel::run() {
#ifdef BLOOM_BUILD_PYTHON
    const auto source = input_->text();
    if (!runtime_ || source.trimmed().isEmpty())
        return;
    if (!runtime_->submit(source)) {
        status_->setText(QStringLiteral("Script queue unavailable or full"));
        return;
    }
    sources_.removeAll(source);
    sources_.prepend(source);
    if (sources_.size() > 32)
        sources_.removeLast();
    const QSignalBlocker blocker(history_);
    history_->clearItems();
    history_->addItem(QStringLiteral("History"));
    for (const auto& entry : sources_)
        history_->addItem(entry);
    input_->clear();
#endif
}
void ScriptPanel::append(const QString& source, const QString& output, bool succeeded) {
    transcript_ += QStringLiteral(">>> ") + source + QLatin1Char('\n') + output + QLatin1Char('\n');
    transcript_ = transcript_.right(262144);
    output_->setText(transcript_);
    status_->setText(succeeded ? QStringLiteral("Completed")
                               : QStringLiteral("Error · see output"));
    emit executionFinished(succeeded);
}
} // namespace bloom::ui
