#include "dependency_artifact_checks_internal.hpp"

#include <functional>

#include <map>

#include <ranges>

#include <set>

#include <vector>

namespace bloom::quality::dependencies {

using namespace detail;

void validateLockFixture(const Value& value, const FixtureContext& context) {
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

    const auto& unicode = value.at("unicodeProfile");
    object(unicode, {"version", "sourceUrl", "archiveSha256", "files"}, "$.unicodeProfile");
    if (!unicode.at("version").isString() || unicode.at("version").asString() != "15.1.0") {
        fail("unicode-version", "$.unicodeProfile.version", "expected Unicode 15.1.0");
    }
    const auto& unicodeUrl = stringValue(unicode.at("sourceUrl"), "$.unicodeProfile.sourceUrl");
    if (!unicodeUrl.starts_with("https://example.invalid/")) {
        fail("fixture-url", "$.unicodeProfile.sourceUrl", "fixture URL must use example.invalid");
    }
    const auto& archiveDigest =
        digestValue(unicode.at("archiveSha256"), "$.unicodeProfile.archiveSha256");
    verifyFixturePayload("dependencies/tests/fixtures/payloads/unicode-archive.txt", archiveDigest,
                         "$.unicodeProfile.archiveSha256", context);
    const auto& unicodeFiles = array(unicode.at("files"), "$.unicodeProfile.files", 5, 5);
    for (std::size_t index = 0; index < unicodeFiles.size(); ++index) {
        const auto location = "$.unicodeProfile.files[" + std::to_string(index) + ']';
        object(unicodeFiles[index], {"path", "sha256"}, location);
        if (!unicodeFiles[index].at("path").isString() ||
            unicodeFiles[index].at("path").asString() != kUnicodeFiles[index]) {
            fail("unicode-files", location + ".path", "Unicode file order/name differs");
        }
        const auto& fileDigest =
            digestValue(unicodeFiles[index].at("sha256"), location + ".sha256");
        verifyFixturePayload(std::string(kPayloadPrefix) + "unicode/" +
                                 std::string(kUnicodeFiles[index]),
                             fileDigest, location, context);
    }

    const auto& profiles = array(value.at("profiles"), "$.profiles", 256, 1);
    for (std::size_t index = 0; index < profiles.size(); ++index) {
        validateProfile(profiles[index], "$.profiles[" + std::to_string(index) + ']');
    }
    requireOrdered(
        profiles, [](const Value& child) { return child.at("id").asString(); }, "$.profiles");

    const auto& components = array(value.at("components"), "$.components", 4096, 1);
    for (std::size_t index = 0; index < components.size(); ++index) {
        validateComponent(components[index], "$.components[" + std::to_string(index) + ']',
                          context);
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
namespace detail {
[[nodiscard]] auto identityVector(const std::string_view domain, const std::string_view encoded)
    -> std::string {
    std::string preimage;
    preimage.reserve(domain.size() + 1 + 8 + encoded.size());
    preimage += domain;
    preimage.push_back('\0');
    const std::uint64_t length = encoded.size();
    for (int shift = 56; shift >= 0; shift -= 8) {
        preimage.push_back(static_cast<char>((length >> static_cast<unsigned>(shift)) & 0xFFU));
    }
    preimage += encoded;
    return "sha256:" + digestHex(preimage);
}

[[nodiscard]] auto buildIdentityVector(const Value& build) -> std::string {
    return identityVector("bloom.dependencies.component-build.v1", encodeCanonical(build));
}

[[nodiscard]] auto componentArtifacts(const Value& component) -> std::vector<ComponentArtifact> {
    std::vector<ComponentArtifact> result;
    const auto add = [&result](const Value& reference, const std::string_view role) {
        result.push_back({reference.at("path").asString(), reference.at("sha256").asString(),
                          std::string(role)});
    };
    const auto& source = component.at("source");
    add(source.at("provenanceReview"), "qualification-evidence");
    for (const auto& record : source.at("provenance").asArray()) {
        add(record.at("evidence"), "qualification-evidence");
    }
    const auto& license = component.at("license");
    for (const auto* field : {"licenseFiles", "copyrightFiles"}) {
        for (const auto& reference : license.at(field).asArray()) {
            add(reference, "license");
        }
    }
    for (const auto& reference : license.at("noticeFiles").asArray()) {
        add(reference, "notice");
    }
    add(license.at("reviewRecord"), "qualification-evidence");
    for (const auto& patch : component.at("patches").asArray()) {
        result.push_back(
            {patch.at("path").asString(), patch.at("sha256").asString(), "qualification-evidence"});
    }
    for (const auto& build : component.at("profileBuilds").asArray()) {
        for (const auto& fixtureSet : build.at("conformanceFixtureSets").asArray()) {
            add(fixtureSet.at("artifact"), "qualification-evidence");
        }
    }
    add(component.at("securityReview").at("record"), "qualification-evidence");
    for (const auto& vulnerability :
         component.at("securityReview").at("vulnerabilities").asArray()) {
        add(vulnerability.at("record"), "qualification-evidence");
    }
    return result;
}
void validateProfile(const Value& value, const std::string& location) {
    object(value,
           {"id", "target", "buildConfiguration", "consumerAbi", "toolchain", "environment",
            "qualificationGates"},
           location);
    identifier(value.at("id"), location + ".id");
    const auto& target = value.at("target");
    object(target, {"triple", "operatingSystem", "architecture", "minimumOsVersion"},
           location + ".target");
    stringValue(target.at("triple"), location + ".target.triple");
    const auto& system = enumString(target.at("operatingSystem"), {"linux", "macos", "windows"},
                                    location + ".target.operatingSystem");
    enumString(target.at("architecture"), {"x86_64", "aarch64"}, location + ".target.architecture");
    const auto minimumOs =
        nullableString(target.at("minimumOsVersion"), location + ".target.minimumOsVersion");
    if (minimumOs.has_value() && !isDottedVersion(*minimumOs)) {
        fail("lexical", location + ".target.minimumOsVersion", "expected dotted version");
    }
    const auto& configuration = enumString(value.at("buildConfiguration"), {"debug", "release"},
                                           location + ".buildConfiguration");

    const auto& abi = value.at("consumerAbi");
    object(abi,
           {"cxxStandard",
            "compilerFamily",
            "compilerAbi",
            "standardLibrary",
            "standardLibraryAbi",
            "cxxRuntime",
            "cxxRuntimeAbi",
            "cxxRuntimeLinkage",
            "platformRuntime",
            "platformRuntimeAbi",
            "exceptions",
            "rtti",
            "libstdcxxCxx11Abi",
            "msvcRuntime",
            "msvcIteratorDebugLevel",
            "windowsSdk",
            "windowsSdkVersion",
            "appleSdk",
            "appleSdkVersion",
            "appleDeploymentTarget",
            "abiFlags"},
           location + ".consumerAbi");
    if (uintValue(abi.at("cxxStandard"), location + ".consumerAbi.cxxStandard") != 20) {
        fail("enum", location + ".consumerAbi.cxxStandard", "expected C++20");
    }
    const auto& compiler =
        enumString(abi.at("compilerFamily"), {"gcc", "clang", "apple-clang", "msvc", "clang-cl"},
                   location + ".consumerAbi.compilerFamily");
    for (const auto* name :
         {"compilerAbi", "standardLibraryAbi", "cxxRuntimeAbi", "platformRuntimeAbi"}) {
        const auto& identityText = stringValue(abi.at(name), location + ".consumerAbi." + name);
        if (!isAscii(identityText) ||
            !std::ranges::all_of(identityText, [](const unsigned char byte) {
                return byte >= 0x20U && byte <= 0x7EU;
            })) {
            fail("ascii-identity", location + ".consumerAbi." + name,
                 "identity must use printable ASCII");
        }
    }
    const auto& library = enumString(abi.at("standardLibrary"), {"libstdc++", "libc++", "msvc-stl"},
                                     location + ".consumerAbi.standardLibrary");
    const auto& runtime = enumString(abi.at("cxxRuntime"), {"libgcc", "compiler-rt", "msvc"},
                                     location + ".consumerAbi.cxxRuntime");
    const auto& linkage = enumString(abi.at("cxxRuntimeLinkage"), {"dynamic", "static"},
                                     location + ".consumerAbi.cxxRuntimeLinkage");
    const auto& platform =
        enumString(abi.at("platformRuntime"), {"glibc", "musl", "ucrt", "macos-libsystem"},
                   location + ".consumerAbi.platformRuntime");
    booleanValue(abi.at("exceptions"), location + ".consumerAbi.exceptions");
    booleanValue(abi.at("rtti"), location + ".consumerAbi.rtti");

    const auto nullOrUnsigned = [&](const char* name, const std::set<std::uint64_t>& allowed) {
        const auto& field = abi.at(name);
        if (!field.isNull()) {
            const auto numeric = uintValue(field, location + ".consumerAbi." + name);
            if (!allowed.contains(numeric)) {
                fail("enum", location + ".consumerAbi." + name,
                     "numeric ABI value is outside the frozen enumeration");
            }
        }
    };
    nullOrUnsigned("libstdcxxCxx11Abi", {0, 1});
    nullOrUnsigned("msvcIteratorDebugLevel", {0, 1, 2});
    if (!abi.at("msvcRuntime").isNull()) {
        enumString(abi.at("msvcRuntime"),
                   {"dynamic-release", "dynamic-debug", "static-release", "static-debug"},
                   location + ".consumerAbi.msvcRuntime");
    }
    for (const auto* name : {"windowsSdk", "windowsSdkVersion", "appleSdk", "appleSdkVersion",
                             "appleDeploymentTarget"}) {
        const auto result = nullableString(abi.at(name), location + ".consumerAbi." + name);
        const std::string_view fieldName{name};
        if (result.has_value() &&
            (fieldName.find("Version") != std::string_view::npos ||
             fieldName.find("Target") != std::string_view::npos) &&
            !isDottedVersion(*result)) {
            fail("lexical", location + ".consumerAbi." + name, "expected dotted version");
        }
    }
    const auto& flags = array(abi.at("abiFlags"), location + ".consumerAbi.abiFlags", 8192);
    for (std::size_t index = 0; index < flags.size(); ++index) {
        const auto itemLocation = location + ".consumerAbi.abiFlags[" + std::to_string(index) + ']';
        validatePrintableToken(stringValue(flags[index], itemLocation, 512), itemLocation);
    }
    requireOrdered(
        flags, [](const Value& child) { return child.asString(); },
        location + ".consumerAbi.abiFlags");

    const auto& toolchain = value.at("toolchain");
    object(toolchain,
           {"cmake", "generator", "buildTool", "compiler", "linker", "standardLibrary", "sdk"},
           location + ".toolchain");
    for (const auto& [name, tool] : toolchain.asObject()) {
        auto toolLocation = location;
        toolLocation += ".toolchain.";
        toolLocation += name;
        object(tool, {"name", "version", "identity"}, toolLocation);
        for (const auto& [field, child] : tool.asObject()) {
            auto fieldLocation = toolLocation;
            fieldLocation.push_back('.');
            fieldLocation += field;
            stringValue(child, fieldLocation);
        }
    }
    const auto& sdk = toolchain.at("sdk");

    const auto& environment = value.at("environment");
    object(environment, {"profileId", "sourceDateEpoch", "variables"}, location + ".environment");
    identifier(environment.at("profileId"), location + ".environment.profileId");
    uintValue(environment.at("sourceDateEpoch"), location + ".environment.sourceDateEpoch",
              253'402'300'799ULL);
    const auto& variables =
        array(environment.at("variables"), location + ".environment.variables", 8192);
    for (std::size_t index = 0; index < variables.size(); ++index) {
        const auto itemLocation =
            location + ".environment.variables[" + std::to_string(index) + ']';
        object(variables[index], {"name", "value"}, itemLocation);
        const auto& name = stringValue(variables[index].at("name"), itemLocation + ".name", 128);
        if (!isEnvironmentName(name)) {
            fail("lexical", itemLocation + ".name", "invalid environment variable name");
        }
        stringValue(variables[index].at("value"), itemLocation + ".value");
    }
    requireOrdered(
        variables, [](const Value& child) { return child.at("name").asString(); },
        location + ".environment.variables");

    const auto& gates =
        array(value.at("qualificationGates"), location + ".qualificationGates", 4096, 1);
    for (std::size_t index = 0; index < gates.size(); ++index) {
        const auto gateLocation = location + ".qualificationGates[" + std::to_string(index) + ']';
        object(gates[index], {"gateId", "disposition", "reason"}, gateLocation);
        identifier(gates[index].at("gateId"), gateLocation + ".gateId");
        const auto& disposition =
            enumString(gates[index].at("disposition"), {"required", "not-applicable"},
                       gateLocation + ".disposition");
        const auto reason = nullableString(gates[index].at("reason"), gateLocation + ".reason");
        if ((disposition == "not-applicable") != reason.has_value()) {
            fail("gate-reason", gateLocation,
                 "reason presence must exactly match not-applicable disposition");
        }
    }
    requireOrdered(
        gates, [](const Value& child) { return child.at("gateId").asString(); },
        location + ".qualificationGates");

    const auto has = [&abi](const char* name) { return !abi.at(name).isNull(); };
    if ((library == "libstdc++") != has("libstdcxxCxx11Abi")) {
        fail("abi-platform", location + ".consumerAbi.libstdcxxCxx11Abi", "applicability mismatch");
    }
    if (system == "linux") {
        if (minimumOs.has_value() || (compiler != "gcc" && compiler != "clang") ||
            (library != "libstdc++" && library != "libc++") ||
            (runtime != "libgcc" && runtime != "compiler-rt") ||
            (platform != "glibc" && platform != "musl") || has("msvcRuntime") ||
            has("msvcIteratorDebugLevel") || has("windowsSdk") || has("windowsSdkVersion") ||
            has("appleSdk") || has("appleSdkVersion") || has("appleDeploymentTarget")) {
            fail("abi-platform", location, "Linux profile fields disagree");
        }
    } else if (system == "windows") {
        const auto expectedRuntime = linkage + '-' + configuration;
        if (!minimumOs.has_value() || (compiler != "msvc" && compiler != "clang-cl") ||
            library != "msvc-stl" || runtime != "msvc" || platform != "ucrt" ||
            !abi.at("msvcRuntime").isString() ||
            abi.at("msvcRuntime").asString() != expectedRuntime || !has("msvcIteratorDebugLevel") ||
            !abi.at("windowsSdk").isString() || abi.at("windowsSdk").asString() != "windows-sdk" ||
            !strictEqual(abi.at("windowsSdkVersion"), sdk.at("version")) ||
            sdk.at("name").asString() != "windows-sdk" || has("appleSdk") ||
            has("appleSdkVersion") || has("appleDeploymentTarget")) {
            fail("abi-platform", location, "Windows profile fields disagree");
        }
    } else if (!minimumOs.has_value() || compiler != "apple-clang" || library != "libc++" ||
               runtime != "compiler-rt" || platform != "macos-libsystem" ||
               !abi.at("appleSdk").isString() || abi.at("appleSdk").asString() != "macosx" ||
               !strictEqual(abi.at("appleSdkVersion"), sdk.at("version")) ||
               sdk.at("name").asString() != "macosx" ||
               !abi.at("appleDeploymentTarget").isString() ||
               abi.at("appleDeploymentTarget").asString() != *minimumOs || has("msvcRuntime") ||
               has("msvcIteratorDebugLevel") || has("windowsSdk") || has("windowsSdkVersion")) {
        fail("abi-platform", location, "macOS profile fields disagree");
    }
}

void validateComponent(const Value& value, const std::string& location,
                       const FixtureContext& context) {
    object(value,
           {"name", "version", "source", "license", "patches", "dependencies", "profileBuilds",
            "securityReview"},
           location);
    identifier(value.at("name"), location + ".name");
    stringValue(value.at("version"), location + ".version");

    const auto& source = value.at("source");
    object(source,
           {"url", "archiveSha256", "commit", "provenancePolicy", "provenanceReview", "provenance"},
           location + ".source");
    const auto& sourceUrl = stringValue(source.at("url"), location + ".source.url");
    if (!sourceUrl.starts_with("https://example.invalid/")) {
        fail("fixture-url", location + ".source.url", "fixture URL must use example.invalid");
    }
    const auto& sourceDigest =
        digestValue(source.at("archiveSha256"), location + ".source.archiveSha256");
    verifyFixturePayload("dependencies/tests/fixtures/payloads/source-archive.txt", sourceDigest,
                         location + ".source.archiveSha256", context);
    nullableString(source.at("commit"), location + ".source.commit");
    const auto& provenancePolicy =
        enumString(source.at("provenancePolicy"), {"required", "not-published"},
                   location + ".source.provenancePolicy");
    artifactReference(source.at("provenanceReview"), location + ".source.provenanceReview",
                      context);
    const auto& provenance = array(source.at("provenance"), location + ".source.provenance", 8192);
    for (std::size_t index = 0; index < provenance.size(); ++index) {
        const auto itemLocation = location + ".source.provenance[" + std::to_string(index) + ']';
        object(provenance[index], {"kind", "evidence", "identity", "issuer", "policy"},
               itemLocation);
        enumString(provenance[index].at("kind"),
                   {"detached-signature", "sigstore-bundle", "signed-tag"}, itemLocation + ".kind");
        artifactReference(provenance[index].at("evidence"), itemLocation + ".evidence", context);
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
        location + ".source.provenance");
    if ((provenancePolicy == "required") != !provenance.empty()) {
        fail("provenance-policy", location + ".source", "policy and evidence presence disagree");
    }

    const auto& license = value.at("license");
    object(license,
           {"spdxExpression", "licenseFiles", "copyrightFiles", "noticeFiles", "sourceObligation",
            "modified", "reviewRecord", "reviewedAt"},
           location + ".license");
    stringValue(license.at("spdxExpression"), location + ".license.spdxExpression");
    validateArtifactArray(license.at("licenseFiles"), location + ".license.licenseFiles", context,
                          1);
    validateArtifactArray(license.at("copyrightFiles"), location + ".license.copyrightFiles",
                          context);
    validateArtifactArray(license.at("noticeFiles"), location + ".license.noticeFiles", context);
    enumString(license.at("sourceObligation"),
               {"none", "ship-corresponding-source", "ship-source-offer"},
               location + ".license.sourceObligation");
    const auto modified = booleanValue(license.at("modified"), location + ".license.modified");
    artifactReference(license.at("reviewRecord"), location + ".license.reviewRecord", context);
    validateDate(license.at("reviewedAt"), location + ".license.reviewedAt");

    const auto& patches = array(value.at("patches"), location + ".patches", 8192);
    for (std::size_t index = 0; index < patches.size(); ++index) {
        const auto itemLocation = location + ".patches[" + std::to_string(index) + ']';
        object(patches[index], {"path", "sha256", "applyOrder", "reason"}, itemLocation);
        const auto& path = portablePath(patches[index].at("path"), itemLocation + ".path");
        const auto& patchDigest =
            digestValue(patches[index].at("sha256"), itemLocation + ".sha256");
        verifyFixturePayload(path, patchDigest, itemLocation, context);
        if (uintValue(patches[index].at("applyOrder"), itemLocation + ".applyOrder",
                      std::numeric_limits<std::uint32_t>::max()) != index) {
            fail("patch-order", itemLocation + ".applyOrder", "apply order must equal index");
        }
        stringValue(patches[index].at("reason"), itemLocation + ".reason");
    }
    if (modified != !patches.empty()) {
        fail("modified", location, "patch presence and modified flag disagree");
    }

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
            artifactReference(fixtureSets[fixtureIndex].at("artifact"), itemLocation + ".artifact",
                              context);
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
    artifactReference(security.at("record"), location + ".securityReview.record", context);
    const auto& vulnerabilities =
        array(security.at("vulnerabilities"), location + ".securityReview.vulnerabilities", 8192);
    for (std::size_t index = 0; index < vulnerabilities.size(); ++index) {
        const auto itemLocation =
            location + ".securityReview.vulnerabilities[" + std::to_string(index) + ']';
        object(vulnerabilities[index], {"id", "disposition", "record"}, itemLocation);
        identifier(vulnerabilities[index].at("id"), itemLocation + ".id");
        enumString(vulnerabilities[index].at("disposition"),
                   {"not-affected", "mitigated", "accepted-risk"}, itemLocation + ".disposition");
        artifactReference(vulnerabilities[index].at("record"), itemLocation + ".record", context);
    }
    requireOrdered(
        vulnerabilities, [](const Value& child) { return child.at("id").asString(); },
        location + ".securityReview.vulnerabilities");
}
} // namespace detail
} // namespace bloom::quality::dependencies
