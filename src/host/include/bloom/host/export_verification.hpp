#pragma once

#include <bloom/core/sha256.hpp>
#include <bloom/document/export_queue.hpp>
#include <bloom/output/output_analysis.hpp>

#include <array>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <variant>
#include <vector>

// Structured verification results of the export queue (ADR 0033). The queue's verify
// step replaces the old pre-export approval prompt: the expected preservation is visible before
// Start (from the preset), the analysis is accepted by an explicit policy instead of a modal
// question, and every published output carries these typed results afterwards.
//
// Everything here is data, not text. `code` strings are stable machine identifiers (dotted,
// lowercase) that the UI and scripting map to wording; there is no free-form message to parse.
// These are runtime results: they are never saved in the project.
namespace bloom::host {

// What one check examined.
enum class ExportCheck : std::uint8_t {
    // The per-frame OutputAnalysis facet states fit the preset's declared preservation profile.
    // This is the policy that approves the analysis digest in place of the old prompt.
    AnalysisPolicy = 1,
    // Each published artifact was reopened by its format's verifier (PNG/EXR/TIFF/DPX reopen
    // verifiers; worker reopen and quality checks for movies and WAV).
    Reopen = 2,
    // Published frame/file count equals the resolved range.
    FrameCount = 3,
    // Frame rate, duration, start timecode and sample cadence of a movie or WAV.
    Timing = 4,
    // Container colour tags (primaries, transfer, matrix, range) match the output colour intent
    // (issue #369; previously always BT.709 regardless of the chosen output).
    ColourTags = 5,
    // Container and codec/profile actually written (e.g. MP4 + H.264 High).
    Container = 6,
    // Audio channel layout, sample rate, bit depth and duration.
    Audio = 7,
    // Sequence names are complete, ascending and collision-free.
    Naming = 8,
    // The staged-file SHA-256 matched before publication.
    ArtifactDigest = 9,
    // Atomic publication result (published, durability warning, superseded, conflict).
    Publication = 10,
    // Which device evaluated the frames. Never Failed; Warning when any frame used the CPU
    // reference fallback, so a CPU frame is never presented as a GPU frame.
    DeviceProvenance = 11,
};

enum class ExportCheckOutcome : std::uint8_t {
    Passed = 1,
    Warning = 2,
    Failed = 3,
    NotApplicable = 4,
    // The output stopped before this check could run (cancel, earlier failure).
    NotRun = 5,
};

// ITU-T H.273 code points, as written in and read back from the container.
struct ExportColourTags final {
    std::uint8_t primaries = 2; // 2 = unspecified
    std::uint8_t transfer = 2;
    std::uint8_t matrix = 2;
    bool fullRange = false;
    friend bool operator==(const ExportColourTags&, const ExportColourTags&) = default;
};

struct ExportRationalValue final {
    std::int64_t numerator = 0;
    std::int64_t denominator = 1;
    friend bool operator==(const ExportRationalValue&, const ExportRationalValue&) = default;
};

// A typed expected/observed value. Tokens (codec, container, profile) use std::string; counts use
// std::uint64_t; frame rates and durations use ExportRationalValue.
using ExportCheckValue =
    std::variant<std::monostate, std::uint64_t, ExportRationalValue, ExportColourTags, std::string>;

struct ExportCheckResult final {
    ExportCheck check = ExportCheck::Reopen;
    ExportCheckOutcome outcome = ExportCheckOutcome::NotRun;
    // Stable identifier such as "export.check.colour-tags.mismatch".
    std::string code;
    ExportCheckValue expected;
    ExportCheckValue observed;
    // The composition frame index the result refers to, when it is about one frame.
    std::optional<std::uint64_t> frame;
};

enum class ExportPublicationOutcome : std::uint8_t {
    Published = 1,
    PublishedWithDurabilityWarning = 2,
    Superseded = 3,
    ExternalModificationConflict = 4,
    CancelledBeforePublication = 5,
    FailedBeforePublication = 6,
};

// What reached the destination. A sequence is summarized: listing ten thousand paths is the job
// of a file manager, not of a snapshot.
struct ExportArtifactSummary final {
    // 1 for a movie, WAV or still; the published frame count for a sequence.
    std::uint64_t publishedArtifacts = 0;
    std::filesystem::path firstPath;
    std::filesystem::path lastPath;
    std::uint64_t totalBytes = 0;
    // A single file's SHA-256, or for a sequence the SHA-256 over the ordered per-file digests.
    std::optional<core::Sha256Digest> digest;
    // The worst publication outcome over the artifacts.
    ExportPublicationOutcome publication = ExportPublicationOutcome::FailedBeforePublication;
};

// Diagnostics only: never part of any pixel, analysis or cache identity.
struct ExportDeviceProvenance final {
    std::uint64_t gpuFrames = 0;
    // Frames evaluated on the CPU reference path because no GPU generation could admit them (no
    // device, or a quarantined generation whose replacement failed). Explicit, never silent.
    std::uint64_t cpuReferenceFrames = 0;
    // Export GPU generations replaced after an unproven phase retirement during this output.
    std::uint32_t recoveredGenerations = 0;
    std::uint64_t gpuNativeDispatches = 0;
    std::uint64_t gpuReadbackSubmissions = 0;
    // Frames taken from the scene-referred composite cache instead of being evaluated again
    // (the output's useCachedFrames setting). Each reused frame still passes the same analysis,
    // approval and verification as an evaluated one.
    std::uint64_t framesReused = 0;
    // Encoder actually used, e.g. "openh264", "vaapi", "videotoolbox", "ffmpeg-prores".
    std::string encoder;
};

struct ExportFacetState final {
    output::OutputFacetIdV1 facet = output::OutputFacetIdV1::Pixels;
    output::OutputPreservationStateV1 state = output::OutputPreservationStateV1::Exact;
};

// One output's verification. Present in the snapshot from the moment the output reaches a
// terminal output state, and carried by the job's terminal event.
struct ExportOutputVerification final {
    document::ExportOutputId output;
    // Failed if any check failed, else Warning if any warned, else Passed. NotRun when the output
    // never started (job stopped earlier).
    ExportCheckOutcome overall = ExportCheckOutcome::NotRun;
    // The eleven facet states of the accepted analysis, in facet-id order.
    std::array<ExportFacetState, output::kOutputAnalysisFacetCountV1> facets{};
    // The analysis digest the policy approved (absent when analysis never completed).
    std::optional<core::Sha256Digest> analysisDigest;
    std::vector<ExportCheckResult> checks;
    ExportArtifactSummary artifacts;
    ExportDeviceProvenance provenance;
};

// Problems known before Start. Destination facts (exists, writable, free space, the existing
// file's fingerprint) are sampled on a blocking-I/O worker and cached; nothing here is computed by
// touching the filesystem on the caller's thread.
enum class ExportPreflightCode : std::uint8_t {
    DestinationMissing = 1,     // no path chosen
    DestinationNotWritable = 2, // folder missing or not writable
    // The destination (or, for a sequence, a file the pattern would write) exists.
    // NeedsConfirmation under AskBeforeReplacing, Warning under Replace, Blocking under Refuse.
    DestinationExists = 3,
    NamingInvalid = 4,            // pattern or destination token refused
    InsufficientSpace = 5,        // estimate exceeds free space (issue #369)
    PresetUnavailable = 6,        // encoder, platform or project config
    CompositionMissing = 7,       // composition deleted
    RangeOutsideComposition = 8,  // custom range no longer fits
    DuplicateTarget = 9,          // two enabled outputs write the same file
    DestinationCheckPending = 10, // the worker has not sampled this destination yet
    NoEnabledOutputs = 11,        // every output of the job is disabled (target Job)
};

// Which part of the job or output an issue is about, so the panel can mark the right field.
enum class ExportPreflightTarget : std::uint8_t {
    Job = 1,
    Composition = 2,
    Range = 3,
    Destination = 4,
    Naming = 5,
    Container = 6,
    Codec = 7,
    Colour = 8,
    Audio = 9,
    Encoder = 10,
};

enum class ExportPreflightSeverity : std::uint8_t {
    // Start is unavailable until the issue is fixed.
    Blocking = 1,
    // Shown; does not block.
    Warning = 2,
    // Start is unavailable until the artist confirms (ExportCommand::ConfirmReplace). Used only
    // for an existing destination under AskBeforeReplacing.
    NeedsConfirmation = 3,
};

struct ExportPreflightIssue final {
    ExportPreflightCode code = ExportPreflightCode::DestinationMissing;
    ExportPreflightTarget target = ExportPreflightTarget::Destination;
    ExportPreflightSeverity severity = ExportPreflightSeverity::Blocking;
    // Human-readable, ready for a tooltip.
    std::string reason;
};

// The result of "Import into Assets after render" for one output. Reported separately from the
// export: a failed import never changes the export's success or its verification.
enum class ExportAssetImportStatus : std::uint8_t {
    // The output's importIntoAssets flag is off.
    NotRequested = 0,
    // Waiting for the verified publish, preparing on a worker, or waiting to apply on the owner
    // thread (for example while an interactive edit gesture is open).
    Pending = 1,
    // A new asset was added (one undoable import).
    Imported = 2,
    // An asset already referred to this file; it was re-identified instead of duplicated.
    Refreshed = 3,
    Failed = 4,
    // The output did not publish (failed or stopped), so there was nothing to import.
    Skipped = 5,
};

struct ExportAssetImportResult final {
    document::ExportOutputId output;
    ExportAssetImportStatus status = ExportAssetImportStatus::NotRequested;
    // The imported or refreshed asset.
    std::optional<document::AssetId> asset;
    // Stable identifier for a failure ("export.import.read-only-project", "export.import.probe").
    std::string code;
    // Human-readable, ready for display; empty unless Failed or Skipped.
    std::string reason;
};

} // namespace bloom::host
