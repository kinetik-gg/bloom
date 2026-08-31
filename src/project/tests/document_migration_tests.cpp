#include <bloom/project/document_migration.hpp>

#include <bloom/document/schema_version.hpp>
#include <bloom/project/canonical_decimal.hpp>
#include <bloom/project/canonical_json_writer.hpp>
#include <bloom/project/project_io_memory.hpp>
#include <bloom/project/strict_json_dom.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <memory_resource>
#include <optional>
#include <source_location>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

// Synthetic proof suite for migrateDocumentDom() (document_migration.hpp). This is the ONLY
// exerciser of the version pair/step table used below: no production schema is {1,1} or {1,2},
// and the "current" version each test migrates to is passed explicitly to migrateDocumentDom()
// rather than read from kCanonicalDocumentSchemaVersionV1 -- exactly the injection seam the
// framework's own header comment describes. save_archive.cpp's runReopenChain() is the one
// production call site, and it always passes the real {1,0} current version and the (currently
// empty) production step table; see open_archive_tests.cpp / save_archive_tests.cpp for the
// existing identity-path coverage of that production wiring with real archives.
//
// The two synthetic steps below migrate a tiny fictional document shape
// `{"schemaVersion":{...},"legacyName"|"name":<string>,"value":<number>}` forward one minor at a
// time: {1,0} -> {1,1} renames "legacyName" to "name" and normalizes "value"'s number spelling
// (the fixture-normalization clause in docs/architecture/project-format.md, "Decimal Strings And
// JSON Integers", made concrete -- "a supported older schema may normalize [non-canonical
// spellings] only through an explicit migration fixture"); {1,1} -> {1,2} renames "name" to
// "label". Chaining both from {1,0} proves real sequential, two-step migration.

namespace {

using bloom::document::SchemaVersion;
using bloom::project::CanonicalJsonWriter;
using bloom::project::JsonValue;
using bloom::project::JsonValueKind;
using bloom::project::migrateDocumentDom;
using bloom::project::MigrationError;
using bloom::project::MigrationOutcome;
using bloom::project::MigrationResult;
using bloom::project::MigrationStepDescriptor;
using bloom::project::MigrationStepOutcome;
using bloom::project::parseKnownFloat64;
using bloom::project::parseStrictJsonDom;
using bloom::project::ProjectIoMemoryCoordinator;
using bloom::project::ProjectIoOperationMemory;
using bloom::project::StrictJsonDomError;
using bloom::project::StrictJsonDomLimits;
using bloom::project::StrictJsonDomResult;

class Expectations final {
  public:
    void expect(const bool condition, const std::string_view message,
                const std::source_location location = std::source_location::current()) {
        if (condition) {
            return;
        }
        ++failures_;
        std::cerr << location.file_name() << ':' << location.line() << ": " << message << '\n';
    }

    [[nodiscard]] int failures() const noexcept { return failures_; }

  private:
    int failures_ = 0;
};

constexpr std::uint64_t kGenerousOperationBudget = 8ULL << 20U; // 8 MiB: ample for every fixture.

[[nodiscard]] ProjectIoOperationMemory makeOperation(const std::uint64_t limitBytes) {
    auto coordinator = ProjectIoMemoryCoordinator::create(limitBytes);
    if (!coordinator.has_value()) {
        std::abort();
    }
    auto operation = coordinator->createOperation(limitBytes, limitBytes);
    if (!operation.has_value()) {
        std::abort();
    }
    return std::move(*operation);
}

[[nodiscard]] ProjectIoOperationMemory makeOperation() {
    return makeOperation(kGenerousOperationBudget);
}

[[nodiscard]] std::span<const std::byte> asBytes(const std::string_view text) noexcept {
    return {reinterpret_cast<const std::byte*>(text.data()), text.size()};
}

constexpr SchemaVersion kVersion0{1, 0};
constexpr SchemaVersion kVersion1{1, 1};
constexpr SchemaVersion kVersion2{1, 2};
// Never a registered step's sourceVersion or targetVersion in any test below -- used only to
// exercise the "detected version isn't even in this table" branch.
constexpr SchemaVersion kNeverRegisteredVersion{1, 5};

constexpr std::string_view kInputV0Json =
    R"({"schemaVersion":{"major":1,"minor":0},"legacyName":"widget","value":1.50})";
constexpr std::string_view kInputV0MissingLegacyNameJson =
    R"({"schemaVersion":{"major":1,"minor":0},"value":1.50})";

void requireWriterSuccess(const bloom::project::CanonicalJsonWriterResult result) noexcept {
    if (!result) {
        std::abort();
    }
}

// Shared emission for both synthetic steps: `{"schemaVersion":{"major":1,"minor":<minor>},
// "<memberName>":"<stringValue>","value":<numberValue>}`. Called once in counting mode to size
// `output`, then again in write mode -- the same measure-then-write pattern
// canonical_document.cpp/canonical_manifest.cpp use for the real document/manifest encoders.
void emitSyntheticDocument(CanonicalJsonWriter& writer, const std::uint32_t minor,
                           const std::string_view memberName, const std::string_view stringValue,
                           const double numberValue) noexcept {
    requireWriterSuccess(writer.beginObject());
    requireWriterSuccess(writer.memberName("schemaVersion"));
    requireWriterSuccess(writer.beginObject());
    requireWriterSuccess(writer.memberName("major"));
    requireWriterSuccess(writer.integerValue(std::uint32_t{1}));
    requireWriterSuccess(writer.memberName("minor"));
    requireWriterSuccess(writer.integerValue(minor));
    requireWriterSuccess(writer.endObject());
    requireWriterSuccess(writer.memberName(memberName));
    requireWriterSuccess(writer.stringValue(stringValue));
    requireWriterSuccess(writer.memberName("value"));
    requireWriterSuccess(writer.float64Value(numberValue));
    requireWriterSuccess(writer.endObject());
    requireWriterSuccess(writer.finish());
}

void writeSyntheticDocument(std::pmr::vector<char>& output, const std::uint32_t minor,
                            const std::string_view memberName, const std::string_view stringValue,
                            const double numberValue) {
    auto counter = CanonicalJsonWriter::counting();
    emitSyntheticDocument(counter, minor, memberName, stringValue, numberValue);
    output.resize(counter.bytesRequired());
    CanonicalJsonWriter writer(output, {});
    emitSyntheticDocument(writer, minor, memberName, stringValue, numberValue);
    if (writer.bytesWritten() != output.size()) {
        std::abort();
    }
}

// A real DOM transform: renames `sourceMember` to `targetMember`, and -- only for the {1,0} ->
// {1,1} step, where sourceMember is "legacyName" -- normalizes "value"'s number spelling via
// parseKnownFloat64()/CanonicalJsonWriter::float64Value() (parseKnownFloat64 accepts any finite
// RFC 8259 decimal spelling; float64Value() independently re-derives Bloom's canonical Float64
// token from the resulting double -- see docs/architecture/project-format.md, "Float64": "The
// reader may accept another finite RFC 8259 decimal spelling and normalizes it on save". This is
// the fixture-normalization clause -- "Decimal Strings And JSON Integers": "A supported older
// schema may normalize [non-canonical spellings] only through an explicit migration fixture" --
// made concrete). Reports MigrationStepError::TransformFailed with a path into `root` for any
// shape this fictional schema doesn't recognize.
[[nodiscard]] MigrationStepOutcome
renameTransform(const std::string_view sourceMember, const std::string_view targetMember,
                const std::uint32_t targetMinor, const JsonValue& root,
                std::pmr::memory_resource* /*resource*/, std::pmr::vector<char>& output) {
    if (root.kind() != JsonValueKind::Object) {
        return MigrationStepOutcome::failure("/");
    }
    const auto* sourceValue = root.findMember(sourceMember);
    if (sourceValue == nullptr || sourceValue->kind() != JsonValueKind::String) {
        return MigrationStepOutcome::failure(sourceMember == "legacyName" ? "/legacyName"
                                                                          : "/name");
    }
    // asString()/asNumberToken() are documented to return a value whenever kind() already
    // matches, but that invariant is not visible to static analysis -- every use re-checks
    // has_value() immediately before dereferencing rather than trusting the prior kind() check
    // alone (mirrors document_decode.cpp's decodeStringMember()/decodeUInt32Member() idiom).
    const auto sourceText = sourceValue->asString();
    if (!sourceText.has_value()) {
        return MigrationStepOutcome::failure(sourceMember == "legacyName" ? "/legacyName"
                                                                          : "/name");
    }
    const auto* value = root.findMember("value");
    if (value == nullptr || value->kind() != JsonValueKind::Number) {
        return MigrationStepOutcome::failure("/value");
    }
    const auto numberToken = value->asNumberToken();
    if (!numberToken.has_value()) {
        return MigrationStepOutcome::failure("/value");
    }
    const auto parsedNumber = parseKnownFloat64(*numberToken);
    if (!parsedNumber) {
        return MigrationStepOutcome::failure("/value");
    }
    const auto* numberValue = parsedNumber.value();
    if (numberValue == nullptr) {
        return MigrationStepOutcome::failure("/value");
    }
    writeSyntheticDocument(output, targetMinor, targetMember, *sourceText, *numberValue);
    return MigrationStepOutcome::success();
}

[[nodiscard]] MigrationStepOutcome stepRenameAndNormalize(const JsonValue& root,
                                                          std::pmr::memory_resource* resource,
                                                          std::pmr::vector<char>& output) {
    return renameTransform("legacyName", "name", std::uint32_t{1}, root, resource, output);
}

[[nodiscard]] MigrationStepOutcome stepRenameLabel(const JsonValue& root,
                                                   std::pmr::memory_resource* resource,
                                                   std::pmr::vector<char>& output) {
    return renameTransform("name", "label", std::uint32_t{2}, root, resource, output);
}

// Deliberately buggy: reports its own success while writing bytes that are not valid JSON at all,
// to prove migrateDocumentDom()'s mandatory strict re-parse catches a step's own bug rather than
// trusting a step's self-reported outcome.
[[nodiscard]] MigrationStepOutcome stepEmitsInvalidJson(const JsonValue& /*root*/,
                                                        std::pmr::memory_resource* /*resource*/,
                                                        std::pmr::vector<char>& output) {
    static constexpr std::string_view kBrokenJson = "{ this is not valid json";
    output.assign(kBrokenJson.begin(), kBrokenJson.end());
    return MigrationStepOutcome::success();
}

constexpr MigrationStepDescriptor kStepV0ToV1{kVersion0, kVersion1, &stepRenameAndNormalize};
constexpr MigrationStepDescriptor kStepV1ToV2{kVersion1, kVersion2, &stepRenameLabel};
// A version pair no other test uses, so it can never accidentally chain into the fixtures above.
constexpr SchemaVersion kBuggyStepSource{9, 0};
constexpr SchemaVersion kBuggyStepTarget{9, 1};
constexpr MigrationStepDescriptor kStepEmitsInvalidJson{kBuggyStepSource, kBuggyStepTarget,
                                                        &stepEmitsInvalidJson};

[[nodiscard]] StrictJsonDomResult parseFixture(const std::string_view json,
                                               const ProjectIoOperationMemory& operation) {
    return parseStrictJsonDom(asBytes(json), StrictJsonDomLimits{}, operation);
}

// Structural, order-sensitive JSON equality: proves determinism at the value level (member order,
// exact number token spelling, exact string content) without needing raw bytes out of
// MigrationResult, which intentionally exposes only the re-parsed DOM (see this module's header
// comment on why re-parsing -- not raw byte return -- is the contract between steps).
[[nodiscard]] bool jsonEqual(const JsonValue& left, const JsonValue& right) noexcept {
    if (left.kind() != right.kind()) {
        return false;
    }
    switch (left.kind()) {
    case JsonValueKind::Null:
        return true;
    case JsonValueKind::Boolean:
        return left.asBoolean() == right.asBoolean();
    case JsonValueKind::Number:
        return left.asNumberToken() == right.asNumberToken();
    case JsonValueKind::String:
        return left.asString() == right.asString();
    case JsonValueKind::Array: {
        const auto leftElements = left.arrayElements();
        const auto rightElements = right.arrayElements();
        if (leftElements.size() != rightElements.size()) {
            return false;
        }
        for (std::size_t index = 0; index < leftElements.size(); ++index) {
            if (!jsonEqual(leftElements[index], rightElements[index])) {
                return false;
            }
        }
        return true;
    }
    case JsonValueKind::Object: {
        const auto leftMembers = left.objectMembers();
        const auto rightMembers = right.objectMembers();
        if (leftMembers.size() != rightMembers.size()) {
            return false;
        }
        for (std::size_t index = 0; index < leftMembers.size(); ++index) {
            if (leftMembers[index].key() != rightMembers[index].key()) {
                return false;
            }
            if (!jsonEqual(leftMembers[index].value(), rightMembers[index].value())) {
                return false;
            }
        }
        return true;
    }
    }
    return false;
}

// ---------------------------------------------------------------------------------------------
// Identity: detectedVersion == currentVersion reports zero steps applied and owns no DOM, for any
// step table (including one that would otherwise apply) -- migrateDocumentDom() never even looks
// at `steps` in this case. This is the exact case save_archive.cpp's runReopenChain() always hits
// in production today (see this file's own top comment).
// ---------------------------------------------------------------------------------------------

void testIdentity(Expectations& expectations) {
    const std::array<MigrationStepDescriptor, 2> steps{kStepV0ToV1, kStepV1ToV2};
    auto operation = makeOperation();
    auto fixture = parseFixture(kInputV0Json, operation);
    expectations.expect(static_cast<bool>(fixture), "identity: fixture parses");
    if (!fixture) {
        return;
    }

    auto result = migrateDocumentDom(fixture.document()->root(), kVersion0, kVersion0, steps,
                                     StrictJsonDomLimits{}, operation);
    expectations.expect(result.outcome() == MigrationOutcome::Identity,
                        "identity: detectedVersion == currentVersion reports Identity");
    expectations.expect(result.stepsApplied() == 0, "identity: zero steps applied");
    expectations.expect(result.migratedRoot() == nullptr,
                        "identity: owns no DOM (caller keeps using its own)");
}

// ---------------------------------------------------------------------------------------------
// Single-step transform correctness (golden values): {1,0} -> {1,1} renames "legacyName" to
// "name" and normalizes "value" from the non-canonical "1.50" spelling to the canonical "1.5"
// token.
// ---------------------------------------------------------------------------------------------

void testSingleStepGoldenValues(Expectations& expectations) {
    const std::array<MigrationStepDescriptor, 1> steps{kStepV0ToV1};
    auto operation = makeOperation();
    auto fixture = parseFixture(kInputV0Json, operation);
    expectations.expect(static_cast<bool>(fixture), "single step: fixture parses");
    if (!fixture) {
        return;
    }

    auto result = migrateDocumentDom(fixture.document()->root(), kVersion0, kVersion1, steps,
                                     StrictJsonDomLimits{}, operation);
    expectations.expect(result.outcome() == MigrationOutcome::Migrated,
                        "single step: one registered step migrates");
    expectations.expect(result.stepsApplied() == 1, "single step: exactly one step applied");
    const auto* root = result.migratedRoot();
    expectations.expect(root != nullptr, "single step: migrated DOM is present");
    if (root == nullptr) {
        return;
    }
    const auto* schemaVersion = root->findMember("schemaVersion");
    expectations.expect(schemaVersion != nullptr && schemaVersion->findMember("minor") != nullptr &&
                            schemaVersion->findMember("minor")->asNumberToken() == "1",
                        "single step: schemaVersion.minor bumped to 1");
    expectations.expect(root->findMember("legacyName") == nullptr,
                        "single step: legacyName no longer present");
    const auto* name = root->findMember("name");
    expectations.expect(name != nullptr && name->asString() == "widget",
                        "single step: legacyName's value moved to name verbatim");
    const auto* value = root->findMember("value");
    expectations.expect(value != nullptr && value->asNumberToken() == "1.5",
                        "single step: value renormalized from non-canonical \"1.50\" to "
                        "canonical \"1.5\" (golden token)");
}

// ---------------------------------------------------------------------------------------------
// Two-step sequential chaining: {1,0} -> {1,1} -> {1,2}, registered in reverse array order to
// prove the chain is driven by version linkage (sourceVersion lookup), not by registration
// position.
// ---------------------------------------------------------------------------------------------

void testTwoStepSequencing(Expectations& expectations) {
    const std::array<MigrationStepDescriptor, 2> reversedSteps{kStepV1ToV2, kStepV0ToV1};
    auto operation = makeOperation();
    auto fixture = parseFixture(kInputV0Json, operation);
    expectations.expect(static_cast<bool>(fixture), "two step: fixture parses");
    if (!fixture) {
        return;
    }

    auto result = migrateDocumentDom(fixture.document()->root(), kVersion0, kVersion2,
                                     reversedSteps, StrictJsonDomLimits{}, operation);
    expectations.expect(result.outcome() == MigrationOutcome::Migrated,
                        "two step: chain from {1,0} to {1,2} migrates");
    expectations.expect(result.stepsApplied() == 2, "two step: both steps applied in sequence");
    const auto* root = result.migratedRoot();
    expectations.expect(root != nullptr, "two step: migrated DOM is present");
    if (root == nullptr) {
        return;
    }
    const auto* schemaVersion = root->findMember("schemaVersion");
    expectations.expect(schemaVersion != nullptr &&
                            schemaVersion->findMember("minor")->asNumberToken() == "2",
                        "two step: schemaVersion.minor bumped to 2 by the final step");
    expectations.expect(root->findMember("legacyName") == nullptr &&
                            root->findMember("name") == nullptr,
                        "two step: both intermediate member names are gone");
    const auto* label = root->findMember("label");
    expectations.expect(label != nullptr && label->asString() == "widget",
                        "two step: final member carries the original string through both steps");
    const auto* value = root->findMember("value");
    expectations.expect(value != nullptr && value->asNumberToken() == "1.5",
                        "two step: value stays canonically normalized through both steps");
}

// ---------------------------------------------------------------------------------------------
// Determinism: two independent runs of the same two-step chain over the same input produce
// structurally (and therefore byte-) identical migrated DOMs.
// ---------------------------------------------------------------------------------------------

void testDeterminism(Expectations& expectations) {
    const std::array<MigrationStepDescriptor, 2> steps{kStepV0ToV1, kStepV1ToV2};
    auto firstOperation = makeOperation();
    auto secondOperation = makeOperation();
    auto firstFixture = parseFixture(kInputV0Json, firstOperation);
    auto secondFixture = parseFixture(kInputV0Json, secondOperation);
    expectations.expect(static_cast<bool>(firstFixture) && static_cast<bool>(secondFixture),
                        "determinism: both independent fixtures parse");
    if (!firstFixture || !secondFixture) {
        return;
    }

    auto firstResult = migrateDocumentDom(firstFixture.document()->root(), kVersion0, kVersion2,
                                          steps, StrictJsonDomLimits{}, firstOperation);
    auto secondResult = migrateDocumentDom(secondFixture.document()->root(), kVersion0, kVersion2,
                                           steps, StrictJsonDomLimits{}, secondOperation);
    expectations.expect(firstResult.outcome() == MigrationOutcome::Migrated &&
                            secondResult.outcome() == MigrationOutcome::Migrated,
                        "determinism: both independent runs migrate");
    expectations.expect(firstResult.stepsApplied() == secondResult.stepsApplied(),
                        "determinism: both runs apply the same number of steps");
    const auto* firstRoot = firstResult.migratedRoot();
    const auto* secondRoot = secondResult.migratedRoot();
    expectations.expect(firstRoot != nullptr && secondRoot != nullptr,
                        "determinism: both migrated DOMs are present");
    if (firstRoot == nullptr || secondRoot == nullptr) {
        return;
    }
    expectations.expect(jsonEqual(*firstRoot, *secondRoot),
                        "determinism: two runs over the same input are structurally (and "
                        "therefore byte-) identical");
}

// ---------------------------------------------------------------------------------------------
// Typed failures: unknown source version, chain gap, step-failure-with-path, and the framework's
// own strict re-parse rejection of a buggy step's output.
// ---------------------------------------------------------------------------------------------

void testUnknownSourceVersion(Expectations& expectations) {
    const std::array<MigrationStepDescriptor, 1> steps{kStepV0ToV1};
    auto operation = makeOperation();
    auto fixture = parseFixture(kInputV0Json, operation);
    expectations.expect(static_cast<bool>(fixture), "unknown source: fixture parses");
    if (!fixture) {
        return;
    }

    auto result = migrateDocumentDom(fixture.document()->root(), kNeverRegisteredVersion, kVersion2,
                                     steps, StrictJsonDomLimits{}, operation);
    expectations.expect(result.outcome() == MigrationOutcome::Failed,
                        "unknown source: an unregistered detected version fails");
    expectations.expect(result.error() == MigrationError::UnknownSourceVersion,
                        "unknown source: typed as UnknownSourceVersion");
    expectations.expect(result.stepsApplied() == 0,
                        "unknown source: no step ever ran (the very first lookup failed)");
}

void testChainGap(Expectations& expectations) {
    // Only the {1,0} -> {1,1} step is registered; {1,1} -> {1,2} is deliberately withheld so the
    // chain has a real hole after one successful step.
    const std::array<MigrationStepDescriptor, 1> steps{kStepV0ToV1};
    auto operation = makeOperation();
    auto fixture = parseFixture(kInputV0Json, operation);
    expectations.expect(static_cast<bool>(fixture), "chain gap: fixture parses");
    if (!fixture) {
        return;
    }

    auto result = migrateDocumentDom(fixture.document()->root(), kVersion0, kVersion2, steps,
                                     StrictJsonDomLimits{}, operation);
    expectations.expect(result.outcome() == MigrationOutcome::Failed,
                        "chain gap: reaching {1,1} with currentVersion {1,2} and no next step "
                        "fails");
    expectations.expect(result.error() == MigrationError::ChainGap,
                        "chain gap: typed as ChainGap, not UnknownSourceVersion");
    expectations.expect(result.stepsApplied() == 1,
                        "chain gap: the one available step still ran before the gap was found");
}

void testStepFailurePropagatesPath(Expectations& expectations) {
    const std::array<MigrationStepDescriptor, 1> steps{kStepV0ToV1};
    auto operation = makeOperation();
    // Missing "legacyName" entirely: stepRenameAndNormalize must report a typed failure rather
    // than crash or silently write a partial document.
    auto fixture = parseFixture(kInputV0MissingLegacyNameJson, operation);
    expectations.expect(static_cast<bool>(fixture), "step failure: fixture parses");
    if (!fixture) {
        return;
    }

    auto result = migrateDocumentDom(fixture.document()->root(), kVersion0, kVersion1, steps,
                                     StrictJsonDomLimits{}, operation);
    expectations.expect(result.outcome() == MigrationOutcome::Failed,
                        "step failure: a step transform failure fails the whole migration");
    expectations.expect(result.error() == MigrationError::StepTransformFailed,
                        "step failure: typed as StepTransformFailed");
    expectations.expect(result.path() == "/legacyName",
                        "step failure: the step's own path diagnostic propagates unchanged");
    expectations.expect(result.failedStepSourceVersion() == kVersion0 &&
                            result.failedStepTargetVersion() == kVersion1,
                        "step failure: names the exact step that failed");
}

void testStrictReparseRejectsInvalidStepOutput(Expectations& expectations) {
    const std::array<MigrationStepDescriptor, 1> steps{kStepEmitsInvalidJson};
    auto operation = makeOperation();
    // Content is irrelevant: stepEmitsInvalidJson ignores its input DOM entirely.
    auto fixture = parseFixture(kInputV0Json, operation);
    expectations.expect(static_cast<bool>(fixture), "invalid step output: fixture parses");
    if (!fixture) {
        return;
    }

    auto result = migrateDocumentDom(fixture.document()->root(), kBuggyStepSource, kBuggyStepTarget,
                                     steps, StrictJsonDomLimits{}, operation);
    expectations.expect(result.outcome() == MigrationOutcome::Failed,
                        "invalid step output: a step reporting success() but writing invalid "
                        "JSON still fails the migration");
    expectations.expect(result.error() == MigrationError::StepEmittedInvalidJson,
                        "invalid step output: typed as StepEmittedInvalidJson -- the framework "
                        "catches its own step's bug rather than trusting it");
    expectations.expect(result.reparseError() == StrictJsonDomError::InvalidSyntax,
                        "invalid step output: the underlying strict-parse error is preserved");
}

// ---------------------------------------------------------------------------------------------
// Budget exhaustion, with a zeroed coordinator snapshot once the failed result goes out of scope
// -- mirrors open_archive_tests.cpp's own expectOpenResourceExhausted() oracle.
// ---------------------------------------------------------------------------------------------

void testBudgetExhaustion(Expectations& expectations) {
    const std::array<MigrationStepDescriptor, 1> steps{kStepV0ToV1};
    // Parsed under an independent, generous operation: only the migrateDocumentDom() call below
    // runs under the constrained budget.
    auto generousOperation = makeOperation();
    auto fixture = parseFixture(kInputV0Json, generousOperation);
    expectations.expect(static_cast<bool>(fixture), "budget exhaustion: fixture parses");
    if (!fixture) {
        return;
    }

    auto coordinator = ProjectIoMemoryCoordinator::create(1ULL << 20U);
    expectations.expect(coordinator.has_value(), "budget exhaustion: coordinator constructs");
    if (!coordinator.has_value()) {
        return;
    }
    {
        // Too small for even the step's own tiny output buffer to be reserved.
        constexpr std::uint64_t kTinyBudget = 16;
        auto operation = coordinator->createOperation(kTinyBudget, kTinyBudget);
        expectations.expect(operation.has_value(), "budget exhaustion: operation constructs");
        if (!operation.has_value()) {
            return;
        }
        auto result = migrateDocumentDom(fixture.document()->root(), kVersion0, kVersion1, steps,
                                         StrictJsonDomLimits{}, *operation);
        expectations.expect(result.outcome() == MigrationOutcome::Failed,
                            "budget exhaustion: a starved budget fails the migration");
        expectations.expect(result.error() == MigrationError::ResourceExhausted,
                            "budget exhaustion: typed as ResourceExhausted");
        expectations.expect(result.stepsApplied() == 0,
                            "budget exhaustion: the step never completed");
    } // `result`/`operation` (holding no resident state on failure) are destroyed before the
      // snapshot check below.
    const auto snapshot = coordinator->snapshot();
    expectations.expect(snapshot.currentBytes == 0,
                        "budget exhaustion: every charge released once the failed result and its "
                        "operation handle are gone");
}

} // namespace

int main() try {
    Expectations expectations;
    testIdentity(expectations);
    testSingleStepGoldenValues(expectations);
    testTwoStepSequencing(expectations);
    testDeterminism(expectations);
    testUnknownSourceVersion(expectations);
    testChainGap(expectations);
    testStepFailurePropagatesPath(expectations);
    testStrictReparseRejectsInvalidStepOutput(expectations);
    testBudgetExhaustion(expectations);
    return expectations.failures() == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
} catch (const std::exception& exception) {
    std::cerr << "FAILED: unexpected exception: " << exception.what() << '\n';
    return EXIT_FAILURE;
} catch (...) {
    std::cerr << "FAILED: unexpected non-standard exception\n";
    return EXIT_FAILURE;
}
