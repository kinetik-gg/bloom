#include "export_panel_text.hpp"

#include "assets_editor_internal.hpp"

#include <QCoreApplication>

#include <algorithm>
#include <ranges>

namespace bloom::ui::exportpanel {
namespace {

QString tr(const char* source) { return QCoreApplication::translate("ExportPanel", source); }

using host::ExportJobState;
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
        return kit::Color::Ok;
    case StatusTone::Warning:
        return kit::Color::Warn;
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

StatusText jobStatus(const host::ExportJobView& job) {
    const auto reasonOf = [&job] { return failureSentence(job); };
    switch (job.state) {
    case ExportJobState::Ready: {
        const auto& start = job.commands[host::ExportCommand::Start];
        if (start.available)
            return {tr("Ready"), {}, StatusTone::Neutral};
        switch (start.code) {
        case host::ExportUnavailableCode::DestinationCheckPending:
            return {tr("Checking destination…"), qs(start.reason), StatusTone::Neutral};
        case host::ExportUnavailableCode::NeedsReplaceConfirmation:
            return {tr("Needs confirmation"), qs(start.reason), StatusTone::Warning};
        case host::ExportUnavailableCode::PlatformNotSupported:
            // Editing works everywhere; only running is a Linux feature in this version.
            return {tr("Ready"), qs(start.reason), StatusTone::Neutral};
        default:
            return {tr("Not ready"), qs(start.reason), StatusTone::Warning};
        }
    }
    case ExportJobState::Waiting:
        return {tr("Waiting"), {}, StatusTone::Neutral};
    case ExportJobState::Preparing:
        return {tr("Preparing"), {}, StatusTone::Active};
    case ExportJobState::Rendering:
        switch (job.progress.route) {
        case host::ExportDeviceRoute::WaitingForViewer:
            return {tr("Waiting for the Viewer"),
                    tr("Waiting for the Viewer to finish playback"), StatusTone::Neutral};
        case host::ExportDeviceRoute::RecoveringGpu:
            return {tr("Recovering GPU"), {}, StatusTone::Active};
        case host::ExportDeviceRoute::CpuReference:
            return {tr("Rendering (CPU)"), tr("Rendering on the CPU reference path"),
                    StatusTone::Active};
        default:
            return {tr("Rendering"), {}, StatusTone::Active};
        }
    case ExportJobState::Pausing:
        return {tr("Pausing"), {}, StatusTone::Neutral};
    case ExportJobState::Paused:
        return {tr("Paused"), {}, StatusTone::Neutral};
    case ExportJobState::Verifying:
        return {tr("Verifying"), {}, StatusTone::Active};
    case ExportJobState::Stopping:
        return {tr("Stopping"), {}, StatusTone::Neutral};
    case ExportJobState::Done:
        return {tr("Done"), {}, StatusTone::Success};
    case ExportJobState::DoneWithWarnings:
        return {tr("Done with warnings"), {}, StatusTone::Warning};
    case ExportJobState::Failed:
        return {tr("Failed"), reasonOf(), StatusTone::Error};
    case ExportJobState::Stopped:
        return {tr("Stopped"), {}, StatusTone::Neutral};
    }
    return {};
}

StatusText outputStatus(const host::ExportOutputView& output) {
    switch (output.state) {
    case ExportOutputState::Pending:
        return {};
    case ExportOutputState::Preparing:
        return {tr("Preparing"), {}, StatusTone::Active};
    case ExportOutputState::Rendering:
        return {tr("Rendering"), {}, StatusTone::Active};
    case ExportOutputState::Verifying:
        return {tr("Verifying"), {}, StatusTone::Active};
    case ExportOutputState::Published:
        return {tr("Published"), {}, StatusTone::Success};
    case ExportOutputState::PublishedWithWarnings:
        return {tr("Published with warnings"), {}, StatusTone::Warning};
    case ExportOutputState::Failed:
        return {tr("Failed"), {}, StatusTone::Error};
    case ExportOutputState::Stopped:
        return {tr("Stopped"), {}, StatusTone::Neutral};
    case ExportOutputState::Skipped:
        return {tr("Skipped"), {}, StatusTone::Neutral};
    }
    return {};
}

QString runLine(const host::ExportJobView& job) {
    if (isWaitingForViewer(job))
        return tr("Waiting for the Viewer to finish playback");
    return jobStatus(job).text;
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
        progress.route == host::ExportDeviceRoute::WaitingForViewer)
        return {};
    return QCoreApplication::translate("ExportPanel", "about %1 left")
        .arg(clockText(*progress.remaining));
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
        return job.resolvedRange ? QCoreApplication::translate("ExportPanel", "Work area (%1)").arg(span())
                                 : tr("Work area");
    case document::ExportRangeKind::Composition:
        return job.resolvedRange
                   ? QCoreApplication::translate("ExportPanel", "Composition (%1)").arg(span())
                   : tr("Composition");
    case document::ExportRangeKind::Custom:
        return QCoreApplication::translate("ExportPanel", "Frames %1–%2")
            .arg(job.record.render.range.first)
            .arg(job.record.render.range.last);
    case document::ExportRangeKind::Frame:
        return QCoreApplication::translate("ExportPanel", "Frame %1").arg(job.record.render.range.first);
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
                       ? QCoreApplication::translate("ExportPanel", "%1 (modified)").arg(name)
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

bool hasIssue(const host::ExportOutputView& output, const host::ExportPreflightCode code) {
    return std::ranges::any_of(output.preflight,
                               [code](const auto& issue) { return issue.code == code; });
}

} // namespace bloom::ui::exportpanel
