#include "strict_json_preflight.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <string_view>

namespace {

using bloom::project::detail::StrictJsonCheckpoint;
using bloom::project::detail::StrictJsonPreflightError;
using bloom::project::detail::StrictJsonPreflightLimits;
using bloom::project::detail::StrictJsonPreflightResult;

[[nodiscard]] constexpr std::uint8_t byteValue(const std::byte value) noexcept {
    return std::to_integer<std::uint8_t>(value);
}

[[nodiscard]] constexpr bool isDigit(const std::uint8_t value) noexcept {
    return value >= static_cast<std::uint8_t>('0') && value <= static_cast<std::uint8_t>('9');
}

[[nodiscard]] constexpr bool isNonZeroDigit(const std::uint8_t value) noexcept {
    return value >= static_cast<std::uint8_t>('1') && value <= static_cast<std::uint8_t>('9');
}

[[nodiscard]] constexpr bool isHexDigit(const std::uint8_t value) noexcept {
    return isDigit(value) ||
           (value >= static_cast<std::uint8_t>('a') && value <= static_cast<std::uint8_t>('f')) ||
           (value >= static_cast<std::uint8_t>('A') && value <= static_cast<std::uint8_t>('F'));
}

[[nodiscard]] constexpr std::uint16_t hexValue(const std::uint8_t value) noexcept {
    if (isDigit(value)) {
        return static_cast<std::uint16_t>(value - static_cast<std::uint8_t>('0'));
    }
    if (value >= static_cast<std::uint8_t>('a')) {
        return static_cast<std::uint16_t>(value - static_cast<std::uint8_t>('a') + 10U);
    }
    return static_cast<std::uint16_t>(value - static_cast<std::uint8_t>('A') + 10U);
}

[[nodiscard]] constexpr bool isWhitespace(const std::uint8_t value) noexcept {
    return value == 0x20U || value == 0x09U || value == 0x0AU || value == 0x0DU;
}

[[nodiscard]] constexpr bool isValueDelimiter(const std::uint8_t value) noexcept {
    return isWhitespace(value) || value == static_cast<std::uint8_t>(',') ||
           value == static_cast<std::uint8_t>(']') || value == static_cast<std::uint8_t>('}');
}

enum class FrameState : std::uint8_t {
    ObjectFirstMemberOrEnd,
    ObjectMember,
    ObjectColon,
    ObjectValue,
    ObjectCommaOrEnd,
    ArrayFirstValueOrEnd,
    ArrayValue,
    ArrayCommaOrEnd,
};

struct Frame final {
    std::uint64_t entryCount = 0;
    FrameState state = FrameState::ObjectFirstMemberOrEnd;
};

struct Utf8ScalarResult final {
    bool valid = false;
    std::size_t byteCount = 0;
    std::size_t errorOffset = 0;
};

class Scanner final {
  public:
    Scanner(const std::span<const std::byte> input, const StrictJsonPreflightLimits limits,
            const StrictJsonCheckpoint checkpoint) noexcept
        : input_(input), limits_(limits), checkpoint_(checkpoint) {}

    [[nodiscard]] StrictJsonPreflightResult run() noexcept {
        if (!limitsAreValid()) {
            return failure(StrictJsonPreflightError::InvalidLimits, 0);
        }
        if (!invokeCheckpoint(0, true)) {
            return failure(StrictJsonPreflightError::Cancelled, 0);
        }
        if (input_.size() > limits_.maximumInputBytes) {
            return failure(StrictJsonPreflightError::InputTooLarge, limits_.maximumInputBytes);
        }
        if (hasBom()) {
            return failure(StrictJsonPreflightError::BomForbidden, 0);
        }

        if (!skipWhitespace()) {
            return cancelledResult();
        }
        if (offset_ == input_.size()) {
            return failure(StrictJsonPreflightError::EmptyInput, input_.size());
        }
        if (!parseValue()) {
            return currentFailure();
        }

        while (depth_ != 0) {
            if (!skipWhitespace()) {
                return cancelledResult();
            }
            if (offset_ == input_.size()) {
                return failure(StrictJsonPreflightError::InvalidSyntax, input_.size());
            }

            switch (frames_[depth_ - 1].state) {
            case FrameState::ObjectFirstMemberOrEnd:
                if (current() == static_cast<std::uint8_t>('}')) {
                    if (!consumeByte()) {
                        return cancelledResult();
                    }
                    --depth_;
                    break;
                }
                if (!parseObjectMember()) {
                    return currentFailure();
                }
                break;
            case FrameState::ObjectMember:
                if (!parseObjectMember()) {
                    return currentFailure();
                }
                break;
            case FrameState::ObjectColon:
                if (current() != static_cast<std::uint8_t>(':')) {
                    return failure(StrictJsonPreflightError::InvalidSyntax, offset_);
                }
                if (!consumeByte()) {
                    return cancelledResult();
                }
                frames_[depth_ - 1].state = FrameState::ObjectValue;
                break;
            case FrameState::ObjectValue:
                if (!parseValue()) {
                    return currentFailure();
                }
                break;
            case FrameState::ObjectCommaOrEnd:
                if (!parseObjectSeparator()) {
                    return currentFailure();
                }
                break;
            case FrameState::ArrayFirstValueOrEnd:
                if (current() == static_cast<std::uint8_t>(']')) {
                    if (!consumeByte()) {
                        return cancelledResult();
                    }
                    --depth_;
                    break;
                }
                if (!parseValue()) {
                    return currentFailure();
                }
                break;
            case FrameState::ArrayValue:
                if (!parseValue()) {
                    return currentFailure();
                }
                break;
            case FrameState::ArrayCommaOrEnd:
                if (!parseArraySeparator()) {
                    return currentFailure();
                }
                break;
            }
        }

        if (!skipWhitespace()) {
            return cancelledResult();
        }
        if (offset_ != input_.size()) {
            return failure(StrictJsonPreflightError::TrailingData, offset_);
        }
        if (!invokeCheckpoint(input_.size(), true)) {
            return failure(StrictJsonPreflightError::Cancelled, input_.size());
        }
        return success();
    }

  private:
    [[nodiscard]] bool limitsAreValid() const noexcept {
        return limits_.maximumInputBytes <=
                   bloom::project::detail::kStrictJsonDocumentMaximumInputBytes &&
               limits_.maximumValues <= bloom::project::detail::kStrictJsonMaximumValues &&
               limits_.maximumContainerEntries <=
                   bloom::project::detail::kStrictJsonMaximumContainerEntries &&
               limits_.maximumDepth <= bloom::project::detail::kStrictJsonMaximumDepth &&
               limits_.maximumDecodedStringBytes <=
                   bloom::project::detail::kStrictJsonMaximumDecodedStringBytes;
    }

    [[nodiscard]] bool hasBom() const noexcept {
        return input_.size() >= 3 && byteValue(input_[0]) == 0xEFU &&
               byteValue(input_[1]) == 0xBBU && byteValue(input_[2]) == 0xBFU;
    }

    [[nodiscard]] std::uint8_t current() const noexcept { return byteValue(input_[offset_]); }

    [[nodiscard]] bool invokeCheckpoint(const std::size_t consumedBytes,
                                        const bool force) noexcept {
        if (checkpoint_.function == nullptr) {
            return true;
        }
        if (!force && consumedBytes - lastCheckpointOffset_ <
                          bloom::project::detail::kStrictJsonCheckpointCadenceBytes) {
            return true;
        }
        lastCheckpointOffset_ = consumedBytes;
        return checkpoint_.function(checkpoint_.context, consumedBytes, input_.size());
    }

    [[nodiscard]] bool consumeByte() noexcept {
        ++offset_;
        if (!invokeCheckpoint(offset_, false)) {
            error_ = StrictJsonPreflightError::Cancelled;
            errorOffset_ = offset_;
            return false;
        }
        return true;
    }

    [[nodiscard]] bool consumeBytes(const std::size_t count) noexcept {
        for (std::size_t index = 0; index < count; ++index) {
            if (!consumeByte()) {
                return false;
            }
        }
        return true;
    }

    [[nodiscard]] bool skipWhitespace() noexcept {
        while (offset_ < input_.size() && isWhitespace(current())) {
            if (!consumeByte()) {
                return false;
            }
        }
        return true;
    }

    [[nodiscard]] bool parseValue() noexcept {
        if (offset_ == input_.size()) {
            setError(StrictJsonPreflightError::InvalidSyntax, input_.size());
            return false;
        }

        const auto valueStart = offset_;
        const auto first = current();
        const bool recognized =
            first == static_cast<std::uint8_t>('{') || first == static_cast<std::uint8_t>('[') ||
            first == static_cast<std::uint8_t>('"') || first == static_cast<std::uint8_t>('t') ||
            first == static_cast<std::uint8_t>('f') || first == static_cast<std::uint8_t>('n') ||
            first == static_cast<std::uint8_t>('-') || isDigit(first);
        if (!recognized) {
            setUnexpectedByteError();
            return false;
        }

        const auto valueDepth = depth_ + 1;
        if (valueDepth > limits_.maximumDepth) {
            setError(StrictJsonPreflightError::DepthLimitExceeded, valueStart);
            return false;
        }
        if (valueCount_ >= limits_.maximumValues) {
            setError(StrictJsonPreflightError::ValueLimitExceeded, valueStart);
            return false;
        }
        if (depth_ != 0 &&
            (frames_[depth_ - 1].state == FrameState::ArrayFirstValueOrEnd ||
             frames_[depth_ - 1].state == FrameState::ArrayValue) &&
            frames_[depth_ - 1].entryCount >= limits_.maximumContainerEntries) {
            setError(StrictJsonPreflightError::ContainerEntryLimitExceeded, valueStart);
            return false;
        }

        ++valueCount_;
        maximumObservedDepth_ =
            std::max(maximumObservedDepth_, static_cast<std::uint32_t>(valueDepth));
        completeParentValue();

        if (first == static_cast<std::uint8_t>('{')) {
            return beginContainer(FrameState::ObjectFirstMemberOrEnd);
        }
        if (first == static_cast<std::uint8_t>('[')) {
            return beginContainer(FrameState::ArrayFirstValueOrEnd);
        }
        if (first == static_cast<std::uint8_t>('"')) {
            return parseString();
        }
        if (first == static_cast<std::uint8_t>('t')) {
            return parseLiteral("true");
        }
        if (first == static_cast<std::uint8_t>('f')) {
            return parseLiteral("false");
        }
        if (first == static_cast<std::uint8_t>('n')) {
            return parseLiteral("null");
        }
        return parseNumber();
    }

    void completeParentValue() noexcept {
        if (depth_ == 0) {
            return;
        }
        auto& parent = frames_[depth_ - 1];
        if (parent.state == FrameState::ObjectValue) {
            ++parent.entryCount;
            parent.state = FrameState::ObjectCommaOrEnd;
        } else {
            ++parent.entryCount;
            parent.state = FrameState::ArrayCommaOrEnd;
        }
    }

    [[nodiscard]] bool beginContainer(const FrameState state) noexcept {
        if (!consumeByte()) {
            return false;
        }
        frames_[depth_] = Frame{.entryCount = 0, .state = state};
        ++depth_;
        return true;
    }

    [[nodiscard]] bool parseObjectMember() noexcept {
        if (current() != static_cast<std::uint8_t>('"')) {
            setUnexpectedByteError();
            return false;
        }
        auto& frame = frames_[depth_ - 1];
        if (frame.entryCount >= limits_.maximumContainerEntries) {
            setError(StrictJsonPreflightError::ContainerEntryLimitExceeded, offset_);
            return false;
        }
        if (!parseString()) {
            return false;
        }
        frame.state = FrameState::ObjectColon;
        return true;
    }

    [[nodiscard]] bool parseObjectSeparator() noexcept {
        const auto value = current();
        if (value == static_cast<std::uint8_t>('}')) {
            if (!consumeByte()) {
                return false;
            }
            --depth_;
            return true;
        }
        if (value != static_cast<std::uint8_t>(',')) {
            setUnexpectedByteError();
            return false;
        }
        if (!consumeByte()) {
            return false;
        }
        frames_[depth_ - 1].state = FrameState::ObjectMember;
        return true;
    }

    [[nodiscard]] bool parseArraySeparator() noexcept {
        const auto value = current();
        if (value == static_cast<std::uint8_t>(']')) {
            if (!consumeByte()) {
                return false;
            }
            --depth_;
            return true;
        }
        if (value != static_cast<std::uint8_t>(',')) {
            setUnexpectedByteError();
            return false;
        }
        if (!consumeByte()) {
            return false;
        }
        frames_[depth_ - 1].state = FrameState::ArrayValue;
        return true;
    }

    [[nodiscard]] bool parseLiteral(const std::string_view literal) noexcept {
        for (const char expected : literal) {
            if (offset_ == input_.size()) {
                setError(StrictJsonPreflightError::InvalidSyntax, input_.size());
                return false;
            }
            if (current() != static_cast<std::uint8_t>(expected)) {
                setUnexpectedByteError();
                return false;
            }
            if (!consumeByte()) {
                return false;
            }
        }
        if (offset_ != input_.size() && !isValueDelimiter(current())) {
            setUnexpectedByteError();
            return false;
        }
        return true;
    }

    [[nodiscard]] bool parseNumber() noexcept {
        if (current() == static_cast<std::uint8_t>('-')) {
            if (!consumeByte()) {
                return false;
            }
            if (offset_ == input_.size()) {
                setError(StrictJsonPreflightError::InvalidNumber, input_.size());
                return false;
            }
        }

        if (current() == static_cast<std::uint8_t>('0')) {
            if (!consumeByte()) {
                return false;
            }
            if (offset_ != input_.size() && isDigit(current())) {
                setError(StrictJsonPreflightError::InvalidNumber, offset_);
                return false;
            }
        } else if (isNonZeroDigit(current())) {
            do {
                if (!consumeByte()) {
                    return false;
                }
            } while (offset_ != input_.size() && isDigit(current()));
        } else {
            setError(StrictJsonPreflightError::InvalidNumber, offset_);
            return false;
        }

        if (offset_ != input_.size() && current() == static_cast<std::uint8_t>('.')) {
            if (!consumeByte()) {
                return false;
            }
            if (offset_ == input_.size()) {
                setError(StrictJsonPreflightError::InvalidNumber, input_.size());
                return false;
            }
            if (!isDigit(current())) {
                setError(StrictJsonPreflightError::InvalidNumber, offset_);
                return false;
            }
            do {
                if (!consumeByte()) {
                    return false;
                }
            } while (offset_ != input_.size() && isDigit(current()));
        }

        if (offset_ != input_.size() && (current() == static_cast<std::uint8_t>('e') ||
                                         current() == static_cast<std::uint8_t>('E'))) {
            if (!consumeByte()) {
                return false;
            }
            if (offset_ != input_.size() && (current() == static_cast<std::uint8_t>('+') ||
                                             current() == static_cast<std::uint8_t>('-'))) {
                if (!consumeByte()) {
                    return false;
                }
            }
            if (offset_ == input_.size()) {
                setError(StrictJsonPreflightError::InvalidNumber, input_.size());
                return false;
            }
            if (!isDigit(current())) {
                setError(StrictJsonPreflightError::InvalidNumber, offset_);
                return false;
            }
            do {
                if (!consumeByte()) {
                    return false;
                }
            } while (offset_ != input_.size() && isDigit(current()));
        }

        if (offset_ != input_.size() && !isValueDelimiter(current())) {
            setError(StrictJsonPreflightError::InvalidNumber, offset_);
            return false;
        }
        return true;
    }

    [[nodiscard]] bool parseString() noexcept {
        if (!consumeByte()) {
            return false;
        }
        std::uint64_t decodedBytes = 0;
        while (offset_ != input_.size()) {
            const auto scalarStart = offset_;
            const auto value = current();
            if (value == static_cast<std::uint8_t>('"')) {
                return consumeByte();
            }
            if (value == 0x5CU) {
                std::size_t escapedBytes = 0;
                std::uint64_t decodedIncrement = 0;
                if (!validateEscape(escapedBytes, decodedIncrement)) {
                    return false;
                }
                if (!addDecodedBytes(decodedBytes, decodedIncrement, scalarStart)) {
                    return false;
                }
                if (!consumeBytes(escapedBytes)) {
                    return false;
                }
                continue;
            }
            if (value <= 0x1FU) {
                setError(StrictJsonPreflightError::InvalidSyntax, scalarStart);
                return false;
            }
            if (value <= 0x7FU) {
                if (!addDecodedBytes(decodedBytes, 1, scalarStart)) {
                    return false;
                }
                if (!consumeByte()) {
                    return false;
                }
                continue;
            }

            const auto scalar = validateUtf8Scalar(scalarStart);
            if (!scalar.valid) {
                setError(StrictJsonPreflightError::InvalidUtf8, scalar.errorOffset);
                return false;
            }
            if (!addDecodedBytes(decodedBytes, scalar.byteCount, scalarStart)) {
                return false;
            }
            if (!consumeBytes(scalar.byteCount)) {
                return false;
            }
        }
        setError(StrictJsonPreflightError::InvalidSyntax, input_.size());
        return false;
    }

    [[nodiscard]] bool validateEscape(std::size_t& escapedBytes,
                                      std::uint64_t& decodedBytes) noexcept {
        const auto escapeStart = offset_;
        if (escapeStart + 1 >= input_.size()) {
            setError(StrictJsonPreflightError::InvalidEscape, input_.size());
            return false;
        }
        const auto code = byteValue(input_[escapeStart + 1]);
        if (code == static_cast<std::uint8_t>('"') || code == 0x5CU ||
            code == static_cast<std::uint8_t>('/') || code == static_cast<std::uint8_t>('b') ||
            code == static_cast<std::uint8_t>('f') || code == static_cast<std::uint8_t>('n') ||
            code == static_cast<std::uint8_t>('r') || code == static_cast<std::uint8_t>('t')) {
            escapedBytes = 2;
            decodedBytes = 1;
            return true;
        }
        if (code != static_cast<std::uint8_t>('u')) {
            setError(StrictJsonPreflightError::InvalidEscape, escapeStart + 1);
            return false;
        }

        std::uint16_t firstCodeUnit = 0;
        if (!parseCodeUnit(escapeStart, firstCodeUnit)) {
            return false;
        }
        if (firstCodeUnit >= 0xDC00U && firstCodeUnit <= 0xDFFFU) {
            setError(StrictJsonPreflightError::InvalidUnicodeScalar, escapeStart);
            return false;
        }
        if (firstCodeUnit >= 0xD800U && firstCodeUnit <= 0xDBFFU) {
            const auto secondEscape = escapeStart + 6;
            if (secondEscape >= input_.size()) {
                setError(StrictJsonPreflightError::InvalidUnicodeScalar, input_.size());
                return false;
            }
            if (byteValue(input_[secondEscape]) != 0x5CU) {
                setError(StrictJsonPreflightError::InvalidUnicodeScalar, secondEscape);
                return false;
            }
            if (secondEscape + 1 >= input_.size()) {
                setError(StrictJsonPreflightError::InvalidUnicodeScalar, input_.size());
                return false;
            }
            if (byteValue(input_[secondEscape + 1]) != static_cast<std::uint8_t>('u')) {
                setError(StrictJsonPreflightError::InvalidUnicodeScalar, secondEscape + 1);
                return false;
            }
            std::uint16_t secondCodeUnit = 0;
            if (!parseCodeUnit(secondEscape, secondCodeUnit)) {
                return false;
            }
            if (secondCodeUnit < 0xDC00U || secondCodeUnit > 0xDFFFU) {
                setError(StrictJsonPreflightError::InvalidUnicodeScalar, secondEscape);
                return false;
            }
            escapedBytes = 12;
            decodedBytes = 4;
            return true;
        }

        escapedBytes = 6;
        if (firstCodeUnit <= 0x7FU) {
            decodedBytes = 1;
        } else if (firstCodeUnit <= 0x7FFU) {
            decodedBytes = 2;
        } else {
            decodedBytes = 3;
        }
        return true;
    }

    [[nodiscard]] bool parseCodeUnit(const std::size_t escapeStart,
                                     std::uint16_t& codeUnit) noexcept {
        constexpr std::size_t escapeBytes = 6;
        if (escapeStart > input_.size() || input_.size() - escapeStart < escapeBytes) {
            setError(StrictJsonPreflightError::InvalidEscape, input_.size());
            return false;
        }
        codeUnit = 0;
        for (std::size_t index = escapeStart + 2; index < escapeStart + escapeBytes; ++index) {
            const auto value = byteValue(input_[index]);
            if (!isHexDigit(value)) {
                setError(StrictJsonPreflightError::InvalidEscape, index);
                return false;
            }
            codeUnit = static_cast<std::uint16_t>((codeUnit << 4U) | hexValue(value));
        }
        return true;
    }

    [[nodiscard]] bool addDecodedBytes(std::uint64_t& total, const std::uint64_t increment,
                                       const std::size_t scalarStart) noexcept {
        if (increment > std::numeric_limits<std::uint64_t>::max() - total) {
            setError(StrictJsonPreflightError::SizeOverflow, scalarStart);
            return false;
        }
        if (increment > limits_.maximumDecodedStringBytes - total) {
            setError(StrictJsonPreflightError::DecodedStringLimitExceeded, scalarStart);
            return false;
        }
        total += increment;
        return true;
    }

    [[nodiscard]] Utf8ScalarResult validateUtf8Scalar(const std::size_t start) const noexcept {
        const auto first = byteValue(input_[start]);
        std::size_t length = 0;
        if (first >= 0xC2U && first <= 0xDFU) {
            length = 2;
        } else if (first >= 0xE0U && first <= 0xEFU) {
            length = 3;
        } else if (first >= 0xF0U && first <= 0xF4U) {
            length = 4;
        } else {
            return {.valid = false, .byteCount = 0, .errorOffset = start};
        }
        if (input_.size() - start < length) {
            return {.valid = false, .byteCount = 0, .errorOffset = input_.size()};
        }

        const auto second = byteValue(input_[start + 1]);
        if ((second & 0xC0U) != 0x80U) {
            return {.valid = false, .byteCount = 0, .errorOffset = start + 1};
        }
        if ((first == 0xE0U && second < 0xA0U) || (first == 0xEDU && second > 0x9FU) ||
            (first == 0xF0U && second < 0x90U) || (first == 0xF4U && second > 0x8FU)) {
            return {.valid = false, .byteCount = 0, .errorOffset = start + 1};
        }
        for (std::size_t index = 2; index < length; ++index) {
            if ((byteValue(input_[start + index]) & 0xC0U) != 0x80U) {
                return {.valid = false, .byteCount = 0, .errorOffset = start + index};
            }
        }
        return {.valid = true, .byteCount = length, .errorOffset = 0};
    }

    void setUnexpectedByteError() noexcept {
        if (offset_ == input_.size()) {
            setError(StrictJsonPreflightError::InvalidSyntax, input_.size());
            return;
        }
        if (current() <= 0x7FU) {
            setError(StrictJsonPreflightError::InvalidSyntax, offset_);
            return;
        }
        const auto scalar = validateUtf8Scalar(offset_);
        setError(scalar.valid ? StrictJsonPreflightError::InvalidSyntax
                              : StrictJsonPreflightError::InvalidUtf8,
                 scalar.valid ? offset_ : scalar.errorOffset);
    }

    void setError(const StrictJsonPreflightError error, const std::size_t offset) noexcept {
        error_ = error;
        errorOffset_ = offset;
    }

    [[nodiscard]] StrictJsonPreflightResult success() const noexcept {
        return {
            .error = StrictJsonPreflightError::None,
            .errorOffset = input_.size(),
            .valueCount = valueCount_,
            .maximumObservedDepth = maximumObservedDepth_,
        };
    }

    [[nodiscard]] StrictJsonPreflightResult failure(const StrictJsonPreflightError error,
                                                    const std::size_t offset) const noexcept {
        return {
            .error = error,
            .errorOffset = offset,
            .valueCount = valueCount_,
            .maximumObservedDepth = maximumObservedDepth_,
        };
    }

    [[nodiscard]] StrictJsonPreflightResult currentFailure() const noexcept {
        return failure(error_, errorOffset_);
    }

    [[nodiscard]] StrictJsonPreflightResult cancelledResult() const noexcept {
        return failure(StrictJsonPreflightError::Cancelled, errorOffset_);
    }

    std::span<const std::byte> input_;
    StrictJsonPreflightLimits limits_;
    StrictJsonCheckpoint checkpoint_;
    std::array<Frame, bloom::project::detail::kStrictJsonMaximumDepth> frames_{};
    std::size_t offset_ = 0;
    std::size_t lastCheckpointOffset_ = 0;
    std::size_t depth_ = 0;
    std::uint64_t valueCount_ = 0;
    std::uint32_t maximumObservedDepth_ = 0;
    StrictJsonPreflightError error_ = StrictJsonPreflightError::InvalidSyntax;
    std::size_t errorOffset_ = 0;
};

} // namespace

namespace bloom::project::detail {

StrictJsonPreflightResult preflightStrictJson(const std::span<const std::byte> input,
                                              const StrictJsonPreflightLimits limits,
                                              const StrictJsonCheckpoint checkpoint) noexcept {
    return Scanner(input, limits, checkpoint).run();
}

} // namespace bloom::project::detail
