// The Export panel's Post-render section and its Log and verification section: what a run did,
// separately for the export and for the optional import into Assets, and the run's bounded log
// appended incrementally.

#include "export_detail_fields.hpp"
#include "export_panel_text.hpp"

#include "properties_sections.hpp"

#include <bloom/ui/export_queue_adapter.hpp>
#include <bloom/ui/kit/button.hpp>
#include <bloom/ui/kit/controls.hpp>
#include <bloom/ui/kit/row.hpp>
#include <bloom/ui/kit/section.hpp>
#include <bloom/ui/kit/switch_control.hpp>
#include <bloom/ui/kit/text_field.hpp>

#include <QHBoxLayout>
#include <QClipboard>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QVBoxLayout>

#include <algorithm>
#include <variant>

namespace bloom::ui::exportpanel {
namespace {

[[nodiscard]] QString checkName(const host::ExportCheck check) {
    switch (check) {
    case host::ExportCheck::AnalysisPolicy:
        return QObject::tr("Preservation");
    case host::ExportCheck::Reopen:
        return QObject::tr("Reopens");
    case host::ExportCheck::FrameCount:
        return QObject::tr("Frame count");
    case host::ExportCheck::Timing:
        return QObject::tr("Timing");
    case host::ExportCheck::ColourTags:
        return QObject::tr("Colour tags");
    case host::ExportCheck::Container:
        return QObject::tr("Container");
    case host::ExportCheck::Audio:
        return QObject::tr("Audio");
    case host::ExportCheck::Naming:
        return QObject::tr("File names");
    case host::ExportCheck::ArtifactDigest:
        return QObject::tr("Digest");
    case host::ExportCheck::Publication:
        return QObject::tr("Publication");
    case host::ExportCheck::DeviceProvenance:
        return QObject::tr("Device");
    }
    return {};
}

[[nodiscard]] QString outcomeName(const host::ExportCheckOutcome outcome) {
    switch (outcome) {
    case host::ExportCheckOutcome::Passed:
        return QObject::tr("Passed");
    case host::ExportCheckOutcome::Warning:
        return QObject::tr("Warning");
    case host::ExportCheckOutcome::Failed:
        return QObject::tr("Failed");
    case host::ExportCheckOutcome::NotApplicable:
        return QObject::tr("Not applicable");
    case host::ExportCheckOutcome::NotRun:
        return QObject::tr("Not run");
    }
    return {};
}

[[nodiscard]] StatusTone outcomeTone(const host::ExportCheckOutcome outcome) {
    switch (outcome) {
    case host::ExportCheckOutcome::Passed:
        return StatusTone::Success;
    case host::ExportCheckOutcome::Warning:
        return StatusTone::Warning;
    case host::ExportCheckOutcome::Failed:
        return StatusTone::Error;
    default:
        return StatusTone::Neutral;
    }
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
                .arg(tags.fullRange ? QObject::tr("full") : QObject::tr("limited"));
        }
        QString operator()(const std::string& text) const { return qs(text); }
    };
    return std::visit(Visitor{}, value);
}

[[nodiscard]] QString importText(const host::ExportAssetImportResult& result) {
    switch (result.status) {
    case host::ExportAssetImportStatus::NotRequested:
        return {};
    case host::ExportAssetImportStatus::Pending:
        return QObject::tr("Importing into Assets…");
    case host::ExportAssetImportStatus::Imported:
        return QObject::tr("Imported into Assets");
    case host::ExportAssetImportStatus::Refreshed:
        return QObject::tr("Refreshed in Assets");
    case host::ExportAssetImportStatus::Skipped:
        return QObject::tr("Not imported: %1").arg(qs(result.reason));
    case host::ExportAssetImportStatus::Failed:
        return QObject::tr("Import into Assets failed: %1").arg(qs(result.reason));
    }
    return {};
}

} // namespace

void ExportDetailPane::buildPostRender(QVBoxLayout& layout) {
    auto* section = properties::makeSection(&layout, this, QStringLiteral("exportPostRenderSection"),
                                            tr("Post-render"),
                                            QStringLiteral("export/sections/post-render/collapsed"));
    section->setBodyPadding(kit::px(kit::Spacing::XS));
    section->setResetEnabled(false);
    auto* rows = section->bodyLayout();
    f_->importIntoAssets = new kit::KSwitch(this);
    f_->importIntoAssets->setObjectName(QStringLiteral("exportImportIntoAssets"));
    f_->importIntoAssets->setAccessibleName(tr("Import into Assets"));
    f_->importIntoAssets->setToolTip(
        tr("After a verified render, import the result into this project's Assets"));
    (void)addRow(*rows, tr("Import into Assets"), {f_->importIntoAssets});
    // An import that fails never fails the export: it is reported on its own line.
    f_->importResult = new kit::KLabel(this);
    f_->importResult->setObjectName(QStringLiteral("exportImportResult"));
    f_->importResult->setTypeRole(kit::TypeRole::UiSmall);
    f_->importResult->setWordWrap(true);
    f_->importResultRow = addRow(*rows, QString{}, {f_->importResult});
    f_->importResultRow->hide();
    f_->outputEditors.push_back(f_->importIntoAssets);
    connect(f_->importIntoAssets, &QAbstractButton::toggled, this, [this](const bool on) {
        if (!refreshing_ && job_ && output_)
            (void)adapter_.editOutput(*job_, *output_, outputEdit([&](auto& edit) { edit.importIntoAssets = on; }));
    });
}

void ExportDetailPane::buildResults(QVBoxLayout& layout) {
    auto* section = properties::makeSection(&layout, this, QStringLiteral("exportResultsSection"),
                                            tr("Log and verification"),
                                            QStringLiteral("export/sections/results/collapsed"));
    section->setBodyPadding(kit::px(kit::Spacing::XS));
    section->setResetEnabled(false);
    auto* rows = section->bodyLayout();
    auto* holder = new QWidget(this);
    holder->setObjectName(QStringLiteral("exportVerification"));
    f_->verification = new QVBoxLayout(holder);
    f_->verification->setContentsMargins(0, 0, 0, 0);
    f_->verification->setSpacing(kit::px(kit::Spacing::XXS));
    rows->addWidget(holder);
    f_->log = new kit::KTextArea(this);
    f_->log->setObjectName(QStringLiteral("exportLog"));
    f_->log->setAccessibleName(tr("Export log"));
    f_->log->setReadOnly(true);
    f_->log->setLineWrapMode(QPlainTextEdit::WidgetWidth);
    f_->log->setMaximumBlockCount(4096);
    f_->log->setMinimumHeight(kit::px(kit::Size::DiagnosticHeight));
    rows->addWidget(f_->log);

    // The host's raw wording and codes: only here, behind a disclosure, for bug reports.
    auto* disclosure = new QWidget(this);
    auto* disclosureLayout = new QHBoxLayout(disclosure);
    disclosureLayout->setContentsMargins(0, 0, 0, 0);
    disclosureLayout->setSpacing(kit::px(kit::Spacing::XS));
    f_->detailsToggle = new kit::KButton(tr("Details"), disclosure);
    f_->detailsToggle->setObjectName(QStringLiteral("exportDetailsToggle"));
    f_->detailsToggle->setCheckable(true);
    f_->detailsToggle->setToolTip(tr("Show the technical detail of the failure"));
    f_->copyDetails = new kit::KButton(tr("Copy details"), disclosure);
    f_->copyDetails->setObjectName(QStringLiteral("exportCopyDetailsButton"));
    f_->copyDetails->setToolTip(tr("Copy the technical detail for a bug report"));
    disclosureLayout->addWidget(f_->detailsToggle);
    disclosureLayout->addWidget(f_->copyDetails);
    disclosureLayout->addStretch(1);
    f_->detailsRow = disclosure;
    rows->addWidget(disclosure);
    f_->details = new kit::KTextArea(this);
    f_->details->setObjectName(QStringLiteral("exportFailureDetails"));
    f_->details->setAccessibleName(tr("Failure details"));
    f_->details->setReadOnly(true);
    f_->details->setLineWrapMode(QPlainTextEdit::WidgetWidth);
    f_->details->hide();
    rows->addWidget(f_->details);
    connect(f_->detailsToggle, &QAbstractButton::toggled, f_->details, &QWidget::setVisible);
    connect(f_->copyDetails, &kit::KButton::clicked, this, [this] {
        QGuiApplication::clipboard()->setText(f_->details->toPlainText());
    });
}

void ExportDetailPane::refreshResults(const host::ExportJobView& job,
                                      const host::ExportOutputView* const output) {
    const auto details = failureDetails(job);
    f_->details->setPlainText(details);
    f_->detailsRow->setVisible(!details.isEmpty());
    if (details.isEmpty()) {
        f_->detailsToggle->setChecked(false);
        f_->details->hide();
    }
    struct Line {
        QString text;
        StatusTone tone = StatusTone::Neutral;
        QString toolTip;
    };
    std::vector<Line> lines;
    if (job.failure.has_value()) {
        // The engine's headline, then its plain explanation (never stage or payload codes).
        lines.push_back({failureSentence(job), StatusTone::Error, {}});
        if (!job.failure->reason.empty())
            lines.push_back({qs(job.failure->reason), StatusTone::Neutral, {}});
    }
    if (output != nullptr && output->verification.has_value()) {
        const auto& verification = *output->verification;
        lines.push_back({tr("Verification: %1").arg(outcomeName(verification.overall)),
                         outcomeTone(verification.overall), {}});
        for (const auto& check : verification.checks) {
            if (check.outcome == host::ExportCheckOutcome::NotApplicable)
                continue;
            auto text = QStringLiteral("%1: %2").arg(checkName(check.check), outcomeName(check.outcome));
            // A difference reads as expected against observed; a pass needs no numbers.
            if (check.outcome == host::ExportCheckOutcome::Failed ||
                check.outcome == host::ExportCheckOutcome::Warning) {
                const auto expected = valueText(check.expected);
                const auto observed = valueText(check.observed);
                if (!expected.isEmpty() || !observed.isEmpty())
                    text += tr(" (expected %1, found %2)").arg(expected, observed);
                if (check.frame.has_value())
                    text += tr(" at frame %1").arg(*check.frame);
            }
            lines.push_back({text, outcomeTone(check.outcome), qs(check.code)});
        }
        const auto& artifacts = verification.artifacts;
        if (artifacts.publishedArtifacts > 0)
            lines.push_back({tr("Published %1 file(s), %2")
                                 .arg(artifacts.publishedArtifacts)
                                 .arg(bytesText(artifacts.totalBytes)),
                             StatusTone::Neutral,
                             QString::fromStdString(artifacts.firstPath.generic_string())});
        const auto& provenance = verification.provenance;
        if (provenance.gpuFrames + provenance.cpuReferenceFrames > 0)
            lines.push_back({tr("%1 frames on the GPU, %2 on the CPU reference%3")
                                 .arg(provenance.gpuFrames)
                                 .arg(provenance.cpuReferenceFrames)
                                 .arg(provenance.encoder.empty() ? QString{}
                                                                 : tr(", encoder %1").arg(qs(provenance.encoder))),
                             StatusTone::Neutral, {}});
        if (provenance.framesReused > 0)
            lines.push_back({tr("%1 of %2 frames reused from cache")
                                 .arg(provenance.framesReused)
                                 .arg(provenance.gpuFrames + provenance.cpuReferenceFrames +
                                      provenance.framesReused),
                             StatusTone::Neutral, {}});
    }
    QString signature;
    for (const auto& line : lines)
        signature += line.text + QLatin1Char('\n') + QString::number(static_cast<int>(line.tone)) +
                     QLatin1Char('\n') + line.toolTip + QLatin1Char('\n');
    if (signature != f_->verificationSignature) {
        f_->verificationSignature = signature;
        while (auto* item = f_->verification->takeAt(0)) {
            delete item->widget();
            delete item;
        }
        for (const auto& line : lines) {
            auto* label = new kit::KLabel(f_->verification->parentWidget());
            label->setObjectName(QStringLiteral("exportVerificationLine"));
            label->setTypeRole(kit::TypeRole::UiSmall);
            label->setTone(line.tone == StatusTone::Neutral ? kit::Color::Muted : toneColor(line.tone));
            label->setWordWrap(true);
            label->setText(line.text);
            label->setToolTip(line.toolTip);
            f_->verification->addWidget(label);
        }
    }
    // Import is reported on its own line, never folded into the export's verdict.
    const auto result = output != nullptr ? importText(output->import) : QString{};
    f_->importResult->setText(result);
    f_->importResult->setTone(output != nullptr &&
                                      output->import.status == host::ExportAssetImportStatus::Failed
                                  ? kit::Color::Error
                                  : kit::Color::Muted);
    f_->importResultRow->setVisible(!result.isEmpty());
}

void ExportDetailPane::refreshLog(const host::ExportJobView& job) {
    const std::uint64_t run = job.run ? job.run->run : 0;
    if (loggedJob_ != job.id || loggedRun_ != run) {
        loggedJob_ = job.id;
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

QString ExportDetailPane::logText() const { return f_->log->toPlainText(); }

} // namespace bloom::ui::exportpanel
