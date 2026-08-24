#include <bloom/document/graph.hpp>
#include <bloom/document/parameter.hpp>
#include <bloom/runtime/node_definition_registry.hpp>

#include <array>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <source_location>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace {

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

[[nodiscard]] bloom::runtime::NodeDefinition customSolid(const std::uint32_t version = 1) {
    using namespace bloom;
    return {{"example.solid", version},
            runtime::NodeLoweringKind::Solid,
            {},
            {{std::string(document::kSolidSourceOutputPort), runtime::SocketValueKind::Image}},
            {{std::string(document::kSolidColorParameterRole),
              std::string(document::kSolidColorParameterSchemaKey),
              runtime::ParameterValueKind::Color4d, true}},
            std::nullopt};
}

[[nodiscard]] bloom::runtime::NodeDefinition unsupportedDefinition(std::string typeId) {
    using namespace bloom::runtime;
    return {{std::move(typeId), 1},
            NodeLoweringKind::Unsupported,
            {},
            {{"image", SocketValueKind::Image}},
            {},
            std::nullopt};
}

[[nodiscard]] bloom::runtime::NodeDefinition builtInDefinition(const std::string_view typeId,
                                                               const std::uint32_t version) {
    bloom::runtime::NodeDefinitionRegistry registry;
    if (!bloom::runtime::registerBuiltInNodeDefinitions(registry)) {
        throw std::logic_error("built-in definition fixture must register");
    }
    const auto* definition = registry.find(typeId, version);
    if (definition == nullptr) {
        throw std::logic_error("built-in definition fixture must be addressable");
    }
    return *definition;
}

void testValidationAndDuplicates(Expectations& expectations) {
    using namespace bloom::runtime;
    NodeDefinitionRegistry registry;

    auto invalid = customSolid();
    invalid.key.typeId.clear();
    expectations.expect(registry.registerDefinition(std::move(invalid)) ==
                            NodeRegistrationStatus::InvalidDefinition,
                        "invalid definitions are rejected");
    expectations.expect(registry.definitions().empty(),
                        "invalid registration leaves the registry unchanged");

    const auto original = customSolid();
    expectations.expect(registry.registerDefinition(original) == NodeRegistrationStatus::Registered,
                        "valid definition is registered");
    auto replacement = original;
    replacement.outputs.front().name = "replacement";
    expectations.expect(registry.registerDefinition(std::move(replacement)) ==
                            NodeRegistrationStatus::InvalidDefinition,
                        "malformed replacement is rejected before duplicate lookup");
    expectations.expect(registry.registerDefinition(original) ==
                            NodeRegistrationStatus::DuplicateDefinition,
                        "an exact type and version duplicate is rejected");
    expectations.expect(registry.definitions().size() == 1 &&
                            registry.definitions().front() == original,
                        "duplicate registration never replaces the original");

    expectations.expect(registry.registerDefinition(customSolid(2)) ==
                            NodeRegistrationStatus::Registered,
                        "different schema versions may coexist");
}

void testFreezeAndBuiltIns(Expectations& expectations) {
    using namespace bloom;
    runtime::NodeDefinitionRegistry registry;
    expectations.expect(runtime::registerBuiltInNodeDefinitions(registry),
                        "built-in definitions register as one startup contribution");
    expectations.expect(registry.definitions().size() == 5,
                        "startup contribution includes four lowerings and recognized Text");

    registry.freeze();
    const auto* solid =
        registry.find(document::kSolidSourceNodeType, document::kSolidSourceNodeSchemaVersion);
    expectations.expect(registry.isFrozen() && solid != nullptr,
                        "freeze publishes deterministic definitions");
    registry.freeze();
    expectations.expect(registry.find(document::kSolidSourceNodeType,
                                      document::kSolidSourceNodeSchemaVersion) == solid,
                        "idempotent freeze preserves lookup addresses");
    expectations.expect(registry.containsType(document::kTextSourceNodeType),
                        "recognized unsupported types remain discoverable");
    const auto* text =
        registry.find(document::kTextSourceNodeType, document::kTextSourceNodeSchemaVersion);
    expectations.expect(text != nullptr && text->lowering == runtime::NodeLoweringKind::Unsupported,
                        "Text is explicit unsupported capability, not an unknown node");
    const auto* layer =
        registry.find(document::kLayerOutputNodeType, document::kLayerOutputNodeSchemaVersion);
    expectations.expect(layer != nullptr && layer->parameters.size() == 2 &&
                            layer->parameters[0].supportsAnimation &&
                            layer->parameters[1].supportsAnimation && solid != nullptr &&
                            !solid->parameters.front().supportsAnimation && text != nullptr &&
                            !text->parameters.front().supportsAnimation,
                        "animation support is an explicit per-parameter evaluator capability");
    expectations.expect(registry.registerDefinition(customSolid()) ==
                            runtime::NodeRegistrationStatus::Frozen,
                        "registration is closed after freeze");
}

void testStructuralLoweringsRequireCanonicalKeys(Expectations& expectations) {
    using namespace bloom;
    struct Case final {
        std::string_view typeId;
        std::uint32_t version;
    };
    constexpr std::array cases{
        Case{document::kLayerOutputNodeType, document::kLayerOutputNodeSchemaVersion},
        Case{document::kLayerStackNodeType, document::kLayerStackNodeSchemaVersion},
        Case{document::kCompositionOutputNodeType, document::kCompositionOutputNodeSchemaVersion},
    };

    for (const auto& testCase : cases) {
        runtime::NodeDefinitionRegistry customTypeRegistry;
        auto customType = builtInDefinition(testCase.typeId, testCase.version);
        customType.key.typeId = "example.structural-spoof";
        expectations.expect(customTypeRegistry.registerDefinition(std::move(customType)) ==
                                runtime::NodeRegistrationStatus::InvalidDefinition,
                            "structural lowering rejects a custom type ID");

        runtime::NodeDefinitionRegistry customVersionRegistry;
        auto customVersion = builtInDefinition(testCase.typeId, testCase.version);
        ++customVersion.key.schemaVersion;
        expectations.expect(customVersionRegistry.registerDefinition(std::move(customVersion)) ==
                                runtime::NodeRegistrationStatus::InvalidDefinition,
                            "structural lowering rejects a non-canonical schema version");
    }

    runtime::NodeDefinitionRegistry customSolidRegistry;
    expectations.expect(customSolidRegistry.registerDefinition(customSolid(17)) ==
                            runtime::NodeRegistrationStatus::Registered,
                        "Solid remains an explicitly extensible lowering contract");
}

void testLargeFrozenRegistryLookup(Expectations& expectations) {
    using namespace bloom::runtime;
    constexpr std::size_t definitionCount = 4'096;
    NodeDefinitionRegistry registry;
    bool registered = true;
    for (std::size_t index = definitionCount; index > 0; --index) {
        const auto typeId = "example.bulk." + std::to_string(index - 1);
        registered = registered && registry.registerDefinition(unsupportedDefinition(typeId)) ==
                                       NodeRegistrationStatus::Registered;
    }
    expectations.expect(registered && registry.definitions().size() == definitionCount,
                        "large registry fixture registers in deliberately unsorted order");
    expectations.expect(registry.find("example.bulk.0", 1) != nullptr &&
                            registry.containsType("example.bulk.4095"),
                        "pre-freeze lookup remains correct for startup duplicate checks");

    registry.freeze();
    expectations.expect(registry.find("example.bulk.0", 1) != nullptr &&
                            registry.find("example.bulk.2048", 1) != nullptr &&
                            registry.find("example.bulk.4095", 1) != nullptr &&
                            registry.find("example.bulk.2048", 2) == nullptr &&
                            registry.find("example.bulk.missing", 1) == nullptr &&
                            registry.containsType("example.bulk.2048") &&
                            !registry.containsType("example.bulk.missing"),
                        "freeze-sorted lookup resolves large registries and exact versions");
}

} // namespace

int main() {
    Expectations expectations;
    testValidationAndDuplicates(expectations);
    testFreezeAndBuiltIns(expectations);
    testStructuralLoweringsRequireCanonicalKeys(expectations);
    testLargeFrozenRegistryLookup(expectations);
    return expectations.failures() == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
