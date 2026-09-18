#include <bloom/commands/command_stack.hpp>
#include <bloom/commands/operations.hpp>
#include <bloom/document/new_project.hpp>
#include <bloom/document/node_definition_registry.hpp>
#include <bloom/host/project_session.hpp>
#include <bloom/runtime/snapshot_compiler.hpp>
#include <bloom/runtime/task_scheduler.hpp>
#include <bloom/scripting/facade.hpp>
#include <bloom/scripting/json_script.hpp>
#include <bloom/scripting/operation_registry.hpp>
#include <bloom/scripting/render.hpp>
#include <bloom/scripting/session.hpp>

#include <filesystem>
#include <iostream>

int main() {
    auto registry = bloom::scripting::OperationRegistry::builtIn();
    if (registry.descriptors().size() != 75U || registry.find("bloom.layer.add-solid") == nullptr ||
        registry.find("bloom.project.set-name") == nullptr) {
        std::cerr << "operation registry inventory failed\n";
        return 1;
    }
    for (const auto& descriptor : registry.descriptors()) {
        if (descriptor.schema.typeId.empty() || !descriptor.factory ||
            registry.find(descriptor.schema.typeId) != &descriptor) {
            std::cerr << "operation schema round trip failed\n";
            return 2;
        }
    }
    const auto parsed = bloom::scripting::parseScriptJson(
        R"([{"op":"bloom.project.set-name","args":{"name":"Parsed"}}])");
    if (!parsed || parsed.transactions().size() != 1U ||
        parsed.transactions().front().operation != "bloom.project.set-name") {
        std::cerr << "script parser failed\n";
        return 3;
    }

    auto created = bloom::scripting::Session::createNew();
    if (!created) {
        std::cerr << "session creation failed: " << created.diagnostic().message << '\n';
        return 4;
    }
    auto session = std::move(created).takeSession();
    std::size_t revisions = 0;
    std::size_t history = 0;
    std::size_t rejected = 0;
    std::size_t documentRevisions = 0;
    std::size_t documentRejections = 0;
    const auto documentObserver =
        session->document()->addObserver([&](const bloom::document::DocumentEvent& event) {
            if (event.kind == bloom::document::DocumentEventKind::RevisionChanged) {
                ++documentRevisions;
            } else {
                ++documentRejections;
            }
        });
    const auto observer =
        session->commandStack()->addObserver([&](const bloom::commands::CommandEvent& event) {
            using Kind = bloom::commands::CommandEventKind;
            if (event.kind == Kind::RevisionChanged) {
                ++revisions;
            } else if (event.kind == Kind::HistoryChanged) {
                ++history;
            } else {
                ++rejected;
            }
        });
    const auto first = session->executeJsonOperation(
        "bloom.project.set-name", {{"name", bloom::scripting::Value{"Scripted"}}});
    if (!first.succeeded() || revisions != 1U || history != 1U || rejected != 0U) {
        std::cerr << "command event seam failed\n";
        return 5;
    }
    const auto rejectedResult = session->executeJsonOperation(
        "bloom.layer.add-solid",
        {{"composition", bloom::scripting::Value{999}},
         {"name", bloom::scripting::Value{"x"}},
         {"color", bloom::scripting::Value{bloom::core::Color4d{1.0, 0.0, 0.0, 1.0}}}});
    if (rejectedResult.succeeded() || revisions != 1U || history != 1U || rejected != 1U) {
        std::cerr << "rejection event seam failed\n";
        return 6;
    }
    auto staleDraft = session->document()->draft(session->snapshot());
    const auto staleCommit =
        session->document()->commit(bloom::document::Revision::fromRaw(99), std::move(staleDraft));
    if (staleCommit.committed() || documentRevisions != 1U || documentRejections != 1U) {
        std::cerr << "document event seam failed\n";
        return 7;
    }
    session->document()->removeObserver(documentObserver);
    session->commandStack()->removeObserver(observer);

    auto renderSessionResult = bloom::scripting::Session::createNew(
        "Scripted", "Composition", bloom::core::RationalTime::fromInteger(48));
    if (!renderSessionResult) {
        std::cerr << "render session creation failed\n";
        return 8;
    }
    auto renderSession = std::move(renderSessionResult).takeSession();
    const auto renderScript = bloom::scripting::parseScriptJson(
        R"([{"op":"bloom.layer.add-solid","args":{"composition":1,"name":"Solid","color":[1,0,0,1],"position":[960,540]}},{"op":"bloom.layer.add-text","args":{"composition":1,"name":"Text","text":"SCRIPT-0","position":[960,540],"size":48,"color":[1,1,1,1]}}])");
    if (!renderScript) {
        std::cerr << "render script parse failed\n";
        return 9;
    }
    for (const auto& transaction : renderScript.transactions()) {
        const auto result =
            renderSession->executeJsonOperation(transaction.operation, transaction.arguments);
        if (!result.succeeded()) {
            std::cerr << "render script execution failed\n";
            return 10;
        }
    }
    bloom::runtime::TaskSchedulerConfig schedulerConfig =
        bloom::runtime::TaskSchedulerConfig::defaults();
    schedulerConfig.rowBandWorkerCount = bloom::runtime::kSerialRowBandWorkers;
    bloom::runtime::TaskScheduler scheduler(schedulerConfig);
    bloom::runtime::SnapshotCompiler compiler(bloom::document::builtInNodeDefinitions());
    const auto outputPath =
        std::filesystem::temp_directory_path() / "bloom-scripting-facade-frame-12.png";
    const auto render = bloom::scripting::Render::run(
        *renderSession, scheduler, compiler,
        {.composition = renderSession->snapshot().project().compositions().front().id(),
         .frame = 12,
         .range = std::nullopt,
         .preset = bloom::output::OutputPresetV1::PngRgba8SrgbV1,
         .destination = outputPath});
    if (!render.succeeded || render.publishedFrames != 1 || !std::filesystem::exists(outputPath)) {
        std::cerr << "headless script render failed\n";
        return 11;
    }
    std::error_code removeError;
    std::filesystem::remove(outputPath, removeError);

    auto readOnly = bloom::scripting::Session::openReadOnly("read-only.bloom");
    if (!readOnly) {
        std::cerr << "read-only session creation failed\n";
        return 8;
    }
    auto readOnlySession = std::move(readOnly).takeSession();
    bloom::commands::Transaction transaction("read-only", bloom::document::Revision{});
    transaction.emplace<bloom::commands::SetProjectName>("Refused");
    const auto readOnlyResult = readOnlySession->execute(std::move(transaction));
    if (readOnlyResult.status != bloom::host::ProjectSessionCommandStatus::ReadOnly ||
        readOnlyResult.succeeded()) {
        std::cerr << "read-only refusal failed\n";
        return 12;
    }
    return 0;
}
