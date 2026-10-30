// Pure destination expansion and "copy as command" (ADR 0033, sections 7 and 11).
#include <bloom/host/export_queue.hpp>

#include "export_output_drivers.hpp"
#include "export_preset_store.hpp"

#include <bloom/host/frame_export_publication.hpp>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <string>

namespace bloom::host {
namespace {

using document::ExportFormat;

ExportPreflightIssue issue(const ExportPreflightCode code, const ExportPreflightTarget target,
                           std::string reason) {
    return {code, target, ExportPreflightSeverity::Blocking, std::move(reason)};
}

std::string lower(std::string text) {
    for (auto& c : text)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return text;
}

// A token value made safe for a file name; nullopt when it carries a path separator.
std::optional<std::string> sanitized(const std::string_view value) {
    if (value.find('/') != std::string_view::npos || value.find('\\') != std::string_view::npos)
        return std::nullopt;
    std::string out(value);
    for (auto& c : out) {
        const auto byte = static_cast<unsigned char>(c);
        if (byte < 0x20 || byte == 0x7f || c == ':' || c == '*' || c == '?' || c == '"' ||
            c == '<' || c == '>' || c == '|')
            c = '_';
    }
    while (!out.empty() && (out.back() == ' ' || out.back() == '.'))
        out.pop_back();
    if (out.empty())
        out = "untitled";
    return out;
}

struct TokenValues final {
    std::string name;
    std::string job;
    std::string format;
    std::string date;
};

enum class Expansion { Ok, UnknownToken, Separator };

// Expands `{name}`, `{job}`, `{format}` and `{date}`. `keep` lists the tokens left for a later
// stage (the sequence pattern's frame token and `<base>`/`<ext>`).
Expansion expand(const std::string_view text, const TokenValues& values, std::string& out,
                 const bool keepFrameTokens) {
    out.clear();
    for (std::size_t at = 0; at < text.size();) {
        const auto rest = text.substr(at);
        if (rest.starts_with('{')) {
            const auto close = rest.find('}');
            if (close == std::string_view::npos)
                return Expansion::UnknownToken;
            const auto token = rest.substr(0, close + 1);
            const std::string* value = token == "{name}"     ? &values.name
                                       : token == "{job}"    ? &values.job
                                       : token == "{format}" ? &values.format
                                       : token == "{date}"   ? &values.date
                                                             : nullptr;
            if (value == nullptr) {
                if (keepFrameTokens && token == "{frame}") {
                    out += token;
                    at += token.size();
                    continue;
                }
                return Expansion::UnknownToken;
            }
            const auto safe = sanitized(*value);
            if (!safe)
                return Expansion::Separator;
            out += *safe;
            at += token.size();
            continue;
        }
        if (rest.starts_with('}'))
            return Expansion::UnknownToken;
        if (!keepFrameTokens && rest.starts_with('#'))
            return Expansion::UnknownToken;
        out.push_back(text[at++]);
    }
    return Expansion::Ok;
}

std::optional<std::filesystem::path> fromFileUri(const std::string_view uri) {
    std::string_view rest;
    if (uri.starts_with("file://"))
        rest = uri.substr(7);
    else if (uri.starts_with("file:"))
        rest = uri.substr(5);
    else
        return std::nullopt;
    std::string decoded;
    for (std::size_t at = 0; at < rest.size(); ++at) {
        if (rest[at] == '%' && at + 2 < rest.size() &&
            std::isxdigit(static_cast<unsigned char>(rest[at + 1])) &&
            std::isxdigit(static_cast<unsigned char>(rest[at + 2]))) {
            decoded.push_back(
                static_cast<char>(std::stoi(std::string(rest.substr(at + 1, 2)), nullptr, 16)));
            at += 2;
        } else {
            decoded.push_back(rest[at]);
        }
    }
#if defined(_WIN32)
    if (decoded.size() > 2 && decoded[0] == '/' && decoded[2] == ':')
        decoded.erase(0, 1);
#endif
    if (decoded.empty())
        return std::nullopt;
    return std::filesystem::path(decoded);
}

std::string displayName(const document::ExportJob& job, const ExportDestinationContext& context) {
    return job.name.empty() ? context.compositionName : job.name;
}

std::string dateText(const ExportDestinationContext& context) {
    char buffer[16];
    std::snprintf(buffer, sizeof buffer, "%04u-%02u-%02u", static_cast<unsigned>(context.year),
                  static_cast<unsigned>(context.month), static_cast<unsigned>(context.day));
    return buffer;
}

bool extensionMatches(const std::filesystem::path& path, const ExportFormat format) {
    const auto extension = lower(path.extension().string());
    const auto wanted = "." + std::string(exportFormatExtension(format));
    return extension == wanted || (format == ExportFormat::Tiff && extension == ".tif");
}

} // namespace

ExportResolvedDestination resolveDestination(const document::ExportJob& job,
                                             const document::ExportOutput& output,
                                             const ExportDestinationContext& context) {
    ExportResolvedDestination result;
    const auto& destination = output.destination;
    const auto format = output.settings.format;
    if (destination.path.empty()) {
        result.issues.push_back(issue(ExportPreflightCode::DestinationMissing,
                                      ExportPreflightTarget::Destination,
                                      "Choose where to save this output"));
        return result;
    }
    const TokenValues values{.name = context.compositionName,
                             .job = displayName(job, context),
                             .format = std::string(exportFormatShortName(format)),
                             .date = dateText(context)};
    std::string expanded;
    switch (expand(destination.path, values, expanded, false)) {
    case Expansion::UnknownToken:
        result.issues.push_back(issue(ExportPreflightCode::NamingInvalid,
                                      ExportPreflightTarget::Destination,
                                      "The destination uses an unknown token; use {name}, {job}, "
                                      "{format} or {date}"));
        return result;
    case Expansion::Separator:
        result.issues.push_back(issue(ExportPreflightCode::NamingInvalid,
                                      ExportPreflightTarget::Destination,
                                      "A token value contains a path separator"));
        return result;
    case Expansion::Ok:
        break;
    }
    std::filesystem::path base;
    if (destination.portability == "external") {
        const auto path = fromFileUri(expanded);
        if (!path || !path->is_absolute()) {
            result.issues.push_back(issue(ExportPreflightCode::NamingInvalid,
                                          ExportPreflightTarget::Destination,
                                          "The destination is not an absolute file URI"));
            return result;
        }
        base = *path;
    } else {
        if (!context.projectFolder) {
            result.issues.push_back(issue(ExportPreflightCode::DestinationMissing,
                                          ExportPreflightTarget::Destination,
                                          "Save the project first, or choose where to save "
                                          "this output"));
            return result;
        }
        const auto relative = std::filesystem::path(expanded).lexically_normal();
        if (relative.is_absolute() || relative.empty() || *relative.begin() == "..") {
            result.issues.push_back(issue(ExportPreflightCode::NamingInvalid,
                                          ExportPreflightTarget::Destination,
                                          "A project-relative destination stays below the "
                                          "project folder"));
            return result;
        }
        base = (*context.projectFolder / relative).lexically_normal();
    }
    if (base.filename().empty() || !extensionMatches(base, format)) {
        result.issues.push_back(
            issue(ExportPreflightCode::NamingInvalid, ExportPreflightTarget::Container,
                  "The destination must end in ." + std::string(exportFormatExtension(format))));
        return result;
    }
    result.basePath = base;
    const bool still = document::isImageExportFormat(format) && context.range &&
                       (job.render.range.kind == document::ExportRangeKind::Frame ||
                        context.range->first == context.range->last);
    if (!document::isImageExportFormat(format) || still || !context.range) {
        result.firstPath = base;
        result.lastPath = base;
        result.fileCount = context.range || !document::isImageExportFormat(format) ? 1 : 0;
        return result;
    }
    std::string pattern;
    if (expand(output.settings.naming.pattern, values, pattern, true) != Expansion::Ok) {
        result.issues.push_back(issue(ExportPreflightCode::NamingInvalid,
                                      ExportPreflightTarget::Naming,
                                      "The naming pattern uses an unknown token or a value with a "
                                      "path separator"));
        return result;
    }
    const SequenceNamingV1 naming{.startFrame = output.settings.naming.startFrame,
                                  .framePadding = output.settings.naming.framePadding,
                                  .namePattern = pattern};
    const auto& range = *context.range;
    const auto first =
        sequencePublicationPathV1(base, values.name, range.first, range.first, range.last, naming);
    const auto last =
        sequencePublicationPathV1(base, values.name, range.last, range.first, range.last, naming);
    if (!first || !last) {
        result.issues.push_back(issue(ExportPreflightCode::NamingInvalid,
                                      ExportPreflightTarget::Naming,
                                      "Sequence names need exactly one #### or {frame}, safe "
                                      "characters and the format's extension"));
        return result;
    }
    result.firstPath = *first;
    result.lastPath = *last;
    result.fileCount = range.last - range.first + 1;
    return result;
}

namespace detail {
std::optional<std::filesystem::path> exportFramePath(const document::ExportJob& job,
                                                     const ExportRunOutput& output,
                                                     const std::uint64_t frame) {
    const auto& context = output.context;
    if (!context.range || frame < context.range->first || frame > context.range->last)
        return std::nullopt;
    const auto& resolved = output.destination;
    if (!resolved.issues.empty() || resolved.firstPath.empty())
        return std::nullopt;
    if (resolved.fileCount <= 1)
        return resolved.firstPath;
    const TokenValues values{.name = context.compositionName,
                             .job = displayName(job, context),
                             .format =
                                 std::string(exportFormatShortName(output.record.settings.format)),
                             .date = dateText(context)};
    std::string pattern;
    if (expand(output.record.settings.naming.pattern, values, pattern, true) != Expansion::Ok)
        return std::nullopt;
    // The base the pattern's <base>/<ext> read: the resolved destination before naming.
    const auto& base = resolved.basePath;
    const SequenceNamingV1 naming{.startFrame = output.record.settings.naming.startFrame,
                                  .framePadding = output.record.settings.naming.framePadding,
                                  .namePattern = pattern};
    return sequencePublicationPathV1(base, values.name, frame, context.range->first,
                                     context.range->last, naming);
}
} // namespace detail

namespace {

std::string jsonQuoted(const std::string_view text) {
    std::string out = "\"";
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
    return out + "\"";
}

std::string outputJson(const document::ExportOutput& output) {
    return "{\"preset\":" + jsonQuoted(output.presetId) +
           ",\"settings\":" + exportSettingsJson(output.settings) +
           ",\"destination\":{\"portability\":" + jsonQuoted(output.destination.portability) +
           ",\"path\":" + jsonQuoted(output.destination.path) + ",\"overwrite\":" +
           jsonQuoted(document::exportOverwriteToken(output.destination.overwrite)) +
           "},\"enabled\":" + (output.enabled ? "true" : "false") +
           ",\"importIntoAssets\":" + (output.importIntoAssets ? "true" : "false") + "}";
}

std::string pythonQuoted(const std::string_view text) {
    std::string out = "'";
    for (const char c : text) {
        if (c == '\'' || c == '\\')
            out.push_back('\\');
        out.push_back(c);
    }
    return out + "'";
}

} // namespace

std::string exportJobScript(const document::ExportJob& job, const ExportScriptFormat format) {
    const auto& range = job.render.range;
    const auto kind = std::string(document::exportRangeKindToken(range.kind));
    std::vector<std::string> outputs;
    for (const auto& output : job.outputs)
        outputs.push_back(outputJson(output));
    if (format == ExportScriptFormat::CliJson) {
        std::string args = "\"composition\":" + std::to_string(job.composition.value());
        if (!job.name.empty())
            args += ",\"name\":" + jsonQuoted(job.name);
        args += ",\"range\":" + jsonQuoted(kind);
        if (range.kind == document::ExportRangeKind::Custom ||
            range.kind == document::ExportRangeKind::Frame)
            args += ",\"first\":" + std::to_string(range.first);
        if (range.kind == document::ExportRangeKind::Custom)
            args += ",\"last\":" + std::to_string(range.last);
        args += std::string(",\"enabled\":") + (job.enabled ? "true" : "false");
        if (job.labelColor)
            args += ",\"labelColor\":[" + std::to_string((*job.labelColor)[0]) + "," +
                    std::to_string((*job.labelColor)[1]) + "," +
                    std::to_string((*job.labelColor)[2]) + "]";
        args += ",\"outputs\":[";
        for (std::size_t index = 0; index < outputs.size(); ++index)
            args += (index ? "," : "") + jsonQuoted(outputs[index]);
        args += "]";
        return "[{\"op\":\"bloom.export.add-job\",\"args\":{" + args +
               "}},\n {\"op\":\"export.start\",\"args\":{}}]\n";
    }
    std::string script = "import bloom\n\njob = bloom.ops.export.add_job(\n    composition=" +
                         std::to_string(job.composition.value());
    if (!job.name.empty())
        script += ",\n    name=" + pythonQuoted(job.name);
    script += ",\n    range=" + pythonQuoted(kind);
    if (range.kind == document::ExportRangeKind::Custom ||
        range.kind == document::ExportRangeKind::Frame)
        script += ",\n    first=" + std::to_string(range.first);
    if (range.kind == document::ExportRangeKind::Custom)
        script += ",\n    last=" + std::to_string(range.last);
    script += std::string(",\n    enabled=") + (job.enabled ? "True" : "False");
    if (job.labelColor)
        script += ",\n    labelColor=[" + std::to_string((*job.labelColor)[0]) + ", " +
                  std::to_string((*job.labelColor)[1]) + ", " +
                  std::to_string((*job.labelColor)[2]) + "]";
    script += ",\n    outputs=[\n";
    for (const auto& output : outputs)
        script += "        " + pythonQuoted(output) + ",\n";
    script += "    ],\n)\nbloom.export.start()\n";
    return script;
}

} // namespace bloom::host
