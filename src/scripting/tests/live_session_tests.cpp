#include <atomic>
#include <bloom/commands/operations.hpp>
#include <bloom/scripting/facade.hpp>
#include <bloom/scripting/render.hpp>
#include <iostream>
#include <stdexcept>

namespace {
using namespace bloom;
void require(bool condition, const char* message) {
    if (!condition)
        throw std::runtime_error(message);
}
void exercise() {
    auto created = scripting::Session::createNew();
    require(static_cast<bool>(created), "Create owning session");
    std::shared_ptr<scripting::Session> owner = std::move(created).takeSession();
    auto stream = std::make_shared<scripting::EventStream>();
    const auto observer = owner->commandStack()->addObserver(
        [stream](const commands::CommandEvent& event) { stream->publish(event); });
    std::atomic_bool valid = true;
    scripting::SessionBinding binding{
        .valid = [&] { return valid.load(); },
        .snapshot = [owner] { return owner->snapshot(); },
        .execute =
            [owner](commands::Transaction transaction) {
                return owner->execute(std::move(transaction));
            },
        .history = [owner](bool redo) { return redo ? owner->redo() : owner->undo(); },
        .events = stream,
        .publication = owner->publicationCoordinator(),
        .artifacts = owner->artifactCoordinator()};
    auto attached = scripting::Session::attach(std::move(binding));
    require(static_cast<bool>(attached), "Attach live session");
    auto client = std::move(attached).takeSession();
    require(client->document() == nullptr && client->commandStack() == nullptr,
            "Live client cannot acquire mutable document pointers");
    scripting::Facade facade(*client);
    int revisions = 0;
    auto subscription = facade.events.subscribe([&](const commands::CommandEvent& event) {
        if (event.kind == commands::CommandEventKind::RevisionChanged)
            ++revisions;
    });
    const auto result = client->executeJsonOperation("bloom.project.set-name",
                                                     {{"name", scripting::Value{"Attached"}}});
    require(result.succeeded() && owner->snapshot().project().name() == "Attached" &&
                revisions == 1,
            "Live transaction reaches original host and event seam");
    require(client->undo().succeeded() && revisions == 2, "Live undo reaches original stack");
    require(client->redo().succeeded() && revisions == 3, "Live redo reaches original stack");
    require(!client->saveAs("must-not-exist.bloom").succeeded(),
            "Live save requires the owning host");
    require(!client
                 ->executeJsonOperation("bloom.animation.create-for-parameter",
                                        {{"composition", scripting::Value{1}},
                                         {"parameter", scripting::Value{999}},
                                         {"time", scripting::Value{scripting::ValueArray{
                                                      scripting::Value{0}, scripting::Value{1}}}}})
                 .succeeded(),
            "Invalid animation target returns a command diagnostic");
    valid = false;
    require(!client->isValid() && !client->undo().succeeded(),
            "Revoked live session refuses history");
    require(!client
                 ->executeJsonOperation("bloom.project.set-name",
                                        {{"name", scripting::Value{"Revoked"}}})
                 .succeeded(),
            "Revoked live session refuses transactions");
    bool snapshotRefused = false;
    try {
        (void)client->snapshot();
    } catch (const std::runtime_error&) {
        snapshotRefused = true;
    }
    require(snapshotRefused, "Revoked live snapshot is unavailable");
    owner->commandStack()->removeObserver(observer);
    client.reset();
    stream.reset();
    subscription.reset();
    require(!scripting::Session::attach({}), "Incomplete live binding is refused");

    runtime::TaskScheduler scheduler;
    runtime::SnapshotCompiler compiler(document::builtInNodeDefinitions());
    const auto cancelled = scripting::Render::run(
        *owner, scheduler, compiler,
        {.composition = owner->snapshot().project().compositions().front().id(),
         .frame = 12,
         .range = std::nullopt,
         .preset = output::OutputPresetV1::PngRgba8SrgbV1,
         .destination = "must-not-render.png",
         .cancelled = [] { return true; }});
    require(!cancelled.succeeded && scheduler.snapshots().empty(),
            "Cancelled render never starts native work");
}
} // namespace
int main() {
    try {
        exercise();
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    return 0;
}
