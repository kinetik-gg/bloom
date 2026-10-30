#include <bloom/host/export_presets.hpp>

#include "export_preset_store.hpp"

#include <bloom/color/bloom_neutral_builtin.hpp>
#include <bloom/core/file_uri.hpp>
#include <bloom/runtime/task_scheduler.hpp>

#include <algorithm>
#include <mutex>
#include <random>
#include <utility>

namespace bloom::host {
namespace {

using document::ExportEncoderPreference;
using document::ExportFormat;
using document::ExportOutputSettings;
using F = output::OutputFacetIdV1;
using S = output::OutputPreservationStateV1;
using Facets = std::array<S, output::kOutputAnalysisFacetCountV1>;

constexpr std::string_view kPlatformReason = "Export runs on Linux in this version";
constexpr std::string_view kReviewDisplay = "Rec.1886 Rec.709 - Display";
constexpr std::string_view kReviewView = "ACES 1.0 - SDR Video";
constexpr std::string_view kAcesInterchange = "ACES2065-1";

ExportAvailability available() {
    ExportAvailability result;
    result.available = true;
    result.code = ExportAvailabilityCode::Available;
    return result;
}

ExportAvailability unavailable(const ExportAvailabilityCode code, std::string reason,
                               const ExportRemedy remedy = ExportRemedy::None) {
    ExportAvailability result;
    result.code = code;
    result.reason = std::move(reason);
    result.remedy = remedy;
    return result;
}

bool needsWorker(const ExportFormat format) {
    return format != ExportFormat::Png && format != ExportFormat::OpenExr;
}

bool isH264(const ExportFormat format) {
    return format == ExportFormat::H264Mov || format == ExportFormat::H264Mp4;
}

bool isHevc(const ExportFormat format) {
    return format == ExportFormat::HevcMov || format == ExportFormat::HevcMp4;
}

// Facet order: Pixels, Precision, Color, AlphaAssociation, Channels, DataWindow, DisplayWindow,
// PixelAspect, Compression, Metadata, ExternalDependencies. Each entry is the worst state the
// format is declared to produce; an analysis fits when every facet is at most that state.
Facets expectedFor(const ExportOutputSettings& settings) {
    switch (settings.format) {
    case ExportFormat::Png:
    case ExportFormat::Tiff:
        return {S::Approximated,
                S::Approximated,
                S::Approximated,
                S::Approximated,
                S::Exact,
                S::Exact,
                S::Exact,
                S::Exact,
                S::Equivalent,
                S::Equivalent,
                S::ExternalReference};
    case ExportFormat::OpenExr:
    case ExportFormat::Dpx:
        return {S::Approximated, S::Approximated,     S::Approximated,
                S::Equivalent,   S::Equivalent,       S::Exact,
                S::Exact,        S::Approximated,     S::Equivalent,
                S::Equivalent,   S::ExternalReference};
    case ExportFormat::Wav:
        return {
            S::Omitted, S::Approximated, S::Omitted, S::Omitted,    S::Equivalent,       S::Exact,
            S::Exact,   S::Exact,        S::Exact,   S::Equivalent, S::ExternalReference};
    default: {
        const bool alpha = settings.format == ExportFormat::ProResMov &&
                           (settings.codecProfile == "4444" || settings.codecProfile == "4444xq");
        return {S::Approximated, S::Approximated,
                S::Approximated, alpha ? S::Approximated : S::Omitted,
                S::Omitted, // audio may be absent from the composition
                S::Exact,        S::Exact,
                S::Exact,        S::Approximated,
                S::Equivalent,   S::ExternalReference};
    }
    }
}

ExportPresetCapabilities capabilitiesFor(const ExportOutputSettings& settings) {
    ExportPresetCapabilities result;
    const auto format = settings.format;
    result.imageSequence = document::isImageExportFormat(format);
    result.still = result.imageSequence;
    result.singleFile = !result.imageSequence;
    result.alpha = format == ExportFormat::Png || format == ExportFormat::OpenExr ||
                   format == ExportFormat::Tiff ||
                   (format == ExportFormat::ProResMov &&
                    (settings.codecProfile == "4444" || settings.codecProfile == "4444xq"));
    result.audio = !result.imageSequence;
    result.extension = std::string(exportFormatExtension(format));
    return result;
}

struct BuiltIn final {
    std::string_view id;
    std::string_view name;
    std::string_view description;
    ExportPresetCategory category;
    ExportOutputSettings settings;
    bool projectDependent = false;
};

ExportOutputSettings video(const ExportFormat format, std::string profile) {
    ExportOutputSettings settings;
    settings.format = format;
    settings.codecProfile = std::move(profile);
    return settings;
}

ExportOutputSettings image(const ExportFormat format, std::string profile = {},
                           std::string compression = {}) {
    auto settings = video(format, std::move(profile));
    settings.compression = std::move(compression);
    settings.includeAudio = false;
    return settings;
}

std::vector<BuiltIn> makeBuiltIns() {
    std::vector<BuiltIn> list;
    const auto add = [&](std::string_view id, std::string_view name, std::string_view description,
                         ExportPresetCategory category, ExportOutputSettings settings,
                         bool dependent = false) {
        list.push_back({id, name, description, category, std::move(settings), dependent});
    };
    using C = ExportPresetCategory;
    add(kExportDefaultVideoPresetId, "H.264 MP4", "H.264 High in MP4, the everyday video default",
        C::Video, video(ExportFormat::H264Mp4, "high"));
    add("builtin:h264-mov", "H.264 MOV", "H.264 High in QuickTime MOV", C::Video,
        video(ExportFormat::H264Mov, "high"));
    {
        auto review = video(ExportFormat::H264Mp4, "high");
        review.displayName = std::string(kReviewDisplay);
        review.viewName = std::string(kReviewView);
        add("builtin:review-h264", "Review H.264", "Rec.709 review deliverable with the look on",
            C::Video, std::move(review), true);
    }
    auto hevc = video(ExportFormat::HevcMp4, "main");
    hevc.encoder = ExportEncoderPreference::Hardware;
    add("builtin:hevc-mp4", "HEVC MP4 (hardware)", "HEVC Main in MP4 on VA-API hardware", C::Video,
        hevc);
    hevc.format = ExportFormat::HevcMov;
    add("builtin:hevc-mov", "HEVC MOV (hardware)", "HEVC Main in MOV on VA-API hardware", C::Video,
        hevc);
    add("builtin:prores-proxy-mov", "ProRes 422 Proxy MOV", "ProRes 422 Proxy", C::Video,
        video(ExportFormat::ProResMov, "proxy"));
    add("builtin:prores-lt-mov", "ProRes 422 LT MOV", "ProRes 422 LT", C::Video,
        video(ExportFormat::ProResMov, "lt"));
    add("builtin:prores-422-mov", "ProRes 422 MOV", "ProRes 422", C::Video,
        video(ExportFormat::ProResMov, "422"));
    add(kExportFallbackVideoPresetId, "ProRes 422 HQ MOV", "ProRes 422 HQ", C::Video,
        video(ExportFormat::ProResMov, "hq"));
    add("builtin:prores-4444-mov", "ProRes 4444 MOV", "ProRes 4444 with alpha", C::Video,
        video(ExportFormat::ProResMov, "4444"));
    add("builtin:prores-4444xq-mov", "ProRes 4444 XQ MOV", "ProRes 4444 XQ with alpha", C::Video,
        video(ExportFormat::ProResMov, "4444xq"));
    add("builtin:dnxhr-hq-mxf", "DNxHR HQ MXF", "DNxHR HQ in MXF", C::Video,
        video(ExportFormat::DnxhrMxf, "hq"));
    {
        auto handoff = image(ExportFormat::OpenExr, {}, "piz");
        handoff.outputColorSpaceId = std::string(kAcesInterchange);
        handoff.bakeLook = false;
        handoff.naming.startFrame = 1001;
        add("builtin:vfx-handoff-exr", "VFX handoff EXR",
            "ACES2065-1 OpenEXR, PIZ, from frame 1001, look off", C::VfxSequences,
            std::move(handoff), true);
    }
    add("builtin:exr-sequence", "OpenEXR sequence", "Scene-linear OpenEXR, ZIP", C::VfxSequences,
        image(ExportFormat::OpenExr, {}, "zip"));
    add("builtin:dpx-sequence", "DPX sequence", "10-bit RGB DPX", C::VfxSequences,
        image(ExportFormat::Dpx, "10bit"));
    add("builtin:png-sequence", "PNG sequence", "8-bit sRGB PNG with alpha", C::VfxSequences,
        image(ExportFormat::Png));
    add("builtin:tiff-sequence", "TIFF sequence", "16-bit sRGB TIFF with alpha", C::VfxSequences,
        image(ExportFormat::Tiff));
    {
        auto wav = image(ExportFormat::Wav, "s24");
        wav.includeAudio = true;
        add("builtin:wav", "PCM WAV/BWF", "Uncompressed 24-bit PCM, RF64 when large", C::Audio,
            std::move(wav));
    }
    return list;
}

} // namespace

std::string_view exportFormatExtension(const ExportFormat format) noexcept {
    switch (format) {
    case ExportFormat::Png:
        return "png";
    case ExportFormat::OpenExr:
        return "exr";
    case ExportFormat::Tiff:
        return "tiff";
    case ExportFormat::Dpx:
        return "dpx";
    case ExportFormat::ProResMov:
    case ExportFormat::H264Mov:
    case ExportFormat::HevcMov:
        return "mov";
    case ExportFormat::DnxhrMxf:
        return "mxf";
    case ExportFormat::H264Mp4:
    case ExportFormat::HevcMp4:
        return "mp4";
    case ExportFormat::Wav:
        return "wav";
    }
    return {};
}

namespace {

ExportFormatDescription describe(const ExportFormat format, const std::string_view displayName,
                                 std::vector<ExportFormatToken> profiles,
                                 const std::string_view defaultProfile,
                                 std::vector<ExportFormatToken> compressions = {},
                                 const std::string_view defaultCompression = {}) {
    ExportFormatDescription description;
    description.format = format;
    description.token = document::exportFormatToken(format);
    description.displayName = displayName;
    description.extension = exportFormatExtension(format);
    description.imageSequence = document::isImageExportFormat(format);
    description.codecProfiles = std::move(profiles);
    description.defaultCodecProfile = defaultProfile;
    description.compressions = std::move(compressions);
    description.defaultCompression = defaultCompression;
    return description;
}

std::vector<ExportFormatDescription> makeFormatDescriptions() {
    const std::vector<ExportFormatToken> prores{
        {"proxy", "ProRes 422 Proxy"}, {"lt", "ProRes 422 LT"}, {"422", "ProRes 422"},
        {"hq", "ProRes 422 HQ"},       {"4444", "ProRes 4444"}, {"4444xq", "ProRes 4444 XQ"}};
    const std::vector<ExportFormatToken> dnxhr{{"lb", "DNxHR LB"},
                                               {"sq", "DNxHR SQ"},
                                               {"hq", "DNxHR HQ"},
                                               {"hqx", "DNxHR HQX"},
                                               {"444", "DNxHR 444"}};
    const std::vector<ExportFormatToken> h264{{"high", "High"}};
    const std::vector<ExportFormatToken> hevc{{"main", "Main"}, {"main10", "Main 10"}};
    const std::vector<ExportFormatToken> exr{
        {"zip", "ZIP"}, {"piz", "PIZ"}, {"zips", "ZIPS"}, {"none", "Uncompressed"}};
    return {
        describe(ExportFormat::Png, "PNG", {}, {}),
        describe(ExportFormat::OpenExr, "OpenEXR", {}, {}, exr, "zip"),
        describe(ExportFormat::Tiff, "TIFF", {}, {}),
        describe(ExportFormat::Dpx, "DPX", {{"10bit", "10-bit"}, {"16bit", "16-bit"}}, "10bit"),
        describe(ExportFormat::ProResMov, "ProRes MOV", prores, "hq"),
        describe(ExportFormat::DnxhrMxf, "DNxHR MXF", dnxhr, "hq"),
        describe(ExportFormat::H264Mov, "H.264 MOV", h264, "high"),
        describe(ExportFormat::H264Mp4, "H.264 MP4", h264, "high"),
        describe(ExportFormat::HevcMov, "HEVC MOV", hevc, "main"),
        describe(ExportFormat::HevcMp4, "HEVC MP4", hevc, "main"),
        describe(ExportFormat::Wav, "WAV", {{"s16", "16-bit PCM"}, {"s24", "24-bit PCM"}}, "s24"),
    };
}

bool representableComponent(const std::filesystem::path& component) {
    const auto text = component.generic_string();
    return !text.empty() && text != "." && text != ".." && text.find(':') == std::string::npos &&
           text.find('\\') == std::string::npos;
}

} // namespace

const std::vector<ExportFormatDescription>& exportFormatDescriptions() {
    static const auto descriptions = makeFormatDescriptions();
    return descriptions;
}

const ExportFormatDescription& exportFormatDescription(const ExportFormat format) {
    const auto& all = exportFormatDescriptions();
    const auto found = std::ranges::find(all, format, &ExportFormatDescription::format);
    return found != all.end() ? *found : all.front();
}

document::ExportDestination defaultExportDestination(const ExportFormat format) {
    return {.portability = "project-relative",
            .path = "Exports/{name}." + std::string(exportFormatExtension(format)),
            .overwrite = document::ExportOverwritePolicy::AskBeforeReplacing};
}

document::ExportJob defaultExportJob(const document::CompositionId composition,
                                     const ExportDefaultPreset& preset) {
    document::ExportJob job;
    job.composition = composition;
    job.render.range = {document::ExportRangeKind::WorkArea, 0, 0};
    document::ExportOutput output;
    output.presetId = preset.presetId;
    output.settings = preset.settings;
    output.destination = preset.destination;
    output.enabled = true;
    output.importIntoAssets = false;
    job.outputs.push_back(std::move(output));
    return job;
}

std::optional<document::ExportDestination>
exportDestinationForPath(const std::filesystem::path& chosen,
                         const std::optional<std::filesystem::path>& projectFolder,
                         const document::ExportOverwritePolicy overwrite) {
    if (chosen.empty() || !chosen.is_absolute())
        return std::nullopt;
    const auto target = chosen.lexically_normal();
    if (target.filename().empty())
        return std::nullopt;
    if (projectFolder && projectFolder->is_absolute()) {
        const auto folder = projectFolder->lexically_normal();
        const auto relative = target.lexically_relative(folder);
        const bool inside = !relative.empty() && relative.is_relative() &&
                            std::ranges::all_of(relative, representableComponent);
        if (inside)
            return document::ExportDestination{.portability = "project-relative",
                                               .path = relative.generic_string(),
                                               .overwrite = overwrite};
    }
    const auto uri = core::fileUriFromAbsolutePath(target);
    if (!uri)
        return std::nullopt;
    return document::ExportDestination{
        .portability = "external", .path = *uri, .overwrite = overwrite};
}

std::string_view exportFormatShortName(const ExportFormat format) noexcept {
    switch (format) {
    case ExportFormat::ProResMov:
        return "prores";
    case ExportFormat::DnxhrMxf:
        return "dnxhr";
    case ExportFormat::H264Mov:
        return "h264";
    case ExportFormat::H264Mp4:
        return "mp4";
    case ExportFormat::HevcMov:
    case ExportFormat::HevcMp4:
        return "hevc";
    default:
        return exportFormatExtension(format);
    }
}

std::array<output::OutputPreservationStateV1, output::kOutputAnalysisFacetCountV1>
exportExpectedPreservation(const document::ExportOutputSettings& settings) noexcept {
    return expectedFor(settings);
}

bool exportPreservationFits(const output::OutputPreservationStateV1 actual,
                            const output::OutputPreservationStateV1 expected) noexcept {
    if (actual == S::Missing || actual == S::Unsupported)
        return false;
    return static_cast<std::uint8_t>(actual) <= static_cast<std::uint8_t>(expected);
}

std::optional<ExportPreset> builtInExportPreset(const std::string_view id) {
    for (const auto& builtIn : makeBuiltIns())
        if (builtIn.id == id) {
            ExportPreset preset;
            preset.id = std::string(builtIn.id);
            preset.displayName = std::string(builtIn.name);
            preset.description = std::string(builtIn.description);
            preset.category = builtIn.category;
            preset.settings = builtIn.settings;
            preset.capabilities = capabilitiesFor(builtIn.settings);
            preset.expectedPreservation = expectedFor(builtIn.settings);
            preset.projectDependent = builtIn.projectDependent;
            return preset;
        }
    return std::nullopt;
}

ExportAvailability exportPlatformAvailability(const document::ExportOutputSettings& settings,
                                              const ExportEncoderStatus& encoders) {
    if (!encoders.exportPlatform)
        return unavailable(ExportAvailabilityCode::PlatformNotSupported,
                           std::string(kPlatformReason));
    const auto format = settings.format;
    if (format == ExportFormat::Dpx)
        return unavailable(ExportAvailabilityCode::NoQualifiedEncoder,
                           std::string(kExportDpxUnavailableReason));
    if (!needsWorker(format))
        return available();
    if (!encoders.probed)
        return unavailable(ExportAvailabilityCode::Probing, "Checking encoders…");
    if (!encoders.mediaWorker)
        return unavailable(ExportAvailabilityCode::WorkerUnavailable,
                           "The media worker could not be started on this computer");
    if (isHevc(format)) {
        if (settings.encoder == ExportEncoderPreference::Software)
            return unavailable(ExportAvailabilityCode::NoQualifiedEncoder,
                               "HEVC has no software encoder; choose Automatic or Hardware");
        if (!encoders.vaapiHevc)
            return unavailable(ExportAvailabilityCode::HardwareEncoderMissing,
                               "HEVC needs a VA-API render node with HEVC encode");
        return available();
    }
    if (isH264(format)) {
        if (settings.encoder == ExportEncoderPreference::Hardware)
            return encoders.vaapiH264 ? available()
                                      : unavailable(ExportAvailabilityCode::HardwareEncoderMissing,
                                                    "No VA-API render node exposes H.264 encode",
                                                    ExportRemedy::ChooseSoftwareEncoder);
        const bool software = encoders.h264Software != output::H264EncoderV1::None &&
                              (encoders.openh264 == ExportEncoderStatus::OpenH264::Installed ||
                               encoders.openh264 == ExportEncoderStatus::OpenH264::NotNeeded);
        if (software)
            return available();
        if (settings.encoder == ExportEncoderPreference::Automatic && encoders.vaapiH264)
            return available();
        if (encoders.h264Software == output::H264EncoderV1::None)
            return unavailable(ExportAvailabilityCode::NoQualifiedEncoder,
                               encoders.h264SoftwareReason.empty()
                                   ? std::string("No qualified H.264 encoder on this computer")
                                   : encoders.h264SoftwareReason);
        if (encoders.openh264 == ExportEncoderStatus::OpenH264::Checking)
            return unavailable(ExportAvailabilityCode::Probing, "Checking OpenH264…");
        return unavailable(ExportAvailabilityCode::EncoderNotInstalled,
                           "H.264 needs Cisco OpenH264, which is not installed yet",
                           ExportRemedy::InstallOpenH264);
    }
    return available();
}

ExportAvailability exportProjectAvailability(const document::ExportOutputSettings& settings,
                                             const document::ColorSettings& project) {
    const auto* builtIn =
        std::get_if<document::BuiltInOcioConfigLocator>(&project.ocioConfig.locator);
    const bool aces = builtIn && std::string_view(builtIn->uri) == color::kAcesCgV1ConfigUri;
    const bool neutral =
        builtIn && std::string_view(builtIn->uri) == color::kBloomNeutralV1ConfigUri;
    const bool wantsAces = settings.outputColorSpaceId == kAcesInterchange ||
                           settings.displayName == kReviewDisplay ||
                           settings.viewName == kReviewView;
    if (wantsAces) {
        if (!aces)
            return unavailable(ExportAvailabilityCode::RequiresAcesConfig,
                               "This output uses ACES names; select the built-in ACES CG "
                               "configuration in Project Settings",
                               ExportRemedy::OpenProjectSettings);
        if (project.ocioConfig.expectedRevision.algorithm !=
                document::OcioRevisionAlgorithm::Sha256 ||
            project.ocioConfig.expectedRevision.digest == core::Sha256Digest{})
            return unavailable(ExportAvailabilityCode::ConfigRevisionUnpinned,
                               "The ACES configuration has no pinned revision; check Project "
                               "Settings",
                               ExportRemedy::OpenProjectSettings);
    }
    // The built-in neutral configuration has no named outputs: any explicit name is missing. Other
    // configurations are checked when the run prepares its colour transform (no I/O here).
    if (neutral &&
        (!settings.outputColorSpaceId.empty() || !settings.displayName.empty() ||
         !settings.viewName.empty()) &&
        settings.outputColorSpaceId != project.processColorSpaceId)
        return unavailable(ExportAvailabilityCode::ColourNameMissing,
                           "The project's colour configuration has no output named here",
                           ExportRemedy::OpenProjectSettings);
    return available();
}

struct ExportPresetCatalogue::State : std::enable_shared_from_this<State> {
    ExportPresetCatalogueOptions options;
    std::vector<BuiltIn> builtIns = makeBuiltIns();
    mutable std::mutex mutex;
    std::vector<ExportUserPresetRecord> user;
    bool storeLoaded = false;
    bool storeFailed = false;
    ExportEncoderStatus encoders;
    std::uint64_t revision = 1;
    std::shared_ptr<const ExportPresetCatalogueSnapshot> published;
    runtime::TaskScheduler* scheduler = nullptr;
    std::uint64_t writeGeneration = 0;

    // Caller holds `mutex`.
    void publish() {
        auto snapshot = std::make_shared<ExportPresetCatalogueSnapshot>();
        snapshot->revision = revision;
        snapshot->encoders = encoders;
        snapshot->formats = exportFormatDescriptions();
        const auto make = [&](std::string id, std::string name, std::string description,
                              ExportPresetCategory category, ExportPresetOrigin origin,
                              const ExportOutputSettings& settings, bool dependent) {
            ExportPreset preset;
            preset.id = std::move(id);
            preset.displayName = std::move(name);
            preset.description = std::move(description);
            preset.category = category;
            preset.origin = origin;
            preset.settings = settings;
            preset.capabilities = capabilitiesFor(settings);
            preset.expectedPreservation = expectedFor(settings);
            preset.platformAvailability = exportPlatformAvailability(settings, encoders);
            preset.projectDependent = dependent;
            snapshot->presets.push_back(std::move(preset));
        };
        for (const auto category : {ExportPresetCategory::Video, ExportPresetCategory::VfxSequences,
                                    ExportPresetCategory::Audio})
            for (const auto& builtIn : builtIns)
                if (builtIn.category == category)
                    make(std::string(builtIn.id), std::string(builtIn.name),
                         std::string(builtIn.description), category, ExportPresetOrigin::BuiltIn,
                         builtIn.settings, builtIn.projectDependent);
        auto sorted = user;
        std::ranges::sort(sorted, {}, &ExportUserPresetRecord::displayName);
        for (const auto& record : sorted)
            make(record.id, record.displayName, {}, ExportPresetCategory::User,
                 ExportPresetOrigin::User, record.settings, true);
        published = std::move(snapshot);
    }

    void changed() {
        ++revision;
        publish();
    }

    // Caller holds `mutex`. Schedules the whole-file rewrite on the blocking-I/O executor; the
    // newest generation wins.
    void scheduleWrite() {
        if (!scheduler || !options.userDataDirectory)
            return;
        const auto generation = ++writeGeneration;
        auto records = user;
        auto path = *options.userDataDirectory / kExportPresetStoreFileName;
        [[maybe_unused]] auto submitted = scheduler->submit<void>(
            runtime::TaskRequest("Save export presets",
                                 {runtime::TaskOwnerKind::Export, runtime::TaskOwnerId::fromRaw(1)},
                                 runtime::TaskPriority::Background,
                                 runtime::TaskExecutor::BlockingIo),
            [weak = weak_from_this(), generation, records = std::move(records),
             path = std::move(path)](runtime::TaskContext&) {
                // A newer write supersedes this one; a catalogue already gone still persists
                // its last change.
                const auto self = weak.lock();
                if (self) {
                    std::scoped_lock lock(self->mutex);
                    if (generation != self->writeGeneration)
                        return runtime::TaskResult<void>::succeeded();
                }
                if (!writeExportUserPresets(path, records) && self) {
                    std::scoped_lock lock(self->mutex);
                    self->storeFailed = true;
                    self->changed();
                }
                return runtime::TaskResult<void>::succeeded();
            });
    }
};

ExportPresetCatalogue::ExportPresetCatalogue(ExportPresetCatalogueOptions options)
    : state_(std::make_shared<State>()) {
    state_->options = std::move(options);
    state_->encoders = state_->options.initialEncoders;
    state_->publish();
}

ExportPresetCatalogue::~ExportPresetCatalogue() = default;

std::shared_ptr<ExportPresetCatalogue>
ExportPresetCatalogue::create(ExportPresetCatalogueOptions options) {
    return std::shared_ptr<ExportPresetCatalogue>(new ExportPresetCatalogue(std::move(options)));
}

std::shared_ptr<const ExportPresetCatalogueSnapshot> ExportPresetCatalogue::snapshot() const {
    std::scoped_lock lock(state_->mutex);
    return state_->published;
}

ExportAvailability
ExportPresetCatalogue::availabilityFor(const document::ExportOutputSettings& settings,
                                       const document::ColorSettings& project) const {
    ExportEncoderStatus encoders;
    {
        std::scoped_lock lock(state_->mutex);
        encoders = state_->encoders;
    }
    auto platform = exportPlatformAvailability(settings, encoders);
    if (!platform)
        return platform;
    return exportProjectAvailability(settings, project);
}

ExportDefaultPreset ExportPresetCatalogue::defaultPreset() const {
    ExportEncoderStatus encoders;
    {
        std::scoped_lock lock(state_->mutex);
        encoders = state_->encoders;
    }
    const auto& list = state_->builtIns;
    const auto find = [&](std::string_view id) {
        return std::ranges::find(list, id, &BuiltIn::id);
    };
    const auto chosen = [&](const std::string_view id, ExportAvailability availability,
                            const bool fallback) {
        ExportDefaultPreset result;
        result.presetId = std::string(id);
        result.preferredAvailability = std::move(availability);
        result.usedFallback = fallback;
        result.settings = find(id)->settings;
        result.destination = defaultExportDestination(result.settings.format);
        return result;
    };
    const auto preferred =
        exportPlatformAvailability(find(kExportDefaultVideoPresetId)->settings, encoders);
    if (preferred)
        return chosen(kExportDefaultVideoPresetId, preferred, false);
    const auto fallback =
        exportPlatformAvailability(find(kExportFallbackVideoPresetId)->settings, encoders);
    if (fallback)
        return chosen(kExportFallbackVideoPresetId, preferred, true);
    return chosen(kExportDefaultVideoPresetId, preferred, false);
}

void ExportPresetCatalogue::setEncoderStatus(ExportEncoderStatus status) {
    std::scoped_lock lock(state_->mutex);
    status.exportPlatform = state_->options.initialEncoders.exportPlatform;
    state_->encoders = std::move(status);
    state_->changed();
}

void ExportPresetCatalogue::refreshEncoders(runtime::TaskScheduler& scheduler) {
    [[maybe_unused]] auto submitted = scheduler.submit<void>(
        runtime::TaskRequest("Probe export encoders",
                             {runtime::TaskOwnerKind::Export, runtime::TaskOwnerId::fromRaw(1)},
                             runtime::TaskPriority::Background, runtime::TaskExecutor::BlockingIo),
        [weak = std::weak_ptr<State>(state_),
         initial = state_->options.initialEncoders](runtime::TaskContext&) {
            auto status = probeExportEncoders(initial);
            if (const auto self = weak.lock()) {
                std::scoped_lock lock(self->mutex);
                status.exportPlatform = initial.exportPlatform;
                self->encoders = std::move(status);
                self->changed();
            }
            return runtime::TaskResult<void>::succeeded();
        });
}

void ExportPresetCatalogue::loadUserPresets(runtime::TaskScheduler& scheduler) {
    {
        std::scoped_lock lock(state_->mutex);
        state_->scheduler = &scheduler;
        if (!state_->options.userDataDirectory) {
            state_->storeFailed = true;
            return;
        }
    }
    auto path = *state_->options.userDataDirectory / kExportPresetStoreFileName;
    [[maybe_unused]] auto submitted = scheduler.submit<void>(
        runtime::TaskRequest("Load export presets",
                             {runtime::TaskOwnerKind::Export, runtime::TaskOwnerId::fromRaw(1)},
                             runtime::TaskPriority::Background, runtime::TaskExecutor::BlockingIo),
        [weak = std::weak_ptr<State>(state_), path = std::move(path)](runtime::TaskContext&) {
            auto loaded = readExportUserPresets(path);
            const auto self = weak.lock();
            if (!self)
                return runtime::TaskResult<void>::succeeded();
            std::scoped_lock lock(self->mutex);
            self->storeLoaded = true;
            if (loaded)
                self->user = std::move(*loaded);
            else
                self->storeFailed = true;
            self->changed();
            return runtime::TaskResult<void>::succeeded();
        });
}

namespace {
bool validPresetName(const std::string_view name) {
    return !name.empty() && name.size() <= 256 && std::ranges::none_of(name, [](const char c) {
        return static_cast<unsigned char>(c) < 0x20 || c == 0x7f;
    });
}
std::string newUserPresetId() {
    std::random_device device;
    std::mt19937_64 generator((static_cast<std::uint64_t>(device()) << 32U) ^ device());
    const auto a = generator();
    const auto b = generator();
    constexpr char kHex[] = "0123456789abcdef";
    std::string text = "user:";
    for (int index = 0; index < 32; ++index) {
        const auto word = index < 16 ? a : b;
        text.push_back(kHex[(word >> ((index % 16) * 4)) & 0xF]);
        if (index == 7 || index == 11 || index == 15 || index == 19)
            text.push_back('-');
    }
    return text;
}
} // namespace

ExportUserPresetStatus
ExportPresetCatalogue::saveUserPreset(const std::string_view displayName,
                                      const document::ExportOutputSettings& settings) {
    if (!validPresetName(displayName))
        return ExportUserPresetStatus::NameInvalid;
    std::scoped_lock lock(state_->mutex);
    if (!state_->options.userDataDirectory || state_->storeFailed || !state_->scheduler ||
        !state_->storeLoaded)
        return ExportUserPresetStatus::StoreUnavailable;
    if (std::ranges::any_of(state_->user,
                            [&](const auto& record) { return record.displayName == displayName; }))
        return ExportUserPresetStatus::NameTaken;
    state_->user.push_back({newUserPresetId(), std::string(displayName), settings});
    state_->changed();
    state_->scheduleWrite();
    return ExportUserPresetStatus::Accepted;
}

ExportUserPresetStatus ExportPresetCatalogue::renameUserPreset(const std::string_view id,
                                                               const std::string_view displayName) {
    if (id.starts_with("builtin:"))
        return ExportUserPresetStatus::BuiltInReadOnly;
    if (!validPresetName(displayName))
        return ExportUserPresetStatus::NameInvalid;
    std::scoped_lock lock(state_->mutex);
    if (!state_->scheduler || state_->storeFailed || !state_->storeLoaded)
        return ExportUserPresetStatus::StoreUnavailable;
    auto found = std::ranges::find(state_->user, id, &ExportUserPresetRecord::id);
    if (found == state_->user.end())
        return ExportUserPresetStatus::UnknownPreset;
    if (std::ranges::any_of(state_->user, [&](const auto& record) {
            return record.id != id && record.displayName == displayName;
        }))
        return ExportUserPresetStatus::NameTaken;
    found->displayName = std::string(displayName);
    state_->changed();
    state_->scheduleWrite();
    return ExportUserPresetStatus::Accepted;
}

ExportUserPresetStatus ExportPresetCatalogue::deleteUserPreset(const std::string_view id) {
    if (id.starts_with("builtin:"))
        return ExportUserPresetStatus::BuiltInReadOnly;
    std::scoped_lock lock(state_->mutex);
    if (!state_->scheduler || state_->storeFailed || !state_->storeLoaded)
        return ExportUserPresetStatus::StoreUnavailable;
    if (std::erase_if(state_->user, [&](const auto& record) { return record.id == id; }) == 0)
        return ExportUserPresetStatus::UnknownPreset;
    state_->changed();
    state_->scheduleWrite();
    return ExportUserPresetStatus::Accepted;
}

} // namespace bloom::host
