// Document 1.29 (ADR 0033): the export queue persists in run order, round-trips byte for byte,
// and the 1.28 -> 1.29 migration gives older projects an empty queue.
#include <bloom/document/document.hpp>
#include <bloom/document/export_queue.hpp>
#include <bloom/document/new_project.hpp>
#include <bloom/project/canonical_document.hpp>
#include <bloom/project/document_decode.hpp>
#include <bloom/project/document_migration.hpp>
#include <bloom/project/document_reconstruct.hpp>
#include <bloom/project/strict_json_dom.hpp>

#include <algorithm>
#include <array>
#include <iostream>
#include <optional>
#include <regex>
#include <span>
#include <stdexcept>
#include <string>
#include <tuple>

namespace {
using namespace bloom;
using namespace bloom::project;
using namespace bloom::document;
int failures = 0;
void expect(const bool condition, const char* message) {
    if (!condition) {
        ++failures;
        std::cerr << "FAIL: " << message << '\n';
    }
}
ProjectIoOperationMemory memory() {
    auto coordinator = ProjectIoMemoryCoordinator::create();
    if (!coordinator)
        throw std::runtime_error("memory coordinator");
    auto operation = coordinator->createOperation();
    if (!operation)
        throw std::runtime_error("memory operation");
    return std::move(*operation);
}

ExportJob job(const std::uint64_t id, const CompositionId composition) {
    ExportJob value;
    value.id = ExportJobId::fromRaw(id);
    value.composition = composition;
    value.name = id == 1 ? "" : "Review \"cut\" {date}";
    value.render.range = id == 1 ? ExportRange{ExportRangeKind::WorkArea, 0, 0}
                                 : ExportRange{ExportRangeKind::Custom, 3, 17};
    value.enabled = id == 1;
    if (id != 1)
        value.labelColor = std::array<std::uint8_t, 3>{255, 0, 64};
    ExportOutput movie;
    movie.id = ExportOutputId::fromRaw(id * 10);
    movie.presetId = "builtin:h264-mp4";
    movie.settings.codecProfile = "high";
    movie.settings.videoBitrateKbps = 24000;
    movie.destination.path = "Exports/{name}.mp4";
    movie.importIntoAssets = id != 1;
    value.outputs.push_back(movie);
    ExportOutput frames;
    frames.id = ExportOutputId::fromRaw(id * 10 + 1);
    frames.presetId = "builtin:vfx-handoff-exr";
    frames.settings.format = ExportFormat::OpenExr;
    frames.settings.compression = "piz";
    frames.settings.outputColorSpaceId = "ACES2065-1";
    frames.settings.bakeLook = false;
    frames.settings.includeAudio = false;
    frames.settings.encoder = ExportEncoderPreference::Software;
    frames.settings.naming = {1001, 5, "{job}_<base>.{frame}.<ext>"};
    frames.destination = {"external", "file:///renders/plate.exr", ExportOverwritePolicy::Replace};
    frames.enabled = false;
    value.outputs.push_back(frames);
    return value;
}

Document makeDocument(const bool withQueue) {
    auto seed = makeNewProject("Export queue", "Main", core::RationalTime::fromInteger(2));
    if (withQueue)
        seed.project.setExportQueue(
            {{job(2, seed.initialCompositionId), job(1, seed.initialCompositionId)}});
    return Document{std::move(seed.project)};
}

std::string encode(const Document& document) {
    const auto snapshot = document.snapshot();
    const auto settings = makeBloomNeutralColorSettingsV1({});
    std::array<char, 64> payloadScratch{};
    std::array<std::size_t, 64> sortScratch{};
    const CanonicalDocumentV1 input{&snapshot, &settings, payloadScratch, sortScratch};
    const auto size = canonicalDocumentSize(input);
    if (!size)
        throw std::runtime_error("canonical size");
    std::string text(*size.value(), '\0');
    if (!encodeCanonicalDocument(input, text))
        throw std::runtime_error("canonical encode");
    return text;
}

std::string replaceOnce(std::string text, const std::string_view from, const std::string_view to) {
    const auto at = text.find(from);
    if (at == std::string::npos)
        throw std::logic_error("fixture anchor missing");
    text.replace(at, from.size(), to);
    return text;
}

void testRoundTrip() {
    const auto document = makeDocument(true);
    const auto text = encode(document);
    expect(text.find("\"exportQueue\"") != std::string::npos, "the queue is written");
    expect(text.find("\"exportJob\": \"2\"") != std::string::npos &&
               text.find("\"exportOutput\": \"21\"") != std::string::npos,
           "the allocator high water covers every export id");
    expect(text.find("\"id\": \"2\"") < text.find("\"id\": \"1\",\n          \"composition\""),
           "jobs are written in run order, not id order");
    auto operation = memory();
    auto parsed = parseStrictJsonDom(std::as_bytes(std::span(text)), {}, operation);
    expect(static_cast<bool>(parsed), "the 1.29 document parses");
    if (!parsed)
        return;
    auto decoded = decodeDocumentEnvelope(parsed.document()->root());
    expect(static_cast<bool>(decoded), "the 1.29 document decodes");
    if (!decoded)
        return;
    expect(decoded.value()->exportQueue == document.snapshot().project().exportQueue(),
           "every job and output member decodes exactly");
    auto envelope = *decoded.value();
    auto rebuilt = reconstructDocument(std::move(envelope));
    expect(static_cast<bool>(rebuilt), "the queue reconstructs into a valid project");
    if (!rebuilt)
        return;
    expect(encode(*rebuilt.value()->document) == text, "a full queue round-trips byte for byte");
}

void testMigration() {
    const auto current = encode(makeDocument(false));
    auto older = replaceOnce(current, "\"minor\": 32", "\"minor\": 28");
    older = std::regex_replace(
        older, std::regex(R"(,\s*"exportQueue"\s*:\s*\{\s*"jobs"\s*:\s*\[\]\s*\})"), "");
    older = std::regex_replace(
        older, std::regex(R"(,\s*"exportJob"\s*:\s*"0",\s*"exportOutput"\s*:\s*"0")"), "");
    expect(older.find("exportQueue") == std::string::npos, "the 1.28 fixture has no queue");
    auto operation = memory();
    auto parsed = parseStrictJsonDom(std::as_bytes(std::span(older)), {}, operation);
    expect(static_cast<bool>(parsed), "the 1.28 fixture parses");
    if (!parsed)
        return;
    const auto legacy = decodeDocumentEnvelope(parsed.document()->root());
    expect(static_cast<bool>(legacy) && legacy.value()->exportQueue.jobs.empty(),
           "a 1.28 document decodes with an empty queue");
    // To the current 1.31: the export-queue step, the easing step, then the version-only node group
    // colour step.
    const auto migrated = migrateDocumentDom(parsed.document()->root(), {1, 28}, {1, 32},
                                             kProductionDocumentMigrationSteps, {}, operation);
    expect(migrated.outcome() == MigrationOutcome::Migrated && migrated.stepsApplied() == 4,
           "1.28 to 1.31 runs the export-queue, easing and node group colour steps");
    if (migrated.outcome() != MigrationOutcome::Migrated)
        return;
    const auto upgraded = decodeDocumentEnvelope(*migrated.migratedRoot());
    auto reparsed = parseStrictJsonDom(std::as_bytes(std::span(current)), {}, operation);
    const auto saved = reparsed ? decodeDocumentEnvelope(reparsed.document()->root())
                                : decodeDocumentEnvelope(*migrated.migratedRoot());
    expect(upgraded && saved && *upgraded.value() == *saved.value(),
           "a migrated 1.28 project equals the same project saved at 1.31");

    // The already-migrated guard: a 1.28-labelled file that already has the queue is refused.
    const auto mislabelled = replaceOnce(current, "\"minor\": 32", "\"minor\": 28");
    auto bad = parseStrictJsonDom(std::as_bytes(std::span(mislabelled)), {}, operation);
    if (bad) {
        const auto refused = migrateDocumentDom(bad.document()->root(), {1, 28}, {1, 32},
                                                kProductionDocumentMigrationSteps, {}, operation);
        expect(refused.outcome() != MigrationOutcome::Migrated,
               "a 1.28 file that already carries the queue is not migrated twice");
    }
}

std::optional<ExportQueue> decodeQueue(const std::string& text) {
    auto operation = memory();
    auto parsed = parseStrictJsonDom(std::as_bytes(std::span(text)), {}, operation);
    if (!parsed)
        return std::nullopt;
    auto decoded = decodeDocumentEnvelope(parsed.document()->root());
    if (!decoded)
        return std::nullopt;
    return decoded.value()->exportQueue;
}

// `useCachedFrames` joined 1.29 after projects were saved at 1.29: it is optional (absent means
// true) and written only when false, so those files open and re-save unchanged.
void testUseCachedFrames() {
    const auto document = makeDocument(true);
    const auto text = encode(document);
    expect(text.find("useCachedFrames") == std::string::npos,
           "the default (true) is not written, so earlier 1.29 files keep their bytes");
    const auto earlier = decodeQueue(text);
    expect(earlier && std::ranges::all_of(earlier->jobs,
                                          [](const auto& job) {
                                              return std::ranges::all_of(
                                                  job.outputs, [](const auto& output) {
                                                      return output.settings.useCachedFrames;
                                                  });
                                          }),
           "a 1.29 document without the member opens with useCachedFrames on");

    auto seed = makeNewProject("Export queue", "Main", core::RationalTime::fromInteger(2));
    auto off = job(1, seed.initialCompositionId);
    off.outputs[0].settings.useCachedFrames = false;
    seed.project.setExportQueue({{off}});
    const Document withOff{std::move(seed.project)};
    const auto offText = encode(withOff);
    expect(offText.find("\"useCachedFrames\": false") != std::string::npos, "false is written");
    const auto decodedOff = decodeQueue(offText);
    expect(decodedOff && !decodedOff->jobs[0].outputs[0].settings.useCachedFrames &&
               decodedOff->jobs[0].outputs[1].settings.useCachedFrames,
           "false decodes per output");
    const auto explicitTrue =
        replaceOnce(offText, "\"useCachedFrames\": false", "\"useCachedFrames\": true");
    const auto decodedTrue = decodeQueue(explicitTrue);
    expect(decodedTrue && decodedTrue->jobs[0].outputs[0].settings.useCachedFrames,
           "an explicit true decodes");
    expect(
        !decodeQueue(replaceOnce(offText, "\"useCachedFrames\": false", "\"useCachedFrames\": 0")),
        "a non-boolean useCachedFrames is refused");
}

void testRejects() {
    const auto text = encode(makeDocument(true));
    for (const auto& [from, to, message] :
         std::array<std::tuple<std::string_view, std::string_view, const char*>, 4>{{
             {"\"format\": \"h264-mp4\"", "\"format\": \"gif\"", "unknown format tokens"},
             {"\"overwrite\": \"ask\"", "\"overwrite\": \"maybe\"", "unknown overwrite policy"},
             {"\"kind\": \"custom\",\n            \"first\": \"3\",\n            \"last\": \"17\"",
              "\"kind\": \"custom\",\n            \"first\": \"3\"", "a custom range without last"},
             {"\"importIntoAssets\": true", "\"importIntoAssets\": 1", "a non-boolean flag"},
         }}) {
        const auto broken = replaceOnce(text, from, to);
        auto operation = memory();
        auto parsed = parseStrictJsonDom(std::as_bytes(std::span(broken)), {}, operation);
        expect(!parsed || !decodeDocumentEnvelope(parsed.document()->root()), message);
    }
    // A job whose composition does not exist decodes but never reconstructs.
    const auto orphan = replaceOnce(text, "\"composition\": \"1\",\n          \"name\": \"Review",
                                    "\"composition\": \"9\",\n          \"name\": \"Review");
    auto operation = memory();
    auto parsed = parseStrictJsonDom(std::as_bytes(std::span(orphan)), {}, operation);
    expect(static_cast<bool>(parsed), "the orphan fixture parses");
    if (!parsed)
        return;
    auto decoded = decodeDocumentEnvelope(parsed.document()->root());
    expect(static_cast<bool>(decoded), "the orphan fixture decodes");
    if (decoded) {
        auto envelope = *decoded.value();
        expect(!reconstructDocument(std::move(envelope)),
               "a job naming a missing composition is refused at reconstruction");
    }
}

} // namespace

int main() {
    try {
        testRoundTrip();
        testMigration();
        testRejects();
        testUseCachedFrames();
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
    return failures == 0 ? 0 : 1;
}
