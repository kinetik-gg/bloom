// A job's Result and Log in plain words. The raw wording and codes of a failure stay behind the
// "Show details" disclosure for bug reports; nothing else on the panel shows a stage, code or
// digest.

#include "export_job_results.hpp"

#include "export_panel_text.hpp"
#include "properties_sections.hpp"
#include "reveal_in_file_manager.hpp"

#include <bloom/ui/export_queue_adapter.hpp>
#include <bloom/ui/kit/button.hpp>
#include <bloom/ui/kit/controls.hpp>
#include <bloom/ui/kit/row.hpp>
#include <bloom/ui/kit/section.hpp>
#include <bloom/ui/kit/text_field.hpp>

#include <QClipboard>
#include <QCoreApplication>
#include <QFileInfo>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QSettings>
#include <QVBoxLayout>

#include <algorithm>
#include <variant>

namespace bloom::ui::exportpanel {
namespace {

[[nodiscard]] QString tr(const char* source) {
    return QCoreApplication::translate("ExportJobResults", source);
}

[[nodiscard]] QString valueText(const host::ExportCheckValue& value) {
    struct Visitor {
        QString operator()(std::monostate) const { return {}; }
        QString operator()(const std::uint64_t number) const { return QString::number(number); }
        QString operator()(const host::ExportRationalValue& rational) const {
            return rational.denominator == 1
                       ? QString::number(rational.numerator)
                       : QStringLiteral("%1/%2").arg(rational.numerator).arg(rational.denominator);
        }
        QString operator()(const host::ExportColourTags& tags) const {
            return QStringLiteral("%1/%2/%3 %4")
                .arg(tags.primaries)
                .arg(tags.transfer)
                .arg(tags.matrix)
                .arg(tags.fullRange ? tr("full") : tr("limited"));
        }
        QString operator()(const std::string& text) const { return qs(text); }
    };
    return std::visit(Visitor{}, value);
}

// What a check that did not pass says, in an artist's words.
[[nodiscard]] QString failedCheckText(const host::ExportCheck check) {
    switch (check) {
    case host::ExportCheck::AnalysisPolicy:
        return tr("This format can't keep everything in the frame");
    case host::ExportCheck::Reopen:
        return tr("The saved file doesn't open");
    case host::ExportCheck::FrameCount:
        return tr("The file has the wrong number of frames");
    case host::ExportCheck::Timing:
        return tr("The frame timing doesn't match");
    case host::ExportCheck::ColourTags:
        return tr("The colors aren't tagged as asked");
    case host::ExportCheck::Container:
        return tr("The file isn't the kind you asked for");
    case host::ExportCheck::Audio:
        return tr("The audio doesn't match");
    case host::ExportCheck::Naming:
        return tr("The file names don't match the pattern");
    case host::ExportCheck::ArtifactDigest:
        return tr("The saved file changed after it was written");
    case host::ExportCheck::Publication:
        return tr("The file couldn't be put in place");
    case host::ExportCheck::DeviceProvenance:
        return tr("Not every frame was rendered the usual way");
    }
    return {};
}

struct Line final {
    QString text;
    StatusTone tone = StatusTone::Neutral;
    QString toolTip;
};

[[nodiscard]] std::vector<Line> verificationLines(const host::ExportOutputView& output,
                                                  const QString& prefix) {
    std::vector<Line> lines;
    if (!output.verification.has_value())
        return lines;
    const auto& verification = *output.verification;
    const auto add = [&](QString text, const StatusTone tone, QString toolTip = {}) {
        lines.push_back({prefix + text, tone, std::move(toolTip)});
    };
    const auto& settings = output.record.settings;
    for (const auto& check : verification.checks) {
        switch (check.outcome) {
        case host::ExportCheckOutcome::NotApplicable:
        case host::ExportCheckOutcome::NotRun:
            continue;
        case host::ExportCheckOutcome::Passed:
            if (check.check == host::ExportCheck::Reopen)
                add(tr("File opens"), StatusTone::Neutral);
            else if (check.check == host::ExportCheck::FrameCount)
                add(tr("Right number of frames"), StatusTone::Neutral);
            else if (check.check == host::ExportCheck::ColourTags) {
                const auto name = !settings.outputColorSpaceId.empty()
                                      ? qs(settings.outputColorSpaceId)
                                      : qs(settings.displayName);
                add(name.isEmpty() ? tr("Colors tagged correctly")
                                   : tr("Colors tagged as %1").arg(name),
                    StatusTone::Neutral);
            }
            continue;
        case host::ExportCheckOutcome::Warning:
        case host::ExportCheckOutcome::Failed: {
            auto text = failedCheckText(check.check);
            const auto expected = valueText(check.expected);
            const auto observed = valueText(check.observed);
            if (!expected.isEmpty() || !observed.isEmpty())
                text += tr(" (expected %1, found %2)").arg(expected, observed);
            if (check.frame.has_value())
                text += tr(" at frame %1").arg(*check.frame);
            add(text, check.outcome == host::ExportCheckOutcome::Failed ? StatusTone::Error
                                                                          : StatusTone::Neutral);
            break;
        }
        }
    }
    const auto& provenance = verification.provenance;
    if (provenance.gpuFrames + provenance.cpuReferenceFrames > 0)
        add(provenance.cpuReferenceFrames == 0 ? tr("Rendered on the GPU")
                                               : tr("Some frames rendered on the CPU"),
            StatusTone::Neutral,
            provenance.encoder.empty() ? QString{} : tr("Encoder: %1").arg(qs(provenance.encoder)));
    if (provenance.framesReused > 0)
        add(tr("%1 of %2 frames reused from cache")
                .arg(provenance.framesReused)
                .arg(provenance.gpuFrames + provenance.cpuReferenceFrames + provenance.framesReused),
            StatusTone::Neutral);
    return lines;
}

[[nodiscard]] int notesIn(const host::ExportJobView& job) {
    int notes = 0;
    for (const auto& output : job.outputs) {
        if (!output.verification.has_value())
            continue;
        for (const auto& check : output.verification->checks)
            notes += check.outcome == host::ExportCheckOutcome::Warning ? 1 : 0;
    }
    return std::max(notes, job.state == host::ExportJobState::DoneWithWarnings ? 1 : 0);
}

[[nodiscard]] QString summarySentence(const host::ExportJobView& job) {
    switch (job.state) {
    case host::ExportJobState::Failed:
        return tr("Couldn't save %1. %2").arg(qs(job.displayName), failureSentence(job));
    case host::ExportJobState::Stopped:
        return tr("Stopped before it finished.");
    case host::ExportJobState::Done:
    case host::ExportJobState::DoneWithWarnings: {
        std::uint64_t files = 0;
        std::uint64_t bytes = 0;
        for (const auto& output : job.outputs)
            if (output.verification.has_value()) {
                files += output.verification->artifacts.publishedArtifacts;
                bytes += output.verification->artifacts.totalBytes;
            }
        const bool note = job.state == host::ExportJobState::DoneWithWarnings;
        if (files == 0)
            return note ? tr("Saved with a note.") : tr("Saved.");
        const auto saved = (files == 1 ? tr("Saved 1 file") : tr("Saved %1 files").arg(files)) +
                           QStringLiteral(", ") + bytesText(bytes);
        if (note) {
            const int notes = notesIn(job);
            return notes == 1 ? tr("%1, with 1 note.").arg(saved)
                              : tr("%1, with %2 notes.").arg(saved).arg(notes);
        }
        return tr("%1, checked and matches what you asked for.").arg(saved);
    }
    default:
        return {};
    }
}

} // namespace

struct JobResults::Fields final {
    kit::KLabel* summary = nullptr;
    kit::KLabel* reason = nullptr;
    kit::KLabel* changed = nullptr;
    QVBoxLayout* outputs = nullptr;
    QVBoxLayout* lines = nullptr;
    QString signature;
    QStringList lineTexts;
    kit::KButton* detailsToggle = nullptr;
    kit::KButton* copyDetails = nullptr;
    QWidget* detailsRow = nullptr;
    kit::KTextArea* details = nullptr;
    kit::KTextArea* log = nullptr;
};

JobResults::JobResults(ExportQueueAdapter& adapter, const document::ExportJobId job,
                       QWidget* const parent)
    : QWidget(parent), adapter_(adapter), job_(job), f_(std::make_unique<Fields>()) {
    setObjectName(QStringLiteral("exportResults"));
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    result_ = properties::makeSection(layout, this, QStringLiteral("exportResultSection"), tr("Result"),
                                      QStringLiteral("export/job/result/collapsed"));
    result_->setBodyPadding(kit::px(kit::Spacing::XS));
    result_->setResetEnabled(false);
    auto* rows = result_->bodyLayout();
    QWidget* parentWidget = result_->body();

    f_->summary = new kit::KLabel(parentWidget);
    f_->summary->setObjectName(QStringLiteral("exportResultSummary"));
    f_->summary->setWordWrap(true);
    rows->addWidget(f_->summary);
    f_->reason = new kit::KLabel(parentWidget);
    f_->reason->setObjectName(QStringLiteral("exportResultReason"));
    f_->reason->setTypeRole(kit::TypeRole::UiSmall);
    f_->reason->setTone(kit::Color::Muted);
    f_->reason->setWordWrap(true);
    rows->addWidget(f_->reason);
    f_->changed = new kit::KLabel(parentWidget);
    f_->changed->setObjectName(QStringLiteral("exportResultChanged"));
    f_->changed->setTypeRole(kit::TypeRole::UiSmall);
    f_->changed->setTone(kit::Color::Muted);
    f_->changed->setText(tr("Changed since this run started."));
    rows->addWidget(f_->changed);

    auto* holder = new QWidget(parentWidget);
    holder->setObjectName(QStringLiteral("exportVerification"));
    f_->lines = new QVBoxLayout(holder);
    f_->lines->setContentsMargins(0, 0, 0, 0);
    f_->lines->setSpacing(kit::px(kit::Spacing::XXS));
    rows->addWidget(holder);

    auto* outputsHolder = new QWidget(parentWidget);
    f_->outputs = new QVBoxLayout(outputsHolder);
    f_->outputs->setContentsMargins(0, 0, 0, 0);
    f_->outputs->setSpacing(kit::px(kit::Spacing::XXS));
    rows->addWidget(outputsHolder);

    // The host's raw wording and codes: only here, behind a disclosure, for bug reports.
    auto* disclosure = new QWidget(parentWidget);
    auto* disclosureLayout = new QHBoxLayout(disclosure);
    disclosureLayout->setContentsMargins(0, 0, 0, 0);
    disclosureLayout->setSpacing(kit::px(kit::Spacing::XS));
    f_->detailsToggle = new kit::KButton(tr("Show details"), disclosure);
    f_->detailsToggle->setObjectName(QStringLiteral("exportDetailsToggle"));
    f_->detailsToggle->setControlSize(kit::KButton::ControlSize::Compact);
    f_->detailsToggle->setCheckable(true);
    f_->copyDetails = new kit::KButton(tr("Copy details"), disclosure);
    f_->copyDetails->setObjectName(QStringLiteral("exportCopyDetailsButton"));
    f_->copyDetails->setControlSize(kit::KButton::ControlSize::Compact);
    f_->copyDetails->setToolTip(tr("Copy the technical detail for a bug report"));
    disclosureLayout->addWidget(f_->detailsToggle);
    disclosureLayout->addWidget(f_->copyDetails);
    disclosureLayout->addStretch(1);
    f_->detailsRow = disclosure;
    rows->addWidget(disclosure);
    f_->details = new kit::KTextArea(parentWidget);
    f_->details->setObjectName(QStringLiteral("exportFailureDetails"));
    f_->details->setAccessibleName(tr("Failure details"));
    f_->details->setReadOnly(true);
    f_->details->setLineWrapMode(QPlainTextEdit::WidgetWidth);
    f_->details->hide();
    rows->addWidget(f_->details);
    connect(f_->detailsToggle, &QAbstractButton::toggled, f_->details, &QWidget::setVisible);
    connect(f_->detailsToggle, &QAbstractButton::toggled, this, [this](const bool shown) {
        f_->detailsToggle->setText(shown ? tr("Hide details") : tr("Show details"));
    });
    connect(f_->copyDetails, &kit::KButton::clicked, this, [this] {
        QGuiApplication::clipboard()->setText(f_->details->toPlainText());
    });

    // The run's log, folded away by default.
    log_ = properties::makeSection(layout, this, QStringLiteral("exportLogSection"), tr("Log"),
                                   QStringLiteral("export/job/log/collapsed"));
    log_->setBodyPadding(kit::px(kit::Spacing::XS));
    log_->setResetEnabled(false);
    if (!QSettings().contains(log_->persistenceKey()))
        log_->setCollapsed(true);
    f_->log = new kit::KTextArea(log_->body());
    f_->log->setObjectName(QStringLiteral("exportLog"));
    f_->log->setAccessibleName(tr("Export log"));
    f_->log->setReadOnly(true);
    f_->log->setLineWrapMode(QPlainTextEdit::WidgetWidth);
    f_->log->setMaximumBlockCount(4096);
    f_->log->setMinimumHeight(kit::px(kit::Size::DiagnosticHeight));
    log_->bodyLayout()->addWidget(f_->log);
    hide();
}

JobResults::~JobResults() = default;

bool JobResults::hasResults(const host::ExportJobView& job) {
    return job.run.has_value() || host::isTerminal(job.state) || job.failure.has_value();
}

QString JobResults::summaryText() const { return f_->summary->text(); }
QStringList JobResults::lineTexts() const { return f_->lineTexts; }
QString JobResults::logText() const { return f_->log->toPlainText(); }

void JobResults::update(const host::ExportJobView& job,
                        const host::ExportPresetCatalogueSnapshot* catalogue) {
    const bool shown = hasResults(job);
    setVisible(shown);
    if (!shown)
        return;
    const auto summary = summarySentence(job);
    f_->summary->setText(summary);
    f_->summary->setTone(job.state == host::ExportJobState::Failed ? kit::Color::Error
                                                                   : kit::Color::Foreground);
    f_->summary->setVisible(!summary.isEmpty());
    // The engine's plain explanation of a failure, under its headline.
    const auto reason = job.failure.has_value() ? qs(job.failure->reason) : QString{};
    f_->reason->setText(reason);
    f_->reason->setVisible(!reason.isEmpty());
    f_->changed->setVisible(job.recordChangedSinceStart);

    const auto details = failureDetails(job);
    f_->details->setPlainText(details);
    f_->detailsRow->setVisible(!details.isEmpty());
    if (details.isEmpty()) {
        f_->detailsToggle->setChecked(false);
        f_->details->hide();
    }

    // Plain lines and one Show in folder row per saved output; rebuilt only when they change.
    std::vector<Line> lines;
    for (const auto& output : job.outputs) {
        const QString prefix = job.outputs.size() > 1
                                   ? presetText(output, catalogue) + QStringLiteral(": ")
                                   : QString{};
        const auto own = verificationLines(output, prefix);
        lines.insert(lines.end(), own.begin(), own.end());
    }
    QString signature;
    for (const auto& line : lines)
        signature += line.text + QLatin1Char('\n') + QString::number(static_cast<int>(line.tone)) +
                     QLatin1Char('\n') + line.toolTip + QLatin1Char('\n');
    for (const auto& output : job.outputs) {
        signature += QStringLiteral("|%1:%2:%3").arg(output.id.value()).arg(static_cast<int>(output.state))
                         .arg(static_cast<int>(output.import.status));
        signature += qs(output.import.reason);
    }
    if (signature != f_->signature) {
        f_->signature = signature;
        for (auto* layout : {f_->lines, f_->outputs})
            while (auto* item = layout->takeAt(0)) {
                delete item->widget();
                delete item;
            }
        f_->lineTexts.clear();
        QWidget* host = f_->lines->parentWidget();
        for (const auto& line : lines) {
            auto* label = new kit::KLabel(host);
            label->setObjectName(QStringLiteral("exportVerificationLine"));
            label->setTypeRole(kit::TypeRole::UiSmall);
            label->setTone(line.tone == StatusTone::Error ? kit::Color::Error : kit::Color::Muted);
            label->setWordWrap(true);
            label->setText(line.text);
            label->setToolTip(line.toolTip);
            f_->lines->addWidget(label);
            f_->lineTexts.push_back(line.text);
        }
        QWidget* outputsHost = f_->outputs->parentWidget();
        for (const auto& output : job.outputs) {
            const bool saved = output.state == host::ExportOutputState::Published ||
                               output.state == host::ExportOutputState::PublishedWithWarnings;
            const auto importResult = [&output]() -> QString {
                switch (output.import.status) {
                case host::ExportAssetImportStatus::Pending:
                    return tr("Adding to Assets…");
                case host::ExportAssetImportStatus::Imported:
                    return tr("Added to Assets");
                case host::ExportAssetImportStatus::Refreshed:
                    return tr("Updated in Assets");
                case host::ExportAssetImportStatus::Skipped:
                    return tr("Not added to Assets: %1").arg(qs(output.import.reason));
                case host::ExportAssetImportStatus::Failed:
                    return tr("Couldn't add to Assets: %1").arg(qs(output.import.reason));
                case host::ExportAssetImportStatus::NotRequested:
                    return {};
                }
                return {};
            }();
            if (!saved && importResult.isEmpty())
                continue;
            auto* row = new QWidget(outputsHost);
            auto* rowLayout = new QHBoxLayout(row);
            rowLayout->setContentsMargins(0, 0, 0, 0);
            rowLayout->setSpacing(kit::px(kit::Spacing::S));
            auto* label = new kit::KLabel(row);
            label->setObjectName(QStringLiteral("exportResultOutput"));
            label->setTypeRole(kit::TypeRole::UiSmall);
            label->setTone(kit::Color::Muted);
            label->setElidedText(job.outputs.size() > 1 || !importResult.isEmpty()
                                     ? (presetText(output, catalogue) +
                                        (importResult.isEmpty() ? QString{} : QStringLiteral(" · ") + importResult))
                                     : destinationText(output));
            rowLayout->addWidget(label, 1);
            if (saved) {
                auto* show = new kit::KButton(tr("Show in folder"), row);
                show->setObjectName(QStringLiteral("exportShowInFolderButton"));
                show->setControlSize(kit::KButton::ControlSize::Compact);
                const auto path = output.verification && !output.verification->artifacts.firstPath.empty()
                                      ? output.verification->artifacts.firstPath
                                      : output.destination.firstPath;
                connect(show, &kit::KButton::clicked, this,
                        [this, path] { revealInFileManager(path, this); });
                rowLayout->addWidget(show, 0);
            }
            f_->outputs->addWidget(row);
            if (!importResult.isEmpty())
                f_->lineTexts.push_back(importResult);
        }
    }
    refreshLog(job);
}

void JobResults::refreshLog(const host::ExportJobView& job) {
    const std::uint64_t run = job.run ? job.run->run : 0;
    if (loggedRun_ != run) {
        loggedRun_ = run;
        logSequence_ = 0;
        f_->log->clear();
    }
    if (job.lastLogSequence <= logSequence_)
        return;
    const auto batch = adapter_.log(job.id, logSequence_);
    if (batch.gap && logSequence_ > 0)
        f_->log->appendPlainText(tr("Earlier entries were trimmed"));
    for (const auto& entry : batch.entries) {
        const char* severity = entry.severity == runtime::DiagnosticSeverity::Error     ? "error"
                               : entry.severity == runtime::DiagnosticSeverity::Warning ? "warning"
                                                                                        : "info";
        auto line = QStringLiteral("%1  %2  %3")
                        .arg(clockText(entry.sinceStart), QString::fromLatin1(severity),
                             qs(entry.message.empty() ? entry.code : entry.message));
        if (entry.frame.has_value())
            line += tr(" (frame %1)").arg(*entry.frame);
        f_->log->appendPlainText(line);
    }
    logSequence_ = std::max(logSequence_, batch.lastSequence);
}

} // namespace bloom::ui::exportpanel
