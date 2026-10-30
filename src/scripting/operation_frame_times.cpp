// The one time contract every scripting client shares: an integer time is a whole frame of the
// composition the operation edits, and a [numerator, denominator] pair is an exact time in
// seconds. Factories read seconds; this file turns frame counts into seconds before a factory
// sees them, so no operation has to know which spelling the caller used.
//
// The frame rate is the composition's own at the moment the operation APPLIES, so a transaction
// that changes the frame rate and then sets a duration in frames uses the new rate. An operation
// whose factory must measure something at creation (align and distribute) converts at creation
// instead, from the context's frame-rate lookup.
#include "operation_values.hpp"

#include <bloom/document/project.hpp>

#include <array>
#include <limits>
#include <memory>
#include <string_view>
#include <utility>

namespace bloom::scripting::detail {
namespace {

// Times nested inside a list argument: {operation, argument, index of the time in each entry}.
struct NestedTime final {
    std::string_view operation;
    std::string_view argument;
    std::size_t index;
};
constexpr std::array<NestedTime, 2> kNestedTimes{{
    {"bloom.animation.move-keyframes", "keys", 2},
    {"bloom.animation.paste-keyframes", "keys", 1},
}};

bool isFrameCount(const Value& value) {
    if (std::holds_alternative<std::int64_t>(value.storage))
        return true;
    const auto* real = std::get_if<double>(&value.storage);
    return real != nullptr && std::isfinite(*real) && std::floor(*real) == *real;
}

// Every frame-count time among the arguments, as pointers into them.
template <typename Visit>
void forEachFrameTime(const OperationSchema& schema, Arguments& arguments, Visit visit) {
    for (const auto& argument : schema.arguments) {
        if (argument.kind != ValueKind::Time)
            continue;
        const auto found = arguments.find(argument.name);
        if (found != arguments.end() && isFrameCount(found->second))
            visit(found->second);
    }
    for (const auto& nested : kNestedTimes) {
        if (nested.operation != schema.typeId)
            continue;
        const auto found = arguments.find(nested.argument);
        auto* list =
            found == arguments.end() ? nullptr : std::get_if<ValueArray>(&found->second.storage);
        if (list == nullptr)
            continue;
        for (auto& item : *list) {
            auto* entry = std::get_if<ValueArray>(&item.storage);
            if (entry != nullptr && entry->size() > nested.index &&
                isFrameCount((*entry)[nested.index]))
                visit((*entry)[nested.index]);
        }
    }
}

// frames * denominator / numerator seconds, exactly; std::nullopt when it cannot be represented.
std::optional<Value> secondsOf(const Value& frames, const std::uint32_t numerator,
                               const std::uint32_t denominator) {
    const auto count = integerOf(frames);
    if (!count || numerator == 0 || denominator == 0)
        return std::nullopt;
    constexpr auto limit = std::numeric_limits<std::int64_t>::max() / 4294967296LL;
    if (*count > limit || *count < -limit)
        return std::nullopt;
    const auto time = core::RationalTime::create(*count * static_cast<std::int64_t>(denominator),
                                                 static_cast<std::int64_t>(numerator));
    if (!time)
        return std::nullopt;
    return Value(ValueArray{Value(time->numerator()), Value(time->denominator())});
}

// Rewrites every frame count to seconds at the given rate; false names nothing when it fails.
bool toSeconds(const OperationSchema& schema, Arguments& arguments, const std::uint32_t numerator,
               const std::uint32_t denominator) {
    bool converted = true;
    forEachFrameTime(schema, arguments, [&](Value& value) {
        if (const auto seconds = secondsOf(value, numerator, denominator))
            value = *seconds;
        else
            converted = false;
    });
    return converted;
}

bool hasArgument(const OperationSchema& schema, const std::string_view name) {
    return std::ranges::find(schema.arguments, name, &ArgumentSchema::name) !=
           schema.arguments.end();
}

std::optional<std::uint32_t> rateComponent(const Arguments& arguments, const std::string& name) {
    const auto found = arguments.find(name);
    if (found == arguments.end())
        return std::nullopt;
    const auto value = integerOf(found->second);
    if (!value || *value <= 0 || *value > std::numeric_limits<std::uint32_t>::max())
        return std::nullopt;
    return static_cast<std::uint32_t>(*value);
}

// An operation whose frame counts become seconds when it applies, at its composition's rate. The
// probe is the same operation built at a placeholder rate: it answers the questions the command
// stack asks before applying (type id, render classification, scope), none of which depend on a
// time value.
class FrameTimedOperation final : public commands::Operation {
  public:
    using Factory = std::function<OperationCreateResult(const Arguments&)>;

    FrameTimedOperation(std::unique_ptr<commands::Operation> probe, OperationSchema schema,
                        Arguments arguments, document::CompositionId composition, Factory factory)
        : probe_(std::move(probe)), schema_(std::move(schema)), arguments_(std::move(arguments)),
          composition_(composition), factory_(std::move(factory)) {}

    [[nodiscard]] std::string_view typeId() const noexcept override { return probe_->typeId(); }
    [[nodiscard]] bool renderAffecting() const noexcept override {
        return probe_->renderAffecting();
    }
    [[nodiscard]] std::optional<document::CompositionId>
    parameterEditScope() const noexcept override {
        return probe_->parameterEditScope();
    }
    [[nodiscard]] std::optional<document::CompositionId> renderScope() const noexcept override {
        return probe_->renderScope();
    }

    [[nodiscard]] commands::OperationResult apply(document::Draft& draft) const override {
        const auto* composition = std::as_const(draft).project().findComposition(composition_);
        if (composition == nullptr)
            return commands::OperationResult::rejected(
                commands::OperationIssueCode::InvalidTarget,
                "Composition " + std::to_string(composition_.value()) +
                    " does not exist, so a time in frames has no frame rate to count in.");
        const auto rate = composition->format().frameRate();
        auto arguments = arguments_;
        if (!toSeconds(schema_, arguments, rate.numerator(), rate.denominator()))
            return commands::OperationResult::rejected(
                commands::OperationIssueCode::InvalidValue,
                "A frame count is too large to convert to a time at composition " +
                    std::to_string(composition_.value()) + "'s frame rate.");
        auto created = factory_(arguments);
        if (!created) {
            const auto* diagnostic = created.diagnostic();
            return commands::OperationResult::rejected(
                commands::OperationIssueCode::InvalidValue,
                diagnostic == nullptr ? std::string("The operation could not be created.")
                                      : diagnostic->message);
        }
        const auto operation = std::move(created).takeOperation();
        return operation->apply(draft);
    }

  private:
    std::unique_ptr<commands::Operation> probe_;
    OperationSchema schema_;
    Arguments arguments_;
    document::CompositionId composition_;
    Factory factory_;
};

} // namespace

OperationCreateResult
createWithFrameTimes(const OperationSchema& schema, const Arguments& arguments,
                     const OperationContext* context, const bool measuresNow,
                     const std::function<OperationCreateResult(const Arguments&)>& factory) {
    bool frames = false;
    {
        auto probe = arguments;
        forEachFrameTime(schema, probe, [&](Value&) { frames = true; });
    }
    if (!frames)
        return factory(arguments);
    const auto failure = [&](std::string message) {
        return argumentFailure(schema.typeId, "time", std::move(message));
    };
    // A frame rate the operation itself sets (Add Composition, or a settings change that also
    // changes the rate) is the rate its own times count in.
    if (hasArgument(schema, "frameRateNumerator") &&
        (arguments.contains("frameRateNumerator") || !hasArgument(schema, "composition"))) {
        const auto numerator = rateComponent(arguments, "frameRateNumerator").value_or(24);
        const auto denominator = rateComponent(arguments, "frameRateDenominator").value_or(1);
        auto converted = arguments;
        if (!toSeconds(schema, converted, numerator, denominator))
            return failure("expects a frame count that fits the frame rate");
        return factory(converted);
    }
    const auto composition = arguments.find("composition");
    const auto compositionId =
        composition == arguments.end() ? std::nullopt : idOf(composition->second);
    if (!compositionId)
        return failure("a time in frames needs the composition it counts in; pass composition, "
                       "or give the time as an exact [numerator, denominator] in seconds");
    // A factory that measures at creation needs seconds now, from the context's rate.
    if (context != nullptr && context->compositionFrameRate) {
        if (const auto rate = context->compositionFrameRate(*compositionId);
            rate && rate->first != 0 && rate->second != 0) {
            auto converted = arguments;
            if (!toSeconds(schema, converted, rate->first, rate->second))
                return failure("expects a frame count that fits the frame rate");
            return factory(converted);
        }
    }
    if (measuresNow)
        return failure("a time in frames needs the composition's frame rate, which this client "
                       "cannot read; give the time as an exact [numerator, denominator] in "
                       "seconds");
    // Otherwise the composition's rate is read when the operation applies. The probe validates
    // every other argument now, so a malformed call is still refused at creation.
    auto probeArguments = arguments;
    static_cast<void>(toSeconds(schema, probeArguments, 1, 1));
    auto probe = factory(probeArguments);
    if (!probe)
        return probe;
    return OperationCreateResult(std::make_unique<FrameTimedOperation>(
                                     std::move(probe).takeOperation(), schema, arguments,
                                     document::CompositionId::fromRaw(*compositionId), factory),
                                 std::nullopt);
}

} // namespace bloom::scripting::detail
