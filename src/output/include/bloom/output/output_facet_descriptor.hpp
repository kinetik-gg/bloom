#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>
#include <type_traits>

namespace bloom::output {

// These are the distinct descriptor shapes used by the source and target sides of the two
// version-1 output presets. Absent is deliberately explicit: no other schema accepts an empty
// descriptor.
enum class OutputFacetDescriptorSchemaV1 : std::uint8_t {
    Absent,
    Pixels,
    Precision,
    Color,
    AlphaAssociation,
    Channels,
    Window,
    PixelAspectRational,
    PixelAspectBinary32,
    Compression,
    Metadata,
    ExternalDependencies,
};

enum class OutputFacetDescriptorValueTagV1 : std::uint8_t {
    Boolean,
    SignedInteger,
    UnsignedInteger,
    Rational,
    Float32,
    Float64,
    Identifier,
    Utf8,
};

enum class OutputFacetDescriptorErrorCode : std::uint8_t {
    None,
    InvalidSchema,
    ExpectedAbsent,
    UnexpectedEmpty,
    EmptyField,
    MissingEquals,
    InvalidKey,
    UnknownKey,
    DuplicateKey,
    OutOfOrderKey,
    MissingKey,
    InvalidValueTag,
    InvalidBoolean,
    InvalidSignedDecimal,
    SignedDecimalOutOfRange,
    InvalidUnsignedDecimal,
    UnsignedDecimalOutOfRange,
    InvalidRational,
    NonNormalizedRational,
    NumericProofUnavailable,
    InvalidFloatBits,
    InvalidIdentifier,
    InvalidUtf8Hex,
    InvalidUtf8,
    EmbeddedNul,
    NormalizationUnavailable,
    InvalidChannelIndex,
    ChannelIndexOutOfRange,
    InternalInvariant,
};

class [[nodiscard]] OutputFacetDescriptorValueValidation final {
  public:
    [[nodiscard]] static constexpr OutputFacetDescriptorValueValidation
    success(const OutputFacetDescriptorValueTagV1 tag) noexcept {
        return OutputFacetDescriptorValueValidation(OutputFacetDescriptorErrorCode::None, 0, tag);
    }
    [[nodiscard]] static constexpr OutputFacetDescriptorValueValidation
    failure(const OutputFacetDescriptorErrorCode code, const std::size_t errorOffset) noexcept {
        return OutputFacetDescriptorValueValidation(
            code == OutputFacetDescriptorErrorCode::None
                ? OutputFacetDescriptorErrorCode::InternalInvariant
                : code,
            errorOffset, OutputFacetDescriptorValueTagV1::Boolean);
    }

    [[nodiscard]] constexpr bool hasValue() const noexcept {
        return error_ == OutputFacetDescriptorErrorCode::None;
    }
    [[nodiscard]] constexpr explicit operator bool() const noexcept { return hasValue(); }
    [[nodiscard]] constexpr OutputFacetDescriptorErrorCode error() const noexcept { return error_; }
    [[nodiscard]] constexpr std::size_t errorOffset() const noexcept { return errorOffset_; }
    // Meaningful only when hasValue() is true.
    [[nodiscard]] constexpr OutputFacetDescriptorValueTagV1 valueTag() const noexcept {
        return tag_;
    }

  private:
    constexpr OutputFacetDescriptorValueValidation(
        const OutputFacetDescriptorErrorCode error, const std::size_t errorOffset,
        const OutputFacetDescriptorValueTagV1 tag) noexcept
        : errorOffset_(errorOffset), error_(error), tag_(tag) {}

    std::size_t errorOffset_;
    OutputFacetDescriptorErrorCode error_;
    OutputFacetDescriptorValueTagV1 tag_;
};

class [[nodiscard]] OutputFacetDescriptorValidation final {
  public:
    [[nodiscard]] static constexpr OutputFacetDescriptorValidation success() noexcept {
        return OutputFacetDescriptorValidation(OutputFacetDescriptorErrorCode::None, 0);
    }
    [[nodiscard]] static constexpr OutputFacetDescriptorValidation
    failure(const OutputFacetDescriptorErrorCode code, const std::size_t errorOffset) noexcept {
        return OutputFacetDescriptorValidation(
            code == OutputFacetDescriptorErrorCode::None
                ? OutputFacetDescriptorErrorCode::InternalInvariant
                : code,
            errorOffset);
    }

    [[nodiscard]] constexpr bool hasValue() const noexcept {
        return error_ == OutputFacetDescriptorErrorCode::None;
    }
    [[nodiscard]] constexpr explicit operator bool() const noexcept { return hasValue(); }
    [[nodiscard]] constexpr OutputFacetDescriptorErrorCode error() const noexcept { return error_; }
    // The first offending input byte. A descriptor-size offset denotes a missing or truncated
    // suffix at end of input. Successful validation reports zero.
    [[nodiscard]] constexpr std::size_t errorOffset() const noexcept { return errorOffset_; }

  private:
    constexpr OutputFacetDescriptorValidation(const OutputFacetDescriptorErrorCode error,
                                              const std::size_t errorOffset) noexcept
        : errorOffset_(errorOffset), error_(error) {}

    std::size_t errorOffset_;
    OutputFacetDescriptorErrorCode error_;
};

// Validates the canonical OutputAnalysis facet-descriptor grammar without allocating. This checks
// the selected closed key set, exact value tags and spellings, global unsigned-ASCII key order, and
// the dynamic contiguous channel-key set. Non-ASCII utf8 values are strict UTF-8 but cannot be
// accepted as Unicode 15.1 NFC until a qualified normalizer is available; they return the distinct
// NormalizationUnavailable result.
[[nodiscard]] OutputFacetDescriptorValidation
validateOutputFacetDescriptorV1(OutputFacetDescriptorSchemaV1 schema,
                                std::string_view descriptor) noexcept;

// Validates one complete tagged value from the version-1 grammar. Signed and unsigned decimal tags
// are arbitrary-width lexical mathematical integers. Rational normalization is proven exactly in
// the allocation-free fixed-width domain; a larger non-trivial GCD returns NumericProofUnavailable
// rather than accepting or rejecting without proof.
[[nodiscard]] OutputFacetDescriptorValueValidation
validateOutputFacetDescriptorValueV1(std::string_view taggedValue) noexcept;

static_assert(std::is_trivially_copyable_v<OutputFacetDescriptorValidation>);
static_assert(std::is_trivially_copyable_v<OutputFacetDescriptorValueValidation>);

} // namespace bloom::output
