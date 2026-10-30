#include "export_preset_store.hpp"

#include <bloom/output/media_output.hpp>

#include <charconv>
#include <cstdio>
#include <fstream>
#include <map>
#include <memory>
#include <sstream>
#include <system_error>
#include <variant>

namespace bloom::host {
namespace {

// A deliberately small JSON value for the preset store: objects, arrays, strings, unsigned
// integers and booleans are all the store uses. Bounded: at most 1 MiB of input, depth 8.
struct Json;
using JsonObject = std::map<std::string, Json, std::less<>>;
using JsonArray = std::vector<Json>;
struct Json final {
    std::variant<std::monostate, bool, std::uint64_t, std::string, JsonArray, JsonObject> value;
    [[nodiscard]] const Json* find(std::string_view key) const {
        const auto* object = std::get_if<JsonObject>(&value);
        if (!object)
            return nullptr;
        const auto found = object->find(key);
        return found == object->end() ? nullptr : &found->second;
    }
};

class Parser final {
  public:
    explicit Parser(std::string_view text) : text_(text) {}
    std::optional<Json> parse() {
        auto value = parseValue(0);
        skip();
        if (!value || at_ != text_.size())
            return std::nullopt;
        return value;
    }

  private:
    void skip() {
        while (at_ < text_.size() && (text_[at_] == ' ' || text_[at_] == '\n' ||
                                      text_[at_] == '\r' || text_[at_] == '\t'))
            ++at_;
    }
    bool literal(std::string_view word) {
        if (text_.substr(at_, word.size()) != word)
            return false;
        at_ += word.size();
        return true;
    }
    std::optional<std::string> parseString() {
        if (at_ >= text_.size() || text_[at_] != '"')
            return std::nullopt;
        ++at_;
        std::string out;
        while (at_ < text_.size() && text_[at_] != '"') {
            char c = text_[at_++];
            if (c == '\\') {
                if (at_ >= text_.size())
                    return std::nullopt;
                const char e = text_[at_++];
                switch (e) {
                case '"':
                case '\\':
                case '/':
                    c = e;
                    break;
                case 'n':
                    c = '\n';
                    break;
                case 't':
                    c = '\t';
                    break;
                case 'r':
                    c = '\r';
                    break;
                case 'b':
                    c = '\b';
                    break;
                case 'f':
                    c = '\f';
                    break;
                case 'u': {
                    unsigned code = 0;
                    if (at_ + 4 > text_.size() ||
                        std::from_chars(text_.data() + at_, text_.data() + at_ + 4, code, 16).ec !=
                            std::errc{})
                        return std::nullopt;
                    at_ += 4;
                    if (code >= 0x80)
                        return std::nullopt; // the writer escapes only control characters
                    c = static_cast<char>(code);
                    break;
                }
                default:
                    return std::nullopt;
                }
            }
            out.push_back(c);
        }
        if (at_ >= text_.size())
            return std::nullopt;
        ++at_;
        return out;
    }
    std::optional<Json> parseValue(const int depth) {
        if (depth > 8)
            return std::nullopt;
        skip();
        if (at_ >= text_.size())
            return std::nullopt;
        const char c = text_[at_];
        if (c == '"') {
            auto text = parseString();
            if (!text)
                return std::nullopt;
            return Json{std::move(*text)};
        }
        if (literal("true"))
            return Json{true};
        if (literal("false"))
            return Json{false};
        if (literal("null"))
            return Json{};
        if (c >= '0' && c <= '9') {
            std::uint64_t number = 0;
            const auto [end, error] =
                std::from_chars(text_.data() + at_, text_.data() + text_.size(), number);
            if (error != std::errc{})
                return std::nullopt;
            at_ = static_cast<std::size_t>(end - text_.data());
            return Json{number};
        }
        if (c == '[') {
            ++at_;
            JsonArray array;
            skip();
            if (at_ < text_.size() && text_[at_] == ']') {
                ++at_;
                return Json{std::move(array)};
            }
            while (true) {
                auto item = parseValue(depth + 1);
                if (!item)
                    return std::nullopt;
                array.push_back(std::move(*item));
                skip();
                if (at_ < text_.size() && text_[at_] == ',') {
                    ++at_;
                    continue;
                }
                if (at_ < text_.size() && text_[at_] == ']') {
                    ++at_;
                    return Json{std::move(array)};
                }
                return std::nullopt;
            }
        }
        if (c == '{') {
            ++at_;
            JsonObject object;
            skip();
            if (at_ < text_.size() && text_[at_] == '}') {
                ++at_;
                return Json{std::move(object)};
            }
            while (true) {
                skip();
                auto key = parseString();
                skip();
                if (!key || at_ >= text_.size() || text_[at_++] != ':')
                    return std::nullopt;
                auto item = parseValue(depth + 1);
                if (!item || !object.emplace(std::move(*key), std::move(*item)).second)
                    return std::nullopt;
                skip();
                if (at_ < text_.size() && text_[at_] == ',') {
                    ++at_;
                    continue;
                }
                if (at_ < text_.size() && text_[at_] == '}') {
                    ++at_;
                    return Json{std::move(object)};
                }
                return std::nullopt;
            }
        }
        return std::nullopt;
    }

    std::string_view text_;
    std::size_t at_ = 0;
};

void quote(std::string& out, const std::string_view text) {
    out.push_back('"');
    for (const char c : text) {
        if (c == '"' || c == '\\') {
            out.push_back('\\');
            out.push_back(c);
        } else if (static_cast<unsigned char>(c) < 0x20) {
            char buffer[8];
            std::snprintf(buffer, sizeof buffer, "\\u%04x", static_cast<unsigned>(c));
            out += buffer;
        } else {
            out.push_back(c);
        }
    }
    out.push_back('"');
}

const std::string* string(const Json& object, std::string_view key) {
    const auto* value = object.find(key);
    return value ? std::get_if<std::string>(&value->value) : nullptr;
}
const std::uint64_t* number(const Json& object, std::string_view key) {
    const auto* value = object.find(key);
    return value ? std::get_if<std::uint64_t>(&value->value) : nullptr;
}
const bool* boolean(const Json& object, std::string_view key) {
    const auto* value = object.find(key);
    return value ? std::get_if<bool>(&value->value) : nullptr;
}

std::optional<document::ExportOutputSettings> settingsFrom(const Json& json) {
    document::ExportOutputSettings out;
    const auto* format = string(json, "format");
    const auto* profile = string(json, "codecProfile");
    const auto* compression = string(json, "compression");
    const auto* colour = string(json, "outputColorSpace");
    const auto* display = string(json, "display");
    const auto* view = string(json, "view");
    const auto* bake = boolean(json, "bakeLook");
    const auto* audio = boolean(json, "includeAudio");
    const auto* rate = number(json, "audioSampleRate");
    const auto* encoder = string(json, "encoder");
    const auto* naming = json.find("naming");
    if (!format || !profile || !compression || !colour || !display || !view || !bake || !audio ||
        !rate || !encoder || !naming)
        return std::nullopt;
    const auto parsedFormat = document::parseExportFormat(*format);
    const auto parsedEncoder = document::parseExportEncoder(*encoder);
    const auto* padding = number(*naming, "framePadding");
    const auto* pattern = string(*naming, "pattern");
    if (!parsedFormat || !parsedEncoder || !padding || !pattern || *rate > 192000 || *padding > 64)
        return std::nullopt;
    out.format = *parsedFormat;
    out.codecProfile = *profile;
    if (const auto* bitrate = number(json, "videoBitrateKbps")) {
        if (*bitrate > 2'000'000)
            return std::nullopt;
        out.videoBitrateKbps = static_cast<std::uint32_t>(*bitrate);
    }
    out.compression = *compression;
    out.outputColorSpaceId = *colour;
    out.displayName = *display;
    out.viewName = *view;
    out.bakeLook = *bake;
    out.includeAudio = *audio;
    // Optional: absent means true (the 1.29 document rule).
    if (const auto* value = json.find("useCachedFrames")) {
        const auto* cached = std::get_if<bool>(&value->value);
        if (!cached)
            return std::nullopt;
        out.useCachedFrames = *cached;
    }
    out.audioSampleRate = static_cast<std::uint32_t>(*rate);
    out.encoder = *parsedEncoder;
    if (const auto* start = string(*naming, "startFrame")) {
        std::uint64_t value = 0;
        if (std::from_chars(start->data(), start->data() + start->size(), value).ec != std::errc{})
            return std::nullopt;
        out.naming.startFrame = value;
    }
    out.naming.framePadding = static_cast<std::uint32_t>(*padding);
    out.naming.pattern = *pattern;
    document::ExportOutput probe;
    probe.id = document::ExportOutputId::fromRaw(1);
    probe.settings = out;
    if (!document::validateExportOutput(probe).ok())
        return std::nullopt;
    return out;
}

} // namespace

std::string exportSettingsJson(const document::ExportOutputSettings& settings) {
    std::string out = "{\"format\":";
    quote(out, document::exportFormatToken(settings.format));
    out += ",\"codecProfile\":";
    quote(out, settings.codecProfile);
    if (settings.videoBitrateKbps)
        out += ",\"videoBitrateKbps\":" + std::to_string(*settings.videoBitrateKbps);
    out += ",\"compression\":";
    quote(out, settings.compression);
    out += ",\"outputColorSpace\":";
    quote(out, settings.outputColorSpaceId);
    out += ",\"display\":";
    quote(out, settings.displayName);
    out += ",\"view\":";
    quote(out, settings.viewName);
    out += std::string(",\"bakeLook\":") + (settings.bakeLook ? "true" : "false");
    out += std::string(",\"includeAudio\":") + (settings.includeAudio ? "true" : "false");
    out += std::string(",\"useCachedFrames\":") + (settings.useCachedFrames ? "true" : "false");
    out += ",\"audioSampleRate\":" + std::to_string(settings.audioSampleRate);
    out += ",\"encoder\":";
    quote(out, document::exportEncoderToken(settings.encoder));
    out += ",\"naming\":{";
    if (settings.naming.startFrame)
        out += "\"startFrame\":\"" + std::to_string(*settings.naming.startFrame) + "\",";
    out += "\"framePadding\":" + std::to_string(settings.naming.framePadding) + ",\"pattern\":";
    quote(out, settings.naming.pattern);
    out += "}}";
    return out;
}

std::string exportSettingsToJson(const document::ExportOutputSettings& settings) {
    return exportSettingsJson(settings);
}

std::optional<document::ExportOutputSettings> exportSettingsFromJson(const std::string_view json) {
    if (json.size() > (1U << 20U))
        return std::nullopt;
    const auto parsed = Parser(json).parse();
    return parsed ? settingsFrom(*parsed) : std::nullopt;
}

std::string exportOutputToJson(const document::ExportOutput& output) {
    std::string out = "{\"preset\":";
    quote(out, output.presetId);
    out += ",\"settings\":" + exportSettingsJson(output.settings) +
           ",\"destination\":{\"portability\":";
    quote(out, output.destination.portability);
    out += ",\"path\":";
    quote(out, output.destination.path);
    out += ",\"overwrite\":";
    quote(out, document::exportOverwriteToken(output.destination.overwrite));
    out += std::string("},\"enabled\":") + (output.enabled ? "true" : "false") +
           ",\"importIntoAssets\":" + (output.importIntoAssets ? "true" : "false") + "}";
    return out;
}

std::optional<document::ExportOutput> exportOutputFromJson(const std::string_view json) {
    if (json.size() > (1U << 20U))
        return std::nullopt;
    const auto parsed = Parser(json).parse();
    if (!parsed)
        return std::nullopt;
    document::ExportOutput output;
    if (const auto* preset = string(*parsed, "preset"))
        output.presetId = *preset;
    if (const auto* settings = parsed->find("settings")) {
        auto value = settingsFrom(*settings);
        if (!value)
            return std::nullopt;
        output.settings = std::move(*value);
    }
    if (const auto* destination = parsed->find("destination")) {
        if (const auto* portability = string(*destination, "portability"))
            output.destination.portability = *portability;
        if (const auto* path = string(*destination, "path"))
            output.destination.path = *path;
        if (const auto* overwrite = string(*destination, "overwrite")) {
            const auto policy = document::parseExportOverwrite(*overwrite);
            if (!policy)
                return std::nullopt;
            output.destination.overwrite = *policy;
        }
    }
    if (const auto* enabled = boolean(*parsed, "enabled"))
        output.enabled = *enabled;
    if (const auto* import = boolean(*parsed, "importIntoAssets"))
        output.importIntoAssets = *import;
    auto probe = output;
    probe.id = document::ExportOutputId::fromRaw(1);
    if (!document::validateExportOutput(probe).ok())
        return std::nullopt;
    return output;
}

std::optional<std::vector<ExportUserPresetRecord>>
readExportUserPresets(const std::filesystem::path& path) {
    std::error_code error;
    if (!std::filesystem::exists(path, error))
        return error ? std::nullopt : std::optional(std::vector<ExportUserPresetRecord>{});
    const auto size = std::filesystem::file_size(path, error);
    if (error || size > (1U << 20U))
        return std::nullopt;
    std::ifstream stream(path, std::ios::binary);
    std::stringstream buffer;
    buffer << stream.rdbuf();
    if (!stream && !stream.eof())
        return std::nullopt;
    const auto text = buffer.str();
    const auto json = Parser(text).parse();
    if (!json)
        return std::nullopt;
    const auto* format = string(*json, "format");
    const auto* version = number(*json, "version");
    const auto* presets = json->find("presets");
    const auto* list = presets ? std::get_if<JsonArray>(&presets->value) : nullptr;
    if (!format || *format != kExportPresetStoreFormat || !version ||
        *version != kExportPresetStoreVersion || !list || list->size() > 1024)
        return std::nullopt;
    std::vector<ExportUserPresetRecord> records;
    for (const auto& item : *list) {
        const auto* id = string(item, "id");
        const auto* name = string(item, "name");
        const auto* settingsJson = item.find("settings");
        if (!id || !id->starts_with("user:") || !name || name->empty() || !settingsJson)
            return std::nullopt;
        auto settings = settingsFrom(*settingsJson);
        if (!settings)
            return std::nullopt;
        records.push_back({*id, *name, std::move(*settings)});
    }
    return records;
}

bool writeExportUserPresets(const std::filesystem::path& path,
                            const std::vector<ExportUserPresetRecord>& records) {
    std::string text = "{\"format\":";
    quote(text, kExportPresetStoreFormat);
    text += ",\"version\":" + std::to_string(kExportPresetStoreVersion) + ",\"presets\":[";
    for (std::size_t index = 0; index < records.size(); ++index) {
        if (index)
            text += ',';
        text += "{\"id\":";
        quote(text, records[index].id);
        text += ",\"name\":";
        quote(text, records[index].displayName);
        text += ",\"settings\":" + exportSettingsJson(records[index].settings) + "}";
    }
    text += "]}\n";
    std::error_code error;
    std::filesystem::create_directories(path.parent_path(), error);
    auto temporary = path;
    temporary += ".tmp";
    {
        std::ofstream stream(temporary, std::ios::binary | std::ios::trunc);
        stream << text;
        stream.flush();
        if (!stream)
            return false;
    }
    std::filesystem::rename(temporary, path, error);
    return !error;
}

ExportEncoderStatus probeExportEncoders(const ExportEncoderStatus& initial) {
    auto status = initial;
    const auto support = output::h264EncoderSupportV1();
    status.h264Software = support.software;
    status.h264SoftwareReason = std::string(support.softwareReason);
    if (support.software == output::H264EncoderV1::OpenH264)
        status.openh264 = output::verifyH264RuntimeV1().installed
                              ? ExportEncoderStatus::OpenH264::Installed
                              : ExportEncoderStatus::OpenH264::NotInstalled;
    else
        status.openh264 = support.software == output::H264EncoderV1::None
                              ? ExportEncoderStatus::OpenH264::NotInstalled
                              : ExportEncoderStatus::OpenH264::NotNeeded;
    status.mediaWorker = output::mediaWorkerAvailableV1();
    const auto vaapi = output::probeVaapiEncodeV1();
    status.vaapiH264 = support.hardwareVaapi && vaapi.h264;
    status.vaapiHevc = support.hardwareVaapi && vaapi.hevc;
    status.vaapiRenderNode = vaapi.renderNode;
    status.probed = true;
    return status;
}

} // namespace bloom::host
