// A job's result and log in plain words. The raw wording and codes of a failure go only into
// Copy details, for bug reports; nothing on the panel or in this dialog shows a stage, code or
// digest.

#include "export_job_details.hpp"

#include "export_panel_text.hpp"
#include "reveal_in_file_manager.hpp"

#include <bloom/ui/export_queue_adapter.hpp>
#include <bloom/ui/kit/button.hpp>
#include <bloom/ui/kit/controls.hpp>
#include <bloom/ui/kit/text_field.hpp>

#include <QClipboard>
#include <QCoreApplication>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QVBoxLayout>

#include <algorithm>
#include <variant>

namespace bloom::ui::exportpanel {
namespace {

[[nodiscard]] QString tr(const char* source) {
    return QCoreApplication::translate("ExportJobDetails", source);
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
        add(provenance.cpuReferenceFrames == 0 && provenance.cpuDisplayFrames == 0
                ? tr("Rendered on the GPU")
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

[[nodiscard]] QString importText(const host::ExportOutputView& output) {
    switch (output.import.status) {
    case host::ExportAssetImportStatus::Pending:
        return tr("Adding to Assets\u2026");
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
}

[[nodiscard]] const host::ExportJobView* findJob(const host::ExportQueueSnapshot* snapshot,
                                                 const document::ExportJobId job) {
    if (snapshot == nullptr)
        return nullptr;
    for (const auto& view : snapshot->jobs)
        if (view.id == job)
            return &view;
    return nullptr;
}

} // namespace

JobDetailsDialog::JobDetailsDialog(ExportQueueAdapter& adapter, const document::ExportJobId job,
                                   QWidget& owner)
    : kit::KDialog(tr("Details"), QStringLiteral("exportJobDetailsDialog"), owner, Qt::WindowModal,
                   Width::Wide),
      adapter_(adapter), job_(job) {
    auto* content = contentLayout();
    summary_ = new kit::KLabel(this);
    summary_->setObjectName(QStringLiteral("exportResultSummary"));
    summary_->setWordWrap(true);
    content->addWidget(summary_);
    reason_ = new kit::KLabel(this);
    reason_->setObjectName(QStringLiteral("exportResultReason"));
    reason_->setTone(kit::Color::Muted);
    reason_->setWordWrap(true);
    content->addWidget(reason_);
    changed_ = new kit::KLabel(tr("Changed since this run started."), this, kit::TypeRole::UiSmall);
    changed_->setObjectName(QStringLiteral("exportResultChanged"));
    changed_->setTone(kit::Color::Muted);
    content->addWidget(changed_);

    auto* holder = new QWidget(this);
    holder->setObjectName(QStringLiteral("exportVerification"));
    lines_ = new QVBoxLayout(holder);
    lines_->setContentsMargins(0, 0, 0, 0);
    lines_->setSpacing(kit::px(kit::Spacing::XXS));
    content->addWidget(holder);

    auto* logTitle = new kit::KLabel(tr("Log"), this, kit::TypeRole::UiSmallStrong);
    logTitle->setObjectName(QStringLiteral("exportLogTitle"));
    content->addWidget(logTitle);
    log_ = new kit::KTextArea(this);
    log_->setObjectName(QStringLiteral("exportLog"));
    log_->setAccessibleName(tr("Export log"));
    log_->setReadOnly(true);
    log_->setLineWrapMode(QPlainTextEdit::WidgetWidth);
    log_->setMaximumBlockCount(4096);
    log_->setMinimumHeight(kit::px(kit::Size::DiagnosticHeight));
    content->addWidget(log_, 1);

    buttonBox()->setObjectName(QStringLiteral("exportJobDetailsButtons"));
    auto* copy = addButton(Role::Apply, tr("Copy details"));
    copy->setObjectName(QStringLiteral("exportCopyDetailsButton"));
    copy->setToolTip(tr("Copy the result, the checks and the log for a bug report"));
    addButton(Role::Accept, tr("Close"))->setObjectName(QStringLiteral("exportJobDetailsClose"));
    connect(this, &kit::KDialog::applyRequested, this,
            [this] { QGuiApplication::clipboard()->setText(detailsText()); });
    connect(&adapter_, &ExportQueueAdapter::queueChanged, this, &JobDetailsDialog::refresh);
    refresh();
}

bool JobDetailsDialog::hasResults(const host::ExportJobView& job) {
    return job.run.has_value() || host::isTerminal(job.state) || job.failure.has_value();
}

QString JobDetailsDialog::summaryText() const { return summary_->text(); }
QString JobDetailsDialog::logText() const { return log_->toPlainText(); }

QString JobDetailsDialog::detailsText() const {
    QStringList parts;
    for (const auto& text : {summary_->text(), reason_->text()})
        if (!text.isEmpty())
            parts.push_back(text);
    parts.append(lineTexts_);
    if (!rawDetails_.isEmpty())
        parts.push_back(rawDetails_);
    if (const auto log = log_->toPlainText(); !log.isEmpty())
        parts.push_back(log);
    return parts.join(QLatin1Char('\n'));
}

void JobDetailsDialog::refresh() {
    const auto snapshot = adapter_.snapshot();
    const auto catalogue = adapter_.catalogue();
    const auto* job = findJob(snapshot.get(), job_);
    if (job == nullptr) {
        summary_->setText(tr("This job is no longer in the list."));
        summary_->setTone(kit::Color::Foreground);
        reason_->hide();
        changed_->hide();
        return;
    }
    setWindowTitle(tr("Details: %1").arg(qs(job->displayName)));
    auto summary = summarySentence(*job);
    if (summary.isEmpty())
        summary = hasResults(*job) ? jobStatus(*job).text : tr("This job has not run yet.");
    summary_->setText(summary);
    summary_->setTone(job->state == host::ExportJobState::Failed ? kit::Color::Error
                                                                 : kit::Color::Foreground);
    // The engine's plain explanation of a failure, under its headline.
    const auto reason = job->failure.has_value() ? qs(job->failure->reason) : QString{};
    reason_->setText(reason);
    reason_->setVisible(!reason.isEmpty());
    changed_->setVisible(job->recordChangedSinceStart);
    rawDetails_ = failureDetails(*job);

    // Plain lines and one Show in folder row per saved output; rebuilt only when they change.
    std::vector<Line> lines;
    for (const auto& output : job->outputs) {
        const QString prefix = job->outputs.size() > 1
                                   ? presetText(output, catalogue.get()) + QStringLiteral(": ")
                                   : QString{};
        const auto own = verificationLines(output, prefix);
        lines.insert(lines.end(), own.begin(), own.end());
    }
    QString signature;
    for (const auto& line : lines)
        signature += line.text + QLatin1Char('\n') + QString::number(static_cast<int>(line.tone)) +
                     QLatin1Char('\n') + line.toolTip + QLatin1Char('\n');
    for (const auto& output : job->outputs)
        signature += QStringLiteral("|%1:%2:%3:")
                         .arg(output.id.value())
                         .arg(static_cast<int>(output.state))
                         .arg(static_cast<int>(output.import.status)) +
                     qs(output.import.reason);
    if (signature != signature_) {
        signature_ = signature;
        while (auto* item = lines_->takeAt(0)) {
            delete item->widget();
            delete item;
        }
        lineTexts_.clear();
        QWidget* host = lines_->parentWidget();
        for (const auto& line : lines) {
            auto* label = new kit::KLabel(host);
            label->setObjectName(QStringLiteral("exportVerificationLine"));
            label->setTone(line.tone == StatusTone::Error ? kit::Color::Error : kit::Color::Muted);
            label->setWordWrap(true);
            label->setText(line.text);
            label->setToolTip(line.toolTip);
            lines_->addWidget(label);
            lineTexts_.push_back(line.text);
        }
        for (const auto& output : job->outputs) {
            const bool saved = output.state == host::ExportOutputState::Published ||
                               output.state == host::ExportOutputState::PublishedWithWarnings;
            const auto imported = importText(output);
            if (!saved && imported.isEmpty())
                continue;
            auto* row = new QWidget(host);
            auto* rowLayout = new QHBoxLayout(row);
            rowLayout->setContentsMargins(0, 0, 0, 0);
            rowLayout->setSpacing(kit::px(kit::Spacing::S));
            auto* label = new kit::KLabel(row);
            label->setObjectName(QStringLiteral("exportResultOutput"));
            label->setTone(kit::Color::Muted);
            label->setElidedText(imported.isEmpty()
                                     ? destinationText(output)
                                     : presetText(output, catalogue.get()) + QStringLiteral(" \u00b7 ") + imported);
            rowLayout->addWidget(label, 1);
            if (saved) {
                auto* show = new kit::KButton(tr("Show in folder"), row);
                show->setObjectName(QStringLiteral("exportShowInFolderButton"));
                show->setControlSize(kit::KButton::ControlSize::Compact);
                const auto path = output.verification && !output.verification->artifacts.firstPath.empty()
                                      ? output.verification->artifacts.firstPath
                                      : output.destination.firstPath;
                connect(show, &kit::KButton::clicked, this, [this, path] { revealInFileManager(path, this); });
                rowLayout->addWidget(show, 0);
            }
            lines_->addWidget(row);
            if (!imported.isEmpty())
                lineTexts_.push_back(imported);
        }
    }
    refreshLog(*job);
}

void JobDetailsDialog::refreshLog(const host::ExportJobView& job) {
    const std::uint64_t run = job.run ? job.run->run : 0;
    if (loggedRun_ != run) {
        loggedRun_ = run;
        logSequence_ = 0;
        log_->clear();
    }
    if (job.lastLogSequence <= logSequence_)
        return;
    const auto batch = adapter_.log(job.id, logSequence_);
    if (batch.gap && logSequence_ > 0)
        log_->appendPlainText(tr("Earlier entries were trimmed"));
    for (const auto& entry : batch.entries) {
        const char* severity = entry.severity == runtime::DiagnosticSeverity::Error     ? "error"
                               : entry.severity == runtime::DiagnosticSeverity::Warning ? "warning"
                                                                                        : "info";
        auto line = QStringLiteral("%1  %2  %3")
                        .arg(clockText(entry.sinceStart), QString::fromLatin1(severity),
                             qs(entry.message.empty() ? entry.code : entry.message));
        if (entry.frame.has_value())
            line += tr(" (frame %1)").arg(*entry.frame);
        log_->appendPlainText(line);
    }
    logSequence_ = std::max(logSequence_, batch.lastSequence);
}

} // namespace bloom::ui::exportpanel
