// Compile-and-value checks for the proposed export queue contract headers (ADR 0033). The
// service, catalogue and subscription are declared but not implemented yet, so this test only
// exercises value types and the constexpr helpers; it must not ODR-use an undefined member.

#include <bloom/document/export_queue.hpp>
#include <bloom/host/export_presets.hpp>
#include <bloom/host/export_queue.hpp>
#include <bloom/host/export_verification.hpp>

#include <iostream>
#include <type_traits>

namespace {

using namespace bloom;

static_assert(!std::is_copy_constructible_v<host::ExportQueueService>);
static_assert(!std::is_copy_constructible_v<host::ExportQueueSubscription>);
static_assert(std::is_nothrow_move_constructible_v<host::ExportQueueSubscription>);
static_assert(!std::is_copy_constructible_v<host::ExportPresetCatalogue>);

// The lifecycle helpers partition every state exactly once into Ready, active and terminal.
static_assert(!host::isActive(host::ExportJobState::Ready));
static_assert(!host::isTerminal(host::ExportJobState::Ready));
static_assert(host::isActive(host::ExportJobState::Waiting));
static_assert(!host::isRunning(host::ExportJobState::Waiting));
static_assert(host::isRunning(host::ExportJobState::Paused));
static_assert(host::isActive(host::ExportJobState::Stopping));
static_assert(!host::isActive(host::ExportJobState::Done));
static_assert(host::isTerminal(host::ExportJobState::Stopped));

// Contract table pins: session commands never touch project truth, edits always do.
static_assert(host::exportCommandKind(host::ExportCommand::Start) ==
              host::ExportCommandKind::Session);
static_assert(host::exportCommandKind(host::ExportCommand::Retry) ==
              host::ExportCommandKind::Session);
static_assert(host::exportCommandKind(host::ExportCommand::RevealOutput) ==
              host::ExportCommandKind::Session);
static_assert(host::exportCommandKind(host::ExportCommand::AddJob) ==
              host::ExportCommandKind::ProjectTruth);
static_assert(host::exportCommandKind(host::ExportCommand::RemoveFinished) ==
              host::ExportCommandKind::ProjectTruth);
static_assert(host::exportCommandKind(host::ExportCommand::EditOutput) ==
              host::ExportCommandKind::ProjectTruth);
static_assert(host::exportCommandKind(host::ExportCommand::ConfirmReplace) ==
              host::ExportCommandKind::Session);
static_assert(static_cast<std::size_t>(host::ExportCommand::ConfirmReplace) + 1 ==
              host::kExportCommandCount);

int expect(const bool condition, const char* message) {
    if (condition)
        return 0;
    std::cerr << message << '\n';
    return 1;
}

} // namespace

int main() {
    int failures = 0;

    document::ExportJob job;
    job.id = document::ExportJobId::fromRaw(7);
    job.composition = document::CompositionId::fromRaw(3);
    document::ExportOutput handoff;
    handoff.id = document::ExportOutputId::fromRaw(1);
    handoff.presetId = "builtin:vfx-handoff-exr";
    handoff.settings.format = document::ExportFormat::OpenExr;
    handoff.settings.compression = "piz";
    handoff.settings.outputColorSpaceId = "ACES2065-1";
    handoff.settings.bakeLook = false;
    handoff.settings.naming.startFrame = 1001;
    job.outputs.push_back(handoff);
    document::ExportQueue queue;
    queue.jobs.push_back(job);
    failures += expect(queue.jobs.front().render.range.kind == document::ExportRangeKind::WorkArea,
                       "a new job renders the work area by default");
    failures += expect(document::ExportOutputSettings{}.format == document::ExportFormat::H264Mp4,
                       "default output settings are the MP4/H.264 video default");
    failures += expect(queue.jobs.front().outputs.front().destination.overwrite ==
                           document::ExportOverwritePolicy::AskBeforeReplacing,
                       "a new output asks before replacing an existing file");
    failures += expect(!queue.jobs.front().outputs.front().importIntoAssets,
                       "import into Assets is off by default");
    failures += expect(!queue.jobs.front().labelColor.has_value(), "a new job has no label");

    host::ExportPreflightIssue exists;
    exists.code = host::ExportPreflightCode::DestinationExists;
    exists.severity = host::ExportPreflightSeverity::NeedsConfirmation;
    failures += expect(exists.target == host::ExportPreflightTarget::Destination,
                       "a preflight issue names the field it is about");

    host::ExportOutputView outputView;
    failures += expect(!outputView.estimatedBytes && !outputView.freeBytesAtDestination &&
                           outputView.import.status == host::ExportAssetImportStatus::NotRequested,
                       "size facts may be absent and imports start NotRequested");

    host::ExportJobView view;
    view.id = job.id;
    view.revision = 1;
    view.record = job;
    failures += expect(view.state == host::ExportJobState::Ready, "a job view starts Ready");
    failures += expect(!view.commands[host::ExportCommand::Pause].available &&
                           view.commands[host::ExportCommand::Pause].code ==
                               host::ExportUnavailableCode::NotApplicable,
                       "a default availability set refuses with NotApplicable");

    host::ExportQueueEvent event;
    event.sequence = 1;
    event.revision = 2;
    event.payload = host::ExportQueueDrained{};
    failures += expect(std::holds_alternative<host::ExportQueueDrained>(event.payload),
                       "queue drained is a typed terminal event");

    host::ExportCheckResult check;
    check.check = host::ExportCheck::ColourTags;
    check.outcome = host::ExportCheckOutcome::Passed;
    check.code = "export.check.colour-tags.match";
    check.expected = host::ExportColourTags{1, 1, 1, false};
    check.observed = host::ExportColourTags{1, 1, 1, false};
    failures += expect(check.expected == check.observed, "colour tags compare structurally");

    host::ExportPreset preset;
    preset.id = std::string(host::kExportDefaultVideoPresetId);
    failures +=
        expect(preset.category == host::ExportPresetCategory::Video &&
                   host::ExportPresetCategory::Video < host::ExportPresetCategory::VfxSequences,
               "the default preset is video and video sorts before sequences");
    failures +=
        expect(!preset.platformAvailability.available &&
                   preset.platformAvailability.code == host::ExportAvailabilityCode::Probing,
               "an unprobed preset reports Probing, not unavailable");

    return failures == 0 ? 0 : 1;
}
