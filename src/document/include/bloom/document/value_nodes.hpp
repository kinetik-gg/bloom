#pragma once

#include <bloom/core/color.hpp>
#include <bloom/document/value_operations.hpp>

#include <array>
#include <cstdint>
#include <string_view>

namespace bloom::document {

// The value-graph node library's own vocabulary (task S7): type ids, schema versions, and the port
// and role names each definition declares.
//
// Kept here rather than in graph.hpp because graph.hpp names the five STRUCTURAL node types a
// composition cannot exist without -- the layer stack, the composition output, the layer boundary
// and the two image sources -- and every one of those is load-bearing for the graph's own
// invariants. None of the types below is: a composition with no value node at all is a complete,
// valid composition, and the library is meant to grow.
//
// PORT NAMES ARE THE PAIRING. A non-Image input port and the ParameterDefinition that backs it
// share one name: that is what makes "unlinked shows the widget, linked shows only the socket" a
// single rule rather than a per-node table, and what lets a driver binding and an edge address the
// same authored value. A parameter with no same-named port is an inline-only selector; a port with
// no same-named parameter exists only on Reroute, which carries no value of its own at all.

// ---------------------------------------------------------------------------------------------
// Shared port and role names
// ---------------------------------------------------------------------------------------------

// The literal Value nodes, and Reroute: one value in, or one value out, named for what it is.
inline constexpr std::string_view kValuePortName = "value";
inline constexpr std::string_view kValueParameterRole = "value";
// Every computing node's single output.
inline constexpr std::string_view kResultPortName = "result";

// A Math node's operand sockets. Deliberately structural names rather than semantic ones: ONE
// definition serves all 24 scalar operations, whose operands mean different things in each
// (Remap's five are a value and two ranges; Mix's three are two endpoints and a factor). The card
// RELABELS each visible row with the live operation's own input name from
// core::primitives::scalarPrimitiveSignature(), so the artist reads "sourceMinimum" while the
// document stores "c" -- the stable name a saved edge can keep across an operation change.
inline constexpr std::array<std::string_view, kMaximumScalarOperationOperands>
    kScalarOperandPortNames{"a", "b", "c", "d", "e"};

inline constexpr std::string_view kFirstOperandPortName = "a";
inline constexpr std::string_view kSecondOperandPortName = "b";
inline constexpr std::string_view kOperationParameterRole = "operation";
inline constexpr std::string_view kClampResultParameterRole = "clamp";
inline constexpr std::string_view kInterpolationParameterRole = "interpolation";
inline constexpr std::string_view kEpsilonParameterRole = "epsilon";
inline constexpr std::string_view kScaleFactorPortName = "factor";

inline constexpr std::string_view kMapRangeValuePortName = "value";
inline constexpr std::string_view kMapRangeFromMinimumPortName = "fromMin";
inline constexpr std::string_view kMapRangeFromMaximumPortName = "fromMax";
inline constexpr std::string_view kMapRangeToMinimumPortName = "toMin";
inline constexpr std::string_view kMapRangeToMaximumPortName = "toMax";

inline constexpr std::string_view kClampValuePortName = "value";
inline constexpr std::string_view kClampMinimumPortName = "min";
inline constexpr std::string_view kClampMaximumPortName = "max";

inline constexpr std::string_view kMixFactorPortName = "factor";

inline constexpr std::string_view kSwitchConditionPortName = "condition";
inline constexpr std::string_view kSwitchFalsePortName = "ifFalse";
inline constexpr std::string_view kSwitchTruePortName = "ifTrue";

inline constexpr std::string_view kVectorPortName = "vector";
inline constexpr std::string_view kColorPortName = "color";
inline constexpr std::array<std::string_view, 3> kVectorComponentPortNames{"x", "y", "z"};
inline constexpr std::array<std::string_view, 4> kColorChannelPortNames{"red", "green", "blue",
                                                                        "alpha"};

inline constexpr std::string_view kRandomSeedPortName = "seed";
inline constexpr std::string_view kRandomMinimumPortName = "min";
inline constexpr std::string_view kRandomMaximumPortName = "max";

// Time has no inputs and no parameters at all: its value is the frame being rendered, which is the
// evaluator's to supply and not the document's to store.
inline constexpr std::string_view kTimeSecondsPortName = "seconds";
inline constexpr std::string_view kTimeFramePortName = "frame";

// ---------------------------------------------------------------------------------------------
// Type ids
// ---------------------------------------------------------------------------------------------

inline constexpr std::string_view kIntegerValueNodeType = "bloom.value-integer";
inline constexpr std::string_view kScalarValueNodeType = "bloom.value-scalar";
inline constexpr std::string_view kVector2ValueNodeType = "bloom.value-vector2";
inline constexpr std::string_view kVector3ValueNodeType = "bloom.value-vector3";
inline constexpr std::string_view kStringValueNodeType = "bloom.value-string";
inline constexpr std::string_view kColorValueNodeType = "bloom.value-color";
inline constexpr std::string_view kBooleanValueNodeType = "bloom.value-boolean";
inline constexpr std::string_view kTimeValueNodeType = "bloom.value-time";

inline constexpr std::string_view kScalarMathNodeType = "bloom.math";
inline constexpr std::string_view kVector2MathNodeType = "bloom.vector2-math";
inline constexpr std::string_view kVector3MathNodeType = "bloom.vector3-math";
inline constexpr std::string_view kVector2ReduceNodeType = "bloom.vector2-reduce";
inline constexpr std::string_view kVector3ReduceNodeType = "bloom.vector3-reduce";
inline constexpr std::string_view kMapRangeNodeType = "bloom.map-range";
inline constexpr std::string_view kClampNodeType = "bloom.clamp";
inline constexpr std::string_view kMixNodeType = "bloom.mix";
inline constexpr std::string_view kColorMixNodeType = "bloom.mix-color";
inline constexpr std::string_view kCompareNodeType = "bloom.compare";

inline constexpr std::string_view kScalarSwitchNodeType = "bloom.switch-scalar";
inline constexpr std::string_view kIntegerSwitchNodeType = "bloom.switch-integer";
inline constexpr std::string_view kBooleanSwitchNodeType = "bloom.switch-boolean";
inline constexpr std::string_view kVector2SwitchNodeType = "bloom.switch-vector2";
inline constexpr std::string_view kVector3SwitchNodeType = "bloom.switch-vector3";
inline constexpr std::string_view kColorSwitchNodeType = "bloom.switch-color";
inline constexpr std::string_view kStringSwitchNodeType = "bloom.switch-string";

inline constexpr std::string_view kSeparateXyNodeType = "bloom.separate-xy";
inline constexpr std::string_view kCombineXyNodeType = "bloom.combine-xy";
inline constexpr std::string_view kSeparateXyzNodeType = "bloom.separate-xyz";
inline constexpr std::string_view kCombineXyzNodeType = "bloom.combine-xyz";
inline constexpr std::string_view kSeparateRgbaNodeType = "bloom.separate-rgba";
inline constexpr std::string_view kCombineRgbaNodeType = "bloom.combine-rgba";

inline constexpr std::string_view kRandomNodeType = "bloom.random";

inline constexpr std::string_view kImageRerouteNodeType = "bloom.reroute-image";
inline constexpr std::string_view kScalarRerouteNodeType = "bloom.reroute-scalar";
inline constexpr std::string_view kIntegerRerouteNodeType = "bloom.reroute-integer";
inline constexpr std::string_view kBooleanRerouteNodeType = "bloom.reroute-boolean";
inline constexpr std::string_view kVector2RerouteNodeType = "bloom.reroute-vector2";
inline constexpr std::string_view kVector3RerouteNodeType = "bloom.reroute-vector3";
inline constexpr std::string_view kColorRerouteNodeType = "bloom.reroute-color";
inline constexpr std::string_view kStringRerouteNodeType = "bloom.reroute-string";

// One schema version for the whole library's first appearance. Each type versions independently
// from here -- the constant is shared only because every one of them ships at version 1 in this
// slice, not because they are bound together.
inline constexpr std::uint32_t kValueNodeSchemaVersion = 1;

// ---------------------------------------------------------------------------------------------
// Defaults
// ---------------------------------------------------------------------------------------------

// A Map Range's and a Mix's unit source range, and a Clamp's unit bound pair: the ranges every one
// of them is most often used with, so a freshly added node already does something meaningful.
inline constexpr double kDefaultRangeMinimum = 0.0;
inline constexpr double kDefaultRangeMaximum = 1.0;
inline constexpr double kDefaultMixFactor = 0.5;
// White, matching the Solid and Text colour defaults so a colour the artist has not yet chosen
// reads the same wherever it appears.
inline constexpr core::Color4d kDefaultValueColor{1.0, 1.0, 1.0, 1.0};

} // namespace bloom::document
