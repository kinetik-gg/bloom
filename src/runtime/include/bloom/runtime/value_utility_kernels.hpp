#pragma once

#include <bloom/core/rational_time.hpp>
#include <bloom/document/composition_settings.hpp>
#include <bloom/document/value_utility_nodes.hpp>
#include <bloom/runtime/compiled_value_graph.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace bloom::runtime {

// The kernels behind task UTIL-1's library: PURE functions from already-read operand values to
// already-typed output values.
//
// Deliberately separated from value_graph_evaluation.cpp rather than added to it as sixty more
// Evaluator methods. The Evaluator owns the plan's bookkeeping -- the output table, memoization,
// diagnostics, the per-operand read -- and none of that has anything to do with what "substring"
// means. Splitting them keeps each file about one thing, keeps every kernel directly testable
// without building a plan at all, and keeps value_graph_evaluation.cpp from growing another
// six hundred lines of string handling.
//
// THE FAILURE MODEL IS THE LIBRARY'S, NOT THE PARSER'S. A parsing node NEVER fails: it answers the
// parsed value or the authored fallback, and a Boolean saying which. The only failure an outcome
// here can report is a MALFORMED PLAN -- an operand that arrived as the wrong alternative, which
// the document's typing already refuses and which therefore means the plan disagrees with the
// document. That is reported, and the node's documented fallbacks are substituted, exactly as
// every other value kernel treats one.

// No library node declares more than four outputs (Separate HSV's four channels), so the outcome
// carries its values inline rather than allocating one vector per operation per frame.
inline constexpr std::size_t kMaximumValueUtilityOutputs = 4;

struct ValueUtilityInvocation final {
    document::ValueUtilityKernel operation = document::ValueUtilityKernel::ScalarToString;
    // One pointer per socket-backed operand, in the node's descriptor order. A null entry is an
    // operand the plan could not supply; the kernel reports it rather than reading it.
    std::span<const CompiledValue* const> operands;
    // The inline selectors, in descriptor order. A selector the document validated is in range;
    // one that is not is treated as its vocabulary's default rather than trusted.
    std::span<const std::int64_t> selectors;
    // The frame being rendered and the composition's rate, which the time conversions read. Exactly
    // the pair a Time node already reads, so a Seconds To Frames node and a Time node can never
    // disagree about which frame "now" is.
    core::RationalTime time;
    document::FrameRate rate = document::FrameRate::framesPerSecond24();
    const document::DataBlockRecord* dataBlock = nullptr;
};

struct ValueUtilityOutcome final {
    std::array<CompiledValue, kMaximumValueUtilityOutputs> outputs{};
    std::uint8_t outputCount = 1;
    // Set only for a malformed plan. `summary` and `detail` are static text, not formatted per
    // frame: a diagnostic that allocated a string on every failing frame would cost more in a
    // broken plan than in a working one.
    bool failed = false;
    std::string_view summary;
    std::string_view detail;
    // Which operand was at fault, so the evaluator can name its parameter. Past the operand count
    // when the fault belongs to the operation rather than to one operand.
    std::size_t failedOperand = 0;
};

// Evaluates one library node. Total: every path writes `outputCount` values of the kinds the node's
// descriptor declares, whether or not it reports a failure.
[[nodiscard]] ValueUtilityOutcome evaluateValueUtility(const ValueUtilityInvocation& invocation);

} // namespace bloom::runtime
