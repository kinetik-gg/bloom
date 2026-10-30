#include <atomic>
#include <bloom/commands/operations.hpp>
#include <bloom/scripting/facade.hpp>
#include <bloom/scripting/render.hpp>
#include <chrono>
#include <iostream>
#include <stdexcept>
#include <system_error>

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
    std::filesystem::path sourceBase = "/captured-project";
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
        .artifacts = owner->artifactCoordinator(),
        .renderSourceSnapshot =
            [&] {
                if (!valid)
                    throw std::runtime_error("The live project was replaced");
                return scripting::SessionRenderSourceSnapshot{
                    owner->snapshot(), sourceBase, *owner->hostSession()->colorSettings()};
            }};
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
    const auto capturedSource = client->renderSourceSnapshot();
    sourceBase = "/saved-as-project";
    require(client->undo().succeeded(), "Source-context capture permits a later authoring change");
    const auto savedAsSource = client->renderSourceSnapshot();
    require(capturedSource.snapshot.project().name() == "Attached" &&
                capturedSource.assetBaseDirectory == "/captured-project" &&
                capturedSource.colorSettings == savedAsSource.colorSettings &&
                savedAsSource.snapshot.project().name() != "Attached" &&
                savedAsSource.assetBaseDirectory == sourceBase,
            "Live render captures immutable snapshot and source base together across Save As");
    require(client->redo().succeeded(), "Restore source-context authoring fixture");
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
    bool sourceRefused = false;
    try {
        (void)client->renderSourceSnapshot();
    } catch (const std::runtime_error&) {
        sourceRefused = true;
    }
    require(sourceRefused, "Revoked live render source context is unavailable");
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

void savedColorIntent() {
    auto created = scripting::Session::createNew();
    require(static_cast<bool>(created), "Create color export session");
    auto session = std::move(created).takeSession();
    auto settings = *session->hostSession()->colorSettings();
    // A new session starts on the qualified built-in ACES configuration.
    const auto* builtIn = std::get_if<document::BuiltInOcioConfigLocator>(&settings.ocioConfig.locator);
    require(builtIn != nullptr, "New session uses the built-in OCIO configuration");
    const std::string configUri = builtIn->uri;
    const auto revision = std::optional{settings.ocioConfig.expectedRevision.digest};
    require(*revision != core::Sha256Digest{}, "Qualified ACES config revision is pinned");
    settings.processColorSpaceId = "Linear P3-D65";
    require(session->hostSession()->setColorSettings(settings) ==
                host::ProjectSessionColorSettingsStatus::Updated,
            "Set nondefault project working color space");
    const auto compositionId = session->snapshot().project().compositions().front().id();
    commands::Transaction overrideWorking("Override working color space",
                                          session->snapshot().revision());
    overrideWorking.emplace<commands::SetCompositionWorkingColorSpace>(
        compositionId, std::optional<std::string>{"ACEScg"});
    require(session->execute(std::move(overrideWorking)).succeeded(),
            "Set composition working-space override");
    const auto path =
        std::filesystem::temp_directory_path() /
        ("bloom-scripting-color-intent-" +
         std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".bloom");
    require(session->saveAs(path).succeeded(), "Save project with ACES color intent");
    auto opened = scripting::Session::open(path);
    std::error_code removeError;
    std::filesystem::remove(path, removeError);
    require(static_cast<bool>(opened), "Reopen project with saved color intent");
    auto reopened = std::move(opened).takeSession();
    const auto source = reopened->renderSourceSnapshot();
    const auto* composition = source.snapshot.project().findComposition(compositionId);
    require(composition != nullptr && source.colorSettings == settings,
            "Saved project retains exact config and project working space");
    const auto intent = scripting::captureRenderColorIntent(source, *composition);
    require(intent.has_value() && intent->workingColorSpaceId == "ACEScg" &&
                intent->ocioConfigRevision == *revision &&
                intent->ocioConfigUri == configUri && intent->storage != nullptr,
            "Shared headless export capture owns the saved effective color intent");
    const auto* originalComposition = session->snapshot().project().findComposition(compositionId);
    require(originalComposition != nullptr, "Original composition exists");
    const auto original =
        scripting::captureRenderColorIntent(session->renderSourceSnapshot(), *originalComposition);
    require(original == intent, "Reopened export identity matches pre-save authoring identity");
    commands::Transaction clearOverride("Inherit project working color space",
                                        reopened->snapshot().revision());
    clearOverride.emplace<commands::SetCompositionWorkingColorSpace>(compositionId, std::nullopt);
    require(reopened->execute(std::move(clearOverride)).succeeded(), "Clear composition override");
    const auto inheritedSource = reopened->renderSourceSnapshot();
    const auto* inheritedComposition =
        inheritedSource.snapshot.project().findComposition(compositionId);
    require(inheritedComposition != nullptr, "Inherited composition exists");
    const auto inherited =
        scripting::captureRenderColorIntent(inheritedSource, *inheritedComposition);
    require(inherited.has_value() && inherited->workingColorSpaceId == "Linear P3-D65" &&
                inherited->ocioConfigRevision == *revision,
            "Headless export uses project working space after override removal");

    auto relative = *reopened->hostSession()->colorSettings();
    relative.ocioConfig.locator = document::ProjectRelativeOciozLocator{"color/config.ocioz"};
    relative.ocioConfig.portability = document::OcioConfigPortability::ProjectRelative;
    relative.ocioConfig.expectedRevision.digest = {};
    require(relative.validate().ok(), "Zero-digest relative OCIO reference is valid");
    require(reopened->hostSession()->setColorSettings(relative) ==
                host::ProjectSessionColorSettingsStatus::Updated,
            "Set relative OCIO archive on headless project");
    const auto relativeSource = reopened->renderSourceSnapshot();
    const auto* relativeComposition =
        relativeSource.snapshot.project().findComposition(compositionId);
    require(relativeComposition != nullptr, "Relative OCIO composition exists");
    const auto relativeIntent =
        scripting::captureRenderColorIntent(relativeSource, *relativeComposition);
    require(relativeIntent.has_value() && relativeIntent->ocioConfigUri == "color/config.ocioz" &&
                relativeIntent->ocioConfigRevision == core::Sha256Digest{},
            "Headless capture preserves relative OCIO locator instead of empty Neutral intent");
}
} // namespace
int main() {
    try {
        exercise();
        savedColorIntent();
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    return 0;
}
