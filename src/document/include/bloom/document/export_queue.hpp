#pragma once

#include <bloom/core/id.hpp>
#include <bloom/document/ids.hpp>
#include <bloom/document/validation.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// The export queue's durable authoring records (ADR 0033; document schema 1.29).
//
// These are project truth: the jobs, their render settings and their outputs are saved in the
// project, mutated only through undoable commands, and restored by undo/redo. Nothing here
// describes a run: run status, progress, ETA, logs and verification results are runtime state
// owned by bloom::host::ExportQueueService and are never saved.
//
// The records are format-independent values. They name output formats with a closed document
// vocabulary (ExportFormat) and keep every setting the render needs, so a project exports the same
// way on a computer that lacks the user preset an output was made from. Mapping these values onto
// bloom::output presets and encoder settings is bloom::host's job; this module must not depend on
// src/output.
//
// The records are members of document::Project (Project::exportJobs()), their ids come from the
// `exportJob`/`exportOutput` allocator namespaces, and src/project persists them from document
// schema 1.29 (`project.exportQueue`).
namespace bloom::document {

// ExportJobId and ExportOutputId (ids.hpp) are stable, allocator-backed and project-global. A
// deleted or undone job never frees its id for reuse; reopen, undo and redo keep ids unchanged, so
// a panel may key rows by them.

// Bounds enforced by validation and by the schema. A command that would exceed one is rejected
// before it changes anything.
inline constexpr std::size_t kExportQueueMaximumJobs = 1024;
inline constexpr std::size_t kExportJobMaximumOutputs = 16;
inline constexpr std::size_t kExportTextMaximumBytes = 4096;

// Which composition frames a job renders. Frame indices are inclusive composition frame indices,
// the same indices the timeline ruler and the transport use.
enum class ExportRangeKind : std::uint8_t {
    // The composition's work area when it has one, otherwise the whole composition. Resolved when
    // the job starts, so moving the work area before Start changes what renders.
    WorkArea = 1,
    // Every frame of the composition, resolved when the job starts.
    Composition = 2,
    // `first` through `last`. Refused at Start when either lies outside the composition.
    Custom = 3,
    // One frame, `first`. An image format then writes a single still (no frame number needed).
    Frame = 4,
};

struct ExportRange final {
    ExportRangeKind kind = ExportRangeKind::WorkArea;
    std::uint64_t first = 0;
    std::uint64_t last = 0;
    friend bool operator==(const ExportRange&, const ExportRange&) = default;
};

// Job-level render settings: what is evaluated. Per-output settings (how it is encoded) are in
// ExportOutputSettings. Every output of a job renders the same frames.
struct ExportRenderSettings final {
    ExportRange range;
    friend bool operator==(const ExportRenderSettings&, const ExportRenderSettings&) = default;
};

// The closed output-format vocabulary of document 1.29. Values are serialized as tokens (ADR 0033,
// "Schema"); the numeric values here are not persisted. A newer minor may add tokens; an unknown
// token at a supported minor is a decode error, exactly as for other closed document vocabularies.
enum class ExportFormat : std::uint8_t {
    // Image formats: one file per frame (a still when the range is one frame).
    Png = 1,     // "png": 8-bit straight RGBA, display-referred
    OpenExr = 2, // "exr": float RGBA, working space or an explicit output space
    Tiff = 3,    // "tiff": 16-bit straight RGBA, display-referred
    Dpx = 4,     // "dpx": 10- or 16-bit RGB (issue #369)
    // Single-file movie and audio formats.
    ProResMov = 5, // "prores-mov"
    DnxhrMxf = 6,  // "dnxhr-mxf"
    H264Mov = 7,   // "h264-mov"
    H264Mp4 = 8,   // "h264-mp4" (issue #369)
    HevcMov = 9,   // "hevc-mov" (issue #369)
    HevcMp4 = 10,  // "hevc-mp4" (issue #369)
    Wav = 11,      // "wav": PCM WAV/BWF, RF64 when large
};

// Which encoder implementation a movie output asks for. A preference, not a guarantee: the
// runner reports the encoder it actually used in the output's verification.
enum class ExportEncoderPreference : std::uint8_t {
    Automatic = 1, // the platform's qualified default (software when one exists)
    Software = 2,  // refuse to start if only a hardware encoder is available
    Hardware = 3,  // VA-API on Linux; refuse to start without one
};

// File naming for image sequences. `startFrame` changes file names only (absent means the range's
// first frame). The pattern accepts the destination tokens (`<base>`, `<ext>`, `{name}`, `{job}`,
// `{format}`, `{date}`; see bloom::host::resolveDestination) and exactly one `####` or `{frame}`,
// padded to `framePadding` digits (widened when the range needs more).
struct ExportSequenceNaming final {
    std::optional<std::uint64_t> startFrame;
    std::uint32_t framePadding = 4;
    std::string pattern = "<base>.####.<ext>";
    friend bool operator==(const ExportSequenceNaming&, const ExportSequenceNaming&) = default;
};

// Everything an output needs to encode, independent of any preset still existing. Empty strings
// mean "the format's or configuration's default", never "unknown". Members that do not apply to
// the chosen format are kept (so switching format back restores them) and ignored by the runner.
struct ExportOutputSettings final {
    ExportFormat format = ExportFormat::H264Mp4;
    // Closed per format: ProRes "proxy|lt|422|hq|4444|4444xq", DNxHR "lb|sq|hq|hqx|444",
    // H.264 "high", HEVC "main|main10", DPX "10bit|16bit", WAV "s16|s24".
    std::string codecProfile;
    // Movie target bit rate for H.264/HEVC; absent means the preset default.
    std::optional<std::uint32_t> videoBitrateKbps;
    // EXR compression token ("zip", "piz", ...).
    std::string compression;
    // Scene-referred formats (EXR, DPX): output colour space id in the project's OCIO config;
    // empty keeps working-space pixels.
    std::string outputColorSpaceId;
    // Display-referred formats (PNG, TIFF, movies): display/view pair; empty means the config's
    // default display and view.
    std::string displayName;
    std::string viewName;
    // Bake look-tagged effects into the output. False is the VFX handoff policy (look off).
    bool bakeLook = true;
    bool includeAudio = true;
    std::uint32_t audioSampleRate = 48000;
    ExportEncoderPreference encoder = ExportEncoderPreference::Automatic;
    ExportSequenceNaming naming;
    friend bool operator==(const ExportOutputSettings&, const ExportOutputSettings&) = default;
};

// What happens when an output's destination already exists. In every policy Bloom refuses to
// replace a file someone else changed after it was checked (frame-output.md,
// ExternalModificationConflict).
enum class ExportOverwritePolicy : std::uint8_t {
    // The default. An existing destination is a preflight issue that needs the artist's explicit
    // confirmation (ExportQueueService::confirmReplace) before Start; never a silent replace.
    AskBeforeReplacing = 1,
    // Replace without asking.
    Replace = 2,
    // Refuse to start while the destination exists.
    Refuse = 3,
};

// Where an output is written. Uses the asset locator portability rules of document 1.22:
// "project-relative" stores a path relative to the project file's folder (preferred, so a project
// moved with its renders keeps working); "external" stores an absolute RFC 8089 file URI. An empty
// path means "not chosen yet" and blocks Start with a reason. The path may contain the destination
// tokens (`{name}`, `{job}`, `{format}`, `{date}`) in any component.
struct ExportDestination final {
    std::string portability = "project-relative";
    std::string path;
    ExportOverwritePolicy overwrite = ExportOverwritePolicy::AskBeforeReplacing;
    friend bool operator==(const ExportDestination&, const ExportDestination&) = default;
};

struct ExportOutput final {
    ExportOutputId id;
    // The catalogue preset the settings came from ("builtin:..." or "user:..."), for display and
    // the "modified" marker only. The runner never reads the preset again.
    std::string presetId;
    ExportOutputSettings settings;
    ExportDestination destination;
    bool enabled = true;
    // After a verified publish, import the result into the project's Assets through the ordinary
    // import path (an undoable import). A failed import never fails the export; it is reported
    // separately. Off by default.
    bool importIntoAssets = false;
    friend bool operator==(const ExportOutput&, const ExportOutput&) = default;
};

struct ExportJob final {
    ExportJobId id;
    CompositionId composition;
    // Empty means "show the composition's name".
    std::string name;
    ExportRenderSettings render;
    // Run order within the job. At least one output; every output renders the same frames.
    std::vector<ExportOutput> outputs;
    // Included by Start All. A disabled job can still be started on its own.
    bool enabled = true;
    // Optional row label, the same sRGB triple representation as a layer's label colour.
    // Compositions have no label colour to mirror, so a job's label is its own.
    std::optional<std::array<std::uint8_t, 3>> labelColor;
    friend bool operator==(const ExportJob&, const ExportJob&) = default;
};

// The project's queue. Vector order is run order.
struct ExportQueue final {
    std::vector<ExportJob> jobs;
    friend bool operator==(const ExportQueue&, const ExportQueue&) = default;
};

// ---------------------------------------------------------------------------------------------
// Closed vocabularies (document 1.29 tokens). Shared by the codec, scripting and the host.

[[nodiscard]] std::string_view exportFormatToken(ExportFormat format) noexcept;
[[nodiscard]] std::optional<ExportFormat> parseExportFormat(std::string_view token) noexcept;
[[nodiscard]] std::string_view exportRangeKindToken(ExportRangeKind kind) noexcept;
[[nodiscard]] std::optional<ExportRangeKind> parseExportRangeKind(std::string_view token) noexcept;
[[nodiscard]] std::string_view exportEncoderToken(ExportEncoderPreference encoder) noexcept;
[[nodiscard]] std::optional<ExportEncoderPreference>
parseExportEncoder(std::string_view token) noexcept;
[[nodiscard]] std::string_view exportOverwriteToken(ExportOverwritePolicy policy) noexcept;
[[nodiscard]] std::optional<ExportOverwritePolicy>
parseExportOverwrite(std::string_view token) noexcept;

// Image formats write one file per frame (a still when the range is one frame); the others write
// one movie or audio file.
[[nodiscard]] bool isImageExportFormat(ExportFormat format) noexcept;
// The codec profiles a format accepts; empty for formats that take no profile token. An empty
// `codecProfile` is always accepted (the format's default).
[[nodiscard]] std::span<const std::string_view> exportCodecProfiles(ExportFormat format) noexcept;

// Self-contained checks of one record: ids, bounds, closed vocabularies, text limits, the
// sequence pattern's frame token. Cross-record rules (unique ids, composition exists, queue size)
// are Project's.
[[nodiscard]] ValidationResult validateExportOutput(const ExportOutput& output);
[[nodiscard]] ValidationResult validateExportJob(const ExportJob& job);

} // namespace bloom::document
