// ADR 0028 persistence: layer masks (constant and keyed paths), a track matte and the layerMask
// allocator namespace save, reopen and decode exactly in document 1.24; a 1.23 document migrates
// with none of them, and a 1.23 document carrying a 1.24 member is refused.

#include <bloom/commands/command_stack.hpp>
#include <bloom/commands/layer_mask_operations.hpp>
#include <bloom/commands/operations.hpp>
#include <bloom/core/pixel_aspect_ratio.hpp>
#include <bloom/document/color_settings.hpp>
#include <bloom/document/document.hpp>
#include <bloom/document/new_project.hpp>
#include <bloom/project/canonical_document.hpp>
#include <bloom/project/document_decode.hpp>
#include <bloom/project/document_migration.hpp>
#include <bloom/project/open_archive.hpp>
#include <bloom/project/save_archive.hpp>
#include <bloom/project/strict_json_dom.hpp>

#include <array>
#include <iostream>
#include <span>
#include <stdexcept>
#include <string>

namespace {
using namespace bloom;
int failures = 0;
void expect(const bool condition, const char* message) {
    if (!condition) {
        ++failures;
        std::cerr << "FAIL: " << message << '\n';
    }
}
project::ProjectIoOperationMemory memory() {
    auto coordinator = project::ProjectIoMemoryCoordinator::create();
    if (!coordinator)
        throw std::runtime_error("memory coordinator");
    auto operation = coordinator->createOperation();
    if (!operation)
        throw std::runtime_error("memory operation");
    return std::move(*operation);
}
document::PathValue rectangle(const double size, const bool curved) {
    document::PathValue path;
    path.closed = true;
    path.anchors = {{{0.0, 0.0}, std::nullopt, std::nullopt},
                    {{size, 0.0},
                     std::nullopt,
                     curved ? std::optional{document::Vec2d{size, 5.0}} : std::nullopt},
                    {{size, size}, std::nullopt, std::nullopt},
                    {{0.0, size}, std::nullopt, std::nullopt}};
    return path;
}

struct Fixture {
    document::NewProject initial = document::makeNewProject(
        "Masks", "Main", core::RationalTime::fromInteger(4),
        *document::CompositionFormat::create(64, 64, core::PixelAspectRatio::square(),
                                             document::FrameRate::framesPerSecond24()));
    document::Document document{std::move(initial.project)};
    commands::CommandStack commands{document};
    document::CompositionId composition = initial.initialCompositionId;

    template <typename Operation, typename... Arguments>
    commands::CommandResult run(Arguments&&... arguments) {
        commands::Transaction transaction("Masks", document.snapshot().revision());
        transaction.emplace<Operation>(std::forward<Arguments>(arguments)...);
        auto result = commands.execute(std::move(transaction));
        if (!result.succeeded())
            throw std::runtime_error("fixture command refused");
        return result;
    }
    Fixture() {
        const auto source =
            *run<commands::AddSolidLayer>(composition, "Matte", core::Color4d{1, 1, 1, 1})
                 .outputId<document::LayerId>(commands::kAddSolidLayerLayerOutput);
        const auto target =
            *run<commands::AddSolidLayer>(composition, "Target", core::Color4d{1, 0, 0, 1})
                 .outputId<document::LayerId>(commands::kAddSolidLayerLayerOutput);
        const auto first = *run<commands::AddLayerMask>(composition, target, rectangle(32, true))
                                .outputId<document::LayerMaskId>("mask");
        run<commands::AddLayerMask>(composition, target, rectangle(16, false),
                                    document::LayerMaskMode::Subtract);
        run<commands::SetLayerMaskInverted>(composition, target, first, true);
        run<commands::AnimateLayerMaskPath>(composition, target, first, core::RationalTime{});
        run<commands::SetLayerMaskPath>(composition, target, first,
                                        core::RationalTime::fromInteger(1), rectangle(40, true));
        run<commands::SetTrackMatte>(
            composition, target,
            document::TrackMatte{source, document::TrackMatteChannel::Luma, true});
    }
};

std::string encode(const document::Document& document) {
    const auto snapshot = document.snapshot();
    const auto settings = document::makeBloomNeutralColorSettingsV1({});
    std::array<char, 64> payloadScratch{};
    std::array<std::size_t, 64> sortScratch{};
    const project::CanonicalDocumentV1 input{&snapshot, &settings, payloadScratch, sortScratch};
    const auto size = project::canonicalDocumentSize(input);
    if (!size)
        throw std::runtime_error("canonical size");
    std::string text(*size.value(), '\0');
    if (!project::encodeCanonicalDocument(input, text))
        throw std::runtime_error("canonical encode");
    return text;
}

project::DocumentDecodeResult decodeText(const std::string& text) {
    auto operation = memory();
    auto parsed = project::parseStrictJsonDom(std::as_bytes(std::span(text)), {}, operation);
    if (!parsed)
        throw std::runtime_error("mask parse");
    return project::decodeDocumentEnvelope(parsed.document()->root());
}

void replaceOnce(std::string& text, const std::string_view from, const std::string_view to) {
    const auto at = text.find(from);
    if (at == std::string::npos)
        throw std::logic_error("fixture anchor missing");
    text.replace(at, from.size(), to);
}

void roundTrip() {
    Fixture fixture;
    const auto snapshot = fixture.document.snapshot();
    expect(snapshot.project().validate().ok(), "the masked project validates");
    const auto text = encode(fixture.document);
    expect(text.find("\"masks\"") != std::string::npos &&
               text.find("\"trackMatte\"") != std::string::npos &&
               text.find("\"kind\": \"path\"") != std::string::npos &&
               text.find("\"layerMask\"") != std::string::npos,
           "masks, the matte, the Path curve and the layerMask namespace are written");
    const auto decoded = decodeText(text);
    expect(static_cast<bool>(decoded), "the 1.24 document decodes");

    const auto settings = document::makeBloomNeutralColorSettingsV1({});
    auto archive = project::buildVerifiedSaveArchive(
        {}, {.snapshot = &snapshot, .colorSettings = &settings}, {}, memory());
    expect(static_cast<bool>(archive), "the masked archive saves and verifies");
    if (!archive)
        return;
    auto result = project::openProjectArchive(archive.archive()->bytes(), {}, memory());
    expect(result.outcome() == project::OpenArchiveOutcome::Opened, "the masked archive opens");
    if (result.outcome() != project::OpenArchiveOutcome::Opened)
        return;
    auto opened = std::move(result).takeOpened();
    const auto reopened = opened.document->snapshot();
    const auto& before = *snapshot.project().findComposition(fixture.composition);
    const auto& after = *reopened.project().findComposition(fixture.composition);
    expect(std::ranges::equal(before.graph().layerOutputs(), after.graph().layerOutputs()),
           "every layer boundary, its masks and its matte equal themselves after save and open");
    expect(
        std::ranges::equal(before.animationCurves().records(), after.animationCurves().records()) &&
            std::ranges::equal(before.parameters().records(), after.parameters().records()),
        "mask parameters and the Path curve equal themselves after save and open");
    expect(reopened.project().validate().ok(), "the reopened project validates");
}

void olderDocumentsMigrateAndRefuseNewMembers() {
    document::Document plain{
        document::makeNewProject(
            "Plain", "Main", core::RationalTime::fromInteger(2),
            *document::CompositionFormat::create(8, 8, core::PixelAspectRatio::square(),
                                                 document::FrameRate::framesPerSecond24()))
            .project};
    auto text = encode(plain);
    replaceOnce(text, "\"minor\": 24", "\"minor\": 23");
    expect(static_cast<bool>(decodeText(text)), "a 1.23 document without masks decodes");
    auto operation = memory();
    auto parsed = project::parseStrictJsonDom(std::as_bytes(std::span(text)), {}, operation);
    if (parsed) {
        const auto migrated =
            project::migrateDocumentDom(parsed.document()->root(), {1, 23}, {1, 24},
                                        project::kProductionDocumentMigrationSteps, {}, operation);
        expect(migrated.outcome() == project::MigrationOutcome::Migrated &&
                   migrated.stepsApplied() == 1 &&
                   static_cast<bool>(project::decodeDocumentEnvelope(*migrated.migratedRoot())),
               "the 1.23 -> 1.24 step only advances the version");
    }

    Fixture fixture;
    auto masked = encode(fixture.document);
    replaceOnce(masked, "\"minor\": 24", "\"minor\": 23");
    expect(!static_cast<bool>(decodeText(masked)), "a 1.23 document carrying masks is refused");
}

} // namespace

int main() {
    try {
        roundTrip();
        olderDocumentsMigrateAndRefuseNewMembers();
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    if (failures == 0)
        std::cout << "layer mask persistence PASS\n";
    return failures == 0 ? 0 : 1;
}
