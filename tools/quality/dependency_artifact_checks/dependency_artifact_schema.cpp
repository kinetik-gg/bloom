#include "dependency_artifact_checks_internal.hpp"

#include <array>

#include <functional>

#include <tuple>

#include <system_error>

namespace bloom::quality::dependencies {

using namespace detail;

auto checkRepository(const Path& inputRoot) -> CheckResult {
    std::error_code error;
    const auto root = std::filesystem::canonical(inputRoot, error);
    if (error) {
        fail("repository-root", inputRoot.string(), error.message());
    }
    const std::array schemaArtifacts{
        std::tuple{ArtifactKind::Lock, "dependency-lock-1.0.schema.json",
                   "10ac9499cf19af39e561a47824a97d737f25fa26081a10dc8f8d9d94c831a5a0"},
        std::tuple{ArtifactKind::Prefix, "prefix-manifest-1.0.schema.json",
                   "f3e2c731497753c15ad2f8fb7d0e6325811f1681e9a5186cbc6784cb1b682386"}};
    for (const auto& [kind, name, expectedDigest] : schemaArtifacts) {
        const auto schema = loadSchemaArtifact(root / Path{kSchemaDirectory} / name);
        if (digestHex(schema.encoded) != expectedDigest) {
            fail("schema-bytes", name, "readable schema bytes differ from frozen artifact");
        }
        validateSchemaArtifact(schema.value, kind);
    }
    const auto context = makeFixtureContext(root);
    const auto lockEncoded = readBounded(root / Path{kFixtureDirectory} / "valid-lock.json",
                                         limitsFor(ArtifactKind::Lock).maximumBytes);
    const auto lock = parseCanonicalFixture(lockEncoded, ArtifactKind::Lock);
    validateLockFixture(lock, context);
    const auto prefixEncoded =
        readBounded(root / Path{kFixtureDirectory} / "valid-prefix-manifest.json",
                    limitsFor(ArtifactKind::Prefix).maximumBytes);
    const auto prefix = parseCanonicalFixture(prefixEncoded, ArtifactKind::Prefix);
    validatePrefixFixture(prefix, lock, lockEncoded, context);
    return {.lockVector = identityVector("bloom.dependencies.lock.v1", lockEncoded),
            .prefixVector = identityVector("bloom.dependencies.prefix.v1", prefixEncoded)};
}
void validateSchemaArtifact(const Value& value, const ArtifactKind kind) {
    constexpr std::string_view draft = "https://json-schema.org/draft/2020-12/schema";
    const auto* expectedId = kind == ArtifactKind::Lock
                                 ? "urn:kinetik:bloom:schema:dependency-lock:1.0"
                                 : "urn:kinetik:bloom:schema:dependency-prefix-manifest:1.0";
    const auto* schema = value.find("$schema");
    const auto* id = value.find("$id");
    if (schema == nullptr || id == nullptr || !schema->isString() || !id->isString() ||
        schema->asString() != draft || id->asString() != expectedId) {
        fail("schema-identity", "$", "expected the frozen Draft 2020-12 schema identity");
    }

    std::function<void(const Value&, const std::string&)> visit;
    visit = [&](const Value& node, const std::string& location) {
        if (node.isArray()) {
            for (std::size_t index = 0; index < node.asArray().size(); ++index) {
                visit(node.asArray()[index], location + '[' + std::to_string(index) + ']');
            }
            return;
        }
        if (!node.isObject()) {
            return;
        }
        if (const auto* reference = node.find("$ref"); reference != nullptr) {
            if (!reference->isString()) {
                fail("schema-ref", location + ".$ref", "reference must be a string");
            }
            const auto& text = reference->asString();
            const bool allowed =
                text.starts_with("#/") || (kind == ArtifactKind::Prefix &&
                                           text.starts_with("dependency-lock-1.0.schema.json#/"));
            if (!allowed) {
                fail("schema-ref", location + ".$ref", "reference is not repository-local");
            }
        }
        const auto* type = node.find("type");
        if (type != nullptr && type->isString() && type->asString() == "object") {
            const auto* properties = node.find("properties");
            const auto* required = node.find("required");
            const auto* closed = node.find("unevaluatedProperties");
            if (properties == nullptr || required == nullptr || closed == nullptr ||
                !properties->isObject() || !required->isArray() || !closed->isBoolean() ||
                closed->asBoolean() ||
                properties->asObject().size() != required->asArray().size()) {
                fail("schema-object", location,
                     "object properties, required order, and closure differ from contract");
            }
            for (std::size_t index = 0; index < properties->asObject().size(); ++index) {
                const auto& requiredName = required->asArray()[index];
                if (!requiredName.isString() ||
                    requiredName.asString() != properties->asObject()[index].first) {
                    fail("schema-object", location,
                         "required members must equal the property declaration order");
                }
            }
        }
        for (const auto& [name, child] : node.asObject()) {
            auto childLocation = location;
            childLocation.push_back('.');
            childLocation += name;
            visit(child, childLocation);
        }
    };
    visit(value, "$");

    const auto* expectedValueDigest =
        kind == ArtifactKind::Lock
            ? "5c64c0a1863181285f14c74c88cdc925e7f979cec8a6105fc24e15f24f8c8142"
            : "786926f6004bd73af98d23f01cc0d6502da7b2627468b3afd5dc264941f4d32f";
    if (digestHex(encodeCanonical(value)) != expectedValueDigest) {
        fail("schema-contract", "$", "schema value differs from the frozen artifact");
    }
}
} // namespace bloom::quality::dependencies
