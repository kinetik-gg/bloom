#include <bloom/document/export_queue.hpp>

#include <algorithm>
#include <array>
#include <string>
#include <unordered_set>
#include <utility>

namespace bloom::document {
namespace {

template <typename Enum, std::size_t Count>
[[nodiscard]] std::string_view
tokenFor(const std::array<std::pair<Enum, std::string_view>, Count>& table, const Enum value) {
    for (const auto& [candidate, token] : table)
        if (candidate == value)
            return token;
    return {};
}

template <typename Enum, std::size_t Count>
[[nodiscard]] std::optional<Enum>
parseToken(const std::array<std::pair<Enum, std::string_view>, Count>& table,
           const std::string_view token) {
    for (const auto& [candidate, text] : table)
        if (text == token)
            return candidate;
    return std::nullopt;
}

constexpr std::array<std::pair<ExportFormat, std::string_view>, 14> kFormats{{
    {ExportFormat::Png, "png"},
    {ExportFormat::OpenExr, "exr"},
    {ExportFormat::Tiff, "tiff"},
    {ExportFormat::Dpx, "dpx"},
    {ExportFormat::ProResMov, "prores-mov"},
    {ExportFormat::DnxhrMxf, "dnxhr-mxf"},
    {ExportFormat::H264Mov, "h264-mov"},
    {ExportFormat::H264Mp4, "h264-mp4"},
    {ExportFormat::HevcMov, "hevc-mov"},
    {ExportFormat::HevcMp4, "hevc-mp4"},
    {ExportFormat::Wav, "wav"},
    {ExportFormat::Vp9Webm, "vp9-webm"},
    {ExportFormat::Av1Webm, "av1-webm"},
    {ExportFormat::Av1Mp4, "av1-mp4"},
}};

constexpr std::array<std::pair<ExportRangeKind, std::string_view>, 4> kRangeKinds{{
    {ExportRangeKind::WorkArea, "work-area"},
    {ExportRangeKind::Composition, "composition"},
    {ExportRangeKind::Custom, "custom"},
    {ExportRangeKind::Frame, "frame"},
}};

constexpr std::array<std::pair<ExportEncoderPreference, std::string_view>, 3> kEncoders{{
    {ExportEncoderPreference::Automatic, "automatic"},
    {ExportEncoderPreference::Software, "software"},
    {ExportEncoderPreference::Hardware, "hardware"},
}};

constexpr std::array<std::pair<ExportOverwritePolicy, std::string_view>, 3> kOverwrite{{
    {ExportOverwritePolicy::AskBeforeReplacing, "ask"},
    {ExportOverwritePolicy::Replace, "replace"},
    {ExportOverwritePolicy::Refuse, "refuse"},
}};

constexpr std::array<std::string_view, 6> kProResProfiles{"proxy", "lt",   "422",
                                                          "hq",    "4444", "4444xq"};
constexpr std::array<std::string_view, 5> kDnxhrProfiles{"lb", "sq", "hq", "hqx", "444"};
constexpr std::array<std::string_view, 1> kH264Profiles{"high"};
constexpr std::array<std::string_view, 2> kHevcProfiles{"main", "main10"};
constexpr std::array<std::string_view, 2> kDpxProfiles{"10bit", "16bit"};
constexpr std::array<std::string_view, 2> kWavProfiles{"s16", "s24"};
constexpr std::array<std::string_view, 2> kBitDepthProfiles{"8bit", "10bit"};

[[nodiscard]] bool validText(const std::string_view value) noexcept {
    if (value.size() > kExportTextMaximumBytes)
        return false;
    return std::ranges::none_of(value, [](const char c) {
        const auto byte = static_cast<unsigned char>(c);
        return byte < 0x20 || byte == 0x7f;
    });
}

void checkText(const std::string_view value, std::string path, std::string_view label,
               ValidationResult& result) {
    if (!validText(value))
        result.add(ValidationCode::InvalidValue, std::move(path),
                   std::string(label) + " must be at most 4 KiB without control characters");
}

// Exactly one frame token: `####` (four hashes, no other `#`) or `{frame}`.
[[nodiscard]] bool validSequencePattern(const std::string_view pattern) noexcept {
    if (pattern.empty())
        return false;
    std::size_t tokens = 0;
    for (auto at = pattern.find("{frame}"); at != std::string_view::npos;
         at = pattern.find("{frame}", at + 1))
        ++tokens;
    const auto hashes = static_cast<std::size_t>(std::ranges::count(pattern, '#'));
    if (hashes != 0) {
        const auto run = pattern.find("####");
        if (hashes != 4 || run == std::string_view::npos)
            return false;
        ++tokens;
    }
    return tokens == 1;
}

} // namespace

std::string_view exportFormatToken(const ExportFormat format) noexcept {
    return tokenFor(kFormats, format);
}
std::optional<ExportFormat> parseExportFormat(const std::string_view token) noexcept {
    return parseToken(kFormats, token);
}
std::string_view exportRangeKindToken(const ExportRangeKind kind) noexcept {
    return tokenFor(kRangeKinds, kind);
}
std::optional<ExportRangeKind> parseExportRangeKind(const std::string_view token) noexcept {
    return parseToken(kRangeKinds, token);
}
std::string_view exportEncoderToken(const ExportEncoderPreference encoder) noexcept {
    return tokenFor(kEncoders, encoder);
}
std::optional<ExportEncoderPreference> parseExportEncoder(const std::string_view token) noexcept {
    return parseToken(kEncoders, token);
}
std::string_view exportOverwriteToken(const ExportOverwritePolicy policy) noexcept {
    return tokenFor(kOverwrite, policy);
}
std::optional<ExportOverwritePolicy> parseExportOverwrite(const std::string_view token) noexcept {
    return parseToken(kOverwrite, token);
}

bool isImageExportFormat(const ExportFormat format) noexcept {
    switch (format) {
    case ExportFormat::Png:
    case ExportFormat::OpenExr:
    case ExportFormat::Tiff:
    case ExportFormat::Dpx:
        return true;
    default:
        return false;
    }
}

std::span<const std::string_view> exportCodecProfiles(const ExportFormat format) noexcept {
    switch (format) {
    case ExportFormat::ProResMov:
        return kProResProfiles;
    case ExportFormat::DnxhrMxf:
        return kDnxhrProfiles;
    case ExportFormat::H264Mov:
    case ExportFormat::H264Mp4:
        return kH264Profiles;
    case ExportFormat::HevcMov:
    case ExportFormat::HevcMp4:
        return kHevcProfiles;
    case ExportFormat::Dpx:
        return kDpxProfiles;
    case ExportFormat::Wav:
        return kWavProfiles;
    case ExportFormat::Vp9Webm:
    case ExportFormat::Av1Webm:
    case ExportFormat::Av1Mp4:
        return kBitDepthProfiles;
    default:
        return {};
    }
}

ValidationResult validateExportOutput(const ExportOutput& output) {
    ValidationResult result;
    if (!output.id.isValid())
        result.add(ValidationCode::InvalidId, "id", "Export output ID must not be zero");
    checkText(output.presetId, "preset", "Preset id", result);
    const auto& settings = output.settings;
    if (exportFormatToken(settings.format).empty())
        result.add(ValidationCode::InvalidValue, "settings.format", "Unknown export format");
    if (!settings.codecProfile.empty()) {
        const auto profiles = exportCodecProfiles(settings.format);
        if (std::ranges::find(profiles, std::string_view(settings.codecProfile)) == profiles.end())
            result.add(ValidationCode::InvalidValue, "settings.codecProfile",
                       "The codec profile does not apply to this format");
    }
    if (settings.videoBitrateKbps &&
        (*settings.videoBitrateKbps == 0 || *settings.videoBitrateKbps > 2'000'000))
        result.add(ValidationCode::InvalidValue, "settings.videoBitrateKbps",
                   "Video bit rate must be 1..2000000 kbit/s");
    checkText(settings.compression, "settings.compression", "Compression", result);
    checkText(settings.outputColorSpaceId, "settings.outputColorSpace", "Output color space",
              result);
    checkText(settings.displayName, "settings.display", "Display", result);
    checkText(settings.viewName, "settings.view", "View", result);
    if (settings.audioSampleRate < 8000 || settings.audioSampleRate > 192000)
        result.add(ValidationCode::InvalidValue, "settings.audioSampleRate",
                   "Audio sample rate must be 8000..192000 Hz");
    if (exportEncoderToken(settings.encoder).empty())
        result.add(ValidationCode::InvalidValue, "settings.encoder", "Unknown encoder preference");
    if (settings.naming.framePadding < 1 || settings.naming.framePadding > 12)
        result.add(ValidationCode::InvalidValue, "settings.naming.framePadding",
                   "Frame padding must be 1..12 digits");
    checkText(settings.naming.pattern, "settings.naming.pattern", "Naming pattern", result);
    if (!validSequencePattern(settings.naming.pattern))
        result.add(ValidationCode::InvalidValue, "settings.naming.pattern",
                   "The naming pattern needs exactly one #### or {frame}");
    const auto& destination = output.destination;
    if (destination.portability != "project-relative" && destination.portability != "external")
        result.add(ValidationCode::InvalidValue, "destination.portability",
                   "Destination portability must be project-relative or external");
    checkText(destination.path, "destination.path", "Destination path", result);
    if (!destination.path.empty()) {
        const bool uri = destination.path.starts_with("file:");
        if (destination.portability == "external" && !uri)
            result.add(ValidationCode::InvalidValue, "destination.path",
                       "An external destination is an RFC 8089 file URI");
        if (destination.portability == "project-relative" &&
            (uri || destination.path.starts_with('/') || destination.path.starts_with('\\') ||
             (destination.path.size() > 1 && destination.path[1] == ':')))
            result.add(ValidationCode::InvalidValue, "destination.path",
                       "A project-relative destination is a relative path");
    }
    if (exportOverwriteToken(destination.overwrite).empty())
        result.add(ValidationCode::InvalidValue, "destination.overwrite",
                   "Unknown overwrite policy");
    return result;
}

ValidationResult validateExportJob(const ExportJob& job) {
    ValidationResult result;
    if (!job.id.isValid())
        result.add(ValidationCode::InvalidId, "id", "Export job ID must not be zero");
    if (!job.composition.isValid())
        result.add(ValidationCode::InvalidId, "composition",
                   "Export job composition must not be zero");
    checkText(job.name, "name", "Job name", result);
    const auto& range = job.render.range;
    if (exportRangeKindToken(range.kind).empty())
        result.add(ValidationCode::InvalidValue, "range.kind", "Unknown range kind");
    if (range.kind == ExportRangeKind::Custom && range.first > range.last)
        result.add(ValidationCode::InvalidOrder, "range", "A custom range ends before it starts");
    if (job.outputs.empty() || job.outputs.size() > kExportJobMaximumOutputs)
        result.add(ValidationCode::InvalidValue, "outputs", "An export job has 1 to 16 outputs");
    std::unordered_set<ExportOutputId> ids;
    for (const auto& output : job.outputs) {
        result.append("outputs[" + std::to_string(output.id.value()) + "]",
                      validateExportOutput(output));
        if (!ids.insert(output.id).second)
            result.add(ValidationCode::DuplicateId, "outputs.id", "Duplicate export output ID");
    }
    return result;
}

} // namespace bloom::document
