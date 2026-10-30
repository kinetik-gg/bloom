#include "export_panel_text.hpp"

#include "assets_editor_internal.hpp"

#include <QCoreApplication>
#include <QLocale>
#include <QStringList>

#include <algorithm>
#include <ranges>

namespace bloom::ui::exportpanel {
namespace {

QString tr(const char* source) { return QCoreApplication::translate("ExportPanel", source); }

using host::ExportJobState;
using host::isActive;
using host::ExportOutputState;

[[nodiscard]] QString twoDigits(const qlonglong value) {
    return QStringLiteral("%1").arg(value, 2, 10, QLatin1Char('0'));
}

} // namespace

QString qs(const std::string& value) {
    return QString::fromUtf8(value.data(), static_cast<qsizetype>(value.size()));
}

kit::Color toneColor(const StatusTone tone) {
    switch (tone) {
    case StatusTone::Neutral:
        return kit::Color::Muted;
    case StatusTone::Active:
        // The progress bar carries the accent; a running status reads in neutral ink.
        return kit::Color::Muted;
    case StatusTone::Success:
    case StatusTone::Warning:
        // A finished render and a note read in neutral ink; only a failure is coloured.
        return kit::Color::Muted;
    case StatusTone::Error:
        return kit::Color::Error;
    }
    return kit::Color::Muted;
}

bool isWaitingForViewer(const host::ExportJobView& job) {
    return job.state == ExportJobState::Rendering &&
           job.progress.route == host::ExportDeviceRoute::WaitingForViewer;
}

QString failureSentence(const host::ExportJobView& job) {
    // The engine's plain headline (exportFailureSummary); failureDetails() carries the reason.
    return job.failure.has_value() ? qs(job.failure->summary) : QString{};
}

QString failureDetails(const host::ExportJobView& job) {
    if (!job.failure.has_value())
        return {};
    auto text = qs(job.failure->reason);
    if (!job.failure->diagnosticCode.empty())
        text += QStringLiteral("\n") + qs(job.failure->diagnosticCode);
    return text;
}

namespace {

[[nodiscard]] QString percentText(const host::ExportJobProgress& progress) {
    if (!progress.fraction.has_value())
        return {};
    return QStringLiteral(" %1%").arg(static_cast<int>(std::clamp(*progress.fraction, 0.0, 1.0) * 100.0));
}

// True when the job cannot start because an output has nowhere (valid) to save.
[[nodiscard]] bool needsLocation(const host::ExportJobView& job) {
    for (const auto& output : job.outputs)
        for (const auto& issue : output.preflight)
            if (issue.severity == host::ExportPreflightSeverity::Blocking &&
                (issue.code == host::ExportPreflightCode::DestinationMissing ||
                 issue.code == host::ExportPreflightCode::DestinationNotWritable))
                return true;
    return false;
}

} // namespace

StatusText jobStatus(const host::ExportJobView& job) {
    const auto reasonOf = [&job] { return failureSentence(job); };
    switch (job.state) {
    case ExportJobState::Ready: {
        const auto& start = job.commands[host::ExportCommand::Start];
        if (start.available)
            return {tr("Queued"), {}, StatusTone::Neutral};
        switch (start.code) {
        case host::ExportUnavailableCode::DestinationCheckPending:
            return {tr("Checking where it saves\u2026"), qs(start.reason), StatusTone::Neutral};
        case host::ExportUnavailableCode::NeedsReplaceConfirmation:
            return {tr("Waiting for your OK"), qs(start.reason), StatusTone::Neutral};
        default:
            return {needsLocation(job) ? tr("Needs output") : tr("Needs attention"),
                    qs(start.reason), StatusTone::Neutral};
        }
    }
    case ExportJobState::Waiting:
        return {tr("Queued"), {}, StatusTone::Neutral};
    case ExportJobState::Preparing:
        return {tr("Getting ready"), {}, StatusTone::Active};
    case ExportJobState::Rendering:
        switch (job.progress.route) {
        case host::ExportDeviceRoute::WaitingForViewer:
            return {tr("Paused while you work in the Viewer"),
                    tr("Export uses only spare time while the Viewer plays. It continues when "
                       "playback stops."),
                    StatusTone::Neutral};
        case host::ExportDeviceRoute::WaitingForMemory: {
            const auto needed = job.progress.memoryNeededBytes;
            return {tr("Waiting for memory"),
                    needed ? tr("Waiting for memory: %1 GB needed")
                                 .arg(static_cast<double>(*needed) / 1e9, 0, 'f', 1)
                           : tr("Waiting for other work to release memory"),
                    StatusTone::Neutral};
        }
        case host::ExportDeviceRoute::RecoveringGpu:
            return {tr("Reconnecting GPU"), {}, StatusTone::Active};
        case host::ExportDeviceRoute::CpuReference:
            return {tr("Rendering (slower: no GPU available)") + percentText(job.progress), {},
                    StatusTone::Active};
        default:
            return {tr("Rendering") + percentText(job.progress), {}, StatusTone::Active};
        }
    case ExportJobState::Pausing:
        return {tr("Pausing"), {}, StatusTone::Neutral};
    case ExportJobState::Paused:
        return {tr("Paused"), {}, StatusTone::Neutral};
    case ExportJobState::Verifying:
        return {tr("Checking the file"), {}, StatusTone::Active};
    case ExportJobState::Stopping:
        return {tr("Stopping"), {}, StatusTone::Neutral};
    case ExportJobState::Done:
        return {tr("Done"), {}, StatusTone::Success};
    case ExportJobState::DoneWithWarnings:
        return {tr("Done, with a note"), {}, StatusTone::Warning};
    case ExportJobState::Failed:
        return {tr("Failed"), reasonOf(), StatusTone::Error};
    case ExportJobState::Stopped:
        return {tr("Stopped"), {}, StatusTone::Neutral};
    }
    return {};
}

StatusText outputStatus(const host::ExportOutputView& output, const host::ExportJobView& job) {
    if (!output.record.enabled)
        return {tr("Off"), {}, StatusTone::Neutral};
    switch (output.state) {
    case ExportOutputState::Pending:
        if (job.state == ExportJobState::Ready) {
            // The output's own blocker says what to fix; a job-level word would repeat it.
            for (const auto& issue : orderedIssues(output.preflight)) {
                if (issue.code == host::ExportPreflightCode::DestinationCheckPending)
                    return {tr("Checking where it saves\u2026"), qs(issue.reason), StatusTone::Neutral};
                if (issue.code == host::ExportPreflightCode::DestinationExists &&
                    issue.severity == host::ExportPreflightSeverity::NeedsConfirmation &&
                    !output.replaceConfirmed)
                    return {tr("Waiting for your OK"), qs(issue.reason), StatusTone::Neutral};
                if (issue.severity == host::ExportPreflightSeverity::Blocking)
                    return {issue.target == host::ExportPreflightTarget::Destination
                                ? tr("Needs output")
                                : tr("Needs attention"),
                            qs(issue.reason), StatusTone::Neutral};
            }
            if (!output.availability.available)
                return {tr("Needs attention"), qs(output.availability.reason), StatusTone::Neutral};
            return {tr("Ready"), {}, StatusTone::Neutral};
        }
        return {isActive(job.state) ? tr("Up next") : QString{}, {}, StatusTone::Neutral};
    case ExportOutputState::Preparing:
        return {tr("Getting ready"), {}, StatusTone::Active};
    case ExportOutputState::Rendering:
        return {tr("Writing") + (job.progress.outputCount <= 1 ? percentText(job.progress) : QString{}),
                {}, StatusTone::Active};
    case ExportOutputState::Verifying:
        return {tr("Checking the file"), {}, StatusTone::Active};
    case ExportOutputState::Published:
        return {tr("Saved"), {}, StatusTone::Success};
    case ExportOutputState::PublishedWithWarnings:
        return {tr("Saved, with a note"), {}, StatusTone::Warning};
    case ExportOutputState::Failed:
        return {tr("Failed"), {}, StatusTone::Error};
    case ExportOutputState::Stopped:
        return {tr("Stopped"), {}, StatusTone::Neutral};
    case ExportOutputState::Skipped:
        return {tr("Skipped"), {}, StatusTone::Neutral};
    }
    return {};
}

QString clockText(const std::chrono::milliseconds duration) {
    const auto total = std::max<std::int64_t>(0, duration.count()) / 1000;
    const auto hours = total / 3600;
    const auto minutes = (total / 60) % 60;
    const auto seconds = total % 60;
    return hours > 0 ? QStringLiteral("%1:%2:%3").arg(hours).arg(twoDigits(minutes), twoDigits(seconds))
                     : QStringLiteral("%1:%2").arg(minutes).arg(twoDigits(seconds));
}

QString frameProgressText(const host::ExportJobProgress& progress) {
    if (progress.framesTotal == 0)
        return {};
    if (progress.currentFrame.has_value())
        return QCoreApplication::translate("ExportPanel", "Frame %1 of %2")
            .arg(progress.framesCompleted + 1 > progress.framesTotal ? progress.framesTotal
                                                                      : progress.framesCompleted + 1)
            .arg(progress.framesTotal);
    return QCoreApplication::translate("ExportPanel", "%1 of %2 frames")
        .arg(progress.framesCompleted)
        .arg(progress.framesTotal);
}

QString elapsedText(const host::ExportJobProgress& progress) {
    return QCoreApplication::translate("ExportPanel", "Elapsed %1").arg(clockText(progress.elapsed));
}

QString remainingText(const host::ExportJobProgress& progress) {
    // No estimate exists while the Viewer holds the GPU, and none before enough frames finished.
    if (!progress.remaining.has_value() ||
        progress.route == host::ExportDeviceRoute::WaitingForViewer ||
        progress.route == host::ExportDeviceRoute::WaitingForMemory)
        return {};
    return QCoreApplication::translate("ExportPanel", "about %1 left")
        .arg(clockText(*progress.remaining));
}

QString leftText(const std::chrono::milliseconds remaining) {
    const auto seconds = std::max<std::int64_t>(1, (remaining.count() + 999) / 1000);
    if (seconds < 60)
        return QCoreApplication::translate("ExportPanel", "about %1 s left").arg(seconds);
    const auto minutes = (seconds + 30) / 60;
    if (minutes < 60)
        return QCoreApplication::translate("ExportPanel", "about %1 min left").arg(minutes);
    return QCoreApplication::translate("ExportPanel", "about %1 h %2 min left")
        .arg(minutes / 60)
        .arg(minutes % 60);
}

QString currentRenderText(const host::ExportJobView* job) {
    if (job == nullptr || !isActive(job->state))
        return tr("Idle");
    const auto& progress = job->progress;
    QStringList parts;
    parts.push_back(QCoreApplication::translate("ExportPanel", "Rendering %1").arg(qs(job->displayName)));
    if (isWaitingForViewer(*job))
        parts.push_back(tr("paused while you work in the Viewer"));
    else if (job->state != ExportJobState::Rendering)
        parts.push_back(jobStatus(*job).text.toLower());
    if (progress.framesTotal > 0)
        parts.push_back(QCoreApplication::translate("ExportPanel", "frame %1 of %2")
                            .arg(std::min(progress.framesCompleted + 1, progress.framesTotal))
                            .arg(progress.framesTotal));
    parts.push_back(QCoreApplication::translate("ExportPanel", "%1 elapsed").arg(clockText(progress.elapsed)));
    if (progress.remaining.has_value() && !isWaitingForViewer(*job))
        parts.push_back(leftText(*progress.remaining));
    return parts.join(QStringLiteral(" \u00b7 "));
}

QString startedText(const QDateTime& started) {
    const QLocale locale;
    return started.date() == QDate::currentDate() ? locale.toString(started.time(), QLocale::ShortFormat)
                                                  : locale.toString(started, QLocale::ShortFormat);
}

QString renderTimeText(const host::ExportJobView& job) {
    if (!job.run.has_value())
        return {};
    return clockText(job.progress.elapsed);
}

QString spanText(const std::optional<host::ExportResolvedRange>& range, const document::FrameRate rate) {
    if (!range.has_value())
        return {};
    const auto frames = range->last - range->first + 1;
    const auto first = assets::timecodeForFrames(range->first, rate);
    const auto count = frames == 1 ? tr("1 frame")
                                   : QCoreApplication::translate("ExportPanel", "%1 frames").arg(frames);
    if (frames == 1)
        return QStringLiteral("%1 \u00b7 %2").arg(first, count);
    return QCoreApplication::translate("ExportPanel", "%1 to %2 \u00b7 %3")
        .arg(first, assets::timecodeForFrames(range->last, rate), count);
}

QString rangeText(const host::ExportJobView& job) {
    const auto span = [&job]() -> QString {
        if (!job.resolvedRange.has_value())
            return {};
        return job.resolvedRange->first == job.resolvedRange->last
                   ? QStringLiteral("%1").arg(job.resolvedRange->first)
                   : QStringLiteral("%1–%2").arg(job.resolvedRange->first).arg(job.resolvedRange->last);
    };
    switch (job.record.render.range.kind) {
    case document::ExportRangeKind::WorkArea:
        return job.resolvedRange ? QCoreApplication::translate("ExportPanel", "Work area \u00b7 %1").arg(span())
                                 : tr("Work area");
    case document::ExportRangeKind::Composition:
        return job.resolvedRange
                   ? QCoreApplication::translate("ExportPanel", "Whole composition \u00b7 %1").arg(span())
                   : tr("Whole composition");
    case document::ExportRangeKind::Custom:
        return QCoreApplication::translate("ExportPanel", "Frames %1–%2")
            .arg(job.record.render.range.first)
            .arg(job.record.render.range.last);
    case document::ExportRangeKind::Frame:
        return QCoreApplication::translate("ExportPanel", "One frame \u00b7 %1").arg(job.record.render.range.first);
    }
    return {};
}

QString rangeTimecodeText(const host::ExportJobView& job, const document::FrameRate rate) {
    if (!job.resolvedRange.has_value())
        return {};
    const auto first = assets::timecodeForFrames(job.resolvedRange->first, rate);
    if (job.resolvedRange->first == job.resolvedRange->last)
        return first;
    return QStringLiteral("%1 – %2")
        .arg(first, assets::timecodeForFrames(job.resolvedRange->last, rate));
}

QString bytesText(const std::uint64_t bytes) { return assets::fileSizeText(bytes); }

QString presetText(const host::ExportOutputView& output,
                   const host::ExportPresetCatalogueSnapshot* catalogue) {
    if (catalogue != nullptr) {
        for (const auto& preset : catalogue->presets) {
            if (preset.id != output.record.presetId)
                continue;
            const auto name = qs(preset.displayName);
            return output.presetModified
                       ? QCoreApplication::translate("ExportPanel", "%1 (edited)").arg(name)
                       : name;
        }
    }
    return tr("Custom");
}

QString destinationText(const host::ExportOutputView& output) {
    if (!output.destination.firstPath.empty())
        return QString::fromStdString(output.destination.firstPath.generic_string());
    return qs(output.record.destination.path);
}

QString destinationToolTip(const host::ExportOutputView& output) {
    auto tip = destinationText(output);
    if (output.destination.fileCount > 1 && !output.destination.lastPath.empty())
        tip += QCoreApplication::translate("ExportPanel", "\nthrough %1 (%2 files)")
                   .arg(QString::fromStdString(output.destination.lastPath.generic_string()))
                   .arg(output.destination.fileCount);
    return tip;
}

std::vector<host::ExportPreflightIssue>
orderedIssues(const std::vector<host::ExportPreflightIssue>& issues) {
    auto ordered = issues;
    const auto rank = [](const host::ExportPreflightIssue& issue) {
        if (issue.code == host::ExportPreflightCode::DestinationCheckPending)
            return 4;
        switch (issue.severity) {
        case host::ExportPreflightSeverity::Blocking:
            return 0;
        case host::ExportPreflightSeverity::NeedsConfirmation:
            return 1;
        case host::ExportPreflightSeverity::Warning:
            return 2;
        }
        return 3;
    };
    std::ranges::stable_sort(ordered, {}, rank);
    return ordered;
}

bool awaitsReplaceConfirmation(const host::ExportOutputView& output) {
    return output.record.enabled && !output.replaceConfirmed &&
           std::ranges::any_of(output.preflight, [](const auto& issue) {
               return issue.code == host::ExportPreflightCode::DestinationExists &&
                      issue.severity == host::ExportPreflightSeverity::NeedsConfirmation;
           });
}

bool hasIssue(const host::ExportOutputView& output, const host::ExportPreflightCode code) {
    return std::ranges::any_of(output.preflight,
                               [code](const auto& issue) { return issue.code == code; });
}

} // namespace bloom::ui::exportpanel
