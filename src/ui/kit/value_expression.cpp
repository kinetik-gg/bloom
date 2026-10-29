#include <bloom/ui/kit/value_expression.hpp>

#include <charconv>
#include <cmath>
#include <limits>
#include <locale>
#include <sstream>
#include <type_traits>

namespace bloom::ui::kit {
namespace {

using Error = ValueExpressionError;

// One literal as the lexer split it: the digits before and after the decimal separator and the
// exponent's own text (sign included), so each arithmetic can convert it on its own terms.
struct Literal {
    std::string_view whole;
    std::string_view fraction;
    std::string_view exponent;
    bool negative = false;
};

// Floating-point arithmetic. Literals go through the classic locale, never the process locale
// QCoreApplication installs, so "0.1" is the nearest double whatever the artist's region is.
struct Floating {
    using Value = double;

    static Error literal(const Literal& parts, Value& out) {
        std::string text;
        text.reserve(parts.whole.size() + parts.fraction.size() + parts.exponent.size() + 4);
        if (parts.negative)
            text += '-';
        text += parts.whole.empty() ? std::string_view("0") : parts.whole;
        if (!parts.fraction.empty()) {
            text += '.';
            text += parts.fraction;
        }
        if (!parts.exponent.empty()) {
            text += 'e';
            text += parts.exponent;
        }
        std::istringstream stream(text);
        stream.imbue(std::locale::classic());
        double value = 0.0;
        stream >> value;
        if (stream.fail() || !std::isfinite(value))
            return Error::NotFinite;
        out = value;
        return Error::None;
    }
    static Error finite(const double value, Value& out) {
        if (!std::isfinite(value))
            return Error::NotFinite;
        out = value;
        return Error::None;
    }
    static Error add(const Value a, const Value b, Value& out) { return finite(a + b, out); }
    static Error subtract(const Value a, const Value b, Value& out) { return finite(a - b, out); }
    static Error multiply(const Value a, const Value b, Value& out) { return finite(a * b, out); }
    static Error divide(const Value a, const Value b, Value& out) {
        if (b == 0.0)
            return Error::DivideByZero;
        return finite(a / b, out);
    }
    static Error negate(const Value a, Value& out) {
        out = -a;
        return Error::None;
    }
};

// Exact int64 arithmetic: every step is checked, and nothing ever passes through a double.
struct Integer {
    using Value = std::int64_t;
    static constexpr Value kMax = std::numeric_limits<Value>::max();
    static constexpr Value kMin = std::numeric_limits<Value>::min();

    static Error literal(const Literal& parts, Value& out) {
        if (!parts.exponent.empty())
            return Error::NotInteger;
        for (const char digit : parts.fraction)
            if (digit != '0')
                return Error::NotInteger;
        if (parts.whole.empty()) {
            out = 0;
            return Error::None;
        }
        // The sign is part of the parse, so the most negative int64 -- whose magnitude has no
        // positive int64 -- is still a literal the artist can type.
        std::string text;
        if (parts.negative)
            text += '-';
        text += parts.whole;
        Value value = 0;
        const auto [end, code] = std::from_chars(text.data(), text.data() + text.size(), value);
        if (code == std::errc::result_out_of_range)
            return Error::Overflow;
        if (code != std::errc{} || end != text.data() + text.size())
            return Error::Syntax;
        out = value;
        return Error::None;
    }
    static Error add(const Value a, const Value b, Value& out) {
        if ((b > 0 && a > kMax - b) || (b < 0 && a < kMin - b))
            return Error::Overflow;
        out = a + b;
        return Error::None;
    }
    static Error subtract(const Value a, const Value b, Value& out) {
        if ((b < 0 && a > kMax + b) || (b > 0 && a < kMin + b))
            return Error::Overflow;
        out = a - b;
        return Error::None;
    }
    static Error multiply(const Value a, const Value b, Value& out) {
        if (a == 0 || b == 0) {
            out = 0;
            return Error::None;
        }
        if ((a == -1 && b == kMin) || (b == -1 && a == kMin))
            return Error::Overflow;
        if (a > 0 ? (b > 0 ? a > kMax / b : b < kMin / a) : (b > 0 ? a < kMin / b : a < kMax / b))
            return Error::Overflow;
        out = a * b;
        return Error::None;
    }
    static Error divide(const Value a, const Value b, Value& out) {
        if (b == 0)
            return Error::DivideByZero;
        if (a == kMin && b == -1)
            return Error::Overflow;
        if (a % b != 0)
            return Error::NotInteger;
        out = a / b;
        return Error::None;
    }
    static Error negate(const Value a, Value& out) {
        if (a == kMin)
            return Error::Overflow;
        out = -a;
        return Error::None;
    }
};

[[nodiscard]] bool isDigit(const char c) noexcept { return c >= '0' && c <= '9'; }
[[nodiscard]] bool isSpace(const char c) noexcept {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}
// A unit is a run of anything that is not a digit, a space, an operator or a separator: "px",
// "%", and the UTF-8 bytes of "°" all qualify.
[[nodiscard]] bool isUnitByte(const char c) noexcept {
    switch (c) {
    case '+':
    case '-':
    case '*':
    case '/':
    case '(':
    case ')':
    case '=':
    case '.':
    case ',':
        return false;
    default:
        return !isDigit(c) && !isSpace(c);
    }
}
[[nodiscard]] char lowerAscii(const char c) noexcept {
    return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
}
[[nodiscard]] bool sameUnit(const std::string_view typed, const std::string_view unit) noexcept {
    if (typed.size() != unit.size())
        return false;
    for (std::size_t index = 0; index < typed.size(); ++index)
        if (lowerAscii(typed[index]) != lowerAscii(unit[index]))
            return false;
    return true;
}

template <typename Arithmetic> class Parser final {
  public:
    using Value = typename Arithmetic::Value;

    Parser(const std::string_view text, const ValueExpressionContext& context)
        : text_(text), context_(context) {}

    [[nodiscard]] ValueExpressionResult run(const Value current) {
        ValueExpressionResult result;
        skipSpace();
        if (atEnd()) {
            result.error = Error::Empty;
            return result;
        }
        char relative = 0;
        if (position_ + 1 < text_.size() && text_[position_ + 1] == '=' &&
            (peek() == '+' || peek() == '-' || peek() == '*' || peek() == '/')) {
            relative = peek();
            position_ += 2;
        }
        Value value{};
        if (!expression(value) || !unit()) {
            result.error = error_;
            result.unit = refusedUnit_;
            return result;
        }
        skipSpace();
        if (!atEnd()) {
            result.error = Error::Syntax;
            return result;
        }
        Error error = Error::None;
        switch (relative) {
        case '+':
            error = Arithmetic::add(current, value, value);
            break;
        case '-':
            error = Arithmetic::subtract(current, value, value);
            break;
        case '*':
            error = Arithmetic::multiply(current, value, value);
            break;
        case '/':
            error = Arithmetic::divide(current, value, value);
            break;
        default:
            break;
        }
        result.error = error;
        if constexpr (std::is_same_v<Value, double>)
            result.value = value;
        else {
            result.integer = value;
            result.value = static_cast<double>(value);
        }
        return result;
    }

  private:
    [[nodiscard]] bool atEnd() const noexcept { return position_ >= text_.size(); }
    [[nodiscard]] char peek() const noexcept { return atEnd() ? '\0' : text_[position_]; }
    void skipSpace() {
        while (!atEnd() && isSpace(peek()))
            ++position_;
    }
    bool fail(const Error error) {
        if (error_ == Error::None)
            error_ = error;
        return false;
    }
    bool check(const Error error) { return error == Error::None || fail(error); }

    bool expression(Value& out) {
        if (!term(out))
            return false;
        for (;;) {
            skipSpace();
            const char op = peek();
            if (op != '+' && op != '-')
                return true;
            ++position_;
            Value right{};
            if (!term(right))
                return false;
            if (!check(op == '+' ? Arithmetic::add(out, right, out)
                                 : Arithmetic::subtract(out, right, out)))
                return false;
        }
    }

    bool term(Value& out) {
        if (!unary(out))
            return false;
        for (;;) {
            skipSpace();
            const char op = peek();
            if (op != '*' && op != '/')
                return true;
            ++position_;
            Value right{};
            if (!unary(right))
                return false;
            if (!check(op == '*' ? Arithmetic::multiply(out, right, out)
                                 : Arithmetic::divide(out, right, out)))
                return false;
        }
    }

    bool unary(Value& out) {
        skipSpace();
        if (peek() == '+') {
            ++position_;
            return unary(out);
        }
        if (peek() == '-') {
            ++position_;
            skipSpace();
            // A minus written straight onto a literal belongs to the literal, which is how the
            // most negative integer stays typeable.
            if (isDigit(peek()) || peek() == '.' || peek() == ',')
                return number(out, true);
            Value inner{};
            return unary(inner) && check(Arithmetic::negate(inner, out));
        }
        return primary(out);
    }

    bool primary(Value& out) {
        skipSpace();
        if (peek() == '(') {
            ++position_;
            if (!expression(out))
                return false;
            skipSpace();
            if (peek() != ')')
                return fail(Error::Syntax);
            ++position_;
            return unit();
        }
        return number(out, false);
    }

    bool number(Value& out, const bool negative) {
        Literal parts;
        parts.negative = negative;
        const std::size_t start = position_;
        while (isDigit(peek()))
            ++position_;
        parts.whole = text_.substr(start, position_ - start);
        if (peek() == '.' || peek() == ',') {
            ++position_;
            const std::size_t fraction = position_;
            while (isDigit(peek()))
                ++position_;
            parts.fraction = text_.substr(fraction, position_ - fraction);
        }
        if (parts.whole.empty() && parts.fraction.empty())
            return fail(Error::Syntax);
        if ((peek() == 'e' || peek() == 'E')) {
            std::size_t probe = position_ + 1;
            if (probe < text_.size() && (text_[probe] == '+' || text_[probe] == '-'))
                ++probe;
            if (probe < text_.size() && isDigit(text_[probe])) {
                const std::size_t exponent = position_ + 1;
                position_ = probe;
                while (isDigit(peek()))
                    ++position_;
                parts.exponent = text_.substr(exponent, position_ - exponent);
            }
        }
        return check(Arithmetic::literal(parts, out)) && unit();
    }

    // An optional unit after a number, a parenthesised group, or the whole entry. Only the
    // field's own unit is accepted.
    bool unit() {
        skipSpace();
        const std::size_t start = position_;
        while (!atEnd() && isUnitByte(peek()))
            ++position_;
        const auto typed = text_.substr(start, position_ - start);
        if (typed.empty() || (!context_.unit.empty() && sameUnit(typed, context_.unit)))
            return true;
        refusedUnit_ = std::string(typed);
        return fail(Error::UnknownUnit);
    }

    std::string_view text_;
    const ValueExpressionContext& context_;
    std::size_t position_ = 0;
    Error error_ = Error::None;
    std::string refusedUnit_;
};

} // namespace

ValueExpressionResult evaluateValueExpression(const std::string_view text,
                                              const ValueExpressionContext& context) {
    return Parser<Floating>(text, context).run(context.current);
}

ValueExpressionResult evaluateIntegerExpression(const std::string_view text,
                                                const ValueExpressionContext& context) {
    return Parser<Integer>(text, context).run(context.currentInteger);
}

} // namespace bloom::ui::kit
