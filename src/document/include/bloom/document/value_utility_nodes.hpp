#pragma once

#include <bloom/core/safe_parse.hpp>
#include <bloom/document/node_definition_registry.hpp>

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>

namespace bloom::document {

// The conversion, string, logic, numeric and readout library (task UTIL-1).
//
// WHY ONE TABLE RATHER THAN SIXTY-TWO DEFINITIONS. The value library's first slice (task S7) gave
// each node shape its own builder, which was right for twenty shapes that genuinely differ -- a
// Math node's five relabelled operands and a Switch's two same-kind branches are not the same
// thing said twice. This slice adds sixty-two nodes whose shapes differ only in WHICH SOCKETS THEY
// HAVE, and sixty-two near-identical builders would be sixty-two places for a socket kind, a
// parameter kind and a default to disagree.
//
// So the shape is DATA. One descriptor per node type names its sockets, its inline selectors and
// its outputs, and four consumers read that one table:
//
//   * value_node_definitions.cpp builds the NodeDefinition,
//   * detail::hasValidValueLoweringShape() validates a registered definition AGAINST it,
//   * the compiler lowers a node's operands in its order,
//   * the evaluator's kernel reads its operands in that same order.
//
// A node type therefore cannot be added half-way: a descriptor with no kernel does not compile, and
// a kernel with no descriptor has nothing to lower.
//
// THE KERNEL ENUMERATION IS NOT PERSISTED. A document stores the TYPE ID, and the type id is what
// names the operation; ValueUtilityKernel exists only to get from a registered definition to a
// compiled kernel inside one process. It may be reordered freely, unlike every enumeration in
// value_operations.hpp whose stored integer is the durable authoring value.

enum class ValueUtilityKernel : std::uint8_t {
    // Conversions (deliverable 1).
    ScalarToString,
    IntegerToString,
    StringToScalar,
    StringToInteger,
    ScalarToInteger,
    IntegerToScalar,
    BooleanToScalar,
    BooleanToInteger,
    ScalarToBoolean,
    IntegerToBoolean,
    BooleanToString,
    StringToBoolean,
    ColorToString,
    StringToColor,
    ColorToVector3,
    Vector3ToColor,
    Vector2ToVector3,
    Vector3ToVector2,
    Vector2ToString,
    Vector3ToString,
};

// ---------------------------------------------------------------------------------------------
// Type ids
// ---------------------------------------------------------------------------------------------

inline constexpr std::string_view kScalarToStringNodeType = "bloom.scalar-to-string";
inline constexpr std::string_view kIntegerToStringNodeType = "bloom.integer-to-string";
inline constexpr std::string_view kStringToScalarNodeType = "bloom.string-to-scalar";
inline constexpr std::string_view kStringToIntegerNodeType = "bloom.string-to-integer";
inline constexpr std::string_view kScalarToIntegerNodeType = "bloom.scalar-to-integer";
inline constexpr std::string_view kIntegerToScalarNodeType = "bloom.integer-to-scalar";
inline constexpr std::string_view kBooleanToScalarNodeType = "bloom.boolean-to-scalar";
inline constexpr std::string_view kBooleanToIntegerNodeType = "bloom.boolean-to-integer";
inline constexpr std::string_view kScalarToBooleanNodeType = "bloom.scalar-to-boolean";
inline constexpr std::string_view kIntegerToBooleanNodeType = "bloom.integer-to-boolean";
inline constexpr std::string_view kBooleanToStringNodeType = "bloom.boolean-to-string";
inline constexpr std::string_view kStringToBooleanNodeType = "bloom.string-to-boolean";
inline constexpr std::string_view kColorToStringNodeType = "bloom.color-to-string";
inline constexpr std::string_view kStringToColorNodeType = "bloom.string-to-color";
inline constexpr std::string_view kColorToVector3NodeType = "bloom.color-to-vector3";
inline constexpr std::string_view kVector3ToColorNodeType = "bloom.vector3-to-color";
inline constexpr std::string_view kVector2ToVector3NodeType = "bloom.vector2-to-vector3";
inline constexpr std::string_view kVector3ToVector2NodeType = "bloom.vector3-to-vector2";
inline constexpr std::string_view kVector2ToStringNodeType = "bloom.vector2-to-string";
inline constexpr std::string_view kVector3ToStringNodeType = "bloom.vector3-to-string";

// ---------------------------------------------------------------------------------------------
// The selector vocabularies
// ---------------------------------------------------------------------------------------------
//
// Each of these IS persisted -- the stored integer is the durable authoring value, exactly as
// value_operations.hpp's are -- so appending a member is additive and renumbering one silently
// reinterprets every saved document.

// How a Scalar becomes an Integer, and what a Rounding node does to a Scalar. The four members are
// the four ScalarPrimitive rounding operations, named again here because a document stores this
// mapping and must not inherit the primitive vocabulary's own numbering.
enum class RoundingMode : std::uint8_t {
    Round = 0,
    Floor = 1,
    Ceiling = 2,
    Truncate = 3,
};

inline constexpr std::array<RoundingMode, 4> kRoundingModes{
    RoundingMode::Round, RoundingMode::Floor, RoundingMode::Ceiling, RoundingMode::Truncate};

inline constexpr RoundingMode kDefaultRoundingMode = RoundingMode::Round;

// The radices an Integer conversion offers on its card. The stored value is the radix ITSELF, not
// an index into this list, so a document that stores 16 means base sixteen whatever this list says
// -- the list is only what the dropdown offers, and core::parseInteger() accepts any radix in
// [kMinimumRadix, kMaximumRadix].
inline constexpr std::array<std::int64_t, 4> kOfferedRadices{2, 8, 10, 16};

template <typename Enumeration>
[[nodiscard]] constexpr std::int64_t selectorStoredValue(const Enumeration value) noexcept {
    return static_cast<std::int64_t>(static_cast<std::uint8_t>(value));
}

template <typename Enumeration, std::size_t Count>
[[nodiscard]] constexpr std::optional<Enumeration>
selectorFromStoredValue(const std::array<Enumeration, Count>& vocabulary,
                        const std::int64_t stored) noexcept {
    for (const auto candidate : vocabulary) {
        if (selectorStoredValue(candidate) == stored) {
            return candidate;
        }
    }
    return std::nullopt;
}

[[nodiscard]] constexpr bool isValidStoredRadix(const std::int64_t stored) noexcept {
    return stored >= core::kMinimumRadix && stored <= core::kMaximumRadix;
}

// ---------------------------------------------------------------------------------------------
// The descriptor table
// ---------------------------------------------------------------------------------------------

// One socket-backed operand. `kind` is the SOCKET's kind; the backing parameter's kind follows from
// it, because socketKindForParameterValueKind() is already the one statement of that pairing and a
// second field here could contradict it.
struct ValueUtilityOperand final {
    std::string_view role;
    SocketValueKind kind = SocketValueKind::Scalar;
    // The authored default, read according to `kind`: `number` for a Scalar and as the first
    // component of a vector, `y` and `z` for the rest of one, `integer`, `flag` and `text` for the
    // kinds that carry those. A Color operand always defaults to kDefaultValueColor -- opaque
    // white, the default every other colour schema in Bloom already has.
    double number = 0.0;
    double y = 0.0;
    double z = 0.0;
    std::int64_t integer = 0;
    bool flag = false;
    std::string_view text{};
};

// An inline selector: a closed integer vocabulary with NO socket, exactly as a Math node's
// operation already is. A mode is a choice the author makes about what the node IS, not a quantity
// something upstream computes, which is why it is not linkable.
struct ValueUtilitySelector final {
    std::string_view role;
    std::string_view schemaKey;
    std::int64_t defaultValue = 0;
};

struct ValueUtilityOutput final {
    std::string_view name;
    SocketValueKind kind = SocketValueKind::Scalar;
};

struct ValueUtilityDescriptor final {
    std::string_view typeId;
    // The artist's name for the node, read by the Add surfaces and the card header. Held here so
    // the node's shape and its name are one record rather than a type id repeated in a UI table.
    std::string_view displayName;
    ValueUtilityKernel kernel = ValueUtilityKernel::ScalarToString;
    NodeCategory category = NodeCategory::Utilities;
    std::span<const ValueUtilityOperand> operands;
    std::span<const ValueUtilitySelector> selectors;
    std::span<const ValueUtilityOutput> outputs;
};

// Every node type this library declares, in Add-surface reading order within each category.
[[nodiscard]] std::span<const ValueUtilityDescriptor> valueUtilityDescriptors() noexcept;

[[nodiscard]] const ValueUtilityDescriptor*
findValueUtilityDescriptor(std::string_view typeId) noexcept;

// The same record, found by the kernel it names. The evaluator has a compiled kernel and no type id
// -- a plan carries the operation, not the document's spelling of it -- and it needs the output
// SHAPE to fill a failed operation's fallbacks with values of the right kinds.
[[nodiscard]] const ValueUtilityDescriptor*
findValueUtilityDescriptor(ValueUtilityKernel kernel) noexcept;

// ---------------------------------------------------------------------------------------------
// Shared port names
// ---------------------------------------------------------------------------------------------
//
// Named here rather than spelled at each use for the reason value_nodes.hpp gives: a port name is
// the pairing between a socket and the parameter behind it, and a typo in one of two spellings
// would register a definition whose socket has no value.

inline constexpr std::string_view kTextPortName = "text";
inline constexpr std::string_view kFallbackPortName = "fallback";
inline constexpr std::string_view kValidPortName = "valid";
inline constexpr std::string_view kModeParameterRole = "mode";
inline constexpr std::string_view kRadixParameterRole = "radix";

} // namespace bloom::document
