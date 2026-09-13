#pragma once

#include <bloom/core/rational_time.hpp>
#include <bloom/document/composition_settings.hpp>
#include <bloom/document/ids.hpp>
#include <bloom/runtime/compiled_curves.hpp>
#include <bloom/runtime/compiled_value_graph.hpp>

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace bloom::runtime {

// The per-frame evaluation of a compiled plan's value graph.
//
// Pure and self-contained: it reads a compiled operation list and the request time, and answers
// with one value per output. No image, no allocation beyond the output table, no dependence on the
// image chain -- which is what makes it directly testable against golden values for every kernel,
// and what lets the frame evaluator call it once before touching a pixel.
//
// FAILURE PHILOSOPHY. The bloom_core kernels beneath this report domain failures and never
// substitute a value, because only the caller knows what the number is for. Here, at the node
// boundary, that caller exists: every operation has a documented fallback, and a failure
// SUBSTITUTES it and records a scoped diagnostic so the frame still renders. That matches how a
// missing module or a muted node is already treated -- the picture degrades visibly and says why --
// rather than aborting a whole frame because one divisor reached zero.

// One failure inside the value graph, scoped to the node and (where one exists) the parameter whose
// operand was at fault, so a diagnostic names the exact thing the artist has to change.
struct ValueGraphDiagnostic final {
    document::NodeId nodeId;
    document::ParameterId parameterId;
    ValueOutputIndex output = ValueOutputIndex::fromRaw(0);
    std::string summary;
    std::string detail;

    friend bool operator==(const ValueGraphDiagnostic&, const ValueGraphDiagnostic&) = default;
};

struct ValueGraphEvaluation final {
    // One entry per plan value output, in the plan's own flat order. Always exactly
    // `valueOutputCount` long: an operation that failed wrote its fallback, so a reader never has
    // to ask whether an output exists.
    std::vector<CompiledValue> outputs;
    std::vector<ValueGraphDiagnostic> diagnostics;
};

// floor(time * rate) as an exact integer, with no floating point anywhere -- the same discipline
// core::FrameTimeMapping holds itself to, so a Time node's frame number and the timeline's agree.
// An overflow in the reduced product is reported rather than wrapped.
[[nodiscard]] std::optional<std::int64_t> valueGraphFrameIndex(core::RationalTime time,
                                                               document::FrameRate rate) noexcept;

// The plan's three curve tables, as the value graph reads them (task FIX1, item G). A literal
// Scalar, Vector 2 or Colour node whose authored value is on a curve lowers to an index into one of
// these, and the evaluator samples it at the request time -- the same tables, the same sampler and
// the same semantics version the image chain's own animated parameters already use.
struct ValueGraphCurves final {
    std::span<const CompiledScalarCurve> scalar;
    std::span<const CompiledVec2Curve> vec2;
    std::span<const CompiledColor4Curve> color4;
};

// Evaluates every operation in order. `operations` must already be topologically ordered (the
// compiler emits them that way), so one linear sweep is enough and no operand can name an output
// that has not been written yet.
[[nodiscard]] ValueGraphEvaluation
evaluateValueGraph(std::span<const CompiledValueOperation> operations, std::size_t outputCount,
                   core::RationalTime time, document::FrameRate rate, ValueGraphCurves curves = {});

} // namespace bloom::runtime
