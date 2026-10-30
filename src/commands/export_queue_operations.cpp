#include <bloom/commands/export_queue_operations.hpp>

#include <algorithm>
#include <unordered_set>
#include <utility>

namespace bloom::commands {
namespace {

using document::ExportJob;
using document::ExportJobId;
using document::ExportOutput;
using document::ExportOutputId;

OperationResult rejected(const OperationIssueCode code, std::string message) {
    return OperationResult::rejected(code, std::move(message));
}

OperationResult jobMissing() {
    return rejected(OperationIssueCode::InvalidTarget, "Export job does not exist");
}

OperationResult outputMissing() {
    return rejected(OperationIssueCode::InvalidTarget, "Export output does not exist");
}

OperationResult invalid(const document::ValidationResult& result) {
    return rejected(OperationIssueCode::InvalidValue, result.issues().empty()
                                                          ? std::string("Invalid export record")
                                                          : result.issues().front().message);
}

std::vector<OperationOutput> jobOutputs(const ExportJob& job) {
    std::vector<OperationOutput> outputs{{std::string(kExportJobOutput), job.id}};
    for (const auto& output : job.outputs)
        outputs.push_back({std::string(kExportOutputOutput), output.id});
    return outputs;
}

// Allocates an id when `id` is invalid; keeps a valid id only when nothing uses it yet.
template <typename Id, typename Allocate, typename InUse>
bool assignId(Id& id, Allocate&& allocate, InUse&& inUse) {
    if (!id.isValid()) {
        const auto fresh = allocate();
        if (!fresh)
            return false;
        id = *fresh;
        return true;
    }
    return !inUse(id);
}

bool outputIdInUse(const document::Project& project, const ExportOutputId id) {
    return project.findExportOutput(id).second != nullptr;
}

// The job owning `output` in the draft, for mutation.
std::pair<ExportJob*, ExportOutput*> mutableOutput(document::Draft& draft,
                                                   const ExportOutputId output) {
    const auto [job, found] = std::as_const(draft.project()).findExportOutput(output);
    if (job == nullptr)
        return {nullptr, nullptr};
    auto* editable = draft.project().findExportJob(job->id);
    auto at = std::ranges::find_if(editable->outputs,
                                   [output](const auto& value) { return value.id == output; });
    return {editable, &*at};
}

template <typename Mutate>
OperationResult editOutput(document::Draft& draft, const ExportOutputId id, Mutate&& mutate) {
    const auto [owner, found] = std::as_const(draft.project()).findExportOutput(id);
    if (found == nullptr)
        return outputMissing();
    auto candidate = *found;
    mutate(candidate);
    if (candidate == *found)
        return OperationResult::noChange({{std::string(kExportOutputOutput), id}});
    if (const auto check = document::validateExportOutput(candidate); !check.ok())
        return invalid(check);
    *mutableOutput(draft, id).second = std::move(candidate);
    return OperationResult::applied({{std::string(kExportOutputOutput), id}});
}

template <typename Mutate>
OperationResult editJob(document::Draft& draft, const ExportJobId id, Mutate&& mutate) {
    const auto* found = std::as_const(draft.project()).findExportJob(id);
    if (found == nullptr)
        return jobMissing();
    auto candidate = *found;
    mutate(candidate);
    if (candidate == *found)
        return OperationResult::noChange({{std::string(kExportJobOutput), id}});
    if (const auto check = document::validateExportJob(candidate); !check.ok())
        return invalid(check);
    *draft.project().findExportJob(id) = std::move(candidate);
    return OperationResult::applied({{std::string(kExportJobOutput), id}});
}

} // namespace

OperationResult AddExportJob::apply(document::Draft& draft) const {
    const auto& project = std::as_const(draft.project());
    if (project.exportJobs().size() >= document::kExportQueueMaximumJobs)
        return rejected(OperationIssueCode::InvalidValue, "The export queue is full (1024 jobs)");
    if (project.findComposition(job_.composition) == nullptr)
        return rejected(OperationIssueCode::MissingReference,
                        "The export job's composition does not exist");
    auto job = job_;
    if (!assignId(
            job.id, [&] { return draft.ids().allocateExportJob(); },
            [&](const ExportJobId id) { return project.findExportJob(id) != nullptr; }))
        return rejected(OperationIssueCode::DuplicateId, "Export job ID is unavailable");
    std::unordered_set<ExportOutputId> seen;
    for (auto& output : job.outputs) {
        if (!assignId(
                output.id, [&] { return draft.ids().allocateExportOutput(); },
                [&](const ExportOutputId id) {
                    return outputIdInUse(project, id) || seen.contains(id);
                }))
            return rejected(OperationIssueCode::DuplicateId, "Export output ID is unavailable");
        seen.insert(output.id);
    }
    if (const auto check = document::validateExportJob(job); !check.ok())
        return invalid(check);
    auto outputs = jobOutputs(job);
    if (!draft.project().insertExportJob(std::move(job),
                                         index_.value_or(project.exportJobs().size())))
        return rejected(OperationIssueCode::DuplicateId, "Export job ID is already present");
    return OperationResult::applied(std::move(outputs));
}

OperationResult RemoveExportJobs::apply(document::Draft& draft) const {
    if (jobs_.empty())
        return OperationResult::noChange();
    std::vector<OperationOutput> outputs;
    for (const auto id : jobs_)
        if (std::as_const(draft.project()).findExportJob(id) == nullptr)
            return jobMissing();
    for (const auto id : jobs_) {
        if (draft.project().removeExportJob(id))
            outputs.push_back({std::string(kExportJobOutput), id});
    }
    return OperationResult::applied(std::move(outputs));
}

OperationResult DuplicateExportJob::apply(document::Draft& draft) const {
    const auto& project = std::as_const(draft.project());
    const auto jobs = project.exportJobs();
    const auto found =
        std::ranges::find_if(jobs, [this](const auto& job) { return job.id == job_; });
    if (found == jobs.end())
        return jobMissing();
    if (jobs.size() >= document::kExportQueueMaximumJobs)
        return rejected(OperationIssueCode::InvalidValue, "The export queue is full (1024 jobs)");
    auto copy = *found;
    const auto index = static_cast<std::size_t>(found - jobs.begin()) + 1;
    const auto jobId = draft.ids().allocateExportJob();
    if (!jobId)
        return rejected(OperationIssueCode::Unsupported, "Export job ID space is exhausted");
    copy.id = *jobId;
    for (auto& output : copy.outputs) {
        const auto outputId = draft.ids().allocateExportOutput();
        if (!outputId)
            return rejected(OperationIssueCode::Unsupported, "Export output ID space is exhausted");
        output.id = *outputId;
    }
    auto outputs = jobOutputs(copy);
    if (!draft.project().insertExportJob(std::move(copy), index))
        return rejected(OperationIssueCode::DuplicateId, "Export job ID is already present");
    return OperationResult::applied(std::move(outputs));
}

OperationResult MoveExportJobs::apply(document::Draft& draft) const {
    const auto& project = std::as_const(draft.project());
    if (jobs_.empty())
        return OperationResult::noChange();
    std::unordered_set<ExportJobId> moving;
    for (const auto id : jobs_) {
        if (project.findExportJob(id) == nullptr)
            return jobMissing();
        if (!moving.insert(id).second)
            return rejected(OperationIssueCode::InvalidValue, "A job is listed twice");
    }
    std::vector<ExportJob> moved;
    std::vector<ExportJob> kept;
    for (const auto& job : project.exportJobs())
        (moving.contains(job.id) ? moved : kept).push_back(job);
    const auto at = std::min(index_, kept.size());
    std::vector<ExportJob> order(kept.begin(), kept.begin() + static_cast<std::ptrdiff_t>(at));
    order.insert(order.end(), moved.begin(), moved.end());
    order.insert(order.end(), kept.begin() + static_cast<std::ptrdiff_t>(at), kept.end());
    const auto before = project.exportJobs();
    if (std::ranges::equal(order, before, {}, &ExportJob::id, &ExportJob::id))
        return OperationResult::noChange();
    draft.project().setExportQueue({std::move(order)});
    std::vector<OperationOutput> outputs;
    for (const auto id : jobs_)
        outputs.push_back({std::string(kExportJobOutput), id});
    return OperationResult::applied(std::move(outputs));
}

OperationResult SetExportJobName::apply(document::Draft& draft) const {
    return editJob(draft, job_, [this](ExportJob& job) { job.name = name_; });
}

OperationResult SetExportJobRange::apply(document::Draft& draft) const {
    return editJob(draft, job_, [this](ExportJob& job) { job.render.range = range_; });
}

OperationResult SetExportJobEnabled::apply(document::Draft& draft) const {
    return editJob(draft, job_, [this](ExportJob& job) { job.enabled = enabled_; });
}

OperationResult SetExportJobLabelColor::apply(document::Draft& draft) const {
    return editJob(draft, job_, [this](ExportJob& job) { job.labelColor = color_; });
}

OperationResult AddExportOutput::apply(document::Draft& draft) const {
    const auto& project = std::as_const(draft.project());
    const auto* job = project.findExportJob(job_);
    if (job == nullptr)
        return jobMissing();
    if (job->outputs.size() >= document::kExportJobMaximumOutputs)
        return rejected(OperationIssueCode::InvalidValue, "An export job has at most 16 outputs");
    auto output = output_;
    if (!assignId(
            output.id, [&] { return draft.ids().allocateExportOutput(); },
            [&](const ExportOutputId id) { return outputIdInUse(project, id); }))
        return rejected(OperationIssueCode::DuplicateId, "Export output ID is unavailable");
    if (const auto check = document::validateExportOutput(output); !check.ok())
        return invalid(check);
    const auto id = output.id;
    draft.project().findExportJob(job_)->outputs.push_back(std::move(output));
    return OperationResult::applied({{std::string(kExportOutputOutput), id}});
}

OperationResult RemoveExportOutput::apply(document::Draft& draft) const {
    const auto [job, found] = std::as_const(draft.project()).findExportOutput(output_);
    if (found == nullptr)
        return outputMissing();
    if (job->outputs.size() <= 1)
        return rejected(OperationIssueCode::InvalidValue, "A job keeps at least one output");
    auto& outputs = draft.project().findExportJob(job->id)->outputs;
    std::erase_if(outputs, [this](const auto& value) { return value.id == output_; });
    return OperationResult::applied({{std::string(kExportOutputOutput), output_}});
}

OperationResult SetExportOutputSettings::apply(document::Draft& draft) const {
    return editOutput(draft, output_,
                      [this](ExportOutput& output) { output.settings = settings_; });
}

OperationResult ApplyExportPreset::apply(document::Draft& draft) const {
    if (presetId_.empty())
        return rejected(OperationIssueCode::InvalidValue, "A preset id is required");
    return editOutput(draft, output_, [this](ExportOutput& output) {
        output.presetId = presetId_;
        output.settings = settings_;
    });
}

OperationResult SetExportOutputDestination::apply(document::Draft& draft) const {
    return editOutput(draft, output_,
                      [this](ExportOutput& output) { output.destination = destination_; });
}

OperationResult SetExportOutputEnabled::apply(document::Draft& draft) const {
    return editOutput(draft, output_, [this](ExportOutput& output) { output.enabled = enabled_; });
}

OperationResult SetExportOutputImport::apply(document::Draft& draft) const {
    return editOutput(draft, output_,
                      [this](ExportOutput& output) { output.importIntoAssets = import_; });
}

} // namespace bloom::commands
