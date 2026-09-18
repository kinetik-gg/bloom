// Every registered operation must be callable from a client that has only its schema: the schema
// names every constructor input, each argument carries an example of its own kind, and the factory
// builds the operation from exactly those examples. A registry entry that cannot do this is an id
// an artist can discover and never use.
#include <bloom/scripting/operation_registry.hpp>

#include <exception>
#include <iostream>
#include <set>
#include <string>

namespace {

using bloom::scripting::Arguments;
using bloom::scripting::ArgumentSchema;
using bloom::scripting::ContextSource;
using bloom::scripting::OperationContext;
using bloom::scripting::OperationRegistry;
using bloom::scripting::StableId;
using bloom::scripting::Value;
using bloom::scripting::ValueArray;
using bloom::scripting::ValueKind;
namespace core = bloom::core;

int failures = 0;

void expect(const bool condition, const std::string& what) {
    if (!condition) {
        std::cerr << "FAIL: " << what << '\n';
        ++failures;
    }
}

// The example must really be of the argument's own kind; a mislabelled one would make the help
// text and the rejection messages lie.
[[nodiscard]] bool matchesKind(const Value& value, const ValueKind kind) {
    switch (kind) {
    case ValueKind::Boolean:
        return std::holds_alternative<bool>(value.storage);
    case ValueKind::Integer:
        return std::holds_alternative<std::int64_t>(value.storage);
    case ValueKind::Double:
        return std::holds_alternative<double>(value.storage);
    case ValueKind::String:
        return std::holds_alternative<std::string>(value.storage);
    case ValueKind::Vec2:
        return std::holds_alternative<bloom::document::Vec2d>(value.storage);
    case ValueKind::Vec3:
        return std::holds_alternative<bloom::document::Vec3d>(value.storage);
    case ValueKind::Color4:
        return std::holds_alternative<bloom::core::Color4d>(value.storage);
    case ValueKind::Id:
        return std::holds_alternative<StableId>(value.storage);
    case ValueKind::Array:
        return std::holds_alternative<ValueArray>(value.storage);
    case ValueKind::Time:
        return std::holds_alternative<std::int64_t>(value.storage) ||
               std::holds_alternative<ValueArray>(value.storage);
    case ValueKind::Value:
        return !std::holds_alternative<std::string>(value.storage);
    }
    return false;
}

[[nodiscard]] int run() {
    const auto registry = OperationRegistry::builtIn();
    expect(!registry.descriptors().empty(), "the registry has operations");

    std::set<std::string> ids;
    for (const auto& descriptor : registry.descriptors()) {
        const auto& schema = descriptor.schema;
        const auto& id = schema.typeId;
        expect(ids.insert(id).second, id + " is registered once");
        expect(static_cast<bool>(descriptor.factory), id + " has a factory");
        // Every Bloom operation constructor takes at least the object it edits, so an empty
        // schema can only mean a missing adapter.
        expect(!schema.arguments.empty(), id + " names its constructor inputs");

        bool optionalSeen = false;
        std::set<std::string> names;
        Arguments fromExamples;
        for (const auto& argument : schema.arguments) {
            expect(!argument.name.empty(), id + " names every argument");
            expect(names.insert(argument.name).second,
                   id + " has a unique argument named " + argument.name);
            expect(argument.example.has_value(), id + ": " + argument.name + " carries an example");
            if (argument.example.has_value()) {
                expect(matchesKind(*argument.example, argument.kind),
                       id + ": " + argument.name + "'s example matches its kind");
                fromExamples.emplace(argument.name, *argument.example);
            }
            expect(!argument.summary.empty(), id + ": " + argument.name + " is described");
            if (!argument.contextual()) {
                // Positional calls bind in schema order, so a required argument may never follow
                // an optional one.
                if (!argument.required) {
                    optionalSeen = true;
                } else {
                    expect(!optionalSeen, id + ": required argument " + argument.name +
                                              " precedes every optional one");
                }
            }
        }

        auto created = registry.create(id, fromExamples);
        expect(static_cast<bool>(created),
               id + " constructs from its own example arguments" +
                   (created ? std::string() : ": " + std::string(created.diagnostic()->message)));

        // Exactly the arguments the published example call names: the shortest working call.
        Arguments shortest;
        for (const auto& argument : schema.arguments) {
            if (argument.inExample && !argument.contextual() && argument.example.has_value()) {
                shortest.emplace(argument.name, *argument.example);
            }
        }
        const OperationContext context{.project = 1,
                                       .composition = 7,
                                       .selection = {11, 12},
                                       .layers = {21},
                                       .time = core::RationalTime::create(1, 2)};
        auto contextual = registry.create(id, shortest, context);
        expect(static_cast<bool>(contextual),
               id + "'s example call constructs once bloom.context answers the rest" +
                   (contextual ? std::string()
                               : ": " + std::string(contextual.diagnostic()->message)));

        const auto example = bloom::scripting::exampleCall(schema);
        expect(example.starts_with("bloom.ops."), id + " has a pasteable example call");
        expect(example.ends_with(")"), id + "'s example call is complete");
    }

    // A rejection names the operation, the argument, the expectation and a runnable example.
    const OperationContext live{.project = 1,
                                .composition = 1,
                                .selection = {},
                                .layers = {},
                                .time = core::RationalTime::fromInteger(0)};
    auto rejected = registry.create("bloom.layer.add-solid", Arguments{}, live);
    expect(!rejected, "an empty add-solid call is rejected");
    if (rejected.diagnostic() != nullptr) {
        const std::string message = rejected.diagnostic()->message;
        expect(message.find("bloom.layer.add-solid") != std::string::npos,
               "the rejection names the operation");
        expect(message.find("'name'") != std::string::npos, "the rejection names the argument");
        expect(
            message.find(R"(Example: bloom.ops.layer.add_solid(name="Red", color=(1, 0, 0, 1)))") !=
                std::string::npos,
            "the rejection carries the owner's example call");
    }

    // A contextual argument that the session cannot answer says so rather than reading as a
    // plain missing keyword.
    auto noContext = registry.create("bloom.layer.set-enabled", Arguments{{"enabled", Value(true)}},
                                     OperationContext{});
    expect(!noContext, "set-enabled without a context is rejected");
    if (noContext.diagnostic() != nullptr) {
        expect(std::string(noContext.diagnostic()->message).find("bloom.context") !=
                   std::string::npos,
               "the rejection points at bloom.context");
    }

    // Filling from context is reported, and never overwrites what the caller passed.
    const auto* descriptor = registry.find("bloom.layer.set-enabled");
    expect(descriptor != nullptr, "set-enabled is registered");
    if (descriptor != nullptr) {
        Arguments arguments{{"enabled", Value(true)}, {"layer", Value(StableId{99})}};
        const OperationContext context{
            .project = 1, .composition = 7, .selection = {11}, .layers = {21}, .time = {}};
        const auto filled =
            bloom::scripting::applyContextDefaults(descriptor->schema, arguments, context);
        expect(filled.size() == 1 && filled.front() == "composition",
               "only the omitted contextual arguments are filled");
        expect(std::get<StableId>(arguments.at("layer").storage).value == 99,
               "an explicit layer survives the context defaults");
        expect(std::get<StableId>(arguments.at("composition").storage).value == 7,
               "the current composition fills the omitted argument");
    }

    if (failures != 0) {
        std::cerr << failures << " operation schema checks failed\n";
        return 1;
    }
    return 0;
}

} // namespace

int main() {
    try {
        return run();
    } catch (const std::exception& error) {
        std::cerr << "operation schema checks threw: " << error.what() << '\n';
        return 2;
    }
}
