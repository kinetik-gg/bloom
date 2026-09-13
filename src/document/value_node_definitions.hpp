#pragma once

#include <bloom/document/node_definition_registry.hpp>

#include <vector>

namespace bloom::document::detail {

// The value-graph half of the built-in registry (task S7), kept in its own translation unit because
// the library is a long, flat table of small definitions while node_definition_registry.cpp is the
// registry's own machinery plus the five structural node types. One file per concern, and neither
// grows into the other.
[[nodiscard]] std::vector<NodeDefinition> valueNodeDefinitions();

// The ONE shape contract every value lowering is held to, stated generically rather than once per
// lowering. See the implementation's comment for what it requires and why a generic rule is the
// right shape here when the five image lowerings each get a bespoke one.
[[nodiscard]] bool hasValidValueLoweringShape(const NodeDefinition& definition) noexcept;

// Whether this parameter is an INLINE SELECTOR -- a value that decides which kernel the plan
// compiles -- rather than an operand a driver may deliver per frame. A selector has no input
// socket, which is exactly why it needs its own predicate: the shape check uses it to tell a
// missing socket from a deliberate absence, and the editor uses it to know which rows never hide.
[[nodiscard]] bool isInlineSelectorSchemaKey(std::string_view schemaKey) noexcept;

} // namespace bloom::document::detail
