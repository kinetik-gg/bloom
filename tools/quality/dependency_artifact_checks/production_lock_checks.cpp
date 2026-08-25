#include "production_lock_checks.hpp"

#include "dependency_artifact_checks_internal.hpp"

#include <bloom/core/sha256.hpp>

#include <algorithm>
#include <cstddef>
#include <functional>
#include <map>
#include <ranges>
#include <set>
#include <span>
#include <system_error>
#include <utility>
#include <vector>

namespace bloom::quality::dependencies {

using namespace detail;

namespace {

constexpr std::string_view kUnicodeRepositoryDirectory = "dependencies/unicode/15.1.0";
constexpr std::string_view kDependenciesPrefix = "dependencies/";
constexpr std::string_view kFixtureTreePrefix = "dependencies/tests/fixtures/";
constexpr std::string_view kPatchesPrefix = "dependencies/patches/";

// ASCII-STRICT v1 TIGHTENING: see production_lock_checks.hpp. The canonical profile emits
// non-control Unicode directly as UTF-8 and admits only lowercase \u00xx control escapes, so a raw
// byte scan before parsing rejects exactly the locks whose decoded strings or keys leave ASCII.
void requireAsciiStrict(const std::string_view encoded) {
    const auto* const offending = std::ranges::find_if(
        encoded, [](const char byte) { return static_cast<unsigned char>(byte) >= 0x80U; });
    if (offending != encoded.end()) {
        fail("ascii-strict", "$",
             "production lock v1 deliberately tightens decoded strings and keys to ASCII; ASCII is "
             "closed under NFC so Unicode normalization machinery stays deferred, and contract "
             "policy may tighten but never loosen");
    }
}

void verifyBootstrapTables(const Path& root) {
    for (const auto& [name, expectedHex] : kUnicodeBootstrapFiles) {
        const auto relative = Path{kUnicodeRepositoryDirectory} / name;
        auto candidate = root;
        for (const auto& component : relative) {
            candidate /= component;
            std::error_code walkError;
            const auto walkStatus = std::filesystem::symlink_status(candidate, walkError);
            if (!walkError && std::filesystem::is_symlink(walkStatus)) {
                fail("unicode-bootstrap", relative.string(),
                     "checked-in Unicode tables must not traverse a symlink");
            }
        }
        std::error_code error;
        const auto absolute = std::filesystem::weakly_canonical(root / relative, error);
        if (error) {
            fail("unicode-bootstrap", relative.string(), error.message());
        }
        const auto status = std::filesystem::symlink_status(absolute, error);
        if (error || !std::filesystem::is_regular_file(status)) {
            fail("unicode-bootstrap", relative.string(),
                 "expected the checked-in regular Unicode 15.1 table");
        }
        const auto actual =
            digestHex(readBounded(absolute, limitsFor(ArtifactKind::Lock).maximumBytes));
        if (actual != expectedHex) {
            fail("unicode-bootstrap", relative.string(),
                 "checked-in bytes differ from the reviewed bootstrap allowlist");
        }
    }
}

// Resolves a lexically validated repository-relative evidence path to its checked-in regular file
// and verifies the bytes reproduce the recorded digest. Enforces the dependencies/ prefix, the
// synthetic-fixture separation, and the patch component-directory rule.
void resolveProductionEvidence(const std::string_view path, const std::string_view digest,
                               const std::string_view location, const Path& root,
                               const std::string_view componentName, const bool patch) {
    if (!path.starts_with(kDependenciesPrefix)) {
        fail("artifact-path", location,
             "repository artifact references are repository-root-relative below dependencies/");
    }
    if (path.starts_with(kFixtureTreePrefix)) {
        fail("production-path", location,
             "the synthetic fixture tree is never a production artifact source");
    }
    if (patch) {
        const std::string expectedPrefix =
            std::string(kPatchesPrefix).append(componentName).append(1, '/');
        if (!path.starts_with(expectedPrefix)) {
            fail("patch-path", location,
                 "patch references resolve below dependencies/patches/<component>/");
        }
    }
    auto candidate = root;
    for (const auto& component : Path(path)) {
        candidate /= component;
        std::error_code walkError;
        const auto walkStatus = std::filesystem::symlink_status(candidate, walkError);
        if (!walkError && std::filesystem::is_symlink(walkStatus)) {
            fail("artifact-evidence", location, "artifact paths must not traverse a symlink");
        }
    }
    std::error_code error;
    const auto absolute = std::filesystem::weakly_canonical(root / Path(path), error);
    if (error) {
        fail("artifact-evidence", location, error.message());
    }
    const auto status = std::filesystem::symlink_status(absolute, error);
    if (error || !std::filesystem::is_regular_file(status)) {
        fail("artifact-evidence", location, "must name a checked-in regular repository file");
    }
    const auto actual =
        "sha256:" + digestHex(readBounded(absolute, limitsFor(ArtifactKind::Lock).maximumBytes));
    if (actual != digest) {
        fail("artifact-evidence", location, "referenced bytes differ from the recorded sha256");
    }
}

void verifyProductionArtifactReference(const Value& reference, const std::string& location,
                                       const Path& root) {
    object(reference, {"path", "sha256"}, location);
    const auto& path = portablePath(reference.at("path"), location + ".path");
    const auto& digest = digestValue(reference.at("sha256"), location + ".sha256");
    resolveProductionEvidence(path, digest, location, root, {}, false);
}

void validateProductionArtifactArray(const Value& value, const std::string& location,
                                     const Path& root, const std::size_t minimum) {
    const auto& values = array(value, location, 8192, minimum);
    for (std::size_t index = 0; index < values.size(); ++index) {
        verifyProductionArtifactReference(values[index],
                                          location + '[' + std::to_string(index) + ']', root);
    }
    requireOrdered(
        values, [](const Value& child) { return child.at("path").asString(); }, location);
}

void validateProductionProvenance(const Value& source, const std::string& location,
                                  const Path& root) {
    const auto& provenance = array(source.at("provenance"), location + ".provenance", 8192);
    for (std::size_t index = 0; index < provenance.size(); ++index) {
        const auto itemLocation = location + ".provenance[" + std::to_string(index) + ']';
        object(provenance[index], {"kind", "evidence", "identity", "issuer", "policy"},
               itemLocation);
        enumString(provenance[index].at("kind"),
                   {"detached-signature", "sigstore-bundle", "signed-tag"}, itemLocation + ".kind");
        verifyProductionArtifactReference(provenance[index].at("evidence"),
                                          itemLocation + ".evidence", root);
        stringValue(provenance[index].at("identity"), itemLocation + ".identity");
        nullableString(provenance[index].at("issuer"), itemLocation + ".issuer");
        stringValue(provenance[index].at("policy"), itemLocation + ".policy");
    }
    requireOrdered(
        provenance,
        [](const Value& child) {
            return std::pair(child.at("kind").asString(),
                             child.at("evidence").at("path").asString());
        },
        location + ".provenance");
    const auto& policy = enumString(source.at("provenancePolicy"), {"required", "not-published"},
                                    location + ".provenancePolicy");
    if ((policy == "required") != !provenance.empty()) {
        fail("provenance-policy", location, "policy and evidence presence disagree");
    }
}

void validateProductionLicense(const Value& license, const std::string& location,
                               const Path& root) {
    object(license,
           {"spdxExpression", "licenseFiles", "copyrightFiles", "noticeFiles", "sourceObligation",
            "modified", "reviewRecord", "reviewedAt"},
           location);
    stringValue(license.at("spdxExpression"), location + ".spdxExpression");
    validateProductionArtifactArray(license.at("licenseFiles"), location + ".licenseFiles", root,
                                    1);
    validateProductionArtifactArray(license.at("copyrightFiles"), location + ".copyrightFiles",
                                    root, 0);
    validateProductionArtifactArray(license.at("noticeFiles"), location + ".noticeFiles", root, 0);
    enumString(license.at("sourceObligation"),
               {"none", "ship-corresponding-source", "ship-source-offer"},
               location + ".sourceObligation");
    static_cast<void>(booleanValue(license.at("modified"), location + ".modified"));
    verifyProductionArtifactReference(license.at("reviewRecord"), location + ".reviewRecord", root);
    validateDate(license.at("reviewedAt"), location + ".reviewedAt");
}

void validateProductionPatches(const Value& component, const std::string& componentName,
                               const std::string& location, const Path& root, const bool modified) {
    const auto& patches = array(component.at("patches"), location + ".patches", 8192);
    for (std::size_t index = 0; index < patches.size(); ++index) {
        const auto itemLocation = location + ".patches[" + std::to_string(index) + ']';
        object(patches[index], {"path", "sha256", "applyOrder", "reason"}, itemLocation);
        const auto& path = portablePath(patches[index].at("path"), itemLocation + ".path");
        const auto& patchDigest =
            digestValue(patches[index].at("sha256"), itemLocation + ".sha256");
        resolveProductionEvidence(path, patchDigest, itemLocation, root, componentName, true);
        if (uintValue(patches[index].at("applyOrder"), itemLocation + ".applyOrder",
                      std::numeric_limits<std::uint32_t>::max()) != index) {
            fail("patch-order", itemLocation + ".applyOrder", "apply order must equal index");
        }
        stringValue(patches[index].at("reason"), itemLocation + ".reason");
    }
    if (modified != !patches.empty()) {
        fail("modified", location, "patch presence and modified flag disagree");
    }
}

void validateComponentProduction(const Value& value, const std::string& location,
                                 const Path& root) {
    object(value,
           {"name", "version", "source", "license", "patches", "dependencies", "profileBuilds",
            "securityReview"},
           location);
    const auto& componentName = identifier(value.at("name"), location + ".name");
    stringValue(value.at("version"), location + ".version");

    const auto& source = value.at("source");
    object(source,
           {"url", "archiveSha256", "commit", "provenancePolicy", "provenanceReview", "provenance"},
           location + ".source");
    stringValue(source.at("url"), location + ".source.url");
    digestValue(source.at("archiveSha256"), location + ".source.archiveSha256");
    nullableString(source.at("commit"), location + ".source.commit");
    verifyProductionArtifactReference(source.at("provenanceReview"),
                                      location + ".source.provenanceReview", root);
    validateProductionProvenance(source, location, root);

    validateProductionLicense(value.at("license"), location + ".license", root);
    validateProductionPatches(
        value, componentName, location, root,
        booleanValue(value.at("license").at("modified"), location + ".license.modified"));

    const auto& dependencies = array(value.at("dependencies"), location + ".dependencies", 8192);
    for (std::size_t index = 0; index < dependencies.size(); ++index) {
        const auto itemLocation = location + ".dependencies[" + std::to_string(index) + ']';
        object(dependencies[index], {"name", "relationship"}, itemLocation);
        identifier(dependencies[index].at("name"), itemLocation + ".name");
        enumString(dependencies[index].at("relationship"),
                   {"build", "link", "runtime-plugin", "vendored"}, itemLocation + ".relationship");
    }
    requireOrdered(
        dependencies,
        [](const Value& child) {
            return std::pair(child.at("name").asString(), child.at("relationship").asString());
        },
        location + ".dependencies");

    const auto& builds = array(value.at("profileBuilds"), location + ".profileBuilds", 256, 1);
    for (std::size_t index = 0; index < builds.size(); ++index) {
        const auto buildLocation = location + ".profileBuilds[" + std::to_string(index) + ']';
        const auto& build = builds[index];
        object(build,
               {"profileId", "linkage", "cmakeOptions", "featureDecisions", "capabilities",
                "shippingRoles", "conformanceFixtureSets"},
               buildLocation);
        identifier(build.at("profileId"), buildLocation + ".profileId");
        enumString(build.at("linkage"),
                   {"dynamic", "static", "header-only", "executable", "data-only"},
                   buildLocation + ".linkage");

        const auto& options =
            array(build.at("cmakeOptions"), buildLocation + ".cmakeOptions", 8192);
        for (std::size_t optionIndex = 0; optionIndex < options.size(); ++optionIndex) {
            const auto itemLocation =
                buildLocation + ".cmakeOptions[" + std::to_string(optionIndex) + ']';
            object(options[optionIndex], {"name", "value"}, itemLocation);
            const auto& name =
                stringValue(options[optionIndex].at("name"), itemLocation + ".name", 256);
            if (!isCmakeOption(name)) {
                fail("lexical", itemLocation + ".name", "invalid CMake option name");
            }
            stringValue(options[optionIndex].at("value"), itemLocation + ".value");
        }
        requireOrdered(
            options, [](const Value& child) { return child.at("name").asString(); },
            buildLocation + ".cmakeOptions");

        // Deferred rule: feature decisions closed against the owning recipe's vocabulary. See
        // production_lock_checks.hpp; only shape, enums, and ordering are checked here.
        const auto& decisions =
            array(build.at("featureDecisions"), buildLocation + ".featureDecisions", 8192);
        for (std::size_t decisionIndex = 0; decisionIndex < decisions.size(); ++decisionIndex) {
            const auto itemLocation =
                buildLocation + ".featureDecisions[" + std::to_string(decisionIndex) + ']';
            object(decisions[decisionIndex], {"id", "state", "reason"}, itemLocation);
            identifier(decisions[decisionIndex].at("id"), itemLocation + ".id");
            enumString(decisions[decisionIndex].at("state"), {"enabled", "disabled"},
                       itemLocation + ".state");
            stringValue(decisions[decisionIndex].at("reason"), itemLocation + ".reason");
        }
        requireOrdered(
            decisions, [](const Value& child) { return child.at("id").asString(); },
            buildLocation + ".featureDecisions");

        const auto& capabilities =
            array(build.at("capabilities"), buildLocation + ".capabilities", 65'536);
        for (std::size_t capabilityIndex = 0; capabilityIndex < capabilities.size();
             ++capabilityIndex) {
            identifier(capabilities[capabilityIndex],
                       buildLocation + ".capabilities[" + std::to_string(capabilityIndex) + ']');
        }
        requireOrdered(
            capabilities, [](const Value& child) { return child.asString(); },
            buildLocation + ".capabilities");

        const auto& roles =
            array(build.at("shippingRoles"), buildLocation + ".shippingRoles", 8, 1);
        for (std::size_t roleIndex = 0; roleIndex < roles.size(); ++roleIndex) {
            enumString(roles[roleIndex],
                       {"library", "executable", "plugin", "data", "cmake-package", "license",
                        "notice", "source"},
                       buildLocation + ".shippingRoles[" + std::to_string(roleIndex) + ']');
        }
        requireOrdered(
            roles, [](const Value& child) { return child.asString(); },
            buildLocation + ".shippingRoles");

        const auto& fixtureSets = array(build.at("conformanceFixtureSets"),
                                        buildLocation + ".conformanceFixtureSets", 8192);
        for (std::size_t fixtureIndex = 0; fixtureIndex < fixtureSets.size(); ++fixtureIndex) {
            const auto itemLocation =
                buildLocation + ".conformanceFixtureSets[" + std::to_string(fixtureIndex) + ']';
            object(fixtureSets[fixtureIndex], {"id", "artifact"}, itemLocation);
            identifier(fixtureSets[fixtureIndex].at("id"), itemLocation + ".id");
            verifyProductionArtifactReference(fixtureSets[fixtureIndex].at("artifact"),
                                              itemLocation + ".artifact", root);
        }
        requireOrdered(
            fixtureSets, [](const Value& child) { return child.at("id").asString(); },
            buildLocation + ".conformanceFixtureSets");
    }
    requireOrdered(
        builds, [](const Value& child) { return child.at("profileId").asString(); },
        location + ".profileBuilds");

    const auto& security = value.at("securityReview");
    object(security, {"reviewedAt", "record", "vulnerabilities"}, location + ".securityReview");
    validateDate(security.at("reviewedAt"), location + ".securityReview.reviewedAt");
    verifyProductionArtifactReference(security.at("record"), location + ".securityReview.record",
                                      root);
    const auto& vulnerabilities =
        array(security.at("vulnerabilities"), location + ".securityReview.vulnerabilities", 8192);
    for (std::size_t index = 0; index < vulnerabilities.size(); ++index) {
        const auto itemLocation =
            location + ".securityReview.vulnerabilities[" + std::to_string(index) + ']';
        object(vulnerabilities[index], {"id", "disposition", "record"}, itemLocation);
        identifier(vulnerabilities[index].at("id"), itemLocation + ".id");
        enumString(vulnerabilities[index].at("disposition"),
                   {"not-affected", "mitigated", "accepted-risk"}, itemLocation + ".disposition");
        verifyProductionArtifactReference(vulnerabilities[index].at("record"),
                                          itemLocation + ".record", root);
    }
    requireOrdered(
        vulnerabilities, [](const Value& child) { return child.at("id").asString(); },
        location + ".securityReview.vulnerabilities");
}

void validateProductionUnicodeProfile(const Value& unicode) {
    object(unicode, {"version", "sourceUrl", "archiveSha256", "files"}, "$.unicodeProfile");
    if (!unicode.at("version").isString() ||
        unicode.at("version").asString() != kUnicodeBootstrapVersion) {
        fail("unicode-version", "$.unicodeProfile.version",
             "expected the bootstrap Unicode version 15.1.0");
    }
    const auto mismatch = [location = std::string_view("$.unicodeProfile")]() {
        fail("unicode-profile", location,
             "lock Unicode profile differs from the reviewed bootstrap allowlist member");
    };
    if (!unicode.at("sourceUrl").isString() ||
        unicode.at("sourceUrl").asString() != kUnicodeBootstrapSourceUrl ||
        !unicode.at("archiveSha256").isString() ||
        unicode.at("archiveSha256").asString() !=
            "sha256:" + std::string(kUnicodeBootstrapArchiveHex)) {
        mismatch();
    }
    const auto& files = array(unicode.at("files"), "$.unicodeProfile.files", 5, 5);
    for (std::size_t index = 0; index < files.size(); ++index) {
        const auto location = "$.unicodeProfile.files[" + std::to_string(index) + ']';
        object(files[index], {"path", "sha256"}, location);
        if (!files[index].at("path").isString() ||
            files[index].at("path").asString() != kUnicodeBootstrapFiles[index].name ||
            !files[index].at("sha256").isString() ||
            files[index].at("sha256").asString() !=
                "sha256:" + std::string(kUnicodeBootstrapFiles[index].sha256Hex)) {
            fail("unicode-profile", location,
                 "Unicode file record differs from the fixed bootstrap order and digests");
        }
    }
}

} // namespace

auto sha256DigestText(const std::string_view bytes) -> std::string {
    const auto digest =
        core::Sha256Hasher::hash(std::as_bytes(std::span{bytes.data(), bytes.size()}));
    if (!digest.has_value()) {
        fail("sha256", "$", "input exceeds the SHA-256 implementation limit");
    }
    const auto hexadecimal = digest->toLowercaseHex();
    return "sha256:" + std::string(hexadecimal.data(), hexadecimal.size());
}

void verifyUnicodeBootstrap(const std::filesystem::path& root) { verifyBootstrapTables(root); }

void validateProductionLockDocument(const Value& value, const Path& root) {
    object(value, {"format", "schemaVersion", "unicodeProfile", "profiles", "components"}, "$");
    if (!value.at("format").isString() ||
        value.at("format").asString() != "org.kinetik.bloom.dependencies.lock") {
        fail("format", "$.format", "unexpected lock format");
    }
    const auto& version = value.at("schemaVersion");
    object(version, {"major", "minor"}, "$.schemaVersion");
    if (!version.at("major").isNumber() || version.at("major").asNumber().spelling != "1" ||
        !version.at("minor").isNumber() || version.at("minor").asNumber().spelling != "0") {
        fail("version", "$.schemaVersion", "expected exact version 1.0");
    }

    validateProductionUnicodeProfile(value.at("unicodeProfile"));

    const auto& profiles = array(value.at("profiles"), "$.profiles", 256, 1);
    for (std::size_t index = 0; index < profiles.size(); ++index) {
        validateProfile(profiles[index], "$.profiles[" + std::to_string(index) + ']');
    }
    requireOrdered(
        profiles, [](const Value& child) { return child.at("id").asString(); }, "$.profiles");

    const auto& components = array(value.at("components"), "$.components", 4096, 1);
    for (std::size_t index = 0; index < components.size(); ++index) {
        validateComponentProduction(components[index],
                                    "$.components[" + std::to_string(index) + ']', root);
    }
    requireOrdered(
        components, [](const Value& child) { return child.at("name").asString(); }, "$.components");

    std::set<std::string> profileIds;
    for (const auto& profile : profiles) {
        profileIds.insert(profile.at("id").asString());
    }
    std::set<std::string> componentNames;
    for (const auto& component : components) {
        componentNames.insert(component.at("name").asString());
    }
    for (const auto& component : components) {
        const auto& componentName = component.at("name").asString();
        for (const auto& build : component.at("profileBuilds").asArray()) {
            if (!profileIds.contains(build.at("profileId").asString())) {
                fail("profile-reference", "component " + componentName,
                     build.at("profileId").asString());
            }
        }
        for (const auto& dependency : component.at("dependencies").asArray()) {
            const auto& dependencyName = dependency.at("name").asString();
            if (!componentNames.contains(dependencyName) || dependencyName == componentName) {
                fail("dependency-reference", "component " + componentName, dependencyName);
            }
        }
    }

    for (const auto& profile : profiles) {
        const auto& profileId = profile.at("id").asString();
        std::map<std::string, const Value*> participating;
        for (const auto& component : components) {
            const auto participates = std::ranges::any_of(
                component.at("profileBuilds").asArray(), [&profileId](const Value& build) {
                    return build.at("profileId").asString() == profileId;
                });
            if (participates) {
                participating.emplace(component.at("name").asString(), &component);
            }
        }
        if (participating.empty()) {
            fail("profile-empty", "profile " + profileId, "no participating component");
        }
        std::map<std::string, std::string> capabilityOwners;
        for (const auto& [name, component] : participating) {
            for (const auto& dependency : component->at("dependencies").asArray()) {
                if (!participating.contains(dependency.at("name").asString())) {
                    fail("dependency-closure", "profile " + profileId, name);
                }
            }
            const auto build = std::ranges::find_if(
                component->at("profileBuilds").asArray(), [&profileId](const Value& candidate) {
                    return candidate.at("profileId").asString() == profileId;
                });
            for (const auto& capability : build->at("capabilities").asArray()) {
                const auto [entry, inserted] =
                    capabilityOwners.emplace(capability.asString(), name);
                static_cast<void>(entry);
                if (!inserted) {
                    fail("capability-owner", "profile " + profileId, capability.asString());
                }
            }
        }

        std::set<std::string> visiting;
        std::set<std::string> visited;
        std::function<void(const std::string&)> visit = [&](const std::string& name) {
            if (visiting.contains(name)) {
                fail("dependency-cycle", "profile " + profileId, name);
            }
            if (visited.contains(name)) {
                return;
            }
            visiting.insert(name);
            for (const auto& edge : participating.at(name)->at("dependencies").asArray()) {
                visit(edge.at("name").asString());
            }
            visiting.erase(name);
            visited.insert(name);
        };
        for (const auto& [name, component] : participating) {
            static_cast<void>(component);
            visit(name);
        }
    }
}

auto validateProductionLock(const std::filesystem::path& inputRoot) -> ProductionLockResult {
    std::error_code error;
    const auto root = std::filesystem::canonical(inputRoot, error);
    if (error) {
        fail("repository-root", inputRoot.string(), error.message());
    }
    verifyUnicodeBootstrap(root);
    const Path expected = root / Path{kProductionLockRepositoryPath};
    std::error_code lockError;
    const auto lockStatus = std::filesystem::symlink_status(expected, lockError);
    if (lockError && lockError != std::errc::no_such_file_or_directory) {
        fail("production-path", expected.string(), lockError.message());
    }
    if (lockError != std::error_code{} || !std::filesystem::exists(lockStatus)) {
        // While no production lock file exists, validation reports absence as success; the
        // Unicode bootstrap self-check above has always run by this point.
        return {};
    }
    if (!std::filesystem::is_regular_file(lockStatus)) {
        fail("production-path", expected.string(), "the production lock must be a regular file");
    }
    rejectFixtureAsProductionPath(expected, expected, root / Path{kFixtureDirectory});
    const auto encoded = readBounded(expected, limitsFor(ArtifactKind::Lock).maximumBytes);
    requireAsciiStrict(encoded);
    const auto value = parseCanonicalFixture(encoded, ArtifactKind::Lock);
    validateProductionLockDocument(value, root);
    return {.present = true, .identity = identityVector("bloom.dependencies.lock.v1", encoded)};
}

} // namespace bloom::quality::dependencies
