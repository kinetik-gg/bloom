#include "dependency_artifact_checks_internal.hpp"

#include <algorithm>

#include <map>

#include <optional>

#include <ranges>

#include <set>

#include <vector>

namespace bloom::quality::dependencies {

using namespace detail;

void validatePrefixFixture(const Value& value, const Value& lock,
                           const std::string_view lockEncoded, const FixtureContext& context) {
    object(value,
           {"format", "schemaVersion", "lockIdentity", "profile", "unicodeProfile", "components",
            "capabilities", "cmakePackages", "installedFiles", "qualificationResults"},
           "$");
    if (!value.at("format").isString() ||
        value.at("format").asString() != "org.kinetik.bloom.dependencies.prefix") {
        fail("format", "$.format", "unexpected prefix format");
    }
    const auto& version = value.at("schemaVersion");
    object(version, {"major", "minor"}, "$.schemaVersion");
    if (!version.at("major").isNumber() || version.at("major").asNumber().spelling != "1" ||
        !version.at("minor").isNumber() || version.at("minor").asNumber().spelling != "0") {
        fail("version", "$.schemaVersion", "expected exact version 1.0");
    }
    const auto expectedLock = identityVector("bloom.dependencies.lock.v1", lockEncoded);
    if (!value.at("lockIdentity").isString() ||
        value.at("lockIdentity").asString() != expectedLock) {
        fail("lock-identity", "$.lockIdentity", "prefix does not bind exact lock bytes");
    }

    const Value* matchingProfile = nullptr;
    for (const auto& profile : lock.at("profiles").asArray()) {
        if (strictEqual(value.at("profile"), profile)) {
            if (matchingProfile != nullptr) {
                fail("lock-copy", "$.profile", "profile copy is ambiguous");
            }
            matchingProfile = &profile;
        }
    }
    if (matchingProfile == nullptr ||
        !strictEqual(value.at("unicodeProfile"), lock.at("unicodeProfile"))) {
        fail("lock-copy", "$", "profile or Unicode profile differs from lock");
    }
    const auto& profileId = matchingProfile->at("id").asString();

    std::map<std::string, const Value*> lockedComponents;
    for (const auto& component : lock.at("components").asArray()) {
        if (std::ranges::any_of(component.at("profileBuilds").asArray(),
                                [&profileId](const Value& build) {
                                    return build.at("profileId").asString() == profileId;
                                })) {
            lockedComponents.emplace(component.at("name").asString(), &component);
        }
    }

    const auto& components = array(value.at("components"), "$.components", 4096, 1);
    for (std::size_t index = 0; index < components.size(); ++index) {
        const auto location = "$.components[" + std::to_string(index) + ']';
        const auto& record = components[index];
        object(record,
               {"name", "version", "sourceArchiveSha256", "sourceCommit", "patches",
                "buildOptionsIdentity"},
               location);
        const auto& name = identifier(record.at("name"), location + ".name");
        const auto locked = lockedComponents.find(name);
        if (locked == lockedComponents.end()) {
            fail("component-coverage", location + ".name", "component is not in locked profile");
        }
        const auto build = std::ranges::find_if(
            locked->second->at("profileBuilds").asArray(), [&profileId](const Value& candidate) {
                return candidate.at("profileId").asString() == profileId;
            });
        if (!strictEqual(record.at("version"), locked->second->at("version")) ||
            !strictEqual(record.at("sourceArchiveSha256"),
                         locked->second->at("source").at("archiveSha256")) ||
            !strictEqual(record.at("sourceCommit"), locked->second->at("source").at("commit")) ||
            !strictEqual(record.at("patches"), locked->second->at("patches")) ||
            !record.at("buildOptionsIdentity").isString() ||
            record.at("buildOptionsIdentity").asString() != buildIdentityVector(*build)) {
            fail("component-copy", location, "result differs from the exact lock component");
        }
    }
    requireOrdered(
        components, [](const Value& child) { return child.at("name").asString(); }, "$.components");
    std::vector<std::string> componentNames;
    componentNames.reserve(components.size());
    for (const auto& component : components) {
        componentNames.push_back(component.at("name").asString());
    }
    std::vector<std::string> expectedNames;
    for (const auto& [name, component] : lockedComponents) {
        static_cast<void>(component);
        expectedNames.push_back(name);
    }
    if (componentNames != expectedNames) {
        fail("component-coverage", "$.components", "must cover all participating components");
    }

    std::vector<std::pair<std::string, std::string>> expectedCapabilities;
    for (const auto& [name, component] : lockedComponents) {
        const auto build = std::ranges::find_if(
            component->at("profileBuilds").asArray(), [&profileId](const Value& candidate) {
                return candidate.at("profileId").asString() == profileId;
            });
        for (const auto& capability : build->at("capabilities").asArray()) {
            expectedCapabilities.emplace_back(capability.asString(), name);
        }
    }
    std::ranges::sort(expectedCapabilities);
    const auto& capabilities = array(value.at("capabilities"), "$.capabilities", 65'536);
    std::vector<std::pair<std::string, std::string>> actualCapabilities;
    for (std::size_t index = 0; index < capabilities.size(); ++index) {
        const auto location = "$.capabilities[" + std::to_string(index) + ']';
        object(capabilities[index], {"id", "providerComponent"}, location);
        actualCapabilities.emplace_back(identifier(capabilities[index].at("id"), location + ".id"),
                                        identifier(capabilities[index].at("providerComponent"),
                                                   location + ".providerComponent"));
    }
    requireOrdered(
        capabilities, [](const Value& child) { return child.at("id").asString(); },
        "$.capabilities");
    if (actualCapabilities != expectedCapabilities) {
        fail("capability-copy", "$.capabilities", "capability union differs from lock");
    }

    const auto& installed = array(value.at("installedFiles"), "$.installedFiles", 200'000);
    std::map<std::string, const Value*> installedByPath;
    std::uint64_t totalBytes = 0;
    for (std::size_t index = 0; index < installed.size(); ++index) {
        const auto location = "$.installedFiles[" + std::to_string(index) + ']';
        const auto& record = installed[index];
        object(record,
               {"path", "type", "component", "role", "size", "sha256", "permissions", "linkTarget"},
               location);
        const auto& path = portablePath(record.at("path"), location + ".path");
        const auto& entryType = enumString(
            record.at("type"), {"regular-file", "directory", "symbolic-link"}, location + ".type");
        if (!record.at("component").isNull()) {
            const auto& component = identifier(record.at("component"), location + ".component");
            if (!lockedComponents.contains(component)) {
                fail("installed-component", location + ".component",
                     "component is not participating");
            }
        }
        const auto& role =
            enumString(record.at("role"),
                       {"directory", "library", "executable", "plugin", "data", "cmake-package",
                        "license", "notice", "source", "qualification-evidence"},
                       location + ".role");
        if (record.at("component").isNull() && role != "directory" &&
            role != "qualification-evidence") {
            fail("installed-component", location, "prefix-wide entry cannot own shipping role");
        }
        if (entryType == "regular-file") {
            const auto size = uintValue(record.at("size"), location + ".size", 4'294'967'296ULL);
            const auto& fileDigest =
                digestValue(record.at("sha256"), location + ".sha256", &context);
            const auto expectedSize = context.payloadSizes.find(fileDigest);
            if (expectedSize == context.payloadSizes.end() || expectedSize->second != size) {
                fail("fixture-size", location + ".size", "size differs from fixture payload");
            }
            if (size > 17'179'869'184ULL - totalBytes) {
                fail("installed-bytes", "$.installedFiles", "aggregate exceeds 16 GiB");
            }
            totalBytes += size;
            if (!record.at("permissions").isString() ||
                (record.at("permissions").asString() != "regular" &&
                 record.at("permissions").asString() != "executable") ||
                !record.at("linkTarget").isNull() || role == "directory") {
                fail("installed-tuple", location, "invalid regular-file tuple");
            }
        } else if (entryType == "directory") {
            if (!record.at("size").isNull() || !record.at("sha256").isNull() ||
                !record.at("permissions").isString() ||
                record.at("permissions").asString() != "none" ||
                !record.at("linkTarget").isNull() || role != "directory") {
                fail("installed-tuple", location, "invalid directory tuple");
            }
        } else {
            fail("fixture-symlink", location,
                 "synthetic fixtures cannot establish production link evidence");
        }
        installedByPath.insert_or_assign(path, &record);
    }
    requireOrdered(
        installed, [](const Value& child) { return child.at("path").asString(); },
        "$.installedFiles");

    std::map<std::string, const Value*> collisionRecords;
    for (const auto& record : installed) {
        const auto key = lowerAscii(record.at("path").asString());
        if (!collisionRecords.emplace(key, &record).second) {
            fail("portable-collision", record.at("path").asString(), "installed path collision");
        }
    }
    for (const auto& [key, record] : collisionRecords) {
        std::size_t separator = key.find('/');
        while (separator != std::string::npos) {
            const auto parent = collisionRecords.find(key.substr(0, separator));
            if (parent != collisionRecords.end() &&
                parent->second->at("type").asString() != "directory") {
                fail("path-prefix", record->at("path").asString(),
                     "non-directory installed path is an ancestor");
            }
            separator = key.find('/', separator + 1);
        }
    }

    struct ExpectedCopy final {
        std::string path;
        std::string digest;
        std::string role;
        std::optional<std::string> component;
    };
    std::vector<ExpectedCopy> expectedCopies;
    for (const auto& [name, component] : lockedComponents) {
        for (const auto& artifact : componentArtifacts(*component)) {
            constexpr std::string_view dependenciesPrefix = "dependencies/";
            const auto relative = std::string_view(artifact.path).substr(dependenciesPrefix.size());
            expectedCopies.push_back({"share/bloom/dependencies/evidence/" + std::string(relative),
                                      artifact.digest, artifact.role, name});
        }
    }
    for (const auto& file : lock.at("unicodeProfile").at("files").asArray()) {
        expectedCopies.push_back(
            {"share/bloom/dependencies/unicode/15.1.0/" + file.at("path").asString(),
             file.at("sha256").asString(), "qualification-evidence", std::nullopt});
    }
    for (const auto& expected : expectedCopies) {
        const auto found = installedByPath.find(expected.path);
        if (found == installedByPath.end()) {
            fail("evidence-copy", expected.path, "required evidence copy is missing");
        }
        const auto& record = *found->second;
        const bool componentMatches =
            expected.component.has_value()
                ? record.at("component").isString() &&
                      record.at("component").asString() == *expected.component
                : record.at("component").isNull();
        if (record.at("type").asString() != "regular-file" ||
            record.at("sha256").asString() != expected.digest ||
            record.at("role").asString() != expected.role || !componentMatches) {
            fail("evidence-copy", expected.path, "required evidence copy differs");
        }
    }

    const auto& packages = array(value.at("cmakePackages"), "$.cmakePackages", 4096);
    for (std::size_t index = 0; index < packages.size(); ++index) {
        const auto location = "$.cmakePackages[" + std::to_string(index) + ']';
        const auto& package = packages[index];
        object(package, {"name", "version", "configPath", "targets"}, location);
        stringValue(package.at("name"), location + ".name");
        stringValue(package.at("version"), location + ".version");
        const auto& configPath = portablePath(package.at("configPath"), location + ".configPath");
        const auto configRecord = installedByPath.find(configPath);
        if (configRecord == installedByPath.end() ||
            configRecord->second->at("type").asString() != "regular-file" ||
            configRecord->second->at("role").asString() != "cmake-package") {
            fail("cmake-config", location + ".configPath",
                 "config path must name an installed CMake package");
        }
        const auto& targets = array(package.at("targets"), location + ".targets", 8192, 1);
        for (std::size_t targetIndex = 0; targetIndex < targets.size(); ++targetIndex) {
            const auto itemLocation = location + ".targets[" + std::to_string(targetIndex) + ']';
            validatePrintableToken(stringValue(targets[targetIndex], itemLocation, 256),
                                   itemLocation);
        }
        requireOrdered(
            targets, [](const Value& child) { return child.asString(); }, location + ".targets");
    }
    requireOrdered(
        packages, [](const Value& child) { return child.at("name").asString(); },
        "$.cmakePackages");

    std::map<std::string, const Value*> gates;
    for (const auto& gate : matchingProfile->at("qualificationGates").asArray()) {
        gates.emplace(gate.at("gateId").asString(), &gate);
    }
    const auto& results =
        array(value.at("qualificationResults"), "$.qualificationResults", 4096, 1);
    const auto completed =
        timestampFromEpoch(uintValue(matchingProfile->at("environment").at("sourceDateEpoch"),
                                     "$.profile.environment.sourceDateEpoch", 253'402'300'799ULL));
    for (std::size_t index = 0; index < results.size(); ++index) {
        const auto location = "$.qualificationResults[" + std::to_string(index) + ']';
        const auto& result = results[index];
        object(result, {"gateId", "status", "evidence", "completedAt"}, location);
        const auto& gateId = identifier(result.at("gateId"), location + ".gateId");
        const auto gate = gates.find(gateId);
        if (gate == gates.end()) {
            fail("gate-coverage", location + ".gateId", "result does not map to locked gate");
        }
        const auto& status = enumString(result.at("status"), {"passed", "failed", "not-applicable"},
                                        location + ".status");
        const auto* expectedStatus =
            gate->second->at("disposition").asString() == "required" ? "passed" : "not-applicable";
        if (status != expectedStatus || !result.at("completedAt").isString() ||
            result.at("completedAt").asString() != completed) {
            fail("qualification", location, "status or deterministic timestamp mismatch");
        }
        validatePrefixArtifactArray(result.at("evidence"), location + ".evidence", context, 1);
        for (const auto& reference : result.at("evidence").asArray()) {
            const auto found = installedByPath.find(reference.at("path").asString());
            if (found == installedByPath.end() ||
                found->second->at("type").asString() != "regular-file" ||
                found->second->at("role").asString() != "qualification-evidence" ||
                found->second->at("sha256").asString() != reference.at("sha256").asString()) {
                fail("gate-evidence", reference.at("path").asString(),
                     "gate evidence differs from installed qualification evidence");
            }
        }
    }
    requireOrdered(
        results, [](const Value& child) { return child.at("gateId").asString(); },
        "$.qualificationResults");
    std::vector<std::string> resultIds;
    for (const auto& result : results) {
        resultIds.push_back(result.at("gateId").asString());
    }
    std::vector<std::string> gateIds;
    for (const auto& [id, gate] : gates) {
        static_cast<void>(gate);
        gateIds.push_back(id);
    }
    if (resultIds != gateIds) {
        fail("gate-coverage", "$.qualificationResults", "gate IDs differ from lock");
    }

    for (const auto& [name, component] : lockedComponents) {
        const auto build = std::ranges::find_if(
            component->at("profileBuilds").asArray(), [&profileId](const Value& candidate) {
                return candidate.at("profileId").asString() == profileId;
            });
        std::set<std::string> expectedRoles;
        for (const auto& role : build->at("shippingRoles").asArray()) {
            expectedRoles.insert(role.asString());
        }
        std::set<std::string> actualRoles;
        for (const auto& entry : installed) {
            if (entry.at("component").isString() && entry.at("component").asString() == name &&
                entry.at("role").asString() != "directory" &&
                entry.at("role").asString() != "qualification-evidence") {
                actualRoles.insert(entry.at("role").asString());
            }
        }
        if (actualRoles != expectedRoles) {
            fail("shipping-role", "component " + name, "installed shipping roles differ from lock");
        }
    }
}
} // namespace bloom::quality::dependencies
