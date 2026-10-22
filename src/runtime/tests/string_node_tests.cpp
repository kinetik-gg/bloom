#include <bloom/runtime/task_scheduler.hpp>
#include <bloom/runtime/value_utility_kernels.hpp>

#include <bloom/document/value_nodes.hpp>
#include <bloom/document/value_utility_nodes.hpp>

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <mutex>
#include <optional>
#include <source_location>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {

using namespace bloom;
using runtime::CompiledValue;
using runtime::ValueUtilityOutcome;

using Kernel = document::ValueUtilityKernel;

class Expectations final {
  public:
    void expect(const bool condition, const std::string_view message,
                const std::source_location location = std::source_location::current()) {
        if (condition) {
            return;
        }
        ++failures_;
        std::cerr << location.file_name() << ':' << location.line() << ": " << message << '\n';
    }

    [[nodiscard]] int failures() const noexcept { return failures_; }

  private:
    int failures_ = 0;
};

[[nodiscard]] ValueUtilityOutcome run(const Kernel kernel, const std::vector<CompiledValue>& values,
                                      const std::vector<std::int64_t>& selectors = {},
                                      const runtime::ValueUtilityLimits limits = {}) {
    std::vector<const CompiledValue*> operands;
    operands.reserve(values.size());
    for (const auto& value : values) {
        operands.push_back(&value);
    }
    return runtime::evaluateValueUtility(
        {kernel, operands, selectors, core::RationalTime::fromInteger(0),
         document::FrameRate::framesPerSecond24(), nullptr, limits});
}

template <typename Value>
[[nodiscard]] bool holds(const ValueUtilityOutcome& outcome, const std::size_t slot,
                         const Value& expected) {
    if (slot >= outcome.outputCount) {
        return false;
    }
    const auto* value = std::get_if<Value>(&outcome.outputs[slot]);
    return value != nullptr && *value == expected;
}

[[nodiscard]] CompiledValue text(const std::string_view value) { return std::string(value); }

// Two characters that are ONE unit each and more than one byte, so every measured operation is
// checked against code points rather than bytes.
constexpr std::string_view kAcute = "é";       // two bytes
constexpr std::string_view kEmoji = "🌸";      // four bytes
constexpr std::string_view kMixed = "aébc🌸d"; // six units, nine bytes

void testConcatenateAndFormat(Expectations& expectations) {
    struct ConcatenateCase final {
        std::string_view a;
        std::string_view b;
        std::string_view c;
        std::string_view d;
        std::string_view separator;
        std::string_view result;
    };
    static const ConcatenateCase kCases[]{
        {"one", "two", "", "", "", "onetwo"},
        {"one", "two", "", "", " ", "one two"},
        // Empty parts are SKIPPED, separator and all: four operands is the shape, but most uses
        // fill two, and joining the rest would leave trailing separators in every caption.
        {"one", "", "three", "", "-", "one-three"},
        {"", "", "", "", "-", ""},
        {"a", "b", "c", "d", ", ", "a, b, c, d"},
        {"", "only", "", "", "-", "only"},
    };
    for (const auto& item : kCases) {
        const auto outcome =
            run(Kernel::StringConcatenate,
                {text(item.a), text(item.b), text(item.c), text(item.d), text(item.separator)});
        expectations.expect(!outcome.failed && holds(outcome, 0, std::string(item.result)),
                            "Concatenate joins the filled parts and nothing else");
    }

    struct FormatCase final {
        std::string_view pattern;
        std::string_view result;
    };
    static const FormatCase kFormats[]{
        {"{0} {1}", "alpha beta"},
        {"{1}{0}", "betaalpha"},
        {"{3}", "delta"},
        // A slot the node does not have is replaced by NOTHING: a literal {7} in a rendered caption
        // would look like a bug in the composition rather than in the template.
        {"[{7}]", "[]"},
        {"{{0}}", "{0}"},
        {"{{}}", "{}"},
        // Unmatched braces are literal: a template is not a program, and refusing to render one
        // would replace a slightly wrong caption with no caption.
        {"{oops", "{oops"},
        {"a}b", "a}b"},
        {"{}", "{}"},
        {"", ""},
        {"plain", "plain"},
    };
    for (const auto& item : kFormats) {
        const auto outcome =
            run(Kernel::StringFormat,
                {text(item.pattern), text("alpha"), text("beta"), text("gamma"), text("delta")});
        expectations.expect(!outcome.failed && holds(outcome, 0, std::string(item.result)),
                            "Format substitutes its slots and escapes its braces");
    }
}

// UTF-8 AWARENESS, which is the whole point of these four: an artist counts characters, not bytes.
void testMeasuredOperations(Expectations& expectations) {
    struct LengthCase final {
        std::string_view value;
        std::int64_t length;
    };
    static const LengthCase kLengths[]{
        {"", 0},
        {"abc", 3},
        {kAcute, 1},
        {kEmoji, 1},
        {kMixed, 6},
        // A byte that is not a well-formed scalar counts as ONE unit and survives unchanged: a
        // document may carry text from anywhere, and refusing to measure it would stop a whole
        // graph at a caption nobody can see is broken.
        {"\xff\xfe", 2},
    };
    for (const auto& item : kLengths) {
        expectations.expect(holds(run(Kernel::StringLength, {text(item.value)}), 0, item.length),
                            "Length counts Unicode scalars, not bytes");
    }

    struct SubstringCase final {
        std::string_view value;
        std::int64_t start;
        std::int64_t length;
        std::string_view result;
    };
    static const SubstringCase kSubstrings[]{
        {"abcdef", 0, 3, "abc"},
        {"abcdef", 3, 3, "def"},
        {"abcdef", 2, -1, "cdef"},
        // Clamped at both ends rather than refused.
        {"abcdef", 4, 10, "ef"},
        {"abcdef", 10, 3, ""},
        {"abcdef", -5, 2, "ab"},
        {"abcdef", 0, 0, ""},
        {kMixed, 1, 1, kAcute},
        {kMixed, 4, 1, kEmoji},
        {kMixed, 0, -1, kMixed},
        {"", 0, 5, ""},
    };
    for (const auto& item : kSubstrings) {
        const auto outcome =
            run(Kernel::StringSubstring, {text(item.value), item.start, item.length});
        expectations.expect(!outcome.failed && holds(outcome, 0, std::string(item.result)),
                            "Substring clamps its range and cuts on scalar boundaries");
    }

    struct CharacterCase final {
        std::string_view value;
        std::int64_t index;
        bool valid;
        std::string_view result;
    };
    static const CharacterCase kCharacters[]{
        {"abc", 0, true, "a"},     {"abc", 2, true, "c"},  {kMixed, 1, true, kAcute},
        {kMixed, 4, true, kEmoji}, {"abc", 3, false, "?"}, {"abc", -1, false, "?"},
        {"", 0, false, "?"},
    };
    for (const auto& item : kCharacters) {
        const auto outcome =
            run(Kernel::StringCharacterAt, {text(item.value), item.index, text("?")});
        expectations.expect(holds(outcome, 0, std::string(item.result)) &&
                                holds(outcome, 1, item.valid) && !outcome.failed,
                            "Character At answers one scalar or the authored fallback");
    }

    struct PadCase final {
        std::string_view value;
        std::int64_t width;
        std::string_view fill;
        document::StringPadSide side;
        std::string_view result;
    };
    static const PadCase kPads[]{
        {"7", 3, "0", document::StringPadSide::Start, "007"},
        {"7", 3, "0", document::StringPadSide::End, "700"},
        {"abcd", 3, "0", document::StringPadSide::Start, "abcd"},
        {kAcute, 3, "-", document::StringPadSide::Start, "--é"},
        // A multi-unit fill contributes its FIRST unit only, so the padded width does not depend on
        // how the fill divides into the gap.
        {"x", 3, "ab", document::StringPadSide::Start, "aax"},
        {"x", 3, "", document::StringPadSide::Start, "x"},
        {"x", 0, "-", document::StringPadSide::Start, "x"},
        {"x", -4, "-", document::StringPadSide::Start, "x"},
    };
    for (const auto& item : kPads) {
        const auto outcome = run(Kernel::StringPad, {text(item.value), item.width, text(item.fill)},
                                 {document::selectorStoredValue(item.side)});
        expectations.expect(!outcome.failed && holds(outcome, 0, std::string(item.result)),
                            "Pad measures its width in scalars and fills from one side");
    }
}

void testEditingOperations(Expectations& expectations) {
    struct SplitCase final {
        std::string_view value;
        std::string_view separator;
        std::int64_t index;
        bool valid;
        std::string_view result;
    };
    static const SplitCase kSplits[]{
        {"a,b,c", ",", 0, true, "a"},
        {"a,b,c", ",", 2, true, "c"},
        {"a,b,c", ",", 3, false, "?"},
        {"a,b,c", ",", -1, false, "?"},
        {"a::b", "::", 1, true, "b"},
        {"abc", ",", 0, true, "abc"},
        {"a,,c", ",", 1, true, ""},
        {",a", ",", 0, true, ""},
        // An empty separator matches everywhere, so the pieces it would produce are not a
        // decomposition of the text.
        {"abc", "", 0, false, "?"},
        {"", ",", 0, true, ""},
    };
    for (const auto& item : kSplits) {
        const auto outcome = run(Kernel::StringSplit,
                                 {text(item.value), text(item.separator), item.index, text("?")});
        expectations.expect(holds(outcome, 0, std::string(item.result)) &&
                                holds(outcome, 1, item.valid) && !outcome.failed,
                            "Split answers one piece or the authored fallback");
    }

    struct ReplaceCase final {
        std::string_view value;
        std::string_view search;
        std::string_view replacement;
        std::string_view result;
    };
    static const ReplaceCase kReplacements[]{
        {"aaa", "a", "b", "bbb"},
        {"abcabc", "bc", "X", "aXaX"},
        {"abc", "z", "X", "abc"},
        {"aaa", "aa", "b", "ba"},
        {"abc", "abc", "", ""},
        // An empty search matches everywhere and nowhere: replacing it would either loop forever or
        // interleave the replacement between every character.
        {"abc", "", "X", "abc"},
        {"", "a", "b", ""},
    };
    for (const auto& item : kReplacements) {
        const auto outcome = run(Kernel::StringReplace,
                                 {text(item.value), text(item.search), text(item.replacement)});
        expectations.expect(!outcome.failed && holds(outcome, 0, std::string(item.result)),
                            "Replace rewrites every occurrence and loops on none");
    }

    for (const auto& pair : {std::pair<std::string_view, std::string_view>{"  a b  ", "a b"},
                             {"\t\nabc\r\n", "abc"},
                             {"", ""},
                             {"   ", ""},
                             {"abc", "abc"}}) {
        expectations.expect(
            holds(run(Kernel::StringTrim, {text(pair.first)}), 0, std::string(pair.second)),
            "Trim removes ASCII whitespace from both ends");
    }

    struct CaseCase final {
        std::string_view value;
        document::StringCaseMode mode;
        std::string_view result;
    };
    static const CaseCase kCases[]{
        {"abc DEF", document::StringCaseMode::Upper, "ABC DEF"},
        {"abc DEF", document::StringCaseMode::Lower, "abc def"},
        {"two-part name", document::StringCaseMode::Title, "Two-Part Name"},
        {"hELLO wORLD", document::StringCaseMode::Title, "Hello World"},
        // Case mapping is ASCII-only and says so: a Unicode mapping is locale- and table-dependent,
        // so everything outside ASCII passes through unchanged.
        {kAcute, document::StringCaseMode::Upper, kAcute},
        {"", document::StringCaseMode::Title, ""},
    };
    for (const auto& item : kCases) {
        const auto outcome =
            run(Kernel::StringCase, {text(item.value)}, {document::selectorStoredValue(item.mode)});
        expectations.expect(!outcome.failed && holds(outcome, 0, std::string(item.result)),
                            "Case maps ASCII and leaves everything else alone");
    }

    struct RepeatCase final {
        std::string_view value;
        std::int64_t count;
        std::string_view result;
    };
    static const RepeatCase kRepeats[]{
        {"ab", 3, "ababab"},
        {"ab", 0, ""},
        {"ab", -5, ""},
        {"", 5, ""},
    };
    for (const auto& item : kRepeats) {
        expectations.expect(holds(run(Kernel::StringRepeat, {text(item.value), item.count}), 0,
                                  std::string(item.result)),
                            "Repeat writes its count of copies");
    }
    const auto grown = run(Kernel::StringRepeat, {text("xy"), std::int64_t{1025}});
    const auto* repeated = std::get_if<std::string>(&grown.outputs[0]);
    expectations.expect(!grown.failed && repeated != nullptr && repeated->size() == 2050 &&
                            repeated->substr(2048) == "xy",
                        "Repeat honours counts above the former clamp");
    const auto empty =
        run(Kernel::StringRepeat, {text(""), std::numeric_limits<std::int64_t>::max()});
    expectations.expect(!empty.failed && holds(empty, 0, std::string{}),
                        "Repeating empty text short-circuits even for a huge count");
}

void testPredicates(Expectations& expectations) {
    struct SearchCase final {
        std::string_view value;
        std::string_view needle;
        Kernel kernel;
        bool caseSensitive;
        bool result;
    };
    static const SearchCase kCases[]{
        {"hello world", "lo w", Kernel::StringContains, true, true},
        {"hello world", "LO W", Kernel::StringContains, true, false},
        {"hello world", "LO W", Kernel::StringContains, false, true},
        {"abc", "abcd", Kernel::StringContains, true, false},
        // A string contains the empty string, which is what every find() already says and what
        // keeps an unfilled operand from reading false.
        {"abc", "", Kernel::StringContains, true, true},
        {"", "", Kernel::StringContains, true, true},
        {"", "a", Kernel::StringContains, true, false},
        {"hello", "hel", Kernel::StringStartsWith, true, true},
        {"hello", "HEL", Kernel::StringStartsWith, true, false},
        {"hello", "HEL", Kernel::StringStartsWith, false, true},
        {"hello", "ello", Kernel::StringStartsWith, true, false},
        {"hel", "hello", Kernel::StringStartsWith, true, false},
        {"abc", "", Kernel::StringStartsWith, true, true},
        {"hello", "llo", Kernel::StringEndsWith, true, true},
        {"hello", "LLO", Kernel::StringEndsWith, true, false},
        {"hello", "LLO", Kernel::StringEndsWith, false, true},
        {"hello", "hell", Kernel::StringEndsWith, true, false},
        {"llo", "hello", Kernel::StringEndsWith, true, false},
        {"abc", "", Kernel::StringEndsWith, true, true},
        {"abc", "abc", Kernel::StringEquals, true, true},
        {"abc", "ABC", Kernel::StringEquals, true, false},
        {"abc", "ABC", Kernel::StringEquals, false, true},
        {"abc", "abcd", Kernel::StringEquals, true, false},
        {"", "", Kernel::StringEquals, true, true},
        // Case folding is ASCII-only here too, so two different non-ASCII spellings stay different.
        {kAcute, "E", Kernel::StringEquals, false, false},
    };
    for (const auto& item : kCases) {
        const auto outcome =
            run(item.kernel, {text(item.value), text(item.needle), item.caseSensitive});
        expectations.expect(!outcome.failed && holds(outcome, 0, item.result),
                            "a string predicate answers its documented comparison");
    }
}

void testAmplificationBudgets(Expectations& expectations) {
    runtime::ValueUtilityLimits limits;
    limits.maxStringBytes = 32;
    limits.availableStringBytes = 7;
    const auto repeat = run(Kernel::StringRepeat, {text("ab"), std::int64_t{4}}, {}, limits);
    expectations.expect(repeat.failed && !repeat.cancelled && holds(repeat, 0, std::string{}) &&
                            repeat.summary == "Value utility string budget exceeded",
                        "Repeat refuses an output beyond the remaining graph allowance");
    const auto huge = run(Kernel::StringRepeat,
                          {text("ab"), std::numeric_limits<std::int64_t>::max()}, {}, limits);
    expectations.expect(huge.failed && holds(huge, 0, std::string{}),
                        "Repeat diagnoses huge counts before multiplication or allocation");
    limits.availableStringBytes = 8;
    const auto padded =
        run(Kernel::StringPad, {text("é"), std::int64_t{4}, text("🌸")}, {}, limits);
    expectations.expect(padded.failed && holds(padded, 0, std::string{}),
                        "Pad checks multibyte fill growth against byte allowance");
    const auto wide = run(Kernel::StringPad, {text("x"), std::int64_t{257}, text("0")});
    const auto* widened = std::get_if<std::string>(&wide.outputs[0]);
    expectations.expect(!wide.failed && widened != nullptr && widened->size() == 257 &&
                            widened->back() == 'x',
                        "Pad honours widths above the former clamp");
    limits.availableStringBytes = 7;
    const auto replace =
        run(Kernel::StringReplace, {text("aaaa"), text("a"), text("bb")}, {}, limits);
    expectations.expect(replace.failed && holds(replace, 0, std::string{}),
                        "Replace checks expansion before retaining its result");
    const auto format =
        run(Kernel::StringFormat, {text("{0}{0}{0}"), text("abc"), text(""), text(""), text("")},
            {}, limits);
    expectations.expect(format.failed && holds(format, 0, std::string{}),
                        "Format checks repeated slots against the same byte allowance");
    limits.availableStringBytes = 8;
    expectations.expect(
        !run(Kernel::StringRepeat, {text("ab"), std::int64_t{4}}, {}, limits).failed &&
            !run(Kernel::StringReplace, {text("aaaa"), text("a"), text("bb")}, {}, limits).failed,
        "an output exactly at the byte budget remains valid");
    limits.maxScratchBytes = 16;
    const auto scratch = run(Kernel::StringLength, {text("abcd")}, {}, limits);
    expectations.expect(scratch.failed &&
                            scratch.summary == "Value utility scratch budget exceeded" &&
                            holds(scratch, 0, std::int64_t{0}),
                        "Unicode indexing refuses scratch work before allocating boundaries");
}

void testCancellationIsNotAMalformedPlan(Expectations& expectations) {
    using namespace std::chrono_literals;
    runtime::TaskScheduler scheduler;
    std::mutex mutex;
    std::condition_variable ready;
    std::optional<runtime::CancellationToken> token;
    auto submission =
        scheduler.submit<bool>(runtime::TaskRequest("String cancellation fixture",
                                                    {.kind = runtime::TaskOwnerKind::Application,
                                                     .id = runtime::TaskOwnerId::fromRaw(1)}),
                               [&](runtime::TaskContext& context) -> runtime::TaskResult<bool> {
                                   {
                                       std::lock_guard lock(mutex);
                                       token = context.cancellation();
                                   }
                                   ready.notify_one();
                                   while (!context.isCancellationRequested())
                                       std::this_thread::sleep_for(1ms);
                                   return runtime::TaskResult<bool>::cancelled();
                               });
    expectations.expect(submission.accepted(), "cancellation fixture starts on a worker");
    if (!submission.accepted())
        return;
    {
        std::unique_lock lock(mutex);
        (void)ready.wait_for(lock, 2s, [&] { return token.has_value(); });
    }
    submission.handle.cancel();
    if (!token) {
        expectations.expect(false, "cancellation fixture supplies a token");
        return;
    }
    runtime::ValueUtilityLimits limits;
    limits.cancellation = &*token;
    const auto outcome =
        run(Kernel::StringReplace, {text("aaaa"), text("a"), text("many")}, {}, limits);
    expectations.expect(outcome.cancelled && !outcome.failed && holds(outcome, 0, std::string{}),
                        "cancellation stops string work without a malformed-plan diagnostic");
    scheduler.beginShutdown();
}

// A mistyped operand is a malformed plan, and the node still writes values of its declared kinds.
void testMalformedOperands(Expectations& expectations) {
    const auto outcome = run(Kernel::StringLength, {42.0});
    expectations.expect(outcome.failed && outcome.failedOperand == 0,
                        "a mistyped operand is reported and names its own position");
    expectations.expect(outcome.outputCount == 1 &&
                            std::holds_alternative<std::int64_t>(outcome.outputs[0]),
                        "and the outcome still carries a value of the declared kind");
    const auto split = run(Kernel::StringSplit, {text("a,b"), 7.0, std::int64_t{0}, text("?")});
    expectations.expect(split.failed && split.outputCount == 2 &&
                            std::holds_alternative<std::string>(split.outputs[0]) &&
                            std::holds_alternative<bool>(split.outputs[1]),
                        "a node with a valid flag keeps both outputs when the plan is malformed");
}

} // namespace

int main() {
    Expectations expectations;
    testConcatenateAndFormat(expectations);
    testMeasuredOperations(expectations);
    testEditingOperations(expectations);
    testPredicates(expectations);
    testAmplificationBudgets(expectations);
    testCancellationIsNotAMalformedPlan(expectations);
    testMalformedOperands(expectations);
    return expectations.failures() == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
