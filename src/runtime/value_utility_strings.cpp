#include "value_utility_support.hpp"

#include <bloom/core/safe_parse.hpp>
#include <bloom/core/utf8.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using namespace bloom;
using runtime::ValueUtilityInvocation;
using runtime::ValueUtilityLimits;
using runtime::ValueUtilityOutcome;
using runtime::detail::selectorAt;
using runtime::detail::shapedOutcome;
using runtime::detail::ValueUtilityReader;

using Kernel = document::ValueUtilityKernel;

// TEXT IS MEASURED IN UNICODE SCALARS, NOT BYTES. `Length`, `Substring`, `Character At` and `Pad`
// all count what an artist counts: "é" is one character whether it arrived as two bytes or one.
//
// A byte that is not a well-formed scalar counts as ONE unit and survives unchanged. That is the
// only total answer: a document may carry text from anywhere, and refusing to measure a string
// because one byte is malformed would make a whole graph stop at a caption nobody can see is
// broken. core::decodeUtf8Scalar() decides what "well formed" means, so this file has no second
// opinion about UTF-8.
[[nodiscard]] std::size_t unitLength(const std::string_view text, const std::size_t offset) {
    const auto scalar = core::decodeUtf8Scalar(text, offset);
    return scalar.isValid() ? scalar.length : 1;
}

// The byte offset of every unit boundary, plus the end. One walk answers every question the string
// nodes ask -- how many units, where the n-th starts, where a run ends -- so a node never walks the
// text twice for the same boundary.
[[nodiscard]] std::vector<std::size_t> unitBoundaries(const std::string_view text) {
    std::vector<std::size_t> boundaries;
    boundaries.reserve(text.size() + 1);
    std::size_t offset = 0;
    while (offset < text.size()) {
        boundaries.push_back(offset);
        offset += unitLength(text, offset);
    }
    boundaries.push_back(text.size());
    return boundaries;
}

[[nodiscard]] std::size_t unitCount(const std::vector<std::size_t>& boundaries) noexcept {
    return boundaries.empty() ? 0 : boundaries.size() - 1;
}

// ASCII case folding, and ASCII ONLY. Unicode case mapping is locale-sensitive (Turkish dotless i),
// context-sensitive (Greek final sigma) and tied to a Unicode table version, so a node that claimed
// to do it would produce a different answer on a different machine or a different year. Everything
// outside ASCII is passed through unchanged, which is documented rather than silently approximated.
[[nodiscard]] constexpr char asciiUpper(const char character) noexcept {
    return character >= 'a' && character <= 'z' ? static_cast<char>(character - 'a' + 'A')
                                                : character;
}

[[nodiscard]] constexpr char asciiLower(const char character) noexcept {
    return character >= 'A' && character <= 'Z' ? static_cast<char>(character - 'A' + 'a')
                                                : character;
}

[[nodiscard]] constexpr bool isAsciiWordCharacter(const char character) noexcept {
    return (character >= 'a' && character <= 'z') || (character >= 'A' && character <= 'Z') ||
           (character >= '0' && character <= '9');
}

[[nodiscard]] std::string foldedAscii(const std::string_view text) {
    std::string folded(text);
    for (auto& character : folded) {
        character = asciiLower(character);
    }
    return folded;
}

// One comparison rule for Contains, Starts With, Ends With and Equals, so the four cannot disagree
// about what "case-insensitive" means.
[[nodiscard]] bool matchesAt(const std::string_view text, const std::size_t offset,
                             const std::string_view needle, const bool caseSensitive,
                             const runtime::CancellationToken* cancellation = nullptr) {
    if (offset + needle.size() > text.size()) {
        return false;
    }
    for (std::size_t index = 0; index < needle.size(); ++index) {
        if ((index & 4095U) == 0U && cancellation && cancellation->isCancellationRequested())
            return false;
        const char left = text[offset + index];
        const char right = needle[index];
        if (caseSensitive ? left != right : asciiLower(left) != asciiLower(right)) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] bool containsText(const std::string_view text, const std::string_view needle,
                                const bool caseSensitive,
                                const runtime::CancellationToken* cancellation) {
    // A string contains the empty string, at every position. That is the convention every standard
    // library's find() already has, and the alternative would make an unfilled operand read false.
    if (needle.empty()) {
        return true;
    }
    if (needle.size() > text.size()) {
        return false;
    }
    for (std::size_t offset = 0; offset + needle.size() <= text.size(); ++offset) {
        if ((offset & 4095U) == 0U && cancellation && cancellation->isCancellationRequested())
            return false;
        if (matchesAt(text, offset, needle, caseSensitive, cancellation)) {
            return true;
        }
    }
    return false;
}

// `{0}` through `{3}`, with `{{` and `}}` as the escapes. A slot naming an argument the node does
// not have is replaced by NOTHING rather than left as text: the template is a layout an artist
// authored, and a literal `{7}` appearing in a rendered caption would look like a bug in the
// composition rather than in the template.
//
// An unmatched `{` or `}` is copied literally. A template is not a program, and refusing to render
// one because a brace is unbalanced would replace a slightly wrong caption with no caption.
class StringBuilder final {
  public:
    explicit StringBuilder(const ValueUtilityLimits& limits) : limits_(limits) {}

    [[nodiscard]] bool append(const std::string_view part) {
        if (cancelled())
            return false;
        const auto limit = std::min(limits_.maxStringBytes, limits_.availableStringBytes);
        if (part.size() > limit - text_.size()) {
            exceeded_ = true;
            return false;
        }
        // Copy in bounded runs so cancellation can stop even a single large operand.
        for (std::size_t offset = 0; offset < part.size();) {
            if (cancelled())
                return false;
            const auto count = std::min(std::size_t{4096}, part.size() - offset);
            text_.append(part.data() + offset, count);
            offset += count;
        }
        return true;
    }
    [[nodiscard]] bool append(const char character) {
        return append(std::string_view(&character, 1));
    }
    [[nodiscard]] bool cancelled() {
        cancelled_ =
            cancelled_ || (limits_.cancellation && limits_.cancellation->isCancellationRequested());
        return cancelled_;
    }
    [[nodiscard]] bool exceeded() const noexcept { return exceeded_; }
    [[nodiscard]] bool wasCancelled() const noexcept { return cancelled_; }
    [[nodiscard]] std::string take() { return std::move(text_); }

  private:
    const ValueUtilityLimits& limits_;
    std::string text_;
    bool exceeded_ = false;
    bool cancelled_ = false;
};

// Repeated text can fill the 1 MiB value allowance. Batch small units into bounded runs so a
// million-character request does not make a million separate append/cancellation calls.
void appendCopies(StringBuilder& result, const std::string_view unit, std::uint64_t count) {
    if (count == 0 || unit.empty())
        return;
    const auto copiesPerRun = std::max(std::size_t{1}, std::size_t{4096} / unit.size());
    const auto runCopies = static_cast<std::size_t>(std::min<std::uint64_t>(count, copiesPerRun));
    std::string run;
    run.reserve(runCopies * unit.size());
    for (std::size_t index = 0; index < runCopies; ++index)
        run.append(unit);
    while (count >= runCopies) {
        if (!result.append(run))
            return;
        count -= runCopies;
    }
    if (count != 0)
        (void)result.append(
            std::string_view(run).substr(0, static_cast<std::size_t>(count) * unit.size()));
}

void finishString(ValueUtilityOutcome& outcome, StringBuilder& builder) {
    if (builder.wasCancelled()) {
        outcome.cancelled = true;
        return;
    }
    if (builder.exceeded()) {
        outcome.failed = true;
        outcome.summary = "Value utility string budget exceeded";
        outcome.detail = "The string output exceeds the per-value or retained-output byte limit.";
        return;
    }
    outcome.outputs[0] = builder.take();
}

void formatTemplate(const std::string_view pattern,
                    const std::span<const std::string_view> arguments, StringBuilder& text) {
    std::size_t index = 0;
    while (index < pattern.size()) {
        if (text.cancelled())
            return;
        const char character = pattern[index];
        if (character == '{' && index + 1 < pattern.size() && pattern[index + 1] == '{') {
            if (!text.append('{'))
                return;
            index += 2;
            continue;
        }
        if (character == '}' && index + 1 < pattern.size() && pattern[index + 1] == '}') {
            if (!text.append('}'))
                return;
            index += 2;
            continue;
        }
        if (character == '{') {
            const auto close = pattern.find('}', index + 1);
            const auto digits = close == std::string_view::npos
                                    ? std::string_view{}
                                    : pattern.substr(index + 1, close - index - 1);
            const auto slot = digits.empty() ? std::nullopt : core::parseInteger(digits, 10);
            if (slot.has_value() && *slot >= 0) {
                if (static_cast<std::size_t>(*slot) < arguments.size()) {
                    if (!text.append(arguments[static_cast<std::size_t>(*slot)]))
                        return;
                }
                index = close + 1;
                continue;
            }
        }
        if (!text.append(character))
            return;
        ++index;
    }
}

void replaceAll(const std::string_view text, const std::string_view search,
                const std::string_view replacement, StringBuilder& result) {
    // An empty search matches everywhere and nowhere: replacing it would either loop forever or
    // interleave the replacement between every character. The text is answered unchanged.
    if (search.empty()) {
        (void)result.append(text);
        return;
    }
    std::size_t offset = 0;
    while (offset < text.size()) {
        if (result.cancelled())
            return;
        const auto found = text.find(search, offset);
        if (found == std::string_view::npos) {
            break;
        }
        if (!result.append(text.substr(offset, found - offset)) || !result.append(replacement))
            return;
        offset = found + search.size();
    }
    (void)result.append(text.substr(offset));
}

// Title case over ASCII: the first word character of each run is upper, the rest lower. A run ends
// at anything that is not an ASCII letter or digit, so "two-part name" becomes "Two-Part Name".
[[nodiscard]] std::string titleCased(const std::string_view text) {
    std::string result(text);
    bool atWordStart = true;
    for (auto& character : result) {
        if (!isAsciiWordCharacter(character)) {
            atWordStart = true;
            continue;
        }
        character = atWordStart ? asciiUpper(character) : asciiLower(character);
        atWordStart = false;
    }
    return result;
}

} // namespace

namespace bloom::runtime::detail {

ValueUtilityOutcome evaluateValueString(const ValueUtilityInvocation& invocation) {
    auto outcome = shapedOutcome(invocation.operation);
    ValueUtilityReader reader(invocation);
    if (invocation.limits.cancellation &&
        invocation.limits.cancellation->isCancellationRequested()) {
        outcome.cancelled = true;
        return outcome;
    }
    const auto needsBoundaries = invocation.operation == Kernel::StringLength ||
                                 invocation.operation == Kernel::StringSubstring ||
                                 invocation.operation == Kernel::StringCharacterAt ||
                                 invocation.operation == Kernel::StringPad;
    if (needsBoundaries) {
        const auto text = reader.text(0);
        if (invocation.limits.maxScratchBytes / sizeof(std::size_t) <= text.size()) {
            outcome.failed = true;
            outcome.failedOperand = 0;
            outcome.summary = "Value utility scratch budget exceeded";
            outcome.detail = "Unicode boundary indexing exceeds the scratch byte limit.";
            return outcome;
        }
    }
    const auto copyText = [&outcome, &invocation](const std::string_view value) {
        if (value.size() >
            std::min(invocation.limits.maxStringBytes, invocation.limits.availableStringBytes)) {
            outcome.failed = true;
            outcome.summary = "Value utility string budget exceeded";
            outcome.detail =
                "The string output exceeds the per-value or retained-output byte limit.";
            return false;
        }
        outcome.outputs[0] = std::string(value);
        return true;
    };
    // The same pair every node with a `valid` output writes: the answer, and whether it is the
    // node's own or the authored fallback. Exhaustion never reports an artist-valid answer.
    const auto answered = [&outcome, &copyText](const std::string_view value, const bool valid) {
        outcome.outputs[1] = copyText(value) && valid;
    };
    switch (invocation.operation) {
    case Kernel::StringConcatenate: {
        // EMPTY PARTS ARE SKIPPED, separator and all. Four operands is the shape, but most uses
        // fill two or three, and joining the unfilled ones would leave trailing separators in every
        // caption an artist writes.
        const std::array<std::string_view, 4> parts{reader.text(0), reader.text(1), reader.text(2),
                                                    reader.text(3)};
        const auto separator = reader.text(4);
        StringBuilder text(invocation.limits);
        bool hasPart = false;
        for (const auto part : parts) {
            if (part.empty()) {
                continue;
            }
            if (hasPart) {
                if (!text.append(separator))
                    break;
            }
            if (!text.append(part))
                break;
            hasPart = true;
        }
        finishString(outcome, text);
        break;
    }
    case Kernel::StringFormat: {
        const auto pattern = reader.text(0);
        const std::array<std::string_view, 4> arguments{reader.text(1), reader.text(2),
                                                        reader.text(3), reader.text(4)};
        StringBuilder text(invocation.limits);
        formatTemplate(pattern, arguments, text);
        finishString(outcome, text);
        break;
    }
    case Kernel::StringLength: {
        const auto boundaries = unitBoundaries(reader.text(0));
        outcome.outputs[0] = static_cast<std::int64_t>(unitCount(boundaries));
        break;
    }
    case Kernel::StringSubstring: {
        const auto text = reader.text(0);
        const auto boundaries = unitBoundaries(text);
        const auto count = static_cast<std::int64_t>(unitCount(boundaries));
        // CLAMPED, never refused: a start past the end answers the empty string, and a negative one
        // answers from the beginning. A NEGATIVE length means "to the end", which is what makes a
        // freshly added node answer the whole string rather than nothing.
        const auto start = std::clamp(reader.integer(1), std::int64_t{0}, count);
        const auto requested = reader.integer(2);
        const auto length = requested < 0 ? count - start : std::min(requested, count - start);
        const auto first = boundaries[static_cast<std::size_t>(start)];
        const auto last = boundaries[static_cast<std::size_t>(start + length)];
        (void)copyText(text.substr(first, last - first));
        break;
    }
    case Kernel::StringCharacterAt: {
        const auto text = reader.text(0);
        const auto index = reader.integer(1);
        const auto fallback = reader.text(2);
        const auto boundaries = unitBoundaries(text);
        const auto count = static_cast<std::int64_t>(unitCount(boundaries));
        if (index < 0 || index >= count) {
            answered(fallback, false);
            break;
        }
        const auto first = boundaries[static_cast<std::size_t>(index)];
        const auto last = boundaries[static_cast<std::size_t>(index) + 1];
        answered(text.substr(first, last - first), true);
        break;
    }
    case Kernel::StringSplit: {
        const auto text = reader.text(0);
        const auto separator = reader.text(1);
        const auto index = reader.integer(2);
        const auto fallback = reader.text(3);
        // An empty separator splits nothing: every position matches it, so the pieces it would
        // produce are not a decomposition of the text.
        if (separator.empty() || index < 0) {
            answered(fallback, false);
            break;
        }
        std::size_t offset = 0;
        std::int64_t piece = 0;
        bool found = false;
        std::string_view answer;
        while (true) {
            const auto next = text.find(separator, offset);
            const auto extent =
                next == std::string_view::npos ? text.size() - offset : next - offset;
            if (piece == index) {
                answer = text.substr(offset, extent);
                found = true;
                break;
            }
            if (next == std::string_view::npos) {
                break;
            }
            offset = next + separator.size();
            ++piece;
        }
        // The fallback, not the empty string, when the index names no piece: an empty piece is a
        // real answer here (",a" splits to an empty first piece), so the two must not look alike.
        answered(found ? answer : fallback, found);
        break;
    }
    case Kernel::StringReplace: {
        StringBuilder text(invocation.limits);
        replaceAll(reader.text(0), reader.text(1), reader.text(2), text);
        finishString(outcome, text);
        break;
    }
    case Kernel::StringTrim:
        (void)copyText(core::trimAsciiWhitespace(reader.text(0)));
        break;
    case Kernel::StringCase: {
        const auto text = reader.text(0);
        if (text.size() >
            std::min(invocation.limits.maxStringBytes, invocation.limits.availableStringBytes)) {
            outcome.failed = true;
            outcome.summary = "Value utility string budget exceeded";
            outcome.detail =
                "The string output exceeds the per-value or retained-output byte limit.";
            break;
        }
        const auto mode =
            selectorAt(invocation, 0, document::kStringCaseModes, document::kDefaultStringCaseMode);
        switch (mode) {
        case document::StringCaseMode::Upper: {
            std::string result(text);
            for (auto& character : result) {
                character = asciiUpper(character);
            }
            outcome.outputs[0] = std::move(result);
            break;
        }
        case document::StringCaseMode::Lower:
            outcome.outputs[0] = foldedAscii(text);
            break;
        case document::StringCaseMode::Title:
            outcome.outputs[0] = titleCased(text);
            break;
        }
        break;
    }
    case Kernel::StringPad: {
        const auto text = reader.text(0);
        const auto width = reader.integer(1);
        const auto fill = reader.text(2);
        const auto side =
            selectorAt(invocation, 0, document::kStringPadSides, document::kDefaultStringPadSide);
        const auto boundaries = unitBoundaries(text);
        const auto count = static_cast<std::int64_t>(unitCount(boundaries));
        const auto target = std::max(width, std::int64_t{0});
        // The fill is one UNIT, taken from the front of the operand: a multi-character fill would
        // make the padded width depend on how the fill divides into the gap. An empty fill pads
        // with nothing, which is the same as not padding.
        const auto fillUnit =
            fill.empty() ? std::string_view{} : fill.substr(0, unitLength(fill, 0));
        if (count >= target || fillUnit.empty()) {
            (void)copyText(text);
            break;
        }
        const auto limit =
            std::min(invocation.limits.maxStringBytes, invocation.limits.availableStringBytes);
        const auto gap = static_cast<std::uint64_t>(target - count);
        if (text.size() > limit || gap > (limit - text.size()) / fillUnit.size()) {
            outcome.failed = true;
            outcome.failedOperand = 1;
            outcome.summary = "Value utility string budget exceeded";
            outcome.detail = "Pad would exceed the per-value or retained-output byte limit.";
            break;
        }
        StringBuilder result(invocation.limits);
        if (side == document::StringPadSide::End && !result.append(text)) {
            finishString(outcome, result);
            break;
        }
        appendCopies(result, fillUnit, gap);
        if (side == document::StringPadSide::Start && !result.exceeded() && !result.wasCancelled())
            (void)result.append(text);
        finishString(outcome, result);
        break;
    }
    case Kernel::StringRepeat: {
        const auto text = reader.text(0);
        const auto count = std::max(reader.integer(1), std::int64_t{0});
        StringBuilder result(invocation.limits);
        const auto limit =
            std::min(invocation.limits.maxStringBytes, invocation.limits.availableStringBytes);
        if (count == 0 || text.empty()) {
            finishString(outcome, result);
            break;
        }
        if (static_cast<std::uint64_t>(count) > limit / text.size()) {
            outcome.failed = true;
            outcome.failedOperand = 1;
            outcome.summary = "Value utility string budget exceeded";
            outcome.detail = "Repeat would exceed the per-value or retained-output byte limit.";
            break;
        }
        appendCopies(result, text, static_cast<std::uint64_t>(count));
        finishString(outcome, result);
        break;
    }
    case Kernel::StringContains:
        outcome.outputs[0] = containsText(reader.text(0), reader.text(1), reader.boolean(2),
                                          invocation.limits.cancellation);
        break;
    case Kernel::StringStartsWith: {
        const auto text = reader.text(0);
        const auto needle = reader.text(1);
        outcome.outputs[0] = needle.empty() || (needle.size() <= text.size() &&
                                                matchesAt(text, 0, needle, reader.boolean(2),
                                                          invocation.limits.cancellation));
        break;
    }
    case Kernel::StringEndsWith: {
        const auto text = reader.text(0);
        const auto needle = reader.text(1);
        outcome.outputs[0] =
            needle.empty() || (needle.size() <= text.size() &&
                               matchesAt(text, text.size() - needle.size(), needle,
                                         reader.boolean(2), invocation.limits.cancellation));
        break;
    }
    case Kernel::StringEquals: {
        const auto left = reader.text(0);
        const auto right = reader.text(1);
        outcome.outputs[0] =
            reader.boolean(2) ? left == right
                              : (left.size() == right.size() &&
                                 matchesAt(left, 0, right, false, invocation.limits.cancellation));
        break;
    }
    default:
        // evaluateValueUtility()'s own switch is exhaustive over every kernel and routes each to
        // exactly one family, so nothing outside this one reaches here.
        break;
    }
    noteMalformedOperand(outcome, reader);
    return outcome;
}

} // namespace bloom::runtime::detail
