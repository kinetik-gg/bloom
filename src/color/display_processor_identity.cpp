#include <bloom/color/display_processor_identity.hpp>

#include <bloom/core/utf8.hpp>

#include <algorithm>
#include <array>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <limits>
#include <memory>
#include <span>
#include <string_view>
#include <utility>

namespace {

constexpr std::string_view kIdentityDomain{"BloomDisplayProcessorIdentity\0", 30};
constexpr std::size_t kFixedHeaderBytes = kIdentityDomain.size() + 2 + 32 + 2;

static_assert(kIdentityDomain.size() == 30 && kIdentityDomain.back() == '\0');

struct TextValidation final {
    bloom::color::DisplayProcessorIdentityError error =
        bloom::color::DisplayProcessorIdentityError::None;
    std::size_t errorOffset = 0;
};

[[nodiscard]] TextValidation validateCanonicalText(const std::string_view text,
                                                   const std::size_t maximumBytes) noexcept {
    using bloom::color::DisplayProcessorIdentityError;

    if (text.size() > maximumBytes) {
        return {DisplayProcessorIdentityError::TextByteCountLimitExceeded, 0};
    }
    if (!bloom::core::isValidUtf8(text)) {
        return {DisplayProcessorIdentityError::InvalidUtf8, 0};
    }
    for (std::size_t offset = 0; offset < text.size(); ++offset) {
        const auto byte = static_cast<unsigned char>(text[offset]);
        if (byte == 0) {
            return {DisplayProcessorIdentityError::EmbeddedNul, offset};
        }
        if (byte > 0x7FU) {
            // Bloom cannot yet prove Unicode 15.1 NFC without the qualified normalizer.
            return {DisplayProcessorIdentityError::NormalizationUnavailable, offset};
        }
    }
    return {};
}

[[nodiscard]] constexpr bool isAsciiLetter(const char value) noexcept {
    return (value >= 'A' && value <= 'Z') || (value >= 'a' && value <= 'z');
}

[[nodiscard]] constexpr bool isAsciiDigit(const char value) noexcept {
    return value >= '0' && value <= '9';
}

[[nodiscard]] bool isContextName(const std::string_view name) noexcept {
    if (name.empty() || (!isAsciiLetter(name.front()) && name.front() != '_')) {
        return false;
    }
    return std::all_of(name.begin() + 1, name.end(), [](const char value) {
        return isAsciiLetter(value) || isAsciiDigit(value) || value == '_';
    });
}

[[nodiscard]] constexpr bool addChecked(std::size_t& value, const std::size_t increment) noexcept {
    if (increment > std::numeric_limits<std::size_t>::max() - value) {
        return false;
    }
    value += increment;
    return true;
}

[[nodiscard]] bool addTextSize(std::size_t& value, const std::string_view text) noexcept {
    return addChecked(value, 4) && addChecked(value, text.size());
}

[[nodiscard]] bool rangesOverlap(const std::span<const std::byte> destination,
                                 const void* const sourceData,
                                 const std::size_t sourceSize) noexcept {
    if (destination.empty() || sourceSize == 0) {
        return false;
    }
    const auto* const destinationBegin = destination.data();
    const auto* const destinationEnd = destinationBegin + destination.size();
    const auto* const sourceBegin = static_cast<const std::byte*>(sourceData);
    const auto* const sourceEnd = sourceBegin + sourceSize;
    const std::less<const std::byte*> less;
    return less(destinationBegin, sourceEnd) && less(sourceBegin, destinationEnd);
}

template <typename Value>
[[nodiscard]] bool rangeOverlapsObjects(const std::span<const std::byte> destination,
                                        const std::span<const Value> source) noexcept {
    return rangesOverlap(destination, source.data(), source.size_bytes());
}

[[nodiscard]] bool
inputAliasesDestination(const bloom::color::DisplayProcessorIdentityV1InputView& input,
                        const std::span<const std::byte> destination) noexcept {
    const auto textOverlaps = [destination](const std::string_view text) noexcept {
        return rangesOverlap(destination, text.data(), text.size());
    };
    if (rangesOverlap(destination, std::addressof(input), sizeof(input)) ||
        rangeOverlapsObjects(destination, input.contextVariables) ||
        rangeOverlapsObjects(destination, input.lookNames) ||
        textOverlaps(input.sourceColorSpaceId) || textOverlaps(input.displayName) ||
        textOverlaps(input.viewName) || textOverlaps(input.outputColorSpaceId) ||
        textOverlaps(input.qualityId) || textOverlaps(input.semanticsProfileId) ||
        textOverlaps(input.packingId)) {
        return true;
    }
    for (const auto& variable : input.contextVariables) {
        if (textOverlaps(variable.name) || textOverlaps(variable.value)) {
            return true;
        }
    }
    return std::ranges::any_of(input.lookNames, textOverlaps);
}

void storeBigEndian16(const std::uint16_t value, std::byte* const destination) noexcept {
    destination[0] = static_cast<std::byte>(value >> 8U);
    destination[1] = static_cast<std::byte>(value);
}

void storeBigEndian32(const std::uint32_t value, std::byte* const destination) noexcept {
    destination[0] = static_cast<std::byte>(value >> 24U);
    destination[1] = static_cast<std::byte>(value >> 16U);
    destination[2] = static_cast<std::byte>(value >> 8U);
    destination[3] = static_cast<std::byte>(value);
}

class IdentityWriter final {
  public:
    explicit IdentityWriter(const std::span<std::byte> destination) noexcept
        : destination_(destination) {}

    void appendByte(const std::uint8_t value) noexcept {
        destination_[offset_] = static_cast<std::byte>(value);
        ++offset_;
    }

    void appendU16(const std::uint16_t value) noexcept {
        storeBigEndian16(value, destination_.data() + offset_);
        offset_ += 2;
    }

    void appendU32(const std::uint32_t value) noexcept {
        storeBigEndian32(value, destination_.data() + offset_);
        offset_ += 4;
    }

    void appendBytes(const std::span<const std::byte> bytes) noexcept {
        if (bytes.empty()) {
            return;
        }
        std::memcpy(destination_.data() + offset_, bytes.data(), bytes.size());
        offset_ += bytes.size();
    }

    void appendText(const std::string_view text) noexcept {
        appendU32(static_cast<std::uint32_t>(text.size()));
        appendBytes(std::as_bytes(std::span(text.data(), text.size())));
    }

  private:
    std::span<std::byte> destination_;
    std::size_t offset_ = 0;
};

class IdentityReader final {
  public:
    explicit IdentityReader(const std::span<const std::byte> bytes) noexcept : bytes_(bytes) {}

    [[nodiscard]] bool readDomain() noexcept {
        for (std::size_t index = 0; index < kIdentityDomain.size(); ++index) {
            if (offset_ >= bytes_.size()) {
                return fail(bloom::color::DisplayProcessorIdentityError::Truncated, bytes_.size());
            }
            const auto expected = static_cast<std::byte>(kIdentityDomain[index]);
            if (bytes_[offset_] != expected) {
                return fail(bloom::color::DisplayProcessorIdentityError::InvalidDomain, offset_);
            }
            ++offset_;
        }
        return true;
    }

    [[nodiscard]] bool readByte(std::uint8_t& value) noexcept {
        if (offset_ >= bytes_.size()) {
            return fail(bloom::color::DisplayProcessorIdentityError::Truncated, bytes_.size());
        }
        value = std::to_integer<std::uint8_t>(bytes_[offset_]);
        ++offset_;
        return true;
    }

    [[nodiscard]] bool readU16(std::uint16_t& value) noexcept {
        if (bytes_.size() - offset_ < 2) {
            return fail(bloom::color::DisplayProcessorIdentityError::Truncated, bytes_.size());
        }
        value = static_cast<std::uint16_t>(
            static_cast<std::uint16_t>(std::to_integer<std::uint8_t>(bytes_[offset_])) << 8U);
        value |= std::to_integer<std::uint8_t>(bytes_[offset_ + 1]);
        offset_ += 2;
        return true;
    }

    [[nodiscard]] bool readRevision(bloom::core::Sha256Digest& revision) noexcept {
        if (bytes_.size() - offset_ < bloom::core::kSha256DigestBytes) {
            return fail(bloom::color::DisplayProcessorIdentityError::Truncated, bytes_.size());
        }
        bloom::core::Sha256Digest::Bytes revisionBytes{};
        for (std::size_t index = 0; index < revisionBytes.size(); ++index) {
            revisionBytes[index] = std::to_integer<std::uint8_t>(bytes_[offset_ + index]);
        }
        offset_ += revisionBytes.size();
        revision = bloom::core::Sha256Digest::fromBytes(revisionBytes);
        return true;
    }

    [[nodiscard]] bool readText(const std::size_t maximumBytes, const bool mustBeNonempty,
                                const bloom::color::DisplayProcessorIdentityError emptyError,
                                std::string_view& value) noexcept {
        const auto lengthOffset = offset_;
        std::uint32_t length = 0;
        if (!readU32(length)) {
            return false;
        }
        if (length > maximumBytes) {
            return fail(bloom::color::DisplayProcessorIdentityError::TextByteCountLimitExceeded,
                        lengthOffset);
        }
        if (bytes_.size() - offset_ < length) {
            return fail(bloom::color::DisplayProcessorIdentityError::Truncated, bytes_.size());
        }

        const auto dataOffset = offset_;
        const auto* const data = reinterpret_cast<const char*>(bytes_.data() + offset_);
        value = std::string_view(data, length);
        offset_ += length;

        if (mustBeNonempty && value.empty()) {
            return fail(emptyError, dataOffset);
        }
        const auto validation = validateCanonicalText(value, maximumBytes);
        if (validation.error != bloom::color::DisplayProcessorIdentityError::None) {
            return fail(validation.error, dataOffset + validation.errorOffset);
        }
        return true;
    }

    [[nodiscard]] std::size_t offset() const noexcept { return offset_; }
    [[nodiscard]] bloom::color::DisplayProcessorIdentityError error() const noexcept {
        return error_;
    }
    [[nodiscard]] std::size_t errorOffset() const noexcept { return errorOffset_; }

  private:
    [[nodiscard]] bool readU32(std::uint32_t& value) noexcept {
        if (bytes_.size() - offset_ < 4) {
            return fail(bloom::color::DisplayProcessorIdentityError::Truncated, bytes_.size());
        }
        value = static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(bytes_[offset_])) << 24U;
        value |= static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(bytes_[offset_ + 1]))
                 << 16U;
        value |= static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(bytes_[offset_ + 2]))
                 << 8U;
        value |= std::to_integer<std::uint8_t>(bytes_[offset_ + 3]);
        offset_ += 4;
        return true;
    }

    [[nodiscard]] bool fail(const bloom::color::DisplayProcessorIdentityError error,
                            const std::size_t errorOffset) noexcept {
        error_ = error;
        errorOffset_ = errorOffset;
        return false;
    }

    std::span<const std::byte> bytes_;
    std::size_t offset_ = 0;
    bloom::color::DisplayProcessorIdentityError error_ =
        bloom::color::DisplayProcessorIdentityError::None;
    std::size_t errorOffset_ = 0;
};

} // namespace

namespace bloom::color {

DisplayProcessorIdentityV1Validation
validateDisplayProcessorIdentityV1(const DisplayProcessorIdentityV1InputView& input) noexcept {
    if (input.contextVariables.size() > kDisplayProcessorIdentityMaximumContextVariables) {
        return DisplayProcessorIdentityV1Validation(
            DisplayProcessorIdentityError::ContextVariableCountLimitExceeded);
    }

    std::size_t requiredBytes = kFixedHeaderBytes;
    std::string_view previousContextName;
    bool hasPreviousContextName = false;
    for (const auto& variable : input.contextVariables) {
        const auto nameValidation =
            validateCanonicalText(variable.name, kDisplayProcessorIdentityMaximumContextNameBytes);
        if (nameValidation.error == DisplayProcessorIdentityError::TextByteCountLimitExceeded) {
            return DisplayProcessorIdentityV1Validation(
                DisplayProcessorIdentityError::ContextNameByteCountLimitExceeded);
        }
        if (nameValidation.error != DisplayProcessorIdentityError::None) {
            return DisplayProcessorIdentityV1Validation(nameValidation.error);
        }
        if (!isContextName(variable.name)) {
            return DisplayProcessorIdentityV1Validation(
                DisplayProcessorIdentityError::InvalidContextName);
        }
        if (hasPreviousContextName) {
            const auto order = core::compareUtf8Bytes(previousContextName, variable.name);
            if (order == std::strong_ordering::equal) {
                return DisplayProcessorIdentityV1Validation(
                    DisplayProcessorIdentityError::DuplicateContextName);
            }
            if (order == std::strong_ordering::greater) {
                return DisplayProcessorIdentityV1Validation(
                    DisplayProcessorIdentityError::ContextVariablesNotStrictlyOrdered);
            }
        }
        previousContextName = variable.name;
        hasPreviousContextName = true;

        const auto valueValidation =
            validateCanonicalText(variable.value, kDisplayProcessorIdentityMaximumTextBytes);
        if (valueValidation.error != DisplayProcessorIdentityError::None) {
            return DisplayProcessorIdentityV1Validation(valueValidation.error);
        }
        if (!addTextSize(requiredBytes, variable.name) ||
            !addTextSize(requiredBytes, variable.value)) {
            return DisplayProcessorIdentityV1Validation(
                DisplayProcessorIdentityError::IdentityByteCountOverflow);
        }
    }

    const auto validateText = [](const std::string_view value) noexcept {
        return validateCanonicalText(value, kDisplayProcessorIdentityMaximumTextBytes).error;
    };
    const auto sourceError = validateText(input.sourceColorSpaceId);
    if (sourceError != DisplayProcessorIdentityError::None) {
        return DisplayProcessorIdentityV1Validation(sourceError);
    }
    if (input.sourceColorSpaceId != kDisplayProcessorIdentitySourceColorSpaceId) {
        return DisplayProcessorIdentityV1Validation(
            DisplayProcessorIdentityError::InvalidSourceColorSpaceId);
    }

    const auto displayError = validateText(input.displayName);
    if (displayError != DisplayProcessorIdentityError::None) {
        return DisplayProcessorIdentityV1Validation(displayError);
    }
    if (input.displayName.empty()) {
        return DisplayProcessorIdentityV1Validation(
            DisplayProcessorIdentityError::EmptyDisplayName);
    }

    const auto viewError = validateText(input.viewName);
    if (viewError != DisplayProcessorIdentityError::None) {
        return DisplayProcessorIdentityV1Validation(viewError);
    }
    if (input.viewName.empty()) {
        return DisplayProcessorIdentityV1Validation(DisplayProcessorIdentityError::EmptyViewName);
    }

    switch (input.lookMode) {
    case DisplayProcessorLookModeV1::Bypass:
        if (!input.lookNames.empty()) {
            return DisplayProcessorIdentityV1Validation(
                DisplayProcessorIdentityError::LookCountMismatch);
        }
        break;
    case DisplayProcessorLookModeV1::Ordered:
        if (input.lookNames.empty()) {
            return DisplayProcessorIdentityV1Validation(
                DisplayProcessorIdentityError::LookCountMismatch);
        }
        if (input.lookNames.size() > kDisplayProcessorIdentityMaximumLooks) {
            return DisplayProcessorIdentityV1Validation(
                DisplayProcessorIdentityError::LookCountLimitExceeded);
        }
        break;
    default:
        return DisplayProcessorIdentityV1Validation(DisplayProcessorIdentityError::InvalidLookMode);
    }

    for (const auto lookName : input.lookNames) {
        const auto lookError = validateText(lookName);
        if (lookError != DisplayProcessorIdentityError::None) {
            return DisplayProcessorIdentityV1Validation(lookError);
        }
        if (lookName.empty()) {
            return DisplayProcessorIdentityV1Validation(
                DisplayProcessorIdentityError::EmptyLookName);
        }
        if (!addTextSize(requiredBytes, lookName)) {
            return DisplayProcessorIdentityV1Validation(
                DisplayProcessorIdentityError::IdentityByteCountOverflow);
        }
    }

    const std::array<std::pair<std::string_view, std::string_view>, 4> fixedFields{{
        {input.outputColorSpaceId, kDisplayProcessorIdentityOutputColorSpaceId},
        {input.qualityId, kDisplayProcessorIdentityQualityId},
        {input.semanticsProfileId, kDisplayProcessorIdentitySemanticsProfileId},
        {input.packingId, kDisplayProcessorIdentityPackingId},
    }};
    const std::array<DisplayProcessorIdentityError, 4> fixedErrors{{
        DisplayProcessorIdentityError::InvalidOutputColorSpaceId,
        DisplayProcessorIdentityError::InvalidQualityId,
        DisplayProcessorIdentityError::InvalidSemanticsProfileId,
        DisplayProcessorIdentityError::InvalidPackingId,
    }};
    for (std::size_t index = 0; index < fixedFields.size(); ++index) {
        const auto fieldError = validateText(fixedFields[index].first);
        if (fieldError != DisplayProcessorIdentityError::None) {
            return DisplayProcessorIdentityV1Validation(fieldError);
        }
        if (fixedFields[index].first != fixedFields[index].second) {
            return DisplayProcessorIdentityV1Validation(fixedErrors[index]);
        }
    }

    if (!addTextSize(requiredBytes, input.sourceColorSpaceId) ||
        !addTextSize(requiredBytes, input.displayName) ||
        !addTextSize(requiredBytes, input.viewName) || !addChecked(requiredBytes, 3) ||
        !addTextSize(requiredBytes, input.outputColorSpaceId) ||
        !addTextSize(requiredBytes, input.qualityId) ||
        !addTextSize(requiredBytes, input.semanticsProfileId) ||
        !addTextSize(requiredBytes, input.packingId)) {
        return DisplayProcessorIdentityV1Validation(
            DisplayProcessorIdentityError::IdentityByteCountOverflow);
    }
    if (requiredBytes > kDisplayProcessorIdentityMaximumBytes) {
        return DisplayProcessorIdentityV1Validation(
            DisplayProcessorIdentityError::IdentityByteCountLimitExceeded);
    }
    return DisplayProcessorIdentityV1Validation(requiredBytes);
}

DisplayProcessorIdentityV1WriteResult
writeDisplayProcessorIdentityV1(const DisplayProcessorIdentityV1InputView& input,
                                const std::span<std::byte> destination) noexcept {
    const auto validation = validateDisplayProcessorIdentityV1(input);
    if (!validation) {
        return DisplayProcessorIdentityV1WriteResult(0, validation.error());
    }
    if (destination.size() < validation.requiredByteCount()) {
        return DisplayProcessorIdentityV1WriteResult(
            validation.requiredByteCount(), DisplayProcessorIdentityError::DestinationTooSmall);
    }

    const auto output =
        std::span<const std::byte>(destination.data(), validation.requiredByteCount());
    if (inputAliasesDestination(input, output)) {
        return DisplayProcessorIdentityV1WriteResult(
            validation.requiredByteCount(), DisplayProcessorIdentityError::InputAliasesDestination);
    }

    IdentityWriter writer(destination.first(validation.requiredByteCount()));
    writer.appendBytes(std::as_bytes(std::span(kIdentityDomain.data(), kIdentityDomain.size())));
    writer.appendU16(kDisplayProcessorIdentityVersion);
    const auto revisionBytes = input.expectedOcioRevision.bytes();
    writer.appendBytes(std::as_bytes(revisionBytes));
    writer.appendU16(static_cast<std::uint16_t>(input.contextVariables.size()));
    for (const auto& variable : input.contextVariables) {
        writer.appendText(variable.name);
        writer.appendText(variable.value);
    }
    writer.appendText(input.sourceColorSpaceId);
    writer.appendText(input.displayName);
    writer.appendText(input.viewName);
    writer.appendByte(static_cast<std::uint8_t>(input.lookMode));
    writer.appendU16(static_cast<std::uint16_t>(input.lookNames.size()));
    for (const auto lookName : input.lookNames) {
        writer.appendText(lookName);
    }
    writer.appendText(input.outputColorSpaceId);
    writer.appendText(input.qualityId);
    writer.appendText(input.semanticsProfileId);
    writer.appendText(input.packingId);

    return DisplayProcessorIdentityV1WriteResult(validation.requiredByteCount());
}

DisplayProcessorIdentityV1ParseResult
parseDisplayProcessorIdentityV1(const std::span<const std::byte> canonicalBytes) noexcept {
    if (canonicalBytes.size() > kDisplayProcessorIdentityMaximumBytes) {
        return DisplayProcessorIdentityV1ParseResult(
            DisplayProcessorIdentityError::IdentityByteCountLimitExceeded, 0);
    }

    IdentityReader reader(canonicalBytes);
    if (!reader.readDomain()) {
        return DisplayProcessorIdentityV1ParseResult(reader.error(), reader.errorOffset());
    }

    const auto rejectReaderError = [&reader]() noexcept {
        return DisplayProcessorIdentityV1ParseResult(reader.error(), reader.errorOffset());
    };
    std::uint16_t version = 0;
    if (!reader.readU16(version)) {
        return rejectReaderError();
    }
    if (version != kDisplayProcessorIdentityVersion) {
        return DisplayProcessorIdentityV1ParseResult(
            DisplayProcessorIdentityError::UnsupportedVersion, kIdentityDomain.size());
    }

    core::Sha256Digest expectedRevision;
    if (!reader.readRevision(expectedRevision)) {
        return rejectReaderError();
    }

    std::uint16_t contextCount = 0;
    if (!reader.readU16(contextCount)) {
        return rejectReaderError();
    }
    if (contextCount > kDisplayProcessorIdentityMaximumContextVariables) {
        return DisplayProcessorIdentityV1ParseResult(
            DisplayProcessorIdentityError::ContextVariableCountLimitExceeded, reader.offset() - 2);
    }

    std::string_view previousContextName;
    bool hasPreviousContextName = false;
    for (std::uint16_t index = 0; index < contextCount; ++index) {
        const auto nameLengthOffset = reader.offset();
        std::string_view name;
        if (!reader.readText(kDisplayProcessorIdentityMaximumContextNameBytes, false,
                             DisplayProcessorIdentityError::InvalidContextName, name)) {
            auto error = reader.error();
            if (error == DisplayProcessorIdentityError::TextByteCountLimitExceeded) {
                error = DisplayProcessorIdentityError::ContextNameByteCountLimitExceeded;
            }
            return DisplayProcessorIdentityV1ParseResult(error, reader.errorOffset());
        }
        if (!isContextName(name)) {
            return DisplayProcessorIdentityV1ParseResult(
                DisplayProcessorIdentityError::InvalidContextName, nameLengthOffset + 4);
        }
        if (hasPreviousContextName) {
            const auto order = core::compareUtf8Bytes(previousContextName, name);
            if (order == std::strong_ordering::equal) {
                return DisplayProcessorIdentityV1ParseResult(
                    DisplayProcessorIdentityError::DuplicateContextName, nameLengthOffset + 4);
            }
            if (order == std::strong_ordering::greater) {
                return DisplayProcessorIdentityV1ParseResult(
                    DisplayProcessorIdentityError::ContextVariablesNotStrictlyOrdered,
                    nameLengthOffset + 4);
            }
        }
        previousContextName = name;
        hasPreviousContextName = true;

        std::string_view value;
        if (!reader.readText(kDisplayProcessorIdentityMaximumTextBytes, false,
                             DisplayProcessorIdentityError::InternalInvariant, value)) {
            return rejectReaderError();
        }
    }

    const auto readFixedText =
        [&reader, &rejectReaderError](const std::string_view expected,
                                      const DisplayProcessorIdentityError mismatchError)
        -> std::optional<DisplayProcessorIdentityV1ParseResult> {
        std::string_view value;
        const auto valueOffset = reader.offset() + 4;
        if (!reader.readText(kDisplayProcessorIdentityMaximumTextBytes, false,
                             DisplayProcessorIdentityError::InternalInvariant, value)) {
            return rejectReaderError();
        }
        if (value != expected) {
            return DisplayProcessorIdentityV1ParseResult(mismatchError, valueOffset);
        }
        return std::nullopt;
    };

    if (const auto error =
            readFixedText(kDisplayProcessorIdentitySourceColorSpaceId,
                          DisplayProcessorIdentityError::InvalidSourceColorSpaceId)) {
        return *error;
    }

    std::string_view displayName;
    if (!reader.readText(kDisplayProcessorIdentityMaximumTextBytes, true,
                         DisplayProcessorIdentityError::EmptyDisplayName, displayName)) {
        return rejectReaderError();
    }
    std::string_view viewName;
    if (!reader.readText(kDisplayProcessorIdentityMaximumTextBytes, true,
                         DisplayProcessorIdentityError::EmptyViewName, viewName)) {
        return rejectReaderError();
    }

    const auto lookModeOffset = reader.offset();
    std::uint8_t lookModeValue = 0;
    if (!reader.readByte(lookModeValue)) {
        return rejectReaderError();
    }
    if (lookModeValue > static_cast<std::uint8_t>(DisplayProcessorLookModeV1::Ordered)) {
        return DisplayProcessorIdentityV1ParseResult(DisplayProcessorIdentityError::InvalidLookMode,
                                                     lookModeOffset);
    }
    const auto lookMode = static_cast<DisplayProcessorLookModeV1>(lookModeValue);

    const auto lookCountOffset = reader.offset();
    std::uint16_t lookCount = 0;
    if (!reader.readU16(lookCount)) {
        return rejectReaderError();
    }
    if (lookCount > kDisplayProcessorIdentityMaximumLooks) {
        return DisplayProcessorIdentityV1ParseResult(
            DisplayProcessorIdentityError::LookCountLimitExceeded, lookCountOffset);
    }
    if ((lookMode == DisplayProcessorLookModeV1::Bypass && lookCount != 0) ||
        (lookMode == DisplayProcessorLookModeV1::Ordered && lookCount == 0)) {
        return DisplayProcessorIdentityV1ParseResult(
            DisplayProcessorIdentityError::LookCountMismatch, lookCountOffset);
    }
    for (std::uint16_t index = 0; index < lookCount; ++index) {
        std::string_view lookName;
        if (!reader.readText(kDisplayProcessorIdentityMaximumTextBytes, true,
                             DisplayProcessorIdentityError::EmptyLookName, lookName)) {
            return rejectReaderError();
        }
    }

    if (const auto error =
            readFixedText(kDisplayProcessorIdentityOutputColorSpaceId,
                          DisplayProcessorIdentityError::InvalidOutputColorSpaceId)) {
        return *error;
    }
    if (const auto error = readFixedText(kDisplayProcessorIdentityQualityId,
                                         DisplayProcessorIdentityError::InvalidQualityId)) {
        return *error;
    }
    if (const auto error =
            readFixedText(kDisplayProcessorIdentitySemanticsProfileId,
                          DisplayProcessorIdentityError::InvalidSemanticsProfileId)) {
        return *error;
    }
    if (const auto error = readFixedText(kDisplayProcessorIdentityPackingId,
                                         DisplayProcessorIdentityError::InvalidPackingId)) {
        return *error;
    }

    if (reader.offset() != canonicalBytes.size()) {
        return DisplayProcessorIdentityV1ParseResult(DisplayProcessorIdentityError::TrailingBytes,
                                                     reader.offset());
    }
    return DisplayProcessorIdentityV1ParseResult(
        DisplayProcessorIdentityV1View(canonicalBytes, expectedRevision));
}

} // namespace bloom::color
