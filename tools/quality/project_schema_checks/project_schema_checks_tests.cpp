#include "schema_checks.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace {

using bloom::quality::json::Number;
using bloom::quality::json::ParseLimits;
using bloom::quality::json::Value;

class TemporaryDirectory final {
  public:
    TemporaryDirectory() {
        static std::atomic_uint64_t sequence{0};
        const auto timestamp = std::chrono::steady_clock::now().time_since_epoch().count();
        for (auto attempt = std::uint64_t{0}; attempt < 100U; ++attempt) {
            root_ = std::filesystem::temp_directory_path() /
                    ("bloom-schema-checks-" + std::to_string(timestamp) + '-' +
                     std::to_string(sequence.fetch_add(1)) + '-' + std::to_string(attempt));
            std::error_code error;
            if (std::filesystem::create_directory(root_, error)) {
                return;
            }
        }
        throw std::runtime_error("could not create a temporary schema-check directory");
    }

    TemporaryDirectory(const TemporaryDirectory&) = delete;
    auto operator=(const TemporaryDirectory&) -> TemporaryDirectory& = delete;
    TemporaryDirectory(TemporaryDirectory&&) = delete;
    auto operator=(TemporaryDirectory&&) -> TemporaryDirectory& = delete;

    ~TemporaryDirectory() {
        std::error_code error;
        std::filesystem::remove_all(root_, error);
    }

    [[nodiscard]] auto path(const std::string_view name) const -> std::filesystem::path {
        return root_ / name;
    }

    [[nodiscard]] auto write(const std::string_view name, const std::string_view bytes) const
        -> std::filesystem::path {
        const auto result = path(name);
        std::ofstream stream{result, std::ios::binary};
        stream.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        stream.close();
        if (!stream) {
            throw std::runtime_error("could not write temporary JSON fixture");
        }
        return result;
    }

  private:
    std::filesystem::path root_;
};

void expect(const bool condition, const std::string_view message, int& failures) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

template <typename ExpectedException, typename Function>
void expectTypedFailure(Function&& function, const std::string_view message, int& failures) {
    try {
        function();
        expect(false, message, failures);
    } catch (const ExpectedException& expected) {
        static_cast<void>(expected);
    } catch (const std::exception& unexpected) {
        std::cerr << "FAIL: " << message << " threw the wrong exception type: " << unexpected.what()
                  << '\n';
        ++failures;
    }
}

[[nodiscard]] auto path(Value& root, const std::initializer_list<std::string_view> components)
    -> Value& {
    auto* current = &root;
    for (const auto component : components) {
        current = &current->at(component);
    }
    return *current;
}

void eraseMember(Value& object, const std::string_view name) {
    auto& members = object.asObject();
    const auto iterator =
        std::ranges::find_if(members, [name](const auto& member) { return member.first == name; });
    if (iterator == members.end()) {
        throw std::logic_error("fixture member does not exist");
    }
    members.erase(iterator);
}

void addMember(Value& object, std::string name, Value value) {
    object.asObject().emplace_back(std::move(name), std::move(value));
}

void testParser(int& failures) {
    using bloom::quality::json::parse;
    using bloom::quality::json::parseFile;
    const auto expectFailure = []<typename Function>(Function&& function,
                                                     const std::string_view message,
                                                     int& failureCount) {
        expectTypedFailure<bloom::quality::json::ParseError>(std::forward<Function>(function),
                                                             message, failureCount);
    };
    expectFailure([&] { static_cast<void>(parse("{\"a\":1,\"a\":2}")); },
                  "duplicate literal object members are rejected", failures);
    expectFailure([&] { static_cast<void>(parse("{\"a\":1,\"\\u0061\":2}")); },
                  "duplicate decoded object members are rejected", failures);
    try {
        static_cast<void>(parse("{\"a\":1,\"\\u0061\":2}"));
        expect(false, "duplicate member diagnostic is emitted", failures);
    } catch (const bloom::quality::json::ParseError& error) {
        expect(std::string_view{error.what()}.find("duplicate decoded JSON object member") !=
                   std::string_view::npos,
               "duplicate member diagnostic names the violated invariant", failures);
    }
    expectFailure([&] { static_cast<void>(parse("\xEF\xBB\xBF{}")); }, "UTF-8 BOM is rejected",
                  failures);
    expectFailure([&] { static_cast<void>(parse("{\"x\":\"\\ud800\"}")); },
                  "lone high surrogate is rejected", failures);
    expectFailure([&] { static_cast<void>(parse("{\"x\":\"\\udc00\"}")); },
                  "lone low surrogate is rejected", failures);
    expectFailure([&] { static_cast<void>(parse("{\"x\":\"\\ud800\\u0041\"}")); },
                  "invalid surrogate pair is rejected", failures);
    expect(parse("{\"x\":\"\\ud83c\\udf38\"}").at("x").asString() == "\xF0\x9F\x8C\xB8",
           "valid escaped surrogate pair decodes to strict UTF-8", failures);

    for (const auto& invalid : {
             std::string{"{\"x\":\"\xC0\x80\"}"},
             std::string{"{\"x\":\"\xED\xA0\x80\"}"},
             std::string{"{\"x\":\"\xF4\x90\x80\x80\"}"},
             std::string{"{\"x\":\"\xE2\x82\"}"},
         }) {
        expectFailure([&] { static_cast<void>(parse(invalid)); },
                      "invalid UTF-8 scalar sequence is rejected", failures);
    }
    for (const std::string_view invalid :
         {"NaN", "Infinity", "-Infinity", "+1", "01", "1.", "1e", "[1,]", "{\"a\":1,}",
          "{\"a\":1} trailing", "/*x*/{}", "'x'"}) {
        expectFailure([&] { static_cast<void>(parse(invalid)); },
                      "invalid or extended JSON syntax is rejected", failures);
    }

    const auto numbers = parse("[-0,0,1,1.0,1e0,1E+0]").asArray();
    expect(numbers[0].asNumber().spelling == "-0" && numbers[3].asNumber().spelling == "1.0" &&
               numbers[5].asNumber().spelling == "1E+0",
           "number tokens retain exact spelling", failures);
    expect(!bloom::quality::json::exactEqual(parse("true"), parse("1")),
           "JSON exactness distinguishes boolean and integer", failures);

    expectFailure([&] { static_cast<void>(parse("{}", ParseLimits{1, 128, 100, 100, 100})); },
                  "input byte limit is enforced", failures);
    expectFailure([&] { static_cast<void>(parse("[[0]]", ParseLimits{32, 2, 100, 100, 100})); },
                  "nesting limit counts the root", failures);
    expectFailure([&] { static_cast<void>(parse("[0,1]", ParseLimits{32, 128, 2, 100, 100})); },
                  "value-count limit is enforced", failures);
    expectFailure([&] { static_cast<void>(parse("[0,1]", ParseLimits{32, 128, 100, 1, 100})); },
                  "container-entry limit is enforced", failures);
    expectFailure([&] { static_cast<void>(parse("\"ab\"", ParseLimits{32, 128, 100, 100, 1})); },
                  "decoded string-byte limit is enforced", failures);

    TemporaryDirectory fixtures;
    const auto bomPath = fixtures.write("bom.json", "\xEF\xBB\xBF{}\n");
    expectFailure([&] { static_cast<void>(parseFile(bomPath)); }, "parseFile rejects a UTF-8 BOM",
                  failures);
    const auto invalidUtf8Path = fixtures.write("invalid-utf8.json", "{\"x\":\"\xC0\x80\"}\n");
    expectFailure([&] { static_cast<void>(parseFile(invalidUtf8Path)); },
                  "parseFile rejects invalid UTF-8", failures);
    const auto oversizedPath = fixtures.write("oversized.json", "{\"value\":123456}\n");
    expectFailure(
        [&] { static_cast<void>(parseFile(oversizedPath, ParseLimits{16, 128, 100, 100, 100})); },
        "parseFile enforces the pre-allocation byte limit", failures);
    expectFailure([&] { static_cast<void>(parseFile(fixtures.path("missing.json"))); },
                  "parseFile reports missing inputs as parser errors", failures);
}

void testManifest(const Value& manifest, int& failures) {
    using bloom::quality::validateManifestSchema;
    const auto expectFailure = []<typename Function>(Function&& function,
                                                     const std::string_view message,
                                                     int& failureCount) {
        expectTypedFailure<bloom::quality::SchemaCheckError>(std::forward<Function>(function),
                                                             message, failureCount);
    };
    validateManifestSchema(manifest);

    auto remote = manifest;
    path(remote, {"properties", "document", "$ref"}) =
        Value{std::string{"https://example.invalid/schema.json"}};
    expectFailure([&] { validateManifestSchema(remote); }, "remote schema references are rejected",
                  failures);

    auto unresolved = manifest;
    path(unresolved, {"properties", "document", "$ref"}) = Value{std::string{"#/$defs/missing"}};
    expectFailure([&] { validateManifestSchema(unresolved); },
                  "unresolved manifest references are rejected", failures);

    auto weakenedIdentifier = manifest;
    path(weakenedIdentifier, {"$defs", "namespacedIdentifier", "maxLength"}) = Value{Number{"129"}};
    expectFailure([&] { validateManifestSchema(weakenedIdentifier); },
                  "weakened identifier bounds are rejected", failures);

    auto weakenedCount = manifest;
    path(weakenedCount, {"properties", "requirements", "maxItems"}) = Value{Number{"1000001"}};
    expectFailure([&] { validateManifestSchema(weakenedCount); },
                  "weakened manifest collection bounds are rejected", failures);

    auto booleanVersion = manifest;
    path(booleanVersion, {"$defs", "fixedVersion-1.0", "properties", "major", "const"}) =
        Value{true};
    expectFailure([&] { validateManifestSchema(booleanVersion); },
                  "boolean is not accepted as integer schema constant", failures);
}

void testDocument(const Value& document, int& failures) {
    using bloom::quality::validateDocumentSchema;
    const auto expectFailure = []<typename Function>(Function&& function,
                                                     const std::string_view message,
                                                     int& failureCount) {
        expectTypedFailure<bloom::quality::SchemaCheckError>(std::forward<Function>(function),
                                                             message, failureCount);
    };
    validateDocumentSchema(document);

    auto unresolved = document;
    path(unresolved, {"properties", "project", "$ref"}) = Value{std::string{"#/$defs/missing"}};
    expectFailure([&] { validateDocumentSchema(unresolved); },
                  "unresolved document references are rejected", failures);

    auto booleanDimension = document;
    path(booleanDimension, {"$defs", "compositionFormat-1.0", "properties", "width", "minimum"}) =
        Value{true};
    expectFailure([&] { validateDocumentSchema(booleanDimension); },
                  "boolean is not accepted as integer dimension", failures);

    auto missingColorDefinition = document;
    eraseMember(path(missingColorDefinition, {"$defs"}), "ocioContextVariable-1.0");
    expectFailure([&] { validateDocumentSchema(missingColorDefinition); },
                  "normative color definitions are required", failures);

    auto changedProcessSpace = document;
    path(changedProcessSpace, {"$defs", "colorSettings-1.0", "properties", "processColorSpaceId",
                               "const"}) = Value{std::string{"scene_linear"}};
    expectFailure([&] { validateDocumentSchema(changedProcessSpace); },
                  "fixed process color identity is required", failures);

    auto driverSource = document;
    path(driverSource, {"$defs", "parameterSource-1.0", "oneOf"})
        .asArray()
        .push_back(bloom::quality::json::parse(
            R"({"type":"object","required":["kind","driverId"],"properties":{"kind":{"const":"driver-binding"},"driverId":{"$ref":"#/$defs/objectId"}},"unevaluatedProperties":true})"));
    expectFailure([&] { validateDocumentSchema(driverSource); },
                  "driver sources remain outside writable schema 1.0", failures);

    auto missingWatermark = document;
    auto& highest = path(missingWatermark, {"$defs", "highestIssued-1.0"});
    auto& required = highest.at("required").asArray();
    required.erase(std::ranges::find_if(required, [](const auto& value) {
        return value.isString() && value.asString() == "driverBinding";
    }));
    eraseMember(highest.at("properties"), "driverBinding");
    expectFailure([&] { validateDocumentSchema(missingWatermark); },
                  "driver allocator watermark remains required", failures);

    auto closedAllocator = document;
    path(closedAllocator, {"$defs", "highestIssued-1.0", "unevaluatedProperties"}) = Value{false};
    expectFailure([&] { validateDocumentSchema(closedAllocator); },
                  "unknown allocator members remain preservable", failures);

    auto addedWatermark = document;
    auto& addedHighest = path(addedWatermark, {"$defs", "highestIssued-1.0"});
    addedHighest.at("required").asArray().push_back(Value{std::string{"futureNamespace"}});
    addMember(addedHighest.at("properties"), "futureNamespace",
              bloom::quality::json::parse(R"({"$ref":"#/$defs/allocatorHighWater"})"));
    expectFailure([&] { validateDocumentSchema(addedWatermark); },
                  "unknown allocator members do not gain known semantics", failures);

    auto weakenedId = document;
    path(weakenedId, {"$defs", "objectId", "pattern"}) = Value{std::string{"^[1-9][0-9]*$"}};
    expectFailure([&] { validateDocumentSchema(weakenedId); },
                  "weakened decimal bounds are rejected", failures);

    auto emptyCurve = document;
    path(emptyCurve, {"$defs", "animationCurve-1.0", "oneOf"})
        .asArray()[0]
        .at("properties")
        .at("keyframes")
        .at("minItems") = Value{Number{"0"}};
    expectFailure([&] { validateDocumentSchema(emptyCurve); },
                  "empty animation curve schemas are rejected", failures);

    auto scalarTextLimit = document;
    addMember(path(scalarTextLimit, {"$defs", "structuralText"}), "maxLength",
              Value{Number{"256"}});
    expectFailure([&] { validateDocumentSchema(scalarTextLimit); },
                  "UTF-8 byte limits are not misstated as scalar limits", failures);

    auto contextLimit = document;
    addMember(path(contextLimit, {"$defs", "ocioContextVariable-1.0", "properties", "value"}),
              "maxLength", Value{Number{"4096"}});
    expectFailure([&] { validateDocumentSchema(contextLimit); },
                  "context UTF-8 byte limit remains a Project I/O check", failures);

    auto changedDestination = document;
    path(changedDestination, {"$defs", "inputPortReference-1.0", "oneOf"})
        .asArray()[0]
        .at("properties")
        .at("kind")
        .at("const") = Value{std::string{"arbitrary-input"}};
    expectFailure([&] { validateDocumentSchema(changedDestination); },
                  "graph discriminator set remains closed", failures);

    auto driverSubject = document;
    path(driverSubject, {"$defs", "extensionTarget-1.0", "properties", "kind", "enum"})
        .asArray()
        .push_back(Value{std::string{"driver-binding"}});
    expectFailure([&] { validateDocumentSchema(driverSubject); },
                  "extension target discriminator set remains closed", failures);

    auto extraRootKeyword = document;
    addMember(extraRootKeyword, "allOf", bloom::quality::json::parse("[false]"));
    expectFailure([&] { validateDocumentSchema(extraRootKeyword); },
                  "unexpected root keywords are rejected", failures);

    for (const auto components : {
             std::initializer_list<std::string_view>{"$defs", "ocioConfigLocator-1.0", "oneOf"},
             std::initializer_list<std::string_view>{"$defs", "externalFileUri"},
             std::initializer_list<std::string_view>{"$defs", "ocioContextVariable-1.0",
                                                     "properties", "value"},
         }) {
        auto weakened = document;
        auto& target = path(weakened, components);
        if (components.size() == 3U) {
            target.asArray()[1].at("properties").at("path") = Value{Value::Object{}};
        } else {
            target = Value{Value::Object{}};
        }
        expectFailure([&] { validateDocumentSchema(weakened); },
                      "color locator and context lexical constraints remain pinned", failures);
    }

    auto unconstrainedCurveId = document;
    path(unconstrainedCurveId, {"$defs", "parameterSource-1.0", "oneOf"})
        .asArray()[1]
        .at("properties")
        .at("curveId") = Value{Value::Object{}};
    expectFailure([&] { validateDocumentSchema(unconstrainedCurveId); },
                  "animation curve references remain constrained", failures);

    auto optionalVec2 = document;
    path(optionalVec2, {"$defs", "vec2Value-1.0", "required"}) = Value{Value::Array{}};
    expectFailure([&] { validateDocumentSchema(optionalVec2); }, "vec2 members remain required",
                  failures);

    auto unconstrainedParameters = document;
    path(unconstrainedParameters, {"$defs", "node-1.0", "properties", "parameters"}) =
        Value{Value::Object{}};
    expectFailure([&] { validateDocumentSchema(unconstrainedParameters); },
                  "node parameter collections remain constrained", failures);

    for (const std::string_view definition : {"humanFacingName", "structuralText"}) {
        auto booleanComment = document;
        path(booleanComment, {"$defs", definition, "$comment"}) = Value{true};
        expectFailure([&] { validateDocumentSchema(booleanComment); },
                      "malformed text definition comments produce typed failures", failures);
    }
    auto booleanBase64 = document;
    path(booleanBase64, {"$defs", "canonicalBase64"}) = Value{true};
    expectFailure([&] { validateDocumentSchema(booleanBase64); },
                  "malformed Base64 definition produces typed failure", failures);
    auto booleanBase64Comment = document;
    path(booleanBase64Comment, {"$defs", "canonicalBase64", "$comment"}) = Value{true};
    expectFailure([&] { validateDocumentSchema(booleanBase64Comment); },
                  "malformed Base64 comment produces typed failure", failures);
}

[[nodiscard]] auto parseRoot(const std::span<const char* const> arguments)
    -> std::filesystem::path {
    if (arguments.size() != 3U || std::string_view{arguments[1]} != "--root") {
        throw std::invalid_argument("usage: project_schema_checks_tests --root <repository>");
    }
    return arguments[2];
}

} // namespace

auto main(const int count, const char* const* values) -> int {
    try {
        const auto root =
            parseRoot(std::span<const char* const>{values, static_cast<std::size_t>(count)});
        auto manifest =
            bloom::quality::json::parseFile(root / "schemas/project/manifest-1.0.schema.json");
        auto document =
            bloom::quality::json::parseFile(root / "schemas/project/document-1.0.schema.json");
        auto failures = 0;
        testParser(failures);
        testManifest(manifest, failures);
        testDocument(document, failures);
        if (failures == 0) {
            std::cout << "Project schema checker self-tests passed\n";
        }
        return failures == 0 ? 0 : 1;
    } catch (const std::exception& error) {
        std::cerr << "Project schema checker self-tests failed: " << error.what() << '\n';
        return 2;
    }
}
