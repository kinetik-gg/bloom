#include "dependency_artifact_checks.hpp"
#include "dependency_artifact_checks_test_support.hpp"

#include <algorithm>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
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

void erase(Value& objectValue, const std::string_view key) {
    auto& members = objectValue.asObject();
    const auto found =
        std::ranges::find_if(members, [key](const auto& member) { return member.first == key; });
    if (found == members.end()) {
        throw std::runtime_error("test mutation member not found");
    }
    members.erase(found);
}

struct Fixture final {
    explicit Fixture(const Path& repositoryRoot)
        : root(repositoryRoot), context(dependency::makeFixtureContext(root)),
          lockBytes(dependency::readBounded(
              root / "dependencies/tests/fixtures/valid-lock.json",
              dependency::limitsFor(dependency::ArtifactKind::Lock).maximumBytes)),
          prefixBytes(dependency::readBounded(
              root / "dependencies/tests/fixtures/valid-prefix-manifest.json",
              dependency::limitsFor(dependency::ArtifactKind::Prefix).maximumBytes)),
          lock(dependency::parseCanonicalFixture(lockBytes, dependency::ArtifactKind::Lock)),
          prefix(dependency::parseCanonicalFixture(prefixBytes, dependency::ArtifactKind::Prefix)) {
    }

    void validateLock(const Value& candidate) const {
        dependency::validateLockFixture(candidate, context);
    }
    void validatePrefix(const Value& candidate) const {
        dependency::validatePrefixFixture(candidate, lock, lockBytes, context);
    }

    [[nodiscard]] auto extraInstalled(const std::string_view path,
                                      std::optional<std::string_view> component = std::nullopt,
                                      const std::string_view role = "qualification-evidence") const
        -> Value {
        const auto& digest =
            lock.at("components").asArray().front().at("source").at("archiveSha256").asString();
        return object({{"path", text(path)},
                       {"type", text("regular-file")},
                       {"component", component.has_value() ? text(*component) : Value{nullptr}},
                       {"role", text(role)},
                       {"size", number(40)},
                       {"sha256", text(digest)},
                       {"permissions", text("regular")},
                       {"linkTarget", Value{nullptr}}});
    }

    Path root;
    dependency::FixtureContext context;
    std::string lockBytes;
    std::string prefixBytes;
    Value lock;
    Value prefix;
};

void sortInstalled(Value& prefix) {
    std::ranges::sort(prefix.at("installedFiles").asArray(), {},
                      [](const Value& child) { return child.at("path").asString(); });
}

void testCheckedInArtifacts(const Fixture& fixture, Expectations& expectations) {
    const auto result = dependency::checkRepository(fixture.root);
    expectations.expect(
        result.lockVector ==
            "sha256:3a12df1cecff55b171ee3351dcad80fbb5cb52f44924c95ce5056535709d8d79",
        "lock identity test vector matches exact canonical bytes");
    expectations.expect(
        result.prefixVector ==
            "sha256:af0d91ce2f5004be73aec1635cf95b83733252fceef733c1b3289f1bdd8ac149",
        "prefix identity test vector matches exact canonical bytes");
    const auto& components = fixture.prefix.at("components").asArray();
    expectations.expect(
        components[0].at("buildOptionsIdentity").asString() ==
                "sha256:5a014d1740b6ca8189d1195c2f670d22ac6e94367f33d18808fa84094f4c424b" &&
            components[1].at("buildOptionsIdentity").asString() ==
                "sha256:5bc93726991d01181405854fcd7f261ca5b2807cdd09a1b5beb6973985f5b4e5",
        "component build identity test vectors remain frozen");
    expectations.rejects(
        "lock-identity",
        [&] {
            dependency::validatePrefixFixture(fixture.prefix, fixture.lock, fixture.lockBytes + ' ',
                                              fixture.context);
        },
        "lock identity vector binds domain, length, and exact bytes");
}

void testSchemaArtifacts(const Fixture& fixture, Expectations& expectations) {
    TemporaryDirectory temporary;
    temporary.write("duplicate.json", R"({"properties":{},"properties":{}})");
    temporary.write("bom.json", "\xEF\xBB\xBF{}");
    expectations.rejects(
        "duplicate-member",
        [&] {
            static_cast<void>(dependency::loadSchemaArtifact(temporary.root() / "duplicate.json"));
        },
        "schema loader rejects duplicate decoded members");
    expectations.rejects(
        "utf8-bom",
        [&] { static_cast<void>(dependency::loadSchemaArtifact(temporary.root() / "bom.json")); },
        "schema loader rejects UTF-8 BOM");

    for (const auto [kind, name] :
         {std::pair{dependency::ArtifactKind::Lock, "dependency-lock-1.0.schema.json"},
          std::pair{dependency::ArtifactKind::Prefix, "prefix-manifest-1.0.schema.json"}}) {
        auto schema = dependency::loadSchemaArtifact(fixture.root / "dependencies/schemas" / name);
        dependency::validateSchemaArtifact(schema.value, kind);
        const auto& comment = schema.value.at("$comment").asString();
        expectations.expect(comment.find("bloom-dependency-artifact-check") != std::string::npos &&
                                comment.find(".py") == std::string::npos,
                            "frozen schema directs maintainers to the native quality target");
        replace(schema.value, "unevaluatedProperties", Value{true});
        expectations.rejects(
            "schema-object", [&] { dependency::validateSchemaArtifact(schema.value, kind); },
            "schema object closure is structural, not hash-only");
    }
    auto lockSchema = dependency::loadSchemaArtifact(
        fixture.root / "dependencies/schemas/dependency-lock-1.0.schema.json");
    replace(lockSchema.value.at("properties").at("schemaVersion"), "$ref", number(7));
    expectations.rejects(
        "schema-ref",
        [&] {
            dependency::validateSchemaArtifact(lockSchema.value, dependency::ArtifactKind::Lock);
        },
        "schema references must be typed repository-local strings");

    const auto cleanLock = dependency::loadSchemaArtifact(
        fixture.root / "dependencies/schemas/dependency-lock-1.0.schema.json");
    const auto cleanPrefix = dependency::loadSchemaArtifact(
        fixture.root / "dependencies/schemas/prefix-manifest-1.0.schema.json");
    const auto& abiPattern = cleanLock.value.at("$defs").at("abiFlag").at("pattern").asString();
    const auto& targetPattern =
        cleanPrefix.value.at("$defs").at("cmakeTarget").at("pattern").asString();
    const auto& identityPattern =
        cleanLock.value.at("$defs").at("asciiIdentity").at("pattern").asString();
    expectations.expect(
        abiPattern == R"(^[!#-:<-\[\]-~]+$)" && targetPattern == R"(^[!#-:<-\[\]-~]+$)",
        "frozen token patterns admit printable ASCII except quote, semicolon, and backslash");
    expectations.expect(identityPattern == "^[ -~]+$",
                        "frozen identity pattern admits printable ASCII including spaces");
}

void testCanonicalJson(const Fixture& fixture, Expectations& expectations) {
    const std::vector<std::pair<std::string, std::string_view>> invalid{
        {R"({"a": 1})", "canonical-token"},
        {R"({"a":1}
)",
         "canonical-trailing"},
        {"\xEF\xBB\xBF{}", "utf8-bom"},
        {R"({"a":0,"a":1})", "duplicate-member"},
        {R"({"a":"\/"})", "canonical-escape"},
        {R"({"a":"\u0061"})", "canonical-escape"},
        {R"({"a":"\u000A"})", "canonical-escape"},
        {R"({"a":"\u000a"})", "canonical-escape"},
        {R"({"a":01})", "canonical-integer"},
        {R"({"a":-1})", "canonical-token"},
        {R"({"a":1.0})", "canonical-comma"},
        {"{\"a\":\"\xC3\xA9\"}", "unicode-bootstrap"}};
    for (const auto& [encoded, code] : invalid) {
        expectations.rejects(
            code,
            [&] {
                static_cast<void>(
                    dependency::parseCanonicalFixture(encoded, dependency::ArtifactKind::Lock));
            },
            "canonical parser rejects non-canonical spelling");
    }
    const auto controls = dependency::parseCanonicalFixture(R"({"s":"\b\f\n\r\t\u0000"})",
                                                            dependency::ArtifactKind::Lock);
    expectations.expect(controls.at("s").asString() == std::string("\b\f\n\r\t\0", 6),
                        "canonical short and control escapes decode exactly");

    const std::vector<std::pair<std::string_view, std::string_view>> adversarial{
        {"duplicate-member.json", "duplicate-member"},
        {"escaped-solidus.json", "canonical-escape"},
        {"leading-zero.json", "canonical-integer"},
        {"trailing-newline.json", "canonical-trailing"},
        {"whitespace.json", "canonical-token"}};
    for (const auto& [name, code] : adversarial) {
        expectations.rejects(
            code,
            [&] {
                const auto encoded = dependency::readBounded(
                    fixture.root / "dependencies/tests/fixtures/adversarial" / name, 4096);
                static_cast<void>(
                    dependency::parseCanonicalFixture(encoded, dependency::ArtifactKind::Lock));
            },
            "checked-in adversarial canonical fixture is rejected");
    }

    const dependency::ArtifactLimits byteLimit{6, 64, 100};
    expectations.rejects(
        "resource-bytes",
        [&] {
            static_cast<void>(dependency::parseCanonicalFixture(
                R"({"a":0})", dependency::ArtifactKind::Lock, &byteLimit));
        },
        "canonical byte budget is enforced");
    const dependency::ArtifactLimits depthLimit{100, 2, 100};
    expectations.rejects(
        "resource-depth",
        [&] {
            static_cast<void>(dependency::parseCanonicalFixture(
                "[[0]]", dependency::ArtifactKind::Lock, &depthLimit));
        },
        "canonical depth budget is enforced");
    const dependency::ArtifactLimits valueLimit{100, 64, 2};
    expectations.rejects(
        "resource-values",
        [&] {
            static_cast<void>(dependency::parseCanonicalFixture(
                "[0,1]", dependency::ArtifactKind::Lock, &valueLimit));
        },
        "canonical value budget is enforced");
    const dependency::ArtifactLimits integerLimit{6000, 64, 10};
    expectations.rejects(
        "uint64",
        [&] {
            static_cast<void>(dependency::parseCanonicalFixture(
                std::string(5000, '9'), dependency::ArtifactKind::Lock, &integerLimit));
        },
        "canonical integers are bounded to uint64");
}

void testLockShapeAndGraph(const Fixture& fixture, Expectations& expectations) {
    auto reordered = fixture.lock;
    std::swap(reordered.asObject()[3], reordered.asObject()[4]);
    expectations.rejects(
        "members", [&] { fixture.validateLock(reordered); }, "lock root member order is exact");

    auto booleanVersion = fixture.lock;
    replace(booleanVersion.at("schemaVersion"), "major", Value{true});
    expectations.rejects(
        "version", [&] { fixture.validateLock(booleanVersion); },
        "boolean is not an integer schema version");

    auto yearZero = fixture.lock;
    replace(yearZero.at("components").asArray().front().at("license"), "reviewedAt",
            text("0000-01-01"));
    expectations.rejects(
        "date", [&] { fixture.validateLock(yearZero); }, "Gregorian year zero is rejected");

    auto unsortedComponents = fixture.lock;
    std::ranges::reverse(unsortedComponents.at("components").asArray());
    expectations.rejects(
        "order", [&] { fixture.validateLock(unsortedComponents); },
        "lock components remain byte-ordered");

    auto unsortedCapabilities = fixture.lock;
    auto& capabilities = unsortedCapabilities.at("components")
                             .asArray()
                             .front()
                             .at("profileBuilds")
                             .asArray()
                             .front()
                             .at("capabilities");
    capabilities = array({text("bloom.z"), text("bloom.a")});
    expectations.rejects(
        "order", [&] { fixture.validateLock(unsortedCapabilities); },
        "component capabilities remain byte-ordered");

    auto missingDependency = fixture.lock;
    replace(
        missingDependency.at("components").asArray().front().at("dependencies").asArray().front(),
        "name", text("component.missing"));
    expectations.rejects(
        "dependency-reference", [&] { fixture.validateLock(missingDependency); },
        "dependency references must resolve");

    auto cycle = fixture.lock;
    replace(cycle.at("components").asArray()[1], "dependencies",
            array({object({{"name", text("component.alpha")}, {"relationship", text("link")}})}));
    expectations.rejects(
        "dependency-cycle", [&] { fixture.validateLock(cycle); },
        "participating dependency graphs must remain acyclic");

    auto duplicateCapability = fixture.lock;
    replace(duplicateCapability.at("components").asArray()[1].at("profileBuilds").asArray().front(),
            "capabilities", array({text("bloom.capability.alpha")}));
    expectations.rejects(
        "capability-owner", [&] { fixture.validateLock(duplicateCapability); },
        "capabilities have one owner per profile");

    auto unknownProfile = fixture.lock;
    replace(unknownProfile.at("components").asArray()[1].at("profileBuilds").asArray().front(),
            "profileId", text("bloom.profile.missing"));
    expectations.rejects(
        "profile-reference", [&] { fixture.validateLock(unknownProfile); },
        "component builds reference declared profiles");
}

void testLockEvidenceAndAbi(const Fixture& fixture, Expectations& expectations) {
    auto productionUrl = fixture.lock;
    replace(productionUrl.at("components").asArray().front().at("source"), "url",
            text("https://example.com/a.tar"));
    expectations.rejects(
        "fixture-url", [&] { fixture.validateLock(productionUrl); },
        "synthetic source URLs cannot claim production provenance");

    auto outsideFixture = fixture.lock;
    replace(outsideFixture.at("components")
                .asArray()
                .front()
                .at("license")
                .at("licenseFiles")
                .asArray()
                .front(),
            "path", text("dependencies/licenses/alpha/LICENSE"));
    expectations.rejects(
        "fixture-separation", [&] { fixture.validateLock(outsideFixture); },
        "synthetic evidence cannot escape payload fixtures");

    auto inventedDigest = fixture.lock;
    replace(inventedDigest.at("components").asArray().front().at("source"), "archiveSha256",
            text("sha256:" + std::string(64, '0')));
    expectations.rejects(
        "fixture-evidence", [&] { fixture.validateLock(inventedDigest); },
        "source digests bind exact fixture bytes");

    auto windowsSdkOnLinux = fixture.lock;
    replace(windowsSdkOnLinux.at("profiles").asArray().front().at("consumerAbi"), "windowsSdk",
            text("windows-sdk"));
    expectations.rejects(
        "abi-platform", [&] { fixture.validateLock(windowsSdkOnLinux); },
        "Linux ABI cannot carry a Windows SDK");

    auto windowsLibraryOnLinux = fixture.lock;
    auto& libraryAbi = windowsLibraryOnLinux.at("profiles").asArray().front().at("consumerAbi");
    replace(libraryAbi, "standardLibrary", text("msvc-stl"));
    replace(libraryAbi, "libstdcxxCxx11Abi", Value{nullptr});
    expectations.rejects(
        "abi-platform", [&] { fixture.validateLock(windowsLibraryOnLinux); },
        "Linux ABI cannot claim MSVC STL");

    auto windowsRuntimeOnLinux = fixture.lock;
    replace(windowsRuntimeOnLinux.at("profiles").asArray().front().at("consumerAbi"), "cxxRuntime",
            text("msvc"));
    expectations.rejects(
        "abi-platform", [&] { fixture.validateLock(windowsRuntimeOnLinux); },
        "Linux ABI cannot claim MSVC runtime");

    auto controlFlag = fixture.lock;
    replace(controlFlag.at("profiles").asArray().front().at("consumerAbi"), "abiFlags",
            array({text(std::string_view("\0", 1))}));
    expectations.rejects(
        "printable-token", [&] { fixture.validateLock(controlFlag); },
        "ABI flags exclude controls and shell-list delimiters");

    auto controlIdentity = fixture.lock;
    replace(controlIdentity.at("profiles").asArray().front().at("consumerAbi"), "compilerAbi",
            text(std::string_view("\0", 1)));
    expectations.rejects(
        "ascii-identity", [&] { fixture.validateLock(controlIdentity); },
        "ABI identities require printable ASCII");

    auto swappedUnicode = fixture.lock;
    auto& unicodeFiles = swappedUnicode.at("unicodeProfile").at("files").asArray();
    std::swap(unicodeFiles[0].at("sha256"), unicodeFiles[1].at("sha256"));
    expectations.rejects(
        "fixture-evidence", [&] { fixture.validateLock(swappedUnicode); },
        "Unicode paths bind exact fixture payloads");

    auto swappedPatch = fixture.lock;
    auto& component = swappedPatch.at("components").asArray().front();
    replace(component.at("license"), "modified", Value{true});
    const auto& otherDigest = fixture.lock.at("components")
                                  .asArray()[1]
                                  .at("license")
                                  .at("licenseFiles")
                                  .asArray()
                                  .front()
                                  .at("sha256")
                                  .asString();
    replace(component, "patches",
            array({object({{"path", text("dependencies/tests/fixtures/payloads/alpha-license.txt")},
                           {"sha256", text(otherDigest)},
                           {"applyOrder", number(0)},
                           {"reason", text("Synthetic path-to-digest mismatch")}})}));
    expectations.rejects(
        "fixture-evidence", [&] { fixture.validateLock(swappedPatch); },
        "patch paths bind their exact fixture digest");
}

void testPrefixLockBindingAndShapes(const Fixture& fixture, Expectations& expectations) {
    auto wrongLock = fixture.prefix;
    replace(wrongLock, "lockIdentity", text("sha256:" + std::string(64, '0')));
    expectations.rejects(
        "lock-identity", [&] { fixture.validatePrefix(wrongLock); },
        "prefix identity binds exact canonical lock bytes");

    auto wrongProfile = fixture.prefix;
    replace(wrongProfile.at("profile").at("toolchain").at("compiler"), "identity", text("changed"));
    expectations.rejects(
        "lock-copy", [&] { fixture.validatePrefix(wrongProfile); },
        "prefix profile is an exact ordered lock copy");

    auto wrongBuild = fixture.prefix;
    replace(wrongBuild.at("components").asArray().front(), "buildOptionsIdentity",
            text("sha256:" + std::string(64, '0')));
    expectations.rejects(
        "component-copy", [&] { fixture.validatePrefix(wrongBuild); },
        "prefix component binds exact build options");

    auto wrongProvider = fixture.prefix;
    replace(wrongProvider.at("capabilities").asArray().front(), "providerComponent",
            text("component.beta"));
    expectations.rejects(
        "capability-copy", [&] { fixture.validatePrefix(wrongProvider); },
        "prefix capability providers match lock ownership");

    auto booleanVersion = fixture.prefix;
    replace(booleanVersion.at("schemaVersion"), "major", Value{true});
    expectations.rejects(
        "version", [&] { fixture.validatePrefix(booleanVersion); },
        "prefix boolean is not an integer schema version");

    auto booleanAbi = fixture.prefix;
    replace(booleanAbi.at("profile").at("consumerAbi"), "cxxStandard", Value{true});
    expectations.rejects(
        "lock-copy", [&] { fixture.validatePrefix(booleanAbi); },
        "prefix ABI copy retains JSON scalar types");

    auto malformedComponent = fixture.prefix;
    erase(malformedComponent.at("components").asArray().front(), "name");
    expectations.rejects(
        "members", [&] { fixture.validatePrefix(malformedComponent); },
        "prefix component hostile shape has typed diagnostic");
    auto malformedInstalled = fixture.prefix;
    erase(malformedInstalled.at("installedFiles").asArray().front(), "path");
    expectations.rejects(
        "members", [&] { fixture.validatePrefix(malformedInstalled); },
        "installed hostile shape has typed diagnostic");
    auto malformedResult = fixture.prefix;
    erase(malformedResult.at("qualificationResults").asArray().front(), "gateId");
    expectations.rejects(
        "members", [&] { fixture.validatePrefix(malformedResult); },
        "qualification hostile shape has typed diagnostic");
}

void testPrefixEvidenceAndQualification(const Fixture& fixture, Expectations& expectations) {
    auto wrongTuple = fixture.prefix;
    replace(wrongTuple.at("installedFiles").asArray().front(), "permissions", text("none"));
    expectations.rejects(
        "installed-tuple", [&] { fixture.validatePrefix(wrongTuple); },
        "installed regular-file tuple is exact");

    auto wrongSize = fixture.prefix;
    auto& size = wrongSize.at("installedFiles").asArray().front().at("size");
    const auto parsedSize = json::asUint64(size);
    if (!parsedSize.has_value()) {
        throw std::runtime_error("valid installed fixture size is not uint64");
    }
    size = number(*parsedSize + 1);
    expectations.rejects(
        "fixture-size", [&] { fixture.validatePrefix(wrongSize); },
        "installed sizes bind checked-in payload bytes");

    auto missingCopy = fixture.prefix;
    missingCopy.at("installedFiles")
        .asArray()
        .erase(missingCopy.at("installedFiles").asArray().begin());
    expectations.rejects(
        "evidence-copy", [&] { fixture.validatePrefix(missingCopy); },
        "all locked evidence is copied into prefix manifest");

    auto failedGate = fixture.prefix;
    replace(failedGate.at("qualificationResults").asArray().front(), "status", text("failed"));
    expectations.rejects(
        "qualification", [&] { fixture.validatePrefix(failedGate); },
        "required qualification gates must pass");

    auto wallClock = fixture.prefix;
    replace(wallClock.at("qualificationResults").asArray().front(), "completedAt",
            text("2000-01-01T00:00:00Z"));
    expectations.rejects(
        "qualification", [&] { fixture.validatePrefix(wallClock); },
        "qualification timestamps derive from SOURCE_DATE_EPOCH");

    auto missingEvidence = fixture.prefix;
    replace(missingEvidence.at("qualificationResults")
                .asArray()
                .front()
                .at("evidence")
                .asArray()
                .front(),
            "path", text("share/bloom/dependencies/evidence/missing.txt"));
    expectations.rejects(
        "gate-evidence", [&] { fixture.validatePrefix(missingEvidence); },
        "gate evidence references installed qualification records");
}

void testPrefixFilesystemAndRoles(const Fixture& fixture, Expectations& expectations) {
    auto collision = fixture.prefix;
    collision.at("installedFiles").asArray().push_back(fixture.extraInstalled("extra/A"));
    collision.at("installedFiles").asArray().push_back(fixture.extraInstalled("extra/a"));
    sortInstalled(collision);
    expectations.rejects(
        "portable-collision", [&] { fixture.validatePrefix(collision); },
        "installed paths are collision-free under portable case folding");

    auto prefixConflict = fixture.prefix;
    prefixConflict.at("installedFiles").asArray().push_back(fixture.extraInstalled("extra/tree"));
    prefixConflict.at("installedFiles")
        .asArray()
        .push_back(fixture.extraInstalled("extra/tree/child"));
    sortInstalled(prefixConflict);
    expectations.rejects(
        "path-prefix", [&] { fixture.validatePrefix(prefixConflict); },
        "regular installed file cannot be an ancestor");

    auto symbolicLink = fixture.prefix;
    auto& link = symbolicLink.at("installedFiles").asArray().front();
    replace(link, "type", text("symbolic-link"));
    replace(link, "size", Value{nullptr});
    replace(link, "sha256", Value{nullptr});
    replace(link, "permissions", text("none"));
    replace(link, "linkTarget", text("target"));
    expectations.rejects(
        "fixture-symlink", [&] { fixture.validatePrefix(symbolicLink); },
        "synthetic manifest cannot claim production symlink evidence");

    auto extraRole = fixture.prefix;
    extraRole.at("installedFiles")
        .asArray()
        .push_back(fixture.extraInstalled("zz/extra-library", "component.alpha", "library"));
    sortInstalled(extraRole);
    expectations.rejects(
        "shipping-role", [&] { fixture.validatePrefix(extraRole); },
        "installed component roles exactly match lock");

    auto prefixWideShipping = fixture.prefix;
    prefixWideShipping.at("installedFiles")
        .asArray()
        .push_back(fixture.extraInstalled("zz/prefix-library", std::nullopt, "library"));
    sortInstalled(prefixWideShipping);
    expectations.rejects(
        "installed-component", [&] { fixture.validatePrefix(prefixWideShipping); },
        "prefix-wide records cannot claim component shipping roles");

    auto cmakeTarget = fixture.prefix;
    constexpr std::string_view configPath = "zz/synthetic-config.cmake";
    cmakeTarget.at("installedFiles")
        .asArray()
        .push_back(fixture.extraInstalled(configPath, "component.alpha", "cmake-package"));
    sortInstalled(cmakeTarget);
    replace(cmakeTarget, "cmakePackages",
            array({object({{"name", text("SyntheticPackage")},
                           {"version", text("synthetic")},
                           {"configPath", text(configPath)},
                           {"targets", array({text(std::string("Bloom::Target") + '\x7F')})}})}));
    expectations.rejects(
        "printable-token", [&] { fixture.validatePrefix(cmakeTarget); },
        "CMake target names reject non-printable bytes");
}

} // namespace

int main(const int argumentCount, char** arguments) {
    if (argumentCount != 2) {
        std::cerr << "usage: bloom_dependency_artifact_checks_test <repository-root>\n";
        return 2;
    }
    try {
        const Fixture fixture(arguments[1]);
        Expectations expectations;
        testCheckedInArtifacts(fixture, expectations);
        testSchemaArtifacts(fixture, expectations);
        testCanonicalJson(fixture, expectations);
        testLockShapeAndGraph(fixture, expectations);
        testLockEvidenceAndAbi(fixture, expectations);
        testPrefixLockBindingAndShapes(fixture, expectations);
        testPrefixEvidenceAndQualification(fixture, expectations);
        testPrefixFilesystemAndRoles(fixture, expectations);
        const auto boundaryFailures =
            bloom::quality::dependencies::tests::runBoundaryTests(fixture.root);
        return expectations.failures() + boundaryFailures == 0 ? 0 : 1;
    } catch (const std::exception& error) {
        std::cerr << "fatal dependency checker self-test error: " << error.what() << '\n';
        return 2;
    }
}
