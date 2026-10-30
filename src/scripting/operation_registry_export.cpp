// ADR 0033 section 13: the bloom.export.* project-truth operations. They build the same undoable
// commands the Export panel issues (commands/export_queue_operations.hpp), so a script edit has the
// panel's validation and undo. Output settings travel as the document 1.29 JSON objects.

#include <bloom/commands/export_queue_operations.hpp>
#include <bloom/host/export_presets.hpp>
#include <bloom/scripting/operation_registry.hpp>

#include "operation_values.hpp"

#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace bloom::scripting::detail {
namespace {

using document::ExportJobId;
using document::ExportOutputId;

[[nodiscard]] ArgumentSchema argument(std::string name, const ValueKind kind, const bool required,
                                      Value example, std::string summary,
                                      const ContextSource context = ContextSource::None) {
    return {.name = std::move(name),
            .kind = kind,
            .required = required,
            .inExample = false,
            .context = context,
            .example = std::move(example),
            .summary = std::move(summary)};
}

[[nodiscard]] const Value* find(const Arguments& arguments, const std::string& name) {
    const auto found = arguments.find(name);
    return found == arguments.end() ? nullptr : &found->second;
}

[[nodiscard]] OperationCreateResult made(std::unique_ptr<commands::Operation> operation) {
    return OperationCreateResult(std::move(operation), std::nullopt);
}

template <typename Id>
[[nodiscard]] std::optional<Id> idArgument(const Arguments& arguments, const std::string& name) {
    const auto* value = find(arguments, name);
    if (value == nullptr)
        return std::nullopt;
    const auto raw = idOf(*value);
    return raw ? std::optional(Id::fromRaw(*raw)) : std::nullopt;
}

template <typename Id>
[[nodiscard]] std::optional<std::vector<Id>> idList(const Arguments& arguments,
                                                    const std::string& name) {
    const auto* value = find(arguments, name);
    const auto* array = value ? std::get_if<ValueArray>(&value->storage) : nullptr;
    if (array == nullptr)
        return std::nullopt;
    std::vector<Id> ids;
    for (const auto& item : *array) {
        const auto raw = idOf(item);
        if (!raw)
            return std::nullopt;
        ids.push_back(Id::fromRaw(*raw));
    }
    return ids;
}

[[nodiscard]] std::optional<std::string> textArgument(const Arguments& arguments,
                                                      const std::string& name) {
    const auto* value = find(arguments, name);
    const auto* text = value ? std::get_if<std::string>(&value->storage) : nullptr;
    return text ? std::optional(*text) : std::nullopt;
}

[[nodiscard]] std::optional<bool> boolArgument(const Arguments& arguments,
                                               const std::string& name) {
    const auto* value = find(arguments, name);
    const auto* flag = value ? std::get_if<bool>(&value->storage) : nullptr;
    return flag ? std::optional(*flag) : std::nullopt;
}

[[nodiscard]] std::optional<document::ExportRange>
rangeArguments(const Arguments& arguments, const std::string& op, OperationCreateResult& error) {
    document::ExportRange range;
    if (const auto kind = textArgument(arguments, "range")) {
        const auto parsed = document::parseExportRangeKind(*kind);
        if (!parsed) {
            error = argumentFailure(op, "range",
                                    R"(Expected "work-area", "composition", "custom" or "frame".)");
            return std::nullopt;
        }
        range.kind = *parsed;
    }
    const auto number = [&](const std::string& name) -> std::optional<std::uint64_t> {
        const auto* value = find(arguments, name);
        if (value == nullptr)
            return std::uint64_t{0};
        const auto integer = integerOf(*value);
        if (!integer || *integer < 0) {
            error = argumentFailure(op, name, "Expected a non-negative frame index.");
            return std::nullopt;
        }
        return static_cast<std::uint64_t>(*integer);
    };
    const auto first = number("first");
    const auto last = number("last");
    if (!first || !last)
        return std::nullopt;
    range.first = *first;
    range.last = range.kind == document::ExportRangeKind::Custom ? *last : 0;
    return range;
}

[[nodiscard]] document::ExportOutput defaultOutput() {
    document::ExportOutput output;
    output.presetId = std::string(host::kExportDefaultVideoPresetId);
    if (const auto preset = host::builtInExportPreset(host::kExportDefaultVideoPresetId))
        output.settings = preset->settings;
    output.destination.path = "Exports/{name}.mp4";
    return output;
}

const std::string kOutputSummary =
    R"(One output as JSON: {"preset": id, "settings": {document 1.29 settings}, )"
    R"("destination": {"portability", "path", "overwrite"}, "enabled", "importIntoAssets"}.)";

} // namespace

void addExportOperations(std::vector<OperationDescriptor>& descriptors) {
    const auto add = [&descriptors](std::string id, std::vector<ArgumentSchema> arguments,
                                    OperationFactory factory) {
        descriptors.push_back(
            {.schema = {.typeId = std::move(id), .arguments = std::move(arguments)},
             .factory = std::move(factory),
             .contextualFactory = {}});
    };

    add("bloom.export.add-job",
        {argument("composition", ValueKind::Id, true, StableId{1},
                  "The composition to export. Defaults to the current composition.",
                  ContextSource::Composition),
         argument("name", ValueKind::String, false, "Review", "The job's name."),
         argument("range", ValueKind::String, false, "work-area",
                  R"("work-area" (default), "composition", "custom" or "frame".)"),
         argument("first", ValueKind::Integer, false, std::int64_t{0},
                  "First frame of a custom range, or the frame of a frame range."),
         argument("last", ValueKind::Integer, false, std::int64_t{47},
                  "Last frame of a custom range."),
         argument("enabled", ValueKind::Boolean, false, true, "Included by Start All."),
         argument(
             "labelColor", ValueKind::Array, false,
             ValueArray{Value(std::int64_t{255}), Value(std::int64_t{0}), Value(std::int64_t{0})},
             "Optional row label as three integers 0..255."),
         argument("outputs", ValueKind::Array, false, ValueArray{},
                  "Outputs as JSON strings; omitted adds one H.264 MP4 output. " + kOutputSummary)},
        [](const std::string& op, const Arguments& args) {
            OperationCreateResult error(nullptr, std::nullopt);
            const auto composition = idArgument<document::CompositionId>(args, "composition");
            if (!composition)
                return argumentFailure(op, "composition", "Expected a composition id.");
            document::ExportJob job;
            job.composition = *composition;
            job.name = textArgument(args, "name").value_or("");
            const auto range = rangeArguments(args, op, error);
            if (!range)
                return error;
            job.render.range = *range;
            job.enabled = boolArgument(args, "enabled").value_or(true);
            if (const auto* color = find(args, "labelColor")) {
                const auto* channels = std::get_if<ValueArray>(&color->storage);
                if (!channels || channels->size() != 3)
                    return argumentFailure(op, "labelColor", "Expected three integers 0..255.");
                std::array<std::uint8_t, 3> rgb{};
                for (std::size_t index = 0; index < 3; ++index) {
                    const auto channel = integerOf((*channels)[index]);
                    if (!channel || *channel < 0 || *channel > 255)
                        return argumentFailure(op, "labelColor", "Expected three integers 0..255.");
                    rgb[index] = static_cast<std::uint8_t>(*channel);
                }
                job.labelColor = rgb;
            }
            if (const auto* outputs = find(args, "outputs")) {
                const auto* list = std::get_if<ValueArray>(&outputs->storage);
                if (!list)
                    return argumentFailure(op, "outputs", "Expected an array of JSON strings.");
                for (const auto& item : *list) {
                    const auto* text = std::get_if<std::string>(&item.storage);
                    auto output = text ? host::exportOutputFromJson(*text) : std::nullopt;
                    if (!output)
                        return argumentFailure(op, "outputs",
                                               "An output is not valid JSON. " + kOutputSummary);
                    job.outputs.push_back(std::move(*output));
                }
            }
            if (job.outputs.empty())
                job.outputs.push_back(defaultOutput());
            return made(std::make_unique<commands::AddExportJob>(std::move(job)));
        });

    add("bloom.export.remove-jobs",
        {argument("jobs", ValueKind::Array, true, ValueArray{Value(StableId{1})}, "The jobs.")},
        [](const std::string& op, const Arguments& args) {
            const auto jobs = idList<ExportJobId>(args, "jobs");
            if (!jobs)
                return argumentFailure(op, "jobs", "Expected an array of job ids.");
            return made(std::make_unique<commands::RemoveExportJobs>(*jobs));
        });
    add("bloom.export.remove-finished-jobs",
        {argument("jobs", ValueKind::Array, true, ValueArray{Value(StableId{1})},
                  "The finished jobs to remove (export.status lists Done jobs).")},
        [](const std::string& op, const Arguments& args) {
            const auto jobs = idList<ExportJobId>(args, "jobs");
            if (!jobs)
                return argumentFailure(op, "jobs", "Expected an array of job ids.");
            return made(std::make_unique<commands::RemoveExportJobs>(*jobs, true));
        });
    add("bloom.export.duplicate-job",
        {argument("job", ValueKind::Id, true, StableId{1}, "The job to copy.")},
        [](const std::string& op, const Arguments& args) {
            const auto job = idArgument<ExportJobId>(args, "job");
            if (!job)
                return argumentFailure(op, "job", "Expected a job id.");
            return made(std::make_unique<commands::DuplicateExportJob>(*job));
        });
    add("bloom.export.move-jobs",
        {argument("jobs", ValueKind::Array, true, ValueArray{Value(StableId{1})}, "The jobs."),
         argument("index", ValueKind::Integer, true, std::int64_t{0},
                  "Where the first moved job lands in run order.")},
        [](const std::string& op, const Arguments& args) {
            const auto jobs = idList<ExportJobId>(args, "jobs");
            const auto* index = find(args, "index");
            const auto value = index ? integerOf(*index) : std::nullopt;
            if (!jobs || !value || *value < 0)
                return argumentFailure(op, "jobs", "Expected job ids and a non-negative index.");
            return made(std::make_unique<commands::MoveExportJobs>(
                *jobs, static_cast<std::size_t>(*value)));
        });
    add("bloom.export.set-job-name",
        {argument("job", ValueKind::Id, true, StableId{1}, "The job."),
         argument("name", ValueKind::String, true, "Review", "Empty shows the composition name.")},
        [](const std::string& op, const Arguments& args) {
            const auto job = idArgument<ExportJobId>(args, "job");
            const auto name = textArgument(args, "name");
            if (!job || !name)
                return argumentFailure(op, "job", "Expected a job id and a name.");
            return made(std::make_unique<commands::SetExportJobName>(*job, *name));
        });
    add("bloom.export.set-job-range",
        {argument("job", ValueKind::Id, true, StableId{1}, "The job."),
         argument("range", ValueKind::String, true, "custom", "The range kind."),
         argument("first", ValueKind::Integer, false, std::int64_t{0}, "First frame."),
         argument("last", ValueKind::Integer, false, std::int64_t{47}, "Last frame (custom).")},
        [](const std::string& op, const Arguments& args) {
            OperationCreateResult error(nullptr, std::nullopt);
            const auto job = idArgument<ExportJobId>(args, "job");
            if (!job)
                return argumentFailure(op, "job", "Expected a job id.");
            const auto range = rangeArguments(args, op, error);
            if (!range)
                return error;
            return made(std::make_unique<commands::SetExportJobRange>(*job, *range));
        });
    add("bloom.export.set-job-enabled",
        {argument("job", ValueKind::Id, true, StableId{1}, "The job."),
         argument("enabled", ValueKind::Boolean, true, true, "Included by Start All.")},
        [](const std::string& op, const Arguments& args) {
            const auto job = idArgument<ExportJobId>(args, "job");
            const auto enabled = boolArgument(args, "enabled");
            if (!job || !enabled)
                return argumentFailure(op, "job", "Expected a job id and a boolean.");
            return made(std::make_unique<commands::SetExportJobEnabled>(*job, *enabled));
        });
    add("bloom.export.set-job-label-color",
        {argument("job", ValueKind::Id, true, StableId{1}, "The job."),
         argument(
             "labelColor", ValueKind::Array, false,
             ValueArray{Value(std::int64_t{255}), Value(std::int64_t{0}), Value(std::int64_t{0})},
             "Three integers 0..255; omitted clears the label.")},
        [](const std::string& op, const Arguments& args) {
            const auto job = idArgument<ExportJobId>(args, "job");
            if (!job)
                return argumentFailure(op, "job", "Expected a job id.");
            std::optional<std::array<std::uint8_t, 3>> color;
            if (const auto* value = find(args, "labelColor")) {
                const auto* channels = std::get_if<ValueArray>(&value->storage);
                if (!channels || channels->size() != 3)
                    return argumentFailure(op, "labelColor", "Expected three integers 0..255.");
                std::array<std::uint8_t, 3> rgb{};
                for (std::size_t index = 0; index < 3; ++index) {
                    const auto channel = integerOf((*channels)[index]);
                    if (!channel || *channel < 0 || *channel > 255)
                        return argumentFailure(op, "labelColor", "Expected three integers 0..255.");
                    rgb[index] = static_cast<std::uint8_t>(*channel);
                }
                color = rgb;
            }
            return made(std::make_unique<commands::SetExportJobLabelColor>(*job, color));
        });
    add("bloom.export.add-output",
        {argument("job", ValueKind::Id, true, StableId{1}, "The job."),
         argument("output", ValueKind::String, true, "{}", kOutputSummary)},
        [](const std::string& op, const Arguments& args) {
            const auto job = idArgument<ExportJobId>(args, "job");
            const auto text = textArgument(args, "output");
            auto output = text ? host::exportOutputFromJson(*text) : std::nullopt;
            if (!job || !output)
                return argumentFailure(op, "output",
                                       "Expected a job id and an output. " + kOutputSummary);
            return made(std::make_unique<commands::AddExportOutput>(*job, std::move(*output)));
        });
    add("bloom.export.remove-output",
        {argument("output", ValueKind::Id, true, StableId{1}, "The output.")},
        [](const std::string& op, const Arguments& args) {
            const auto output = idArgument<ExportOutputId>(args, "output");
            if (!output)
                return argumentFailure(op, "output", "Expected an output id.");
            return made(std::make_unique<commands::RemoveExportOutput>(*output));
        });
    add("bloom.export.set-output-settings",
        {argument("output", ValueKind::Id, true, StableId{1}, "The output."),
         argument("settings", ValueKind::String, true, R"({"format":"h264-mp4"})",
                  "The document 1.29 settings object as JSON.")},
        [](const std::string& op, const Arguments& args) {
            const auto output = idArgument<ExportOutputId>(args, "output");
            const auto text = textArgument(args, "settings");
            auto settings = text ? host::exportSettingsFromJson(*text) : std::nullopt;
            if (!output || !settings)
                return argumentFailure(op, "settings", "Expected an output id and settings JSON.");
            return made(
                std::make_unique<commands::SetExportOutputSettings>(*output, std::move(*settings)));
        });
    add("bloom.export.apply-preset",
        {argument("output", ValueKind::Id, true, StableId{1}, "The output."),
         argument("preset", ValueKind::String, true, "builtin:prores-422hq-mov",
                  "A built-in preset id (export.presets lists them).")},
        [](const std::string& op, const Arguments& args) {
            const auto output = idArgument<ExportOutputId>(args, "output");
            const auto id = textArgument(args, "preset");
            const auto preset = id ? host::builtInExportPreset(*id) : std::nullopt;
            if (!output || !preset)
                return argumentFailure(op, "preset", "Expected an output id and a preset id.");
            return made(std::make_unique<commands::ApplyExportPreset>(*output, preset->id,
                                                                      preset->settings));
        });
    add("bloom.export.set-output-destination",
        {argument("output", ValueKind::Id, true, StableId{1}, "The output."),
         argument("path", ValueKind::String, true, "Exports/{name}.mp4",
                  "Project-relative path, or a file URI with portability external."),
         argument("portability", ValueKind::String, false, "project-relative",
                  R"("project-relative" (default) or "external".)"),
         argument("overwrite", ValueKind::String, false, "ask",
                  R"("ask" (default), "replace" or "refuse".)")},
        [](const std::string& op, const Arguments& args) {
            const auto output = idArgument<ExportOutputId>(args, "output");
            const auto path = textArgument(args, "path");
            if (!output || !path)
                return argumentFailure(op, "path", "Expected an output id and a path.");
            document::ExportDestination destination;
            destination.path = *path;
            destination.portability =
                textArgument(args, "portability").value_or("project-relative");
            if (const auto overwrite = textArgument(args, "overwrite")) {
                const auto parsed = document::parseExportOverwrite(*overwrite);
                if (!parsed)
                    return argumentFailure(op, "overwrite",
                                           R"(Expected "ask", "replace" or "refuse".)");
                destination.overwrite = *parsed;
            }
            return made(std::make_unique<commands::SetExportOutputDestination>(
                *output, std::move(destination)));
        });
    add("bloom.export.set-output-enabled",
        {argument("output", ValueKind::Id, true, StableId{1}, "The output."),
         argument("enabled", ValueKind::Boolean, true, true, "Render this output.")},
        [](const std::string& op, const Arguments& args) {
            const auto output = idArgument<ExportOutputId>(args, "output");
            const auto enabled = boolArgument(args, "enabled");
            if (!output || !enabled)
                return argumentFailure(op, "output", "Expected an output id and a boolean.");
            return made(std::make_unique<commands::SetExportOutputEnabled>(*output, *enabled));
        });
    add("bloom.export.set-output-import",
        {argument("output", ValueKind::Id, true, StableId{1}, "The output."),
         argument("importIntoAssets", ValueKind::Boolean, true, true,
                  "Import the verified result into Assets.")},
        [](const std::string& op, const Arguments& args) {
            const auto output = idArgument<ExportOutputId>(args, "output");
            const auto flag = boolArgument(args, "importIntoAssets");
            if (!output || !flag)
                return argumentFailure(op, "output", "Expected an output id and a boolean.");
            return made(std::make_unique<commands::SetExportOutputImport>(*output, *flag));
        });
}

} // namespace bloom::scripting::detail
