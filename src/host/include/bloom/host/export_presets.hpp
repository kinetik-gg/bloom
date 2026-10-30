#pragma once

#include <bloom/document/color_settings.hpp>
#include <bloom/document/export_queue.hpp>
#include <bloom/output/media_output.hpp>
#include <bloom/output/output_analysis.hpp>

#include <array>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace bloom::runtime {
class TaskScheduler;
} // namespace bloom::runtime

// The export preset catalogue (ADR 0033): built-in presets plus the user's own, with
// categories and an explicit availability reason for every preset on this computer.
//
// A preset is a named ExportOutputSettings value. Applying one copies its settings into an output
// (an undoable project edit); the output never refers back to the preset for rendering. User
// presets are a per-user preference stored outside any project (a versioned JSON file in the
// platform user-data directory, read and written on the blocking-I/O executor). Saving, renaming
// or deleting a user preset is therefore not undoable and does not dirty the project.
//
// Encoder availability (H.264, HEVC, VA-API, the supervised media worker) is probed on a worker and
// published into the catalogue as an ExportEncoderStatus value. In the desktop application the
// existing H264EncoderService (Qt) forwards its state with setEncoderStatus(); headless owners call
// refreshEncoders().
//
// Threading guarantee: every member is non-blocking and performs no I/O on the calling thread.
// snapshot(), availabilityFor() and defaultPreset() read in-memory state only and are safe on the
// UI thread; the user-preset store and every encoder probe run on scheduler workers.
//
// Export runs on Linux in this version. On macOS and Windows every preset is listed and editable,
// and its platform availability is PlatformNotSupported (tracked by #371 and #370).
namespace bloom::host {

// Categories in catalogue order: video first. Image sequences are first-class presets in their own
// category but are never the default output.
enum class ExportPresetCategory : std::uint8_t {
    Video = 1,        // H.264 / HEVC MP4 and MOV, ProRes, DNxHR, and the Review H.264 deliverable
    VfxSequences = 2, // "VFX / sequences": EXR, DPX, PNG, TIFF, and the VFX handoff EXR deliverable
    Audio = 3,        // WAV/BWF
    User = 4,         // the user's saved presets, whatever their format
};

enum class ExportPresetOrigin : std::uint8_t { BuiltIn = 1, User = 2 };

// Why something cannot be used right now. `Available` is the only code with available == true.
enum class ExportAvailabilityCode : std::uint8_t {
    Available = 1,
    // The first worker probe has not answered yet; shown as "Checking…", not as unavailable.
    Probing = 2,
    // Linux x86-64 software H.264 needs Cisco OpenH264, which is not installed yet.
    EncoderNotInstalled = 3,
    // No qualified encoder for this codec on this computer.
    NoQualifiedEncoder = 4,
    // The codec is hardware-only (HEVC) or the output asks for hardware, and no VA-API render
    // node exposes the needed encode entry point. HEVC has no software encoder by design.
    HardwareEncoderMissing = 5,
    // The supervised media worker could not be started on this computer.
    WorkerUnavailable = 6,
    // Export runs on Linux in this version (macOS #371, Windows #370). Start is unavailable; the
    // queue stays fully editable.
    PlatformNotSupported = 7,
    // Project-dependent: a named deliverable needs the built-in ACES CG config.
    RequiresAcesConfig = 8,
    // Project-dependent: the selected OCIO config has no pinned revision.
    ConfigRevisionUnpinned = 9,
    // Project-dependent: an output colour space or display/view does not exist in the config.
    ColourNameMissing = 10,
};

// An action the UI can offer next to the reason. The engine never shows it; it only names it.
enum class ExportRemedy : std::uint8_t {
    None = 0,
    InstallOpenH264 = 1, // consent + download flow (H264EncoderService)
    LocateOpenH264 = 2,  // verify-and-copy a downloaded binary
    OpenProjectSettings = 3,
    ChooseSoftwareEncoder = 4,
};

struct ExportAvailability final {
    bool available = false;
    ExportAvailabilityCode code = ExportAvailabilityCode::Probing;
    // Human-readable, ready for a tooltip; empty when available.
    std::string reason;
    ExportRemedy remedy = ExportRemedy::None;
    [[nodiscard]] explicit operator bool() const noexcept { return available; }
};

// What a preset's format can do; drives which controls the panel shows.
struct ExportPresetCapabilities final {
    bool imageSequence = false;
    bool singleFile = false;
    bool still = false;
    bool alpha = false;
    bool audio = false;
    // The destination extension, without the dot ("exr", "mov", "mp4").
    std::string extension;
};

// A preset's stable id: "builtin:<name>" for built-ins (e.g. "builtin:h264-mp4",
// "builtin:review-h264", "builtin:prores-422hq-mov", "builtin:vfx-handoff-exr",
// "builtin:exr-sequence"), "user:<uuid>" for user presets. Ids never change once published; a
// renamed user preset keeps its id. An image preset renders a single still when its job's range
// is one frame; there is no separate still category.
struct ExportPreset final {
    std::string id;
    std::string displayName;
    std::string description;
    ExportPresetCategory category = ExportPresetCategory::Video;
    ExportPresetOrigin origin = ExportPresetOrigin::BuiltIn;
    document::ExportOutputSettings settings;
    ExportPresetCapabilities capabilities;
    // The preservation the format is declared to produce, facet by facet, in facet-id order. The
    // queue's AnalysisPolicy check accepts an analysis whose facet states fit this profile; the
    // panel shows it before Start so the old approval prompt's information is never lost.
    std::array<output::OutputPreservationStateV1, output::kOutputAnalysisFacetCountV1>
        expectedPreservation{};
    // Availability on this computer, independent of any project. Project-dependent requirements
    // (ACES config, colour names) are evaluated per output by availabilityFor().
    ExportAvailability platformAvailability;
    // True when availabilityFor() may refuse it for a project-dependent reason.
    bool projectDependent = false;
};

// The neutral twin of H264EncoderService::State plus the other encoders #369 adds. Produced on a
// worker; read anywhere.
struct ExportEncoderStatus final {
    enum class OpenH264 : std::uint8_t { NotNeeded, Checking, Installed, NotInstalled };
    output::H264EncoderV1 h264Software = output::H264EncoderV1::None;
    std::string h264SoftwareReason;
    OpenH264 openh264 = OpenH264::Checking;
    // A VA-API render node exists and its driver exposes the encode entry point.
    bool vaapiH264 = false;
    // HEVC is hardware-only: VA-API HEVC Main/Main 10 encode. There is no software HEVC encoder.
    bool vaapiHevc = false;
    // The supervised media worker can be started on this computer.
    bool mediaWorker = false;
    // The VA-API render node whose driver opened the encode entry points (#369).
    std::string vaapiRenderNode;
    // This build's platform runs exports (Linux in this version).
    bool exportPlatform = false;
    // True once the first worker probe has answered.
    bool probed = false;
};

struct ExportPresetCatalogueSnapshot final {
    // Monotonic; bumps on any preset, user-preset or encoder-status change.
    std::uint64_t revision = 0;
    // Category order (Video, VFX / sequences, Audio, User); built-ins in a fixed order within a
    // category, user presets by display name.
    std::vector<ExportPreset> presets;
    ExportEncoderStatus encoders;
};

inline constexpr std::string_view kExportDefaultVideoPresetId = "builtin:h264-mp4";
inline constexpr std::string_view kExportDpxUnavailableReason =
    "DPX sequences are not available in this build";
inline constexpr std::string_view kExportFallbackVideoPresetId = "builtin:prores-422hq-mov";

struct ExportDefaultPreset final {
    // The preset the new output uses.
    std::string presetId;
    // Why the preferred H.264 MP4 preset was not used (fallback chosen), or why the chosen preset
    // cannot start (neither available). Available when H.264 MP4 itself is usable.
    ExportAvailability preferredAvailability;
    bool usedFallback = false;
};

// The result of a user-preset file operation, delivered through the catalogue revision.
enum class ExportUserPresetStatus : std::uint8_t {
    Accepted = 1, // scheduled; the catalogue revision changes when it lands
    NameInvalid = 2,
    NameTaken = 3,
    UnknownPreset = 4,
    BuiltInReadOnly = 5,
    StoreUnavailable = 6, // no user-data directory, or the store failed to load
};

struct ExportPresetCatalogueOptions final {
    // Where the user-preset store lives (platform::userDataDirectory() in the application; a test
    // or headless owner may pass its own folder, or none to run without user presets).
    std::optional<std::filesystem::path> userDataDirectory;
    // The platform's pure H.264 answer (output::h264EncoderSupportV1()); refined by probes.
    ExportEncoderStatus initialEncoders;
};

// ---------------------------------------------------------------------------------------------
// Pure helpers shared by the catalogue, the runner, scripting and the panel. No I/O.

// The destination extension without the dot ("mp4", "exr", "tiff").
[[nodiscard]] std::string_view exportFormatExtension(document::ExportFormat format) noexcept;
// The `{format}` token value ("mp4", "prores", "exr").
[[nodiscard]] std::string_view exportFormatShortName(document::ExportFormat format) noexcept;
// The preservation the settings' format is declared to produce, facet by facet.
[[nodiscard]] std::array<output::OutputPreservationStateV1, output::kOutputAnalysisFacetCountV1>
exportExpectedPreservation(const document::ExportOutputSettings& settings) noexcept;
// The AnalysisPolicy rule: an analysed facet fits when it is at most the declared state on the
// scale Exact < Equivalent < Approximated < Omitted < ExternalReference. Missing and Unsupported
// never fit.
[[nodiscard]] bool exportPreservationFits(output::OutputPreservationStateV1 actual,
                                          output::OutputPreservationStateV1 expected) noexcept;
// Platform availability of `settings` on a computer with `encoders` (no project input).
[[nodiscard]] ExportAvailability
exportPlatformAvailability(const document::ExportOutputSettings& settings,
                           const ExportEncoderStatus& encoders);
// Project-dependent requirements: ACES names need the pinned built-in ACES CG config; names the
// built-in neutral config does not have are missing.
[[nodiscard]] ExportAvailability
exportProjectAvailability(const document::ExportOutputSettings& settings,
                          const document::ColorSettings& project);
// This build's platform answer for ExportEncoderStatus::exportPlatform.
[[nodiscard]] constexpr bool exportPlatformSupported() noexcept {
#if defined(__linux__)
    return true;
#else
    return false;
#endif
}

class ExportPresetCatalogue final {
  public:
    // Cheap: builds the built-in list and schedules nothing. The user-preset store is read by
    // loadUserPresets() on the blocking-I/O executor, never on the caller's thread.
    [[nodiscard]] static std::shared_ptr<ExportPresetCatalogue>
    create(ExportPresetCatalogueOptions options);
    ExportPresetCatalogue(const ExportPresetCatalogue&) = delete;
    ExportPresetCatalogue& operator=(const ExportPresetCatalogue&) = delete;
    ~ExportPresetCatalogue();

    // Any thread. Immutable; diff by revision.
    [[nodiscard]] std::shared_ptr<const ExportPresetCatalogueSnapshot> snapshot() const;

    // Any thread, cheap, no I/O: the preset's platform availability combined with the project's
    // colour settings (ACES deliverables, missing colour names).
    [[nodiscard]] ExportAvailability availabilityFor(const document::ExportOutputSettings& settings,
                                                     const document::ColorSettings& project) const;

    // The preset a new job's first output starts from ("Add to Export Queue"). Always a video
    // preset, never an image sequence: MP4/H.264 ("builtin:h264-mp4") when available here, else
    // the stated fallback video preset, ProRes 422 HQ MOV ("builtin:prores-422hq-mov"). When
    // neither is available on this computer the job still gets H.264 MP4, and its availability
    // reason (with a remedy such as Install OpenH264) blocks Start until the artist chooses.
    [[nodiscard]] ExportDefaultPreset defaultPreset() const;

    // Publishes a probe result (desktop: forwarded from H264EncoderService on the UI thread).
    void setEncoderStatus(ExportEncoderStatus status);
    // Headless owners: schedules the encoder probe on `scheduler`'s workers. Non-blocking.
    void refreshEncoders(runtime::TaskScheduler& scheduler);

    // Schedules the user-preset store read on `scheduler`'s blocking-I/O executor. Non-blocking;
    // the presets appear with the next catalogue revision. A missing file is an empty store; a
    // corrupt or newer-versioned file is kept untouched and reported as StoreUnavailable.
    void loadUserPresets(runtime::TaskScheduler& scheduler);

    // User presets. Non-blocking; the file write runs on the blocking-I/O executor. Session/
    // preference state: not undoable, never dirties the project.
    [[nodiscard]] ExportUserPresetStatus saveUserPreset(std::string_view displayName,
                                                        const document::ExportOutputSettings&);
    [[nodiscard]] ExportUserPresetStatus renameUserPreset(std::string_view id,
                                                          std::string_view displayName);
    [[nodiscard]] ExportUserPresetStatus deleteUserPreset(std::string_view id);

  private:
    explicit ExportPresetCatalogue(ExportPresetCatalogueOptions options);
    struct State;
    // Shared with the catalogue's worker tasks, which hold it weakly.
    std::shared_ptr<State> state_;
};

} // namespace bloom::host
