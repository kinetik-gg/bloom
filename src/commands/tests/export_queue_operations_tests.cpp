// ADR 0033: export queue records (document validation) and their undoable edits.

#include "command_test_support.hpp"

#include <bloom/commands/export_queue_operations.hpp>
#include <bloom/document/export_queue.hpp>

namespace bloom::commands::test {
namespace {

using document::ExportFormat;
using document::ExportJob;
using document::ExportJobId;
using document::ExportOutput;
using document::ExportOutputId;
using document::ExportRangeKind;

ExportJob jobTemplate() {
    ExportJob job;
    job.composition = kCompositionId;
    ExportOutput output;
    output.presetId = "builtin:h264-mp4";
    output.destination.path = "Exports/{name}.mp4";
    job.outputs.push_back(output);
    return job;
}

CommandResult run(CommandStack& stack, std::unique_ptr<Operation> operation) {
    Transaction transaction("Export");
    if (!transaction.add(std::move(operation)))
        throw std::logic_error("operation refused");
    return stack.execute(std::move(transaction));
}

template <typename Op, typename... Args> CommandResult run(CommandStack& stack, Args&&... args) {
    return run(stack, std::make_unique<Op>(std::forward<Args>(args)...));
}

std::span<const ExportJob> jobs(const Document& document) {
    return document.snapshot().project().exportJobs();
}

void testValidation(TestContext& test) {
    auto job = jobTemplate();
    job.id = ExportJobId::fromRaw(1);
    job.outputs[0].id = ExportOutputId::fromRaw(1);
    test.expect(document::validateExportJob(job).ok(), "a default job validates");

    auto badProfile = job;
    badProfile.outputs[0].settings.codecProfile = "4444";
    test.expect(!document::validateExportJob(badProfile).ok(),
                "a ProRes profile is refused for H.264");
    auto badPattern = job;
    badPattern.outputs[0].settings.naming.pattern = "<base>.<ext>";
    test.expect(!document::validateExportJob(badPattern).ok(),
                "a pattern without a frame token is refused");
    badPattern.outputs[0].settings.naming.pattern = "<base>.{frame}.####.<ext>";
    test.expect(!document::validateExportJob(badPattern).ok(), "two frame tokens are refused");
    auto badRange = job;
    badRange.render.range = {ExportRangeKind::Custom, 9, 3};
    test.expect(!document::validateExportJob(badRange).ok(), "a reversed range is refused");
    auto noOutputs = job;
    noOutputs.outputs.clear();
    test.expect(!document::validateExportJob(noOutputs).ok(), "a job keeps one output");
    auto tooMany = job;
    for (std::uint64_t index = 2; index <= 17; ++index) {
        auto output = job.outputs[0];
        output.id = ExportOutputId::fromRaw(index);
        tooMany.outputs.push_back(output);
    }
    test.expect(!document::validateExportJob(tooMany).ok(), "17 outputs are refused");
    auto absolute = job;
    absolute.outputs[0].destination.path = "/renders/out.mp4";
    test.expect(!document::validateExportJob(absolute).ok(),
                "a project-relative destination is relative");
    absolute.outputs[0].destination.portability = "external";
    absolute.outputs[0].destination.path = "file:///renders/out.mp4";
    test.expect(document::validateExportJob(absolute).ok(), "an external file URI validates");
    auto longName = job;
    longName.name.assign(document::kExportTextMaximumBytes + 1, 'a');
    test.expect(!document::validateExportJob(longName).ok(), "names are bounded at 4 KiB");

    for (const auto format :
         {ExportFormat::Png, ExportFormat::OpenExr, ExportFormat::Tiff, ExportFormat::Dpx,
          ExportFormat::ProResMov, ExportFormat::DnxhrMxf, ExportFormat::H264Mov,
          ExportFormat::H264Mp4, ExportFormat::HevcMov, ExportFormat::HevcMp4, ExportFormat::Wav})
        test.expect(document::parseExportFormat(document::exportFormatToken(format)) == format,
                    "format tokens round-trip");
    test.expect(!document::parseExportFormat("gif"), "unknown format tokens are refused");
}

void testAddEditUndo(TestContext& test) {
    Document document(makeProject());
    CommandStack stack(document);
    const auto added = run<AddExportJob>(stack, jobTemplate());
    test.expect(added.changed(), "add-job applies");
    test.expect(!added.renderAffecting, "queue edits are not render-affecting");
    const auto jobId = added.outputId<ExportJobId>(kExportJobOutput);
    const auto outputId = added.outputId<ExportOutputId>(kExportOutputOutput);
    test.expect(jobId.has_value() && outputId.has_value(), "add-job returns the new ids");
    test.expect(jobs(document).size() == 1, "the queue holds the job");
    if (!jobId || !outputId)
        return;

    test.expect(run<SetExportJobName>(stack, *jobId, "Final").changed(), "rename applies");
    test.expect(
        run<SetExportJobRange>(stack, *jobId, document::ExportRange{ExportRangeKind::Custom, 2, 9})
            .changed(),
        "set range applies");
    test.expect(run<SetExportJobEnabled>(stack, *jobId, false).changed(), "disable applies");
    test.expect(run<SetExportJobLabelColor>(stack, *jobId, std::array<std::uint8_t, 3>{200, 10, 10})
                    .changed(),
                "label color applies");
    test.expect(run<SetExportOutputImport>(stack, *outputId, true).changed(), "import applies");
    test.expect(run<SetExportOutputEnabled>(stack, *outputId, false).changed(),
                "output disable applies");
    document::ExportDestination destination;
    destination.path = "Renders/{job}.mp4";
    destination.overwrite = document::ExportOverwritePolicy::Replace;
    test.expect(run<SetExportOutputDestination>(stack, *outputId, destination).changed(),
                "destination applies");
    document::ExportOutputSettings prores;
    prores.format = ExportFormat::ProResMov;
    prores.codecProfile = "hq";
    test.expect(
        run<ApplyExportPreset>(stack, *outputId, "builtin:prores-422hq-mov", prores).changed(),
        "apply preset applies");
    auto bad = prores;
    bad.codecProfile = "main10";
    test.expect(run<SetExportOutputSettings>(stack, *outputId, bad).status ==
                    CommandStatus::Rejected,
                "invalid settings are rejected");
    test.expect(run<SetExportJobName>(stack, *jobId, "Final").status == CommandStatus::NoChange,
                "an unchanged edit is a no-op");

    const auto& job = jobs(document).front();
    test.expect(job.name == "Final" && !job.enabled && job.labelColor &&
                    job.render.range.kind == ExportRangeKind::Custom,
                "job edits land");
    test.expect(job.outputs[0].presetId == "builtin:prores-422hq-mov" &&
                    job.outputs[0].settings.format == ExportFormat::ProResMov &&
                    job.outputs[0].importIntoAssets && !job.outputs[0].enabled &&
                    job.outputs[0].destination.path == "Renders/{job}.mp4",
                "output edits land");

    const auto second = run<AddExportOutput>(stack, *jobId, jobTemplate().outputs[0]);
    test.expect(second.changed() && jobs(document).front().outputs.size() == 2,
                "add-output applies");
    test.expect(run<RemoveExportOutput>(stack, *outputId).changed(), "remove-output applies");
    const auto last = jobs(document).front().outputs.front().id;
    test.expect(run<RemoveExportOutput>(stack, last).status == CommandStatus::Rejected,
                "the last output cannot be removed");

    // Every edit undoes back to the empty queue and redoes to the same records.
    const auto edited = document.snapshot().project().exportQueue();
    while (stack.canUndo())
        test.expect(stack.undo().succeeded(), "undo succeeds");
    test.expect(jobs(document).empty(), "undo restores the empty queue");
    while (stack.canRedo())
        test.expect(stack.redo().succeeded(), "redo succeeds");
    test.expect(document.snapshot().project().exportQueue() == edited,
                "redo restores the same records and ids");
}

void testDuplicateMoveRemove(TestContext& test) {
    Document document(makeProject());
    CommandStack stack(document);
    const auto first =
        run<AddExportJob>(stack, jobTemplate()).outputId<ExportJobId>(kExportJobOutput);
    const auto second =
        run<AddExportJob>(stack, jobTemplate()).outputId<ExportJobId>(kExportJobOutput);
    if (!first || !second) {
        test.fail("jobs added");
        return;
    }
    const auto copy = run<DuplicateExportJob>(stack, *first);
    const auto copyId = copy.outputId<ExportJobId>(kExportJobOutput);
    test.expect(copy.changed() && copyId && *copyId != *first, "duplicate has a new id");
    test.expect(jobs(document)[1].id == *copyId, "the copy follows the original");
    test.expect(jobs(document)[1].outputs[0].id != jobs(document)[0].outputs[0].id,
                "the copy's outputs have new ids");

    test.expect(run<MoveExportJobs>(stack, std::vector{*second}, 0).changed(), "move applies");
    test.expect(jobs(document)[0].id == *second, "the moved job runs first");
    test.expect(run<MoveExportJobs>(stack, std::vector{*second}, 0).status ==
                    CommandStatus::NoChange,
                "moving into place is a no-op");

    test.expect(run<RemoveExportJobs>(stack, std::vector{*first, *copyId}).changed(),
                "remove applies");
    test.expect(jobs(document).size() == 1 && jobs(document)[0].id == *second,
                "only the kept job remains");
    test.expect(run<RemoveExportJobs>(stack, std::vector{*first}).status == CommandStatus::Rejected,
                "removing a missing job is rejected");
    test.expect(stack.undo().succeeded() && jobs(document).size() == 3, "undo restores removal");

    // Ids are never reused after removal.
    test.expect(stack.redo().succeeded(), "redo removal");
    const auto third =
        run<AddExportJob>(stack, jobTemplate()).outputId<ExportJobId>(kExportJobOutput);
    test.expect(third && third->value() > copyId->value(), "ids are not reused");
}

// The panel reads new ids from the command result (no queue diffing).
void testAddedIds(TestContext& test) {
    Document document(makeProject());
    CommandStack stack(document);
    auto job = jobTemplate();
    job.outputs.push_back(job.outputs.front());
    job.outputs.push_back(job.outputs.front());
    const auto added = addedExportIds(run<AddExportJob>(stack, job));
    const auto record = jobs(document).front();
    test.expect(added.job == record.id && added.outputs.size() == 3 &&
                    added.outputs[0] == record.outputs[0].id &&
                    added.outputs[1] == record.outputs[1].id &&
                    added.outputs[2] == record.outputs[2].id,
                "add-job reports the job and every output id in order");
    const auto output =
        addedExportIds(run<AddExportOutput>(stack, record.id, jobTemplate().outputs[0]));
    test.expect(!output.job && output.outputs.size() == 1 &&
                    output.outputs[0] == jobs(document).front().outputs.back().id,
                "add-output reports the new output id");
    const auto copy = addedExportIds(run<DuplicateExportJob>(stack, record.id));
    test.expect(copy.job == jobs(document)[1].id && copy.outputs.size() == 4 &&
                    copy.outputs.front() == jobs(document)[1].outputs.front().id,
                "duplicate-job reports the copy's ids");
    Transaction transaction("Two jobs");
    transaction.emplace<AddExportJob>(jobTemplate());
    transaction.emplace<AddExportJob>(jobTemplate());
    const auto both = stack.execute(std::move(transaction));
    test.expect(addedExportIds(both, 0).job == jobs(document)[2].id &&
                    addedExportIds(both, 1).job == jobs(document)[3].id,
                "each operation of a transaction reports its own ids");
    test.expect(!addedExportIds(run<SetExportJobName>(stack, record.id, "x")).job,
                "other edits report no created ids");
    test.expect(!addedExportIds(run<RemoveExportOutput>(stack, ExportOutputId::fromRaw(999))).job,
                "a rejected edit reports no ids");
}

void testCompositionDeletionCascades(TestContext& test) {
    Document document(makeProject());
    CommandStack stack(document);
    test.expect(run<AddExportJob>(stack, jobTemplate()).changed(), "add job");
    auto missing = jobTemplate();
    missing.composition = CompositionId::fromRaw(999);
    test.expect(run<AddExportJob>(stack, missing).status == CommandStatus::Rejected,
                "a job needs an existing composition");
    test.expect(run<DeleteComposition>(stack, kCompositionId).changed(), "delete composition");
    test.expect(jobs(document).empty(), "deleting a composition removes its jobs");
    test.expect(document.snapshot().project().validate().ok(), "the project stays valid");
    test.expect(stack.undo().succeeded() && jobs(document).size() == 1,
                "undo restores the composition and its job");
}

void testQueueBound(TestContext& test) {
    Document document(makeProject());
    CommandStack stack(document);
    Transaction transaction("Fill");
    for (std::size_t index = 0; index < document::kExportQueueMaximumJobs; ++index)
        transaction.emplace<AddExportJob>(jobTemplate());
    test.expect(stack.execute(std::move(transaction)).changed(), "1024 jobs fit");
    test.expect(run<AddExportJob>(stack, jobTemplate()).status == CommandStatus::Rejected,
                "the 1025th job is refused");
}

} // namespace
} // namespace bloom::commands::test

int main() {
    bloom::commands::test::TestContext test;
    bloom::commands::test::testValidation(test);
    bloom::commands::test::testAddEditUndo(test);
    bloom::commands::test::testDuplicateMoveRemove(test);
    bloom::commands::test::testAddedIds(test);
    bloom::commands::test::testCompositionDeletionCascades(test);
    bloom::commands::test::testQueueBound(test);
    return test.failures() == 0 ? 0 : 1;
}
