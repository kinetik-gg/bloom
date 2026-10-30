#pragma once

// The Export panel's wording: what each job and output state is called, how a range, a time, a
// size and a destination read. Pure functions of host snapshots, so the panel, the rows and the
// tests share one answer and nothing here owns a widget.

#include <bloom/document/composition_settings.hpp>
#include <bloom/host/export_queue.hpp>
#include <bloom/ui/kit/tokens.hpp>

#include <QDateTime>
#include <QString>

#include <chrono>
#include <optional>
#include <string>

namespace bloom::ui::exportpanel {

// How a status is inked. Waiting for the Viewer is Neutral on purpose: the Viewer has strict
// priority (ADR 0033), so it is the expected state while the artist plays back, not a fault.
enum class StatusTone : std::uint8_t { Neutral, Active, Success, Warning, Error };

struct StatusText final {
    QString text;
    // The sentence behind the word: a failure's reason, a refused command's reason. Tooltip.
    QString detail;
    StatusTone tone = StatusTone::Neutral;
};

[[nodiscard]] QString qs(const std::string& value);
[[nodiscard]] kit::Color toneColor(StatusTone tone);

// The plain sentence for a failed job, built from the typed failure alone: it names the frame or the
// output and says what to do. The host's own reason and diagnostic code are raw detail for the
// log's Details disclosure and are never part of this sentence.
[[nodiscard]] QString failureSentence(const host::ExportJobView& job);
// The raw detail for bug reports: the host's reason text and diagnostic code.
[[nodiscard]] QString failureDetails(const host::ExportJobView& job);

// The status of a job row, in plain words. Every tone is neutral ink except a failure.
[[nodiscard]] StatusText jobStatus(const host::ExportJobView& job);
// The status of an output row inside `job`: its own word once it has run, the job's progress while
// it renders, and what blocks it before it starts.
[[nodiscard]] StatusText outputStatus(const host::ExportOutputView& output,
                                      const host::ExportJobView& job);
[[nodiscard]] bool isWaitingForViewer(const host::ExportJobView& job);

// "0:07", "1:02:03".
[[nodiscard]] QString clockText(std::chrono::milliseconds duration);
// "Frame 120 of 240" (or "120 of 240 frames" when the current frame is unknown).
[[nodiscard]] QString frameProgressText(const host::ExportJobProgress& progress);
// "Elapsed 0:07" and, when the host gave one, "about 0:42 left". Empty remaining when absent.
[[nodiscard]] QString elapsedText(const host::ExportJobProgress& progress);
[[nodiscard]] QString remainingText(const host::ExportJobProgress& progress);
// "about 40 s left", "about 2 min left", "about 1 h 5 min left".
[[nodiscard]] QString leftText(std::chrono::milliseconds remaining);
// The Current Render strip's line: "Rendering SC020 · frame 404 of 2156 · 0:28 elapsed · about 2
// min left", or "Idle" when nothing runs (`job` null or not active).
[[nodiscard]] QString currentRenderText(const host::ExportJobView* job);
// The Started column: the time today, the date and time before today.
[[nodiscard]] QString startedText(const QDateTime& started);
// The Render Time column: the run's elapsed time once the job has run, empty before.
[[nodiscard]] QString renderTimeText(const host::ExportJobView& job);
// "0:00:00:00 to 0:00:09:23 · 240 frames" at `rate`; empty when the range does not resolve.
[[nodiscard]] QString spanText(const std::optional<host::ExportResolvedRange>& range,
                               document::FrameRate rate);

// "Work area", "Composition", "Frames 12 to 48", "Frame 12" with the resolved span in the
// parenthesis when it is known.
[[nodiscard]] QString rangeText(const host::ExportJobView& job);
// The resolved span as timecode at `rate`, for a tooltip; empty when it cannot resolve.
[[nodiscard]] QString rangeTimecodeText(const host::ExportJobView& job, document::FrameRate rate);

[[nodiscard]] QString bytesText(std::uint64_t bytes);

// The catalogue's name for the output's preset, "Modified" appended when its settings differ;
// "Custom" when no preset names it.
[[nodiscard]] QString presetText(const host::ExportOutputView& output,
                                 const host::ExportPresetCatalogueSnapshot* catalogue);
// The first path the output writes (a sequence: the first frame), or the destination as typed.
[[nodiscard]] QString destinationText(const host::ExportOutputView& output);
// The tooltip for a destination: the whole path, and for a sequence its last file and count.
[[nodiscard]] QString destinationToolTip(const host::ExportOutputView& output);

// Blocking issues first, then NeedsConfirmation, then warnings; DestinationCheckPending last.
[[nodiscard]] std::vector<host::ExportPreflightIssue>
orderedIssues(const std::vector<host::ExportPreflightIssue>& issues);
// The output would replace a file that exists, the artist has not said yes, and the output renders.
[[nodiscard]] bool awaitsReplaceConfirmation(const host::ExportOutputView& output);
[[nodiscard]] bool hasIssue(const host::ExportOutputView& output, host::ExportPreflightCode code);

} // namespace bloom::ui::exportpanel
