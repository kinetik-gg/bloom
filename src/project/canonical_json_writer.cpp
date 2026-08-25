#include <bloom/project/canonical_json_writer.hpp>

#include <bloom/project/canonical_decimal.hpp>
#include <bloom/project/canonical_json_string.hpp>
#include <bloom/project/unknown_json_number.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <string_view>

namespace {

using bloom::project::CanonicalJsonWriterError;
using bloom::project::CanonicalJsonWriterResult;

[[nodiscard]] constexpr bool checkedAdd(const std::size_t left, const std::size_t right,
                                        std::size_t& result) noexcept {
    if (right > std::numeric_limits<std::size_t>::max() - left) {
        return false;
    }
    result = left + right;
    return true;
}

[[nodiscard]] constexpr CanonicalJsonWriterResult invalidState() noexcept {
    return CanonicalJsonWriterResult::failure(CanonicalJsonWriterError::InvalidState);
}

} // namespace

namespace bloom::project {

CanonicalJsonWriter::CanonicalJsonWriter(const std::span<char> output,
                                         const CanonicalJsonWriterLimits limits) noexcept
    : output_(output), limits_(limits) {}

CanonicalJsonWriterResult CanonicalJsonWriter::validateLimits() const noexcept {
    if (limits_.maximumDepth > kCanonicalJsonMaximumDepth ||
        limits_.maximumValues > kCanonicalJsonMaximumValues ||
        limits_.maximumContainerEntries > kCanonicalJsonMaximumContainerEntries) {
        return CanonicalJsonWriterResult::failure(CanonicalJsonWriterError::InvalidLimits);
    }
    return CanonicalJsonWriterResult::success();
}

CanonicalJsonWriterResult
CanonicalJsonWriter::ensureAdditionalCapacity(const std::size_t additionalBytes) const noexcept {
    std::size_t requiredCapacity = 0;
    if (!checkedAdd(offset_, additionalBytes, requiredCapacity)) {
        return CanonicalJsonWriterResult::failure(CanonicalJsonWriterError::SizeOverflow);
    }
    if (requiredCapacity > output_.size()) {
        return CanonicalJsonWriterResult::failure(CanonicalJsonWriterError::OutputCapacityExceeded,
                                                  requiredCapacity);
    }
    return CanonicalJsonWriterResult::success();
}

CanonicalJsonWriterResult CanonicalJsonWriter::prepareValue(ValuePrefix& prefix) const noexcept {
    if (const auto limits = validateLimits(); !limits) {
        return limits;
    }
    if (finished_ || (depth_ == 0 && rootWritten_)) {
        return invalidState();
    }
    if (depth_ + 1 > limits_.maximumDepth) {
        return CanonicalJsonWriterResult::failure(CanonicalJsonWriterError::DepthLimitExceeded);
    }
    if (valueCount_ >= limits_.maximumValues) {
        return CanonicalJsonWriterResult::failure(CanonicalJsonWriterError::ValueLimitExceeded);
    }

    prefix.size = 0;
    if (depth_ == 0) {
        return CanonicalJsonWriterResult::success();
    }

    const auto& parent = frames_[depth_ - 1];
    if (parent.kind == ContainerKind::Object) {
        return parent.awaitingValue ? CanonicalJsonWriterResult::success() : invalidState();
    }
    if (parent.entryCount >= limits_.maximumContainerEntries) {
        return CanonicalJsonWriterResult::failure(CanonicalJsonWriterError::ContainerLimitExceeded);
    }

    const std::size_t separatorSize = parent.entryCount == 0 ? 1 : 2;
    if (!checkedAdd(separatorSize, depth_ * 2, prefix.size)) {
        return CanonicalJsonWriterResult::failure(CanonicalJsonWriterError::SizeOverflow);
    }
    return CanonicalJsonWriterResult::success();
}

void CanonicalJsonWriter::writeValuePrefix(const ValuePrefix& prefix) noexcept {
    if (prefix.size == 0) {
        return;
    }

    const auto& parent = frames_[depth_ - 1];
    if (parent.entryCount != 0) {
        output_[offset_++] = ',';
    }
    output_[offset_++] = '\n';
    std::ranges::fill(output_.subspan(offset_, depth_ * 2), ' ');
    offset_ += depth_ * 2;
}

void CanonicalJsonWriter::completeValue() noexcept {
    ++valueCount_;
    if (depth_ == 0) {
        rootWritten_ = true;
        return;
    }

    auto& parent = frames_[depth_ - 1];
    if (parent.kind == ContainerKind::Array) {
        ++parent.entryCount;
    } else {
        parent.awaitingValue = false;
    }
}

CanonicalJsonWriterResult CanonicalJsonWriter::writeContainerStart(const ContainerKind kind,
                                                                   const char opening) noexcept {
    ValuePrefix prefix;
    if (const auto prepared = prepareValue(prefix); !prepared) {
        return prepared;
    }
    std::size_t operationSize = 0;
    if (!checkedAdd(prefix.size, 1, operationSize)) {
        return CanonicalJsonWriterResult::failure(CanonicalJsonWriterError::SizeOverflow);
    }
    if (const auto capacity = ensureAdditionalCapacity(operationSize); !capacity) {
        return capacity;
    }

    writeValuePrefix(prefix);
    output_[offset_++] = opening;
    completeValue();
    frames_[depth_++] = Frame{0, kind, false};
    return CanonicalJsonWriterResult::success();
}

CanonicalJsonWriterResult CanonicalJsonWriter::writeContainerEnd(const ContainerKind kind,
                                                                 const char closing) noexcept {
    if (const auto limits = validateLimits(); !limits) {
        return limits;
    }
    if (finished_ || depth_ == 0) {
        return invalidState();
    }

    const auto& frame = frames_[depth_ - 1];
    if (frame.kind != kind || (kind == ContainerKind::Object && frame.awaitingValue)) {
        return invalidState();
    }

    std::size_t operationSize = 1;
    if (frame.entryCount != 0) {
        operationSize = 2 + (depth_ - 1) * 2;
    }
    if (const auto capacity = ensureAdditionalCapacity(operationSize); !capacity) {
        return capacity;
    }

    if (frame.entryCount != 0) {
        output_[offset_++] = '\n';
        std::ranges::fill(output_.subspan(offset_, (depth_ - 1) * 2), ' ');
        offset_ += (depth_ - 1) * 2;
    }
    output_[offset_++] = closing;
    --depth_;
    return CanonicalJsonWriterResult::success();
}

CanonicalJsonWriterResult CanonicalJsonWriter::beginObject() noexcept {
    return writeContainerStart(ContainerKind::Object, '{');
}

CanonicalJsonWriterResult CanonicalJsonWriter::endObject() noexcept {
    return writeContainerEnd(ContainerKind::Object, '}');
}

CanonicalJsonWriterResult CanonicalJsonWriter::beginArray() noexcept {
    return writeContainerStart(ContainerKind::Array, '[');
}

CanonicalJsonWriterResult CanonicalJsonWriter::endArray() noexcept {
    return writeContainerEnd(ContainerKind::Array, ']');
}

CanonicalJsonWriterResult CanonicalJsonWriter::memberName(const std::string_view name) noexcept {
    if (const auto limits = validateLimits(); !limits) {
        return limits;
    }
    if (finished_ || depth_ == 0) {
        return invalidState();
    }

    auto& frame = frames_[depth_ - 1];
    if (frame.kind != ContainerKind::Object || frame.awaitingValue) {
        return invalidState();
    }
    if (frame.entryCount >= limits_.maximumContainerEntries) {
        return CanonicalJsonWriterResult::failure(CanonicalJsonWriterError::ContainerLimitExceeded);
    }
    if (depth_ + 1 > limits_.maximumDepth) {
        return CanonicalJsonWriterResult::failure(CanonicalJsonWriterError::DepthLimitExceeded);
    }
    if (valueCount_ >= limits_.maximumValues) {
        return CanonicalJsonWriterResult::failure(CanonicalJsonWriterError::ValueLimitExceeded);
    }

    const auto tokenSizeResult = canonicalJsonStringTokenSize(name);
    if (!tokenSizeResult) {
        const auto error = tokenSizeResult.error() == CanonicalJsonStringError::InvalidUtf8
                               ? CanonicalJsonWriterError::InvalidUtf8
                               : CanonicalJsonWriterError::SizeOverflow;
        return CanonicalJsonWriterResult::failure(error);
    }

    const std::size_t prefixSize = (frame.entryCount == 0 ? 1 : 2) + depth_ * 2;
    std::size_t operationSize = 0;
    if (!checkedAdd(prefixSize, *tokenSizeResult.value(), operationSize) ||
        !checkedAdd(operationSize, 2, operationSize)) {
        return CanonicalJsonWriterResult::failure(CanonicalJsonWriterError::SizeOverflow);
    }
    if (const auto capacity = ensureAdditionalCapacity(operationSize); !capacity) {
        return capacity;
    }

    const auto tokenOffset = offset_ + prefixSize;
    const auto tokenOutput = output_.subspan(tokenOffset, *tokenSizeResult.value());
    const auto encoded = encodeCanonicalJsonStringToken(name, tokenOutput);
    if (!encoded) {
        return CanonicalJsonWriterResult::failure(CanonicalJsonWriterError::InvalidUtf8);
    }

    if (frame.entryCount != 0) {
        output_[offset_++] = ',';
    }
    output_[offset_++] = '\n';
    std::ranges::fill(output_.subspan(offset_, depth_ * 2), ' ');
    offset_ += depth_ * 2 + *tokenSizeResult.value();
    output_[offset_++] = ':';
    output_[offset_++] = ' ';
    ++frame.entryCount;
    frame.awaitingValue = true;
    return CanonicalJsonWriterResult::success();
}

CanonicalJsonWriterResult CanonicalJsonWriter::stringValue(const std::string_view value) noexcept {
    ValuePrefix prefix;
    if (const auto prepared = prepareValue(prefix); !prepared) {
        return prepared;
    }

    const auto tokenSizeResult = canonicalJsonStringTokenSize(value);
    if (!tokenSizeResult) {
        const auto error = tokenSizeResult.error() == CanonicalJsonStringError::InvalidUtf8
                               ? CanonicalJsonWriterError::InvalidUtf8
                               : CanonicalJsonWriterError::SizeOverflow;
        return CanonicalJsonWriterResult::failure(error);
    }
    std::size_t operationSize = 0;
    if (!checkedAdd(prefix.size, *tokenSizeResult.value(), operationSize)) {
        return CanonicalJsonWriterResult::failure(CanonicalJsonWriterError::SizeOverflow);
    }
    if (const auto capacity = ensureAdditionalCapacity(operationSize); !capacity) {
        return capacity;
    }

    const auto tokenOutput = output_.subspan(offset_ + prefix.size, *tokenSizeResult.value());
    const auto encoded = encodeCanonicalJsonStringToken(value, tokenOutput);
    if (!encoded) {
        return CanonicalJsonWriterResult::failure(CanonicalJsonWriterError::InvalidUtf8);
    }
    writeValuePrefix(prefix);
    offset_ += *tokenSizeResult.value();
    completeValue();
    return CanonicalJsonWriterResult::success();
}

CanonicalJsonWriterResult CanonicalJsonWriter::writeToken(const std::string_view token) noexcept {
    ValuePrefix prefix;
    if (const auto prepared = prepareValue(prefix); !prepared) {
        return prepared;
    }
    std::size_t operationSize = 0;
    if (!checkedAdd(prefix.size, token.size(), operationSize)) {
        return CanonicalJsonWriterResult::failure(CanonicalJsonWriterError::SizeOverflow);
    }
    if (const auto capacity = ensureAdditionalCapacity(operationSize); !capacity) {
        return capacity;
    }

    writeValuePrefix(prefix);
    std::ranges::copy(token, output_.subspan(offset_, token.size()).begin());
    offset_ += token.size();
    completeValue();
    return CanonicalJsonWriterResult::success();
}

CanonicalJsonWriterResult CanonicalJsonWriter::booleanValue(const bool value) noexcept {
    return writeToken(value ? std::string_view{"true"} : std::string_view{"false"});
}

CanonicalJsonWriterResult CanonicalJsonWriter::nullValue() noexcept { return writeToken("null"); }

CanonicalJsonWriterResult CanonicalJsonWriter::integerValue(const std::uint32_t value) noexcept {
    const auto token = formatCanonicalUInt64(value);
    return writeToken(token.view());
}

CanonicalJsonWriterResult CanonicalJsonWriter::float64Value(const double value) noexcept {
    const auto token = formatCanonicalFloat64(value);
    if (!token) {
        return CanonicalJsonWriterResult::failure(CanonicalJsonWriterError::NonFiniteNumber);
    }
    return writeToken(token.value()->view());
}

CanonicalJsonWriterResult
CanonicalJsonWriter::unknownNumberValue(const UnknownJsonNumber& value) noexcept {
    const auto token = formatUnknownJsonNumber(value);
    return writeToken(token.view());
}

CanonicalJsonWriterResult CanonicalJsonWriter::finish() noexcept {
    if (const auto limits = validateLimits(); !limits) {
        return limits;
    }
    if (finished_ || !rootWritten_ || depth_ != 0) {
        return invalidState();
    }
    if (const auto capacity = ensureAdditionalCapacity(1); !capacity) {
        return capacity;
    }

    output_[offset_++] = '\n';
    finished_ = true;
    return CanonicalJsonWriterResult::success();
}

} // namespace bloom::project
