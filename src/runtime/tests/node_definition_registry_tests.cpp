#include <bloom/core/color.hpp>
#include <bloom/document/graph.hpp>
#include <bloom/document/parameter.hpp>
#include <bloom/runtime/node_definition_registry.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <source_location>
#include <span>
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
    return {
        {"example.solid", version},
        runtime::NodeLoweringKind::Solid,
        // ADAPTED (task S7): the Solid lowering's shape check now also requires one linkable
        // operand socket per parameter role, so a custom Solid declares the colour socket too.
        {{std::string(document::kSolidColorParameterRole), runtime::SocketValueKind::Color, false}},
        {{std::string(document::kSolidSourceOutputPort), runtime::SocketValueKind::Image}},
        // ADAPTED (task S5): the Solid lowering's shape check requires the colour parameter's
        // supportsAnimation to equal document::isAnimatableSchemaKey() for its schema, and a
        // solid colour is animatable now -- so a custom Solid must declare it too.
        {{std::string(document::kSolidColorParameterRole),
          std::string(document::kSolidColorParameterSchemaKey),
          runtime::ParameterValueKind::Color4d, true, true}},
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
    replacement.parameters.front().role = "replacement";
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
    // ADAPTED (task S7, then FIX1 item I, then UTIL-1): the five structural node types, the seven
    // compatibility schemas, the value library's first slice, and UTIL-1's twenty conversions, four
    // time conversions and fifteen string utilities. The number is pinned rather than computed so
    // that adding a node type is a deliberate edit here.
    expectations.expect(registry.definitions().size() == 81,
                        "startup contribution includes every built-in definition");

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
                        "every built-in type remains discoverable");
    const auto* text =
        registry.find(document::kTextSourceNodeType, document::kTextSourceNodeSchemaVersion);
    // ADAPTED (task S3): Text was NodeLoweringKind::Unsupported while no portable CPU glyph
    // rasterizer existed. It now has its own lowering, so the contract pinned here is its parameter
    // shape -- content, then size, then color, in that order, none animatable -- rather than the
    // absence of one.
    expectations.expect(text != nullptr && text->lowering == runtime::NodeLoweringKind::Text,
                        "Text is a lowered capability with its own compiled operation");
    expectations.expect(
        text != nullptr && text->parameters.size() == 6 &&
            text->parameters[0].role == document::kTextParameterRole &&
            text->parameters[0].schemaKey == document::kTextParameterSchemaKey &&
            text->parameters[0].valueKind == runtime::ParameterValueKind::String &&
            text->parameters[1].role == document::kTextSizeParameterRole &&
            text->parameters[1].schemaKey == document::kTextSizeParameterSchemaKey &&
            text->parameters[1].valueKind == runtime::ParameterValueKind::Float64 &&
            text->parameters[2].role == document::kTextColorParameterRole &&
            text->parameters[2].schemaKey == document::kTextColorParameterSchemaKey &&
            text->parameters[2].valueKind == runtime::ParameterValueKind::Color4d,
        "the text schema is exactly content, size, and color, in the registered order");
    expectations.expect(text != nullptr && text->parameters.size() == 6 &&
                            text->parameters[1].defaultValue ==
                                document::ParameterValue{document::kDefaultTextSizePixels} &&
                            text->parameters[2].defaultValue ==
                                document::ParameterValue{core::Color4d{1.0, 1.0, 1.0, 1.0}},
                        "a new text layer defaults to 72 px opaque white");
    // The font is deliberately absent from the schema: the CPU reference path has exactly one
    // embedded face, so a font parameter would persist a choice nothing can honor.
    expectations.expect(text != nullptr &&
                            std::ranges::none_of(text->parameters,
                                                 [](const auto& parameter) {
                                                     return parameter.role.find("font") !=
                                                            std::string::npos;
                                                 }),
                        "the text schema names no font");
    const auto* layer =
        registry.find(document::kLayerOutputNodeType, document::kLayerOutputNodeSchemaVersion);
    // ADAPTED (task S4): the Layer Output schema grew from two parameters to five, so this
    // ADAPTED (task S5): a source parameter no longer declares "no animation" as a class. Animation
    // support is still an explicit per-parameter capability, but what it must EQUAL is the shared
    // schema predicate -- so a registered definition can never be a second opinion about what is
    // animatable. Solid colour and text size/colour now declare it; text content, a String, does
    // not.
    expectations.expect(
        layer != nullptr && layer->parameters.size() == 6 &&
            std::ranges::all_of(
                std::span(layer->parameters).first(5),
                [](const auto& parameter) { return parameter.supportsAnimation; }) &&
            !layer->parameters[5].supportsAnimation && solid != nullptr &&
            solid->parameters.front().supportsAnimation && text != nullptr &&
            text->parameters.size() == 6 && !text->parameters[0].supportsAnimation &&
            text->parameters[1].supportsAnimation && text->parameters[2].supportsAnimation,
        "animation support is an explicit per-parameter evaluator capability");
    const auto declarationMatchesSchema = [](const auto& definition) {
        return std::ranges::all_of(definition->parameters, [](const auto& parameter) {
            return parameter.supportsAnimation ==
                   document::isAnimatableSchemaKey(parameter.schemaKey);
        });
    };
    expectations.expect(layer != nullptr && solid != nullptr && text != nullptr &&
                            declarationMatchesSchema(layer) && declarationMatchesSchema(solid) &&
                            declarationMatchesSchema(text),
                        "and it agrees with the shared schema predicates for every registered "
                        "parameter, so the two can never drift");
    expectations.expect(
        layer != nullptr && layer->parameters.size() == 6 &&
            layer->parameters[0].role == document::kPositionParameterRole &&
            layer->parameters[0].schemaKey == document::kPositionParameterSchemaKey &&
            layer->parameters[0].valueKind == runtime::ParameterValueKind::Vec2d &&
            layer->parameters[1].role == document::kAnchorParameterRole &&
            layer->parameters[1].schemaKey == document::kAnchorParameterSchemaKey &&
            layer->parameters[1].valueKind == runtime::ParameterValueKind::Vec2d &&
            layer->parameters[2].role == document::kScaleParameterRole &&
            layer->parameters[2].schemaKey == document::kScaleParameterSchemaKey &&
            layer->parameters[2].valueKind == runtime::ParameterValueKind::Vec2d &&
            layer->parameters[3].role == document::kRotationParameterRole &&
            layer->parameters[3].schemaKey == document::kRotationParameterSchemaKey &&
            layer->parameters[3].valueKind == runtime::ParameterValueKind::Float64 &&
            layer->parameters[4].role == document::kOpacityParameterRole &&
            layer->parameters[4].schemaKey == document::kOpacityParameterSchemaKey &&
            layer->parameters[4].valueKind == runtime::ParameterValueKind::Float64 &&
            layer->parameters[5].role == document::kBlendModeParameterRole &&
            layer->parameters[5].schemaKey == document::kBlendModeParameterSchemaKey &&
            layer->parameters[5].valueKind == runtime::ParameterValueKind::Integer,
        "the Layer Output schema is exactly position, anchor, scale, rotation, opacity, and blend "
        "mode, in the registered order");
    expectations.expect(layer != nullptr && layer->parameters.size() == 6 &&
                            layer->parameters[1].defaultValue ==
                                document::ParameterValue{document::kDefaultAnchor} &&
                            layer->parameters[2].defaultValue ==
                                document::ParameterValue{document::kDefaultScale} &&
                            layer->parameters[3].defaultValue ==
                                document::ParameterValue{document::kDefaultRotationDegrees} &&
                            layer->parameters[5].defaultValue ==
                                document::ParameterValue{document::kDefaultBlendModeValue},
                        "the transform defaults are the identity transform -- centre anchor, unit "
                        "scale, no rotation -- and the blend mode defaults to Normal");
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

    // Task FIX1, item H: the composition Output is a SINK. It declares one input and no output at
    // all, and a definition claiming one is refused -- which is what keeps "nothing connects from
    // the end of the composition" a registry fact rather than an editor convention.
    {
        const auto output = builtInDefinition(document::kCompositionOutputNodeType,
                                              document::kCompositionOutputNodeSchemaVersion);
        expectations.expect(output.outputs.empty() && output.inputs.size() == 1,
                            "the composition Output declares one input and no output");
        runtime::NodeDefinitionRegistry sourcingOutput;
        auto spoof = output;
        spoof.outputs.push_back(
            {std::string(document::kCompositionOutputOutputPort), runtime::SocketValueKind::Image});
        expectations.expect(sourcingOutput.registerDefinition(std::move(spoof)) ==
                                runtime::NodeRegistrationStatus::InvalidDefinition,
                            "and a definition that gives it one is refused");
    }

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

// Task S7: the value library's ONE shape contract, exercised through the registry rather than by
// reaching into it. Every built-in registers (the count above proves that), so what is pinned here
// is the refusals -- each perturbation below breaks exactly one clause of the contract, and a
// definition that still registered would mean the clause is not being checked.
void testValueLoweringShapeContract(Expectations& expectations) {
    using namespace bloom;

    const auto refuses = [&expectations](runtime::NodeDefinition definition,
                                         const std::string_view message) {
        runtime::NodeDefinitionRegistry registry;
        expectations.expect(registry.registerDefinition(std::move(definition)) ==
                                runtime::NodeRegistrationStatus::InvalidDefinition,
                            message);
    };

    // A literal: no inputs, one output, one parameter.
    const auto literal =
        builtInDefinition(document::kScalarValueNodeType, document::kValueNodeSchemaVersion);
    {
        auto extraSocket = literal;
        extraSocket.inputs.push_back(
            {std::string(document::kValueParameterRole), runtime::SocketValueKind::Scalar, false});
        refuses(std::move(extraSocket), "a literal Value node may not take an input at all");
    }
    {
        // ADAPTED (task FIX1, item G): the Scalar, Vector 2 and Colour LITERALS are animatable now,
        // so the clause is no longer "never" but "exactly what the schema predicates say". A
        // definition that disagrees with them in either direction is refused, which is what these
        // two pin.
        auto notAnimatable = literal;
        notAnimatable.parameters.front().supportsAnimation = false;
        refuses(std::move(notAnimatable),
                "a Scalar literal that denies its own animatable schema is refused");
        auto animatable =
            builtInDefinition(document::kStringValueNodeType, document::kValueNodeSchemaVersion);
        animatable.parameters.front().supportsAnimation = true;
        refuses(std::move(animatable),
                "and a String literal claiming a curve kind that does not exist is refused too");
    }
    {
        auto structural = literal;
        structural.cardinality = runtime::NodeCardinality::OnePerComposition;
        refuses(std::move(structural), "a value node is never a structural singleton");
    }
    {
        auto misfiled = literal;
        misfiled.category = runtime::NodeCategory::Compositing;
        refuses(std::move(misfiled),
                "a value node is listed under Values or Utilities and nowhere else");
    }

    // An operand node: every socket backed by a parameter of the matching kind, and never required.
    const auto clamp =
        builtInDefinition(document::kClampNodeType, document::kValueNodeSchemaVersion);
    {
        auto required = clamp;
        required.inputs.front().required = true;
        refuses(std::move(required),
                "an operand socket is never required: its parameter is the value when nothing is "
                "connected");
    }
    {
        auto mismatched = clamp;
        mismatched.inputs.front().valueKind = runtime::SocketValueKind::Color;
        refuses(std::move(mismatched),
                "an operand socket's kind must be the one its parameter's value kind carries");
    }
    {
        auto unbacked = clamp;
        unbacked.inputs.push_back({"stray", runtime::SocketValueKind::Scalar, false});
        refuses(std::move(unbacked),
                "a socket with no parameter behind it exists only on a Reroute");
    }
    {
        auto unsocketed = clamp;
        unsocketed.parameters.push_back(
            {"stray", "example.operand", runtime::ParameterValueKind::Float64, true, false, 0.0});
        refuses(
            std::move(unsocketed),
            "a parameter with no socket must be an inline selector, not an unreachable operand");
    }

    // The Reroute: ADAPTED (task FIX1, item I). There is ONE reroute type now, and the kind it
    // carries comes from the link it sits on rather than from its name, so the clause that used to
    // read "a kind-named type and its sockets cannot disagree" reads "its two sockets agree with
    // each other".
    const auto reroute =
        builtInDefinition(document::kRerouteNodeType, document::kValueNodeSchemaVersion);
    {
        auto optional = reroute;
        optional.inputs.front().required = false;
        refuses(std::move(optional),
                "a Reroute's pass-through is required: nothing else can supply it");
    }
    {
        auto retyped = reroute;
        retyped.outputs.front().valueKind = runtime::SocketValueKind::Color;
        refuses(std::move(retyped), "a Reroute's two sockets declare one kind, not two");
    }

    // Time: no inputs, no parameters, and its two outputs in their declared units.
    {
        auto time =
            builtInDefinition(document::kTimeValueNodeType, document::kValueNodeSchemaVersion);
        time.outputs.pop_back();
        refuses(std::move(time), "Time declares both of its units or neither");
    }

    // A Switch: both branches carry the result's kind, and the condition is Boolean.
    {
        auto mixedSwitch =
            builtInDefinition(document::kScalarSwitchNodeType, document::kValueNodeSchemaVersion);
        mixedSwitch.inputs.front().valueKind = runtime::SocketValueKind::Scalar;
        refuses(std::move(mixedSwitch), "a Switch's condition is Boolean and only Boolean");
    }
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

int main() try {
    Expectations expectations;
    testValidationAndDuplicates(expectations);
    testFreezeAndBuiltIns(expectations);
    testStructuralLoweringsRequireCanonicalKeys(expectations);
    testValueLoweringShapeContract(expectations);
    testLargeFrozenRegistryLookup(expectations);
    return expectations.failures() == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
} catch (const std::exception& error) {
    std::cerr << "Unexpected test exception: " << error.what() << '\n';
    return EXIT_FAILURE;
} catch (...) {
    std::cerr << "Unexpected non-standard test exception\n";
    return EXIT_FAILURE;
}
