#pragma once

#include <bloom/commands/operation.hpp>
#include <bloom/core/rational_time.hpp>
#include <bloom/scripting/value.hpp>

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace bloom::scripting {

enum class ValueKind : std::uint8_t {
    Boolean,
    Integer,
    Double,
    String,
    Vec2,
    Vec3,
    Color4,
    Id,
    Array,
    // An exact rational time: a whole frame count, or a [numerator, denominator] pair.
    Time,
    // One authoring value whose width the operation itself decides: a number, a vec2, a vec3 or a
    // colour. Parameter-keyed animation operations take the parameter's own width, which the
    // caller knows and the schema cannot.
    Value,
};

// Which live-session value fills an omitted argument. The flag travels with the schema so every
// client -- Python, MCP and the JSON/CLI path -- reports and applies the same defaults instead of
// each one inventing its own.
enum class ContextSource : std::uint8_t {
    None,
    Project,
    Composition,
    Time,
    // The first selected object, for operations that edit exactly one layer or node.
    Layer,
    // The whole selection, for operations that take a set.
    Selection,
};

struct ArgumentSchema final {
    std::string name;
    ValueKind kind = ValueKind::String;
    bool required = true;
    // Shown in the operation's example call even though it is optional: one of a set of
    // alternatives, of which this is the ordinary one. `bloom.node.connect` needs either a port
    // or a stack role, and the example names the port.
    bool inExample = false;
    ContextSource context = ContextSource::None;
    // A value of this argument's own kind that the factory accepts. It is what the help text and
    // the rejection messages show, and what the conformance test constructs every operation from.
    std::optional<Value> example{};
    std::string summary;

    [[nodiscard]] bool contextual() const noexcept { return context != ContextSource::None; }
};

struct OperationSchema final {
    std::string typeId;
    std::vector<ArgumentSchema> arguments;
};

// The live session values that contextual arguments fall back to. Every field is optional because
// a headless session may have no selection, and a project may have no composition at all.
struct OperationContext final {
    std::optional<std::uint64_t> project;
    std::optional<std::uint64_t> composition;
    std::vector<std::uint64_t> selection;
    // The selected layers. A node selection is not a layer selection -- the two are different id
    // spaces -- so only a selected Layer Output boundary names a layer here.
    std::vector<std::uint64_t> layers;
    std::optional<core::RationalTime> time;
};

// Fills every omitted contextual argument the context can answer and returns their names in schema
// order. Arguments the caller supplied are never overwritten, and a context with nothing to offer
// leaves the arguments exactly as they were so the ordinary missing-argument rejection still names
// the argument.
[[nodiscard]] std::vector<std::string> applyContextDefaults(const OperationSchema& schema,
                                                            Arguments& arguments,
                                                            const OperationContext& context);

struct OperationDiagnostic final {
    std::string code;
    std::string operationId;
    std::string argument;
    std::string message;
};

class [[nodiscard]] OperationCreateResult final {
  public:
    OperationCreateResult(std::unique_ptr<commands::Operation> operation,
                          std::optional<OperationDiagnostic> diagnostic) noexcept
        : operation_(std::move(operation)), diagnostic_(std::move(diagnostic)) {}
    OperationCreateResult(OperationCreateResult&&) noexcept = default;
    OperationCreateResult& operator=(OperationCreateResult&&) noexcept = default;
    OperationCreateResult(const OperationCreateResult&) = delete;
    OperationCreateResult& operator=(const OperationCreateResult&) = delete;
    ~OperationCreateResult() = default;

    [[nodiscard]] explicit operator bool() const noexcept { return operation_ != nullptr; }
    [[nodiscard]] const OperationDiagnostic* diagnostic() const noexcept {
        return diagnostic_.has_value() ? &*diagnostic_ : nullptr;
    }
    [[nodiscard]] std::unique_ptr<commands::Operation> takeOperation() && noexcept {
        return std::move(operation_);
    }

  private:
    std::unique_ptr<commands::Operation> operation_;
    std::optional<OperationDiagnostic> diagnostic_;
};

using OperationFactory = std::function<OperationCreateResult(const std::string&, const Arguments&)>;

struct OperationDescriptor final {
    OperationSchema schema;
    OperationFactory factory;
};

class OperationRegistry final {
  public:
    OperationRegistry() = default;
    OperationRegistry(const OperationRegistry&) = delete;
    OperationRegistry& operator=(const OperationRegistry&) = delete;
    OperationRegistry(OperationRegistry&&) noexcept = default;
    OperationRegistry& operator=(OperationRegistry&&) noexcept = default;
    ~OperationRegistry() = default;

    [[nodiscard]] static OperationRegistry builtIn();

    [[nodiscard]] const OperationDescriptor* find(std::string_view typeId) const noexcept;
    [[nodiscard]] std::span<const OperationDescriptor> descriptors() const noexcept {
        return descriptors_;
    }
    [[nodiscard]] OperationCreateResult create(std::string_view typeId,
                                               const Arguments& arguments) const;
    // The same creation, with every omitted contextual argument answered from the live session
    // first. Clients call this one so a script, an MCP tool call and a JSON step all inherit the
    // same current composition, playhead and selection.
    [[nodiscard]] OperationCreateResult create(std::string_view typeId, Arguments arguments,
                                               const OperationContext& context) const;

  private:
    std::vector<OperationDescriptor> descriptors_;
};

// A call an artist can paste, built from the schema's own examples. It names only the required
// arguments that do not come from context, which is exactly the shortest working call.
[[nodiscard]] std::string exampleCall(const OperationSchema& schema);
// `bloom.layer.add-solid` -> `bloom.ops.layer.add_solid`, the name the Python client binds.
[[nodiscard]] std::string pythonOperationName(std::string_view typeId);
// One value rendered in Python syntax, for example calls and rejection messages.
[[nodiscard]] std::string pythonLiteral(const Value& value);
// The artist-facing name of a kind: the same spelling the schema projections publish.
[[nodiscard]] std::string_view valueKindName(ValueKind kind) noexcept;

} // namespace bloom::scripting
