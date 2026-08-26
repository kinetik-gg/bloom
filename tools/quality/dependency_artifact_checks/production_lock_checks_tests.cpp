#include "dependency_artifact_checks_test_support.hpp"

#include "production_lock_checks.hpp"

#include <array>
#include <filesystem>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace {

namespace dependency = bloom::quality::dependencies;
namespace json = bloom::quality::json;
using Path = std::filesystem::path;
using Value = json::Value;
using bloom::quality::dependencies::tests::Expectations;
using bloom::quality::dependencies::tests::TemporaryDirectory;

[[nodiscard]] auto text(const std::string_view value) -> Value { return Value{std::string(value)}; }

[[nodiscard]] auto number(const std::uint64_t value) -> Value {
    return Value{json::Number{std::to_string(value)}};
}

[[nodiscard]] auto object(std::initializer_list<std::pair<std::string, Value>> members) -> Value {
    return Value{Value::Object(members)};
}

[[nodiscard]] auto array(std::initializer_list<Value> values) -> Value {
    return Value{Value::Array(values)};
}

void replace(Value& objectValue, const std::string_view key, Value replacement) {
    objectValue.at(key) = std::move(replacement);
}

constexpr std::string_view kSyntheticEvidenceBytes =
    "synthetic evidence bytes for the production lock validator\n";
constexpr std::string_view kFrozenLockIdentity =
    "sha256:39d25ceb673a07c28045271cb3138f44d03e41df2345ede4ac73147c0ef576a8";

[[nodiscard]] auto evidenceDigest() -> std::string {
    return dependency::sha256DigestText(kSyntheticEvidenceBytes);
}

[[nodiscard]] auto evidenceReference(const std::string_view path) -> Value {
    return object({{"path", text(path)}, {"sha256", text(evidenceDigest())}});
}

[[nodiscard]] auto zeroDigest() -> std::string { return "sha256:" + std::string(64, '0'); }

[[nodiscard]] auto bootstrapFileReference(const std::size_t index) -> Value {
    const auto& record = dependency::kUnicodeBootstrapFiles[index];
    return object(
        {{"path", text(record.name)}, {"sha256", text("sha256:" + std::string(record.sha256Hex))}});
}

[[nodiscard]] auto unicodeProfile() -> Value {
    return object(
        {{"version", text(dependency::kUnicodeBootstrapVersion)},
         {"sourceUrl", text(dependency::kUnicodeBootstrapSourceUrl)},
         {"archiveSha256", text("sha256:" + std::string(dependency::kUnicodeBootstrapArchiveHex))},
         {"files",
          array({bootstrapFileReference(0), bootstrapFileReference(1), bootstrapFileReference(2),
                 bootstrapFileReference(3), bootstrapFileReference(4)})}});
}

[[nodiscard]] auto minimalProductionLock() -> Value {
    return object(
        {{"format", text("org.kinetik.bloom.dependencies.lock")},
         {"schemaVersion", object({{"major", number(1)}, {"minor", number(0)}})},
         {"unicodeProfile", unicodeProfile()},
         {"profiles",
          array({object(
              {{"id", text("bloom.profile.linux-release")},
               {"target", object({{"triple", text("x86_64-unknown-linux-gnu")},
                                  {"operatingSystem", text("linux")},
                                  {"architecture", text("x86_64")},
                                  {"minimumOsVersion", Value{nullptr}}})},
               {"buildConfiguration", text("release")},
               {"consumerAbi", object({{"cxxStandard", number(20)},
                                       {"compilerFamily", text("gcc")},
                                       {"compilerAbi", text("gcc.synthetic-abi")},
                                       {"standardLibrary", text("libstdc++")},
                                       {"standardLibraryAbi", text("libstdcxx.synthetic-abi")},
                                       {"cxxRuntime", text("libgcc")},
                                       {"cxxRuntimeAbi", text("libgcc.synthetic-abi")},
                                       {"cxxRuntimeLinkage", text("dynamic")},
                                       {"platformRuntime", text("glibc")},
                                       {"platformRuntimeAbi", text("glibc.synthetic-abi")},
                                       {"exceptions", Value{true}},
                                       {"rtti", Value{true}},
                                       {"libstdcxxCxx11Abi", number(1)},
                                       {"msvcRuntime", Value{nullptr}},
                                       {"msvcIteratorDebugLevel", Value{nullptr}},
                                       {"windowsSdk", Value{nullptr}},
                                       {"windowsSdkVersion", Value{nullptr}},
                                       {"appleSdk", Value{nullptr}},
                                       {"appleSdkVersion", Value{nullptr}},
                                       {"appleDeploymentTarget", Value{nullptr}},
                                       {"abiFlags", array({})}})},
               {"toolchain",
                object({{"cmake", object({{"name", text("cmake")},
                                          {"version", text("synthetic")},
                                          {"identity", text("synthetic-cmake")}})},
                        {"generator", object({{"name", text("ninja")},
                                              {"version", text("synthetic")},
                                              {"identity", text("synthetic-generator")}})},
                        {"buildTool", object({{"name", text("ninja")},
                                              {"version", text("synthetic")},
                                              {"identity", text("synthetic-build-tool")}})},
                        {"compiler", object({{"name", text("gcc")},
                                             {"version", text("synthetic")},
                                             {"identity", text("synthetic-compiler")}})},
                        {"linker", object({{"name", text("ld")},
                                           {"version", text("synthetic")},
                                           {"identity", text("synthetic-linker")}})},
                        {"standardLibrary",
                         object({{"name", text("libstdc++")},
                                 {"version", text("synthetic")},
                                 {"identity", text("synthetic-standard-library")}})},
                        {"sdk", object({{"name", text("linux-sysroot")},
                                        {"version", text("synthetic")},
                                        {"identity", text("synthetic-sdk")}})}})},
               {"environment", object({{"profileId", text("bloom.environment.linux-release")},
                                       {"sourceDateEpoch", number(0)},
                                       {"variables", array({})}})},
               {"qualificationGates", array({object({{"gateId", text("bloom.gate.synthetic")},
                                                     {"disposition", text("required")},
                                                     {"reason", Value{nullptr}}})})}})})},
         {"components",
          array({object(
              {{"name", text("component.synthetic")},
               {"version", text("synthetic-1.0")},
               {"source", object({{"url", text("https://example.invalid/synthetic-source.tar")},
                                  {"archiveSha256", text(zeroDigest())},
                                  {"commit", Value{nullptr}},
                                  {"provenancePolicy", text("not-published")},
                                  {"provenanceReview",
                                   evidenceReference(
                                       "dependencies/licenses/component.synthetic/provenance.md")},
                                  {"provenance", array({})}})},
               {"license",
                object(
                    {{"spdxExpression", text("LicenseRef-Synthetic")},
                     {"licenseFiles", array({evidenceReference(
                                          "dependencies/licenses/component.synthetic/LICENSE")})},
                     {"copyrightFiles", array({})},
                     {"noticeFiles", array({})},
                     {"sourceObligation", text("none")},
                     {"modified", Value{false}},
                     {"reviewRecord",
                      evidenceReference("dependencies/licenses/component.synthetic/review.md")},
                     {"reviewedAt", text("2026-08-26")}})},
               {"patches", array({})},
               {"dependencies", array({})},
               {"profileBuilds", array({object({{"profileId", text("bloom.profile.linux-release")},
                                                {"linkage", text("dynamic")},
                                                {"cmakeOptions", array({})},
                                                {"featureDecisions", array({})},
                                                {"capabilities", array({})},
                                                {"shippingRoles", array({text("library")})},
                                                {"conformanceFixtureSets", array({})}})})},
               {"securityReview",
                object({{"reviewedAt", text("2026-08-26")},
                        {"record", evidenceReference(
                                       "dependencies/licenses/component.synthetic/security.md")},
                        {"vulnerabilities", array({})}})}})})}});
}

void copyBootstrapTables(const Path& repositoryRoot, const TemporaryDirectory& temporary) {
    for (const auto& record : dependency::kUnicodeBootstrapFiles) {
        const auto table = dependency::readBounded(
            repositoryRoot / "dependencies/unicode/15.1.0" / record.name,
            dependency::limitsFor(dependency::ArtifactKind::Lock).maximumBytes);
        temporary.write(Path("dependencies/unicode/15.1.0") / record.name, table);
    }
}

void writeEvidenceTree(const TemporaryDirectory& temporary) {
    for (const auto* name : {"provenance.md", "LICENSE", "review.md", "security.md"}) {
        temporary.write(Path("dependencies/licenses/component.synthetic") / name,
                        kSyntheticEvidenceBytes);
    }
}

// Every present-lock case runs through the real orchestrator, so each synthetic repository root
// carries verified Unicode tables plus the minimal lock's supporting evidence tree.
struct ProductionFixture final {
    explicit ProductionFixture(const Path& repositoryRoot) : temporary() {
        copyBootstrapTables(repositoryRoot, temporary);
        writeEvidenceTree(temporary);
    }

    void rejectsWith(const Value& lock, const std::string_view code, Expectations& expectations,
                     const std::string_view message) const {
        temporary.write("dependencies/dependencies.lock.json", dependency::encodeCanonical(lock));
        expectations.rejects(
            code, [&] { static_cast<void>(dependency::validateProductionLock(temporary.root())); },
            message);
    }

    TemporaryDirectory temporary;
};

void testAbsentLockAndGoldenConstants(const Path& repositoryRoot, Expectations& expectations) {
    const ProductionFixture absentFixture{repositoryRoot};
    const auto absent = dependency::validateProductionLock(absentFixture.temporary.root());
    expectations.expect(!absent.present && absent.identity.empty(),
                        "absent production lock reports absence as success without identity");
    const auto repository = dependency::validateProductionLock(repositoryRoot);
    expectations.expect(repository.present && repository.identity.starts_with("sha256:") &&
                            repository.identity.size() == 7 + 64,
                        "the reviewed repository lock validates with a well-formed identity");
    expectations.expect(dependency::kProductionLockRepositoryPath ==
                            std::string_view("dependencies/dependencies.lock.json"),
                        "production mode binds exactly the contract lock path");
    constexpr std::array<std::string_view, 5> kFixedOrder{
        "UnicodeData.txt", "CompositionExclusions.txt", "DerivedNormalizationProps.txt",
        "CaseFolding.txt", "NormalizationTest.txt"};
    for (std::size_t index = 0; index < dependency::kUnicodeBootstrapFiles.size(); ++index) {
        expectations.expect(dependency::kUnicodeBootstrapFiles[index].name == kFixedOrder[index],
                            "bootstrap allowlist keeps the fixed Unicode record order");
    }
    expectations.expect(dependency::kUnicodeBootstrapSourceUrl ==
                            std::string_view("https://www.unicode.org/Public/15.1.0/ucd/UCD.zip"),
                        "bootstrap archive URL matches the reviewed provenance record");
    expectations.expect(dependency::kUnicodeBootstrapArchiveHex ==
                            std::string_view("cb1c663d053926500cd501229736045752713a066bd7580209"
                                             "8598b7a7056177"),
                        "bootstrap archive digest matches the reviewed provenance record");
}

void testBootstrapDetectsDriftedTables(const Path& repositoryRoot, Expectations& expectations) {
    TemporaryDirectory temporary;
    copyBootstrapTables(repositoryRoot, temporary);
    auto corrupted =
        dependency::readBounded(repositoryRoot / "dependencies/unicode/15.1.0/"
                                                 "CompositionExclusions.txt",
                                dependency::limitsFor(dependency::ArtifactKind::Lock).maximumBytes);
    corrupted.at(0) = corrupted.at(0) == '!' ? '#' : '!';
    temporary.write("dependencies/unicode/15.1.0/CompositionExclusions.txt", corrupted);
    expectations.rejects(
        "unicode-bootstrap", [&] { dependency::verifyUnicodeBootstrap(temporary.root()); },
        "bootstrap self-check rejects drifted checked-in Unicode tables");

    TemporaryDirectory missing;
    copyBootstrapTables(repositoryRoot, missing);
    std::error_code error;
    std::filesystem::remove(missing.root() / "dependencies/unicode/15.1.0/CaseFolding.txt", error);
    expectations.rejects(
        "unicode-bootstrap", [&] { dependency::verifyUnicodeBootstrap(missing.root()); },
        "bootstrap self-check requires every checked-in Unicode table");
}

void testMinimalLockIdentityVector(const Path& repositoryRoot, Expectations& expectations) {
    const ProductionFixture fixture(repositoryRoot);
    fixture.temporary.write("dependencies/dependencies.lock.json",
                            dependency::encodeCanonical(minimalProductionLock()));
    const auto result = dependency::validateProductionLock(fixture.temporary.root());
    expectations.expect(result.present && result.identity == kFrozenLockIdentity,
                        "minimal production lock reproduces the frozen identity vector");
}

void testByteInequalityAndAsciiStrictness(const Path& repositoryRoot, Expectations& expectations) {
    const ProductionFixture fixture(repositoryRoot);
    auto leadingSpace = dependency::encodeCanonical(minimalProductionLock());
    leadingSpace.insert(leadingSpace.begin(), ' ');
    fixture.temporary.write("dependencies/dependencies.lock.json", leadingSpace);
    expectations.rejects(
        "canonical-token",
        [&] { static_cast<void>(dependency::validateProductionLock(fixture.temporary.root())); },
        "production locks reproduce canonical bytes exactly; whitespace is rejected");

    const std::string nonAscii = "{\"format\":\"\xC3\xA9\"}";
    fixture.temporary.write("dependencies/dependencies.lock.json", nonAscii);
    expectations.rejects(
        "ascii-strict",
        [&] { static_cast<void>(dependency::validateProductionLock(fixture.temporary.root())); },
        "ASCII-strict v1 tightening rejects any non-ASCII byte before parsing");
}

void testUnicodeProfileEquality(const Path& repositoryRoot, Expectations& expectations) {
    const ProductionFixture fixture(repositoryRoot);

    auto wrongArchive = minimalProductionLock();
    replace(wrongArchive.at("unicodeProfile"), "archiveSha256", text(zeroDigest()));
    fixture.rejectsWith(wrongArchive, "unicode-profile", expectations,
                        "lock archive digest must equal the bootstrap allowlist member");

    auto wrongFile = minimalProductionLock();
    replace(wrongFile.at("unicodeProfile").at("files").asArray().at(3), "sha256",
            text(zeroDigest()));
    fixture.rejectsWith(wrongFile, "unicode-profile", expectations,
                        "each locked Unicode digest must equal the bootstrap member");

    auto wrongUrl = minimalProductionLock();
    replace(wrongUrl.at("unicodeProfile"), "sourceUrl", text("https://example.invalid/ucd.zip"));
    fixture.rejectsWith(wrongUrl, "unicode-profile", expectations,
                        "locked Unicode source URL must equal the reviewed official archive");
}

void testArtifactReferenceDigests(const Path& repositoryRoot, Expectations& expectations) {
    const ProductionFixture fixture(repositoryRoot);

    auto inventedDigest = minimalProductionLock();
    replace(inventedDigest.at("components")
                .asArray()
                .front()
                .at("license")
                .at("licenseFiles")
                .asArray()
                .front(),
            "sha256", text(zeroDigest()));
    fixture.rejectsWith(inventedDigest, "artifact-evidence", expectations,
                        "recorded artifact digests must reproduce referenced bytes");

    auto missingFile = minimalProductionLock();
    replace(missingFile.at("components").asArray().front().at("source").at("provenanceReview"),
            "path", text("dependencies/licenses/component.synthetic/absent.md"));
    fixture.rejectsWith(missingFile, "artifact-evidence", expectations,
                        "artifact references must resolve to checked-in regular files");

    auto outsideTree = minimalProductionLock();
    replace(outsideTree.at("components").asArray().front().at("securityReview").at("record"),
            "path", text("dependencies/tests/fixtures/payloads/alpha-security-review.txt"));
    fixture.rejectsWith(outsideTree, "production-path", expectations,
                        "fixture-tree references never count as production artifacts");
}

void testPatchRules(const Path& repositoryRoot, Expectations& expectations) {
    const ProductionFixture fixture(repositoryRoot);

    fixture.temporary.write("dependencies/patches/component.synthetic/fix.patch",
                            kSyntheticEvidenceBytes);
    auto unmodifiedPatch = minimalProductionLock();
    replace(unmodifiedPatch.at("components").asArray().front(), "patches",
            array({object({{"path", text("dependencies/patches/component.synthetic/fix.patch")},
                           {"sha256", text(evidenceDigest())},
                           {"applyOrder", number(0)},
                           {"reason", text("Synthetic patch without modified flag")}})}));
    fixture.rejectsWith(unmodifiedPatch, "modified", expectations,
                        "non-empty patches require license.modified true");

    auto misplacedPatch = minimalProductionLock();
    replace(misplacedPatch.at("components").asArray().front(), "patches",
            array({object({{"path", text("dependencies/patches/other.component/fix.patch")},
                           {"sha256", text(evidenceDigest())},
                           {"applyOrder", number(0)},
                           {"reason", text("Synthetic patch outside component directory")}})}));
    fixture.rejectsWith(misplacedPatch, "patch-path", expectations,
                        "patches resolve below dependencies/patches/<component>/");
}

void testCrossReferencesAndSets(const Path& repositoryRoot, Expectations& expectations) {
    const ProductionFixture fixture(repositoryRoot);

    auto selfDependency = minimalProductionLock();
    replace(
        selfDependency.at("components").asArray().front(), "dependencies",
        array({object({{"name", text("component.synthetic")}, {"relationship", text("link")}})}));
    fixture.rejectsWith(selfDependency, "dependency-reference", expectations,
                        "dependency edges may not name their own component");

    auto unknownProfile = minimalProductionLock();
    replace(unknownProfile.at("components").asArray().front().at("profileBuilds").asArray().front(),
            "profileId", text("bloom.profile.missing"));
    fixture.rejectsWith(unknownProfile, "profile-reference", expectations,
                        "profile builds must reference declared root profiles");

    auto duplicateCapability = minimalProductionLock();
    replace(duplicateCapability.at("components")
                .asArray()
                .front()
                .at("profileBuilds")
                .asArray()
                .front(),
            "capabilities",
            array({text("bloom.capability.alpha"), text("bloom.capability.alpha")}));
    fixture.rejectsWith(duplicateCapability, "duplicate-identity", expectations,
                        "ordered sets remain duplicate-free");
}

} // namespace

namespace bloom::quality::dependencies::tests {

auto runProductionLockTests(const std::filesystem::path& repositoryRoot) -> int {
    Expectations expectations;
    testAbsentLockAndGoldenConstants(repositoryRoot, expectations);
    testBootstrapDetectsDriftedTables(repositoryRoot, expectations);
    testMinimalLockIdentityVector(repositoryRoot, expectations);
    testByteInequalityAndAsciiStrictness(repositoryRoot, expectations);
    testUnicodeProfileEquality(repositoryRoot, expectations);
    testArtifactReferenceDigests(repositoryRoot, expectations);
    testPatchRules(repositoryRoot, expectations);
    testCrossReferencesAndSets(repositoryRoot, expectations);
    return expectations.failures();
}

} // namespace bloom::quality::dependencies::tests
