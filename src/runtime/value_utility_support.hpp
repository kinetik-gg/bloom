#pragma once

#include <bloom/runtime/value_utility_kernels.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>

namespace bloom::runtime::detail {

// The shared machinery every library kernel uses to read its operands and to answer with the right
// SHAPE whether or not it succeeded. Written once, here, so a kernel's body is the operation it
// performs and nothing else.

// One operand read as the alternative its socket declares. A kind that does not match is a
// MALFORMED PLAN -- the document's typing already refuses such a link, and every widening the
// whitelist allows became its own promotion operation upstream -- so it is recorded and reported
// rather than coerced.
class ValueUtilityReader final {
  public:
    explicit ValueUtilityReader(const ValueUtilityInvocation& invocation) noexcept
        : invocation_(&invocation) {}

    [[nodiscard]] double scalar(const std::size_t index) noexcept {
        const auto* value = read<double>(index);
        return value == nullptr ? 0.0 : *value;
    }
    [[nodiscard]] std::int64_t integer(const std::size_t index) noexcept {
        const auto* value = read<std::int64_t>(index);
        return value == nullptr ? 0 : *value;
    }
    [[nodiscard]] bool boolean(const std::size_t index) noexcept {
        const auto* value = read<bool>(index);
        return value != nullptr && *value;
    }
    [[nodiscard]] std::string_view text(const std::size_t index) noexcept {
        const auto* value = read<std::string>(index);
        return value == nullptr ? std::string_view{} : std::string_view{*value};
    }
    [[nodiscard]] core::Color4d color(const std::size_t index) noexcept {
        const auto* value = read<core::Color4d>(index);
        return value == nullptr ? core::Color4d{} : *value;
    }
    [[nodiscard]] document::Vec2d vector2(const std::size_t index) noexcept {
        const auto* value = read<document::Vec2d>(index);
        return value == nullptr ? document::Vec2d{} : *value;
    }
    [[nodiscard]] document::Vec3d vector3(const std::size_t index) noexcept {
        const auto* value = read<document::Vec3d>(index);
        return value == nullptr ? document::Vec3d{} : *value;
    }

    // A count, an index or a width, narrowed to the range a formatter or a buffer can hold. A
    // document may store any std::int64_t in these operands -- they are ordinary integer operands
    // and something upstream may have computed one -- so the clamp is part of the contract rather
    // than a guard against a malformed plan.
    [[nodiscard]] std::int32_t bounded(const std::size_t index, const std::int32_t lowest,
                                       const std::int32_t highest) noexcept {
        const auto value = integer(index);
        if (value <= static_cast<std::int64_t>(lowest)) {
            return lowest;
        }
        if (value >= static_cast<std::int64_t>(highest)) {
            return highest;
        }
        return static_cast<std::int32_t>(value);
    }

    [[nodiscard]] bool malformed() const noexcept { return malformed_; }
    [[nodiscard]] std::size_t malformedOperand() const noexcept { return malformedOperand_; }

  private:
    template <typename Value> [[nodiscard]] const Value* read(const std::size_t index) noexcept {
        if (index >= invocation_->operands.size()) {
            note(index);
            return nullptr;
        }
        const auto* held = invocation_->operands[index];
        if (held == nullptr) {
            note(index);
            return nullptr;
        }
        const auto* typed = std::get_if<Value>(held);
        if (typed == nullptr) {
            note(index);
        }
        return typed;
    }

    void note(const std::size_t index) noexcept {
        if (!malformed_) {
            malformed_ = true;
            malformedOperand_ = index;
        }
    }

    const ValueUtilityInvocation* invocation_;
    bool malformed_ = false;
    std::size_t malformedOperand_ = 0;
};

// The zero of a socket kind: what a failed operation writes, and what an output slot holds before
// anything writes it.
[[nodiscard]] inline CompiledValue zeroValueOf(const document::SocketValueKind kind) {
    switch (kind) {
    case document::SocketValueKind::Integer:
        return std::int64_t{0};
    case document::SocketValueKind::Boolean:
        return false;
    case document::SocketValueKind::String:
        return std::string{};
    case document::SocketValueKind::Vector2:
        return document::Vec2d{};
    case document::SocketValueKind::Vector3:
        return document::Vec3d{};
    case document::SocketValueKind::Color:
        return core::Color4d{};
    case document::SocketValueKind::Image:
    case document::SocketValueKind::Scalar:
        break;
    }
    return 0.0;
}

// An outcome already shaped for `kernel`: the right number of outputs, each the zero of its own
// declared kind. Every kernel starts from one of these, so a path that forgets to write a slot
// writes a usable value of the right alternative rather than leaving a Scalar where a String
// belongs.
[[nodiscard]] inline ValueUtilityOutcome shapedOutcome(const document::ValueUtilityKernel kernel) {
    ValueUtilityOutcome outcome;
    const auto* descriptor = document::findValueUtilityDescriptor(kernel);
    if (descriptor == nullptr) {
        return outcome;
    }
    const auto count = std::min(descriptor->outputs.size(), kMaximumValueUtilityOutputs);
    outcome.outputCount = static_cast<std::uint8_t>(count);
    for (std::size_t slot = 0; slot < count; ++slot) {
        outcome.outputs[slot] = zeroValueOf(descriptor->outputs[slot].kind);
    }
    return outcome;
}

// Records a reader's malformed-plan failure on an outcome that already carries its fallbacks.
inline void noteMalformedOperand(ValueUtilityOutcome& outcome, const ValueUtilityReader& reader) {
    if (!reader.malformed()) {
        return;
    }
    outcome.failed = true;
    outcome.failedOperand = reader.malformedOperand();
    outcome.summary = "Value utility operand has the wrong kind";
    outcome.detail = "Every operand must reach the kernel as the kind its socket declares.";
}

// One selector, read as its own vocabulary. A stored value naming no member is treated as the
// vocabulary's default rather than trusted: document validation already refuses one, so reaching
// here means the plan disagrees with the document, and computing the default is a better answer
// than computing whatever the cast produced.
template <typename Enumeration, std::size_t Count>
[[nodiscard]] Enumeration
selectorAt(const ValueUtilityInvocation& invocation, const std::size_t index,
           const std::array<Enumeration, Count>& vocabulary, const Enumeration fallback) noexcept {
    if (index >= invocation.selectors.size()) {
        return fallback;
    }
    return document::selectorFromStoredValue(vocabulary, invocation.selectors[index])
        .value_or(fallback);
}

// The radix selector. The stored value IS the radix, so the check is a range rather than a
// vocabulary lookup.
[[nodiscard]] inline std::int32_t radixAt(const ValueUtilityInvocation& invocation,
                                          const std::size_t index) noexcept {
    if (index >= invocation.selectors.size() ||
        !document::isValidStoredRadix(invocation.selectors[index])) {
        return core::kDefaultRadix;
    }
    return static_cast<std::int32_t>(invocation.selectors[index]);
}

// The family entry points. One per deliverable's worth of kernels, so no single translation unit
// carries the whole library.
[[nodiscard]] ValueUtilityOutcome evaluateValueConversion(const ValueUtilityInvocation& invocation);
[[nodiscard]] ValueUtilityOutcome evaluateValueTime(const ValueUtilityInvocation& invocation);
[[nodiscard]] ValueUtilityOutcome evaluateValueString(const ValueUtilityInvocation& invocation);

} // namespace bloom::runtime::detail
