#include <bloom/output/output_facet_descriptor.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <source_location>
#include <string_view>
#include <type_traits>

namespace {

namespace output = bloom::output;

using Error = output::OutputFacetDescriptorErrorCode;
using Schema = output::OutputFacetDescriptorSchemaV1;

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

[[nodiscard]] bool hasError(const output::OutputFacetDescriptorValidation result, const Error error,
                            const std::size_t offset) noexcept {
    return !result && result.error() == error && result.errorOffset() == offset;
}

template <typename Enum> [[nodiscard]] Enum enumWithBits(const std::uint8_t bits) noexcept {
    static_assert(sizeof(Enum) == sizeof(bits));
    Enum value{};
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

void testAllClosedSchemas(Expectations& expectations) {
    struct Case final {
        Schema schema;
        std::string_view descriptor;
    };
    constexpr std::array cases{
        Case{Schema::Absent, ""},
        Case{Schema::Pixels, "height=u:1080;packing=id:rgba;sample-type=id:float32;width=u:1920"},
        Case{Schema::Precision, "component-type=id:float32"},
        Case{Schema::Color, "color-id=id:lin_rec709_scene"},
        Case{Schema::AlphaAssociation, "association=id:premultiplied;zero-alpha=id:preserve-rgb"},
        Case{Schema::Channels,
             "count=u:4;name-0=utf8:52;name-1=utf8:47;name-2=utf8:42;name-3=utf8:41;"
             "role-0=id:red;role-1=id:green;role-2=id:blue;role-3=id:alpha"},
        Case{Schema::Window, "height=u:4294967295;origin-x=i:-9223372036854775808;"
                             "origin-y=i:9223372036854775807;width=u:1"},
        Case{Schema::PixelAspectRational, "denominator=u:1;numerator=u:1"},
        Case{Schema::PixelAspectBinary32, "value=f32:3f800000"},
        Case{Schema::Compression, "method=id:zip"},
        Case{Schema::Metadata, "profile=id:none"},
        Case{Schema::ExternalDependencies, "kind=id:ocio;revision=id:none"},
    };

    bool allValid = true;
    for (const auto& test : cases) {
        allValid = allValid && static_cast<bool>(output::validateOutputFacetDescriptorV1(
                                   test.schema, test.descriptor));
    }
    expectations.expect(allValid,
                        "every source/target descriptor shape has a canonical valid fixture");
}

void testAbsenceAndClosedKeys(Expectations& expectations) {
    expectations.expect(
        hasError(output::validateOutputFacetDescriptorV1(Schema::Absent, "kind=id:none"),
                 Error::ExpectedAbsent, 0),
        "only an explicitly selected absent side accepts an empty descriptor");
    expectations.expect(hasError(output::validateOutputFacetDescriptorV1(Schema::Metadata, ""),
                                 Error::UnexpectedEmpty, 0),
                        "a present facet schema rejects the empty descriptor");
    expectations.expect(
        hasError(output::validateOutputFacetDescriptorV1(
                     Schema::Pixels,
                     "height=u:1;packing=id:rgba;sample-type=id:float32;surprise=id:no;"
                     "width=u:1"),
                 Error::UnknownKey, 50),
        "a closed descriptor rejects an unknown key at its first byte");
    expectations.expect(hasError(output::validateOutputFacetDescriptorV1(
                                     Schema::Pixels, "height=u:1;packing=id:rgba;width=u:1"),
                                 Error::MissingKey, 36),
                        "a closed descriptor reports a missing required key at end of input");
}

void testFieldSyntaxAndOrdering(Expectations& expectations) {
    expectations.expect(hasError(output::validateOutputFacetDescriptorV1(Schema::Metadata, ";"),
                                 Error::EmptyField, 0),
                        "a leading empty field is rejected");
    expectations.expect(
        hasError(output::validateOutputFacetDescriptorV1(Schema::Metadata, "profile=id:none;"),
                 Error::EmptyField, 16),
        "a trailing separator is rejected as an empty field");
    expectations.expect(
        hasError(output::validateOutputFacetDescriptorV1(Schema::Metadata, "profile"),
                 Error::MissingEquals, 7),
        "a field without equals reports its end offset");
    expectations.expect(
        hasError(output::validateOutputFacetDescriptorV1(Schema::Metadata, "Profile=id:none"),
                 Error::InvalidKey, 0),
        "keys use the exact lowercase ASCII grammar");
    expectations.expect(
        hasError(output::validateOutputFacetDescriptorV1(Schema::Metadata, "pro_file=id:none"),
                 Error::InvalidKey, 3),
        "an invalid later key byte reports its exact input offset");
    expectations.expect(
        hasError(output::validateOutputFacetDescriptorV1(
                     Schema::Pixels,
                     "height=u:1;packing=id:rgba;packing=id:rgba;sample-type=id:float32;"
                     "width=u:1"),
                 Error::DuplicateKey, 27),
        "duplicate keys are distinguished from other ordering failures");
    expectations.expect(
        hasError(output::validateOutputFacetDescriptorV1(
                     Schema::Pixels, "packing=id:rgba;height=u:1;sample-type=id:float32;width=u:1"),
                 Error::OutOfOrderKey, 16),
        "keys must be strictly increasing ASCII byte strings");
}

void testExactFieldValueForms(Expectations& expectations) {
    expectations.expect(
        hasError(output::validateOutputFacetDescriptorV1(
                     Schema::Window, "height=u:01;origin-x=i:0;origin-y=i:0;width=u:1"),
                 Error::InvalidUnsignedDecimal, 10),
        "unsigned integers reject redundant leading zeroes");
    expectations.expect(
        hasError(output::validateOutputFacetDescriptorV1(
                     Schema::Window, "height=u:1;origin-x=i:-0;origin-y=i:0;width=u:1"),
                 Error::InvalidSignedDecimal, 23),
        "signed integers reject negative zero");
    expectations.expect(
        hasError(output::validateOutputFacetDescriptorV1(
                     Schema::Window,
                     "height=u:18446744073709551616;origin-x=i:0;origin-y=i:0;width=u:1"),
                 Error::UnsignedDecimalOutOfRange, 19),
        "unsigned extent fields fail deterministically beyond uint32");
    expectations.expect(
        hasError(
            output::validateOutputFacetDescriptorV1(
                Schema::Window, "height=u:1;origin-x=i:9223372036854775808;origin-y=i:0;width=u:1"),
            Error::SignedDecimalOutOfRange, 40),
        "signed descriptor fields fail deterministically beyond int64");
    expectations.expect(hasError(output::validateOutputFacetDescriptorV1(
                                     Schema::PixelAspectBinary32, "value=f32:3F800000"),
                                 Error::InvalidFloatBits, 11),
                        "floating-point bits require exact lowercase hexadecimal");
    expectations.expect(hasError(output::validateOutputFacetDescriptorV1(
                                     Schema::PixelAspectBinary32, "value=f64:3ff0000000000000"),
                                 Error::InvalidValueTag, 6),
                        "the EXR target pixel-aspect schema requires binary32 bits");
    expectations.expect(
        hasError(output::validateOutputFacetDescriptorV1(Schema::Compression, "method=id:"),
                 Error::InvalidIdentifier, 10),
        "identifiers are non-empty");
    expectations.expect(hasError(output::validateOutputFacetDescriptorV1(Schema::Compression,
                                                                         "method=id:zip level"),
                                 Error::InvalidIdentifier, 13),
                        "identifier bytes are restricted to the exact portable alphabet");
}

void testGenericTaggedValueGrammar(Expectations& expectations) {
    using Tag = output::OutputFacetDescriptorValueTagV1;
    struct Case final {
        std::string_view encoded;
        Tag tag;
    };
    constexpr std::array cases{
        Case{"b:0", Tag::Boolean},
        Case{"b:1", Tag::Boolean},
        Case{"i:-999999999999999999999999999999999999999", Tag::SignedInteger},
        Case{"u:999999999999999999999999999999999999999", Tag::UnsignedInteger},
        Case{"r:0/1", Tag::Rational},
        Case{"r:-6/35", Tag::Rational},
        Case{"r:9223372036854775808/3", Tag::Rational},
        Case{"r:999999999999999999999999999999999999999/1", Tag::Rational},
        Case{"f32:80000000", Tag::Float32},
        Case{"f64:7ff0000000000000", Tag::Float64},
        Case{"id:A-Za-z_09./:value", Tag::Identifier},
        Case{"utf8:4153434949", Tag::Utf8},
    };

    bool allValid = true;
    for (const auto& test : cases) {
        const auto result = output::validateOutputFacetDescriptorValueV1(test.encoded);
        allValid = allValid && result.hasValue() && result.valueTag() == test.tag;
    }
    expectations.expect(allValid,
                        "all eight exact tags accept their canonical lexical boundary forms");

    const auto nonNormalized = output::validateOutputFacetDescriptorValueV1("r:6/8");
    expectations.expect(!nonNormalized && nonNormalized.error() == Error::NonNormalizedRational &&
                            nonNormalized.errorOffset() == 2,
                        "a fixed-width rational must be reduced");
    const auto nonCanonicalZero = output::validateOutputFacetDescriptorValueV1("r:0/2");
    expectations.expect(!nonCanonicalZero &&
                            nonCanonicalZero.error() == Error::NonNormalizedRational &&
                            nonCanonicalZero.errorOffset() == 2,
                        "zero has the unique normalized rational spelling zero over one");
    const auto unproven =
        output::validateOutputFacetDescriptorValueV1("r:999999999999999999999999999999999999991/"
                                                     "999999999999999999999999999999999999989");
    expectations.expect(!unproven && unproven.error() == Error::NumericProofUnavailable &&
                            unproven.errorOffset() == 2,
                        "a large non-trivial rational reports unavailable GCD proof honestly");
    const auto badBoolean = output::validateOutputFacetDescriptorValueV1("b:2");
    expectations.expect(!badBoolean && badBoolean.error() == Error::InvalidBoolean &&
                            badBoolean.errorOffset() == 2,
                        "the boolean payload is exactly zero or one");
    const auto unknownTag = output::validateOutputFacetDescriptorValueV1("decimal:1");
    expectations.expect(!unknownTag && unknownTag.error() == Error::InvalidValueTag &&
                            unknownTag.errorOffset() == 0,
                        "unknown value tags fail at the first byte");
}

void testChannelShapeAndLexicalIndices(Expectations& expectations) {
    constexpr std::string_view twelveChannels =
        "count=u:12;name-0=utf8:30;name-1=utf8:31;name-10=utf8:3130;"
        "name-11=utf8:3131;name-2=utf8:32;name-3=utf8:33;name-4=utf8:34;"
        "name-5=utf8:35;name-6=utf8:36;name-7=utf8:37;name-8=utf8:38;"
        "name-9=utf8:39;role-0=id:r0;role-1=id:r1;role-10=id:r10;role-11=id:r11;"
        "role-2=id:r2;role-3=id:r3;role-4=id:r4;role-5=id:r5;role-6=id:r6;"
        "role-7=id:r7;role-8=id:r8;role-9=id:r9";
    expectations.expect(
        output::validateOutputFacetDescriptorV1(Schema::Channels, twelveChannels).hasValue(),
        "multi-digit channel keys follow ASCII lexical order while covering contiguous indices");

    expectations.expect(
        hasError(output::validateOutputFacetDescriptorV1(
                     Schema::Channels,
                     "count=u:3;name-0=utf8:52;name-2=utf8:42;role-0=id:red;role-1=id:green;"
                     "role-2=id:blue"),
                 Error::MissingKey, 84),
        "unique in-range channel keys must still cover every contiguous index");
    expectations.expect(
        hasError(output::validateOutputFacetDescriptorV1(
                     Schema::Channels,
                     "count=u:2;name-00=utf8:52;name-1=utf8:47;role-0=id:red;role-1=id:green"),
                 Error::InvalidChannelIndex, 16),
        "channel indices use minimal unsigned decimal");
    expectations.expect(
        hasError(output::validateOutputFacetDescriptorV1(
                     Schema::Channels,
                     "count=u:2;name-0=utf8:52;name-2=utf8:42;role-0=id:red;role-1=id:green"),
                 Error::ChannelIndexOutOfRange, 30),
        "channel indices must be less than the declared count");
    expectations.expect(
        hasError(output::validateOutputFacetDescriptorV1(
                     Schema::Channels, "count=u:12;name-0=utf8:30;name-1=utf8:31;name-2=utf8:32;"
                                       "name-10=utf8:3130"),
                 Error::OutOfOrderKey, 56),
        "numeric iteration order is not accepted in place of canonical ASCII key order");
    expectations.expect(
        output::validateOutputFacetDescriptorV1(Schema::Channels, "count=u:0").hasValue(),
        "a zero-channel descriptor has no undeclared list fields");
    expectations.expect(hasError(output::validateOutputFacetDescriptorV1(
                                     Schema::Channels, "count=u:18446744073709551616"),
                                 Error::MissingKey, 28),
                        "an arbitrary-width count is lexically valid and fails only because its "
                        "fields are missing");
}

void testUtf8HexBoundary(Expectations& expectations) {
    expectations.expect(output::validateOutputFacetDescriptorV1(
                            Schema::Channels, "count=u:1;name-0=utf8:4153434949;role-0=id:data")
                            .hasValue(),
                        "ASCII utf8 hex is strict UTF-8 and provably NFC");
    expectations.expect(hasError(output::validateOutputFacetDescriptorV1(
                                     Schema::Channels, "count=u:1;name-0=utf8:4;role-0=id:data"),
                                 Error::InvalidUtf8Hex, 23),
                        "utf8 hex requires an even number of digits");
    expectations.expect(hasError(output::validateOutputFacetDescriptorV1(
                                     Schema::Channels, "count=u:1;name-0=utf8:4A;role-0=id:data"),
                                 Error::InvalidUtf8Hex, 23),
                        "utf8 bytes require lowercase hexadecimal");
    expectations.expect(hasError(output::validateOutputFacetDescriptorV1(
                                     Schema::Channels, "count=u:1;name-0=utf8:00;role-0=id:data"),
                                 Error::EmbeddedNul, 22),
                        "decoded NUL is forbidden");
    expectations.expect(hasError(output::validateOutputFacetDescriptorV1(
                                     Schema::Channels, "count=u:1;name-0=utf8:c080;role-0=id:data"),
                                 Error::InvalidUtf8, 22),
                        "overlong UTF-8 is rejected before normalization status");
    expectations.expect(
        hasError(output::validateOutputFacetDescriptorV1(
                     Schema::Channels, "count=u:1;name-0=utf8:eda080;role-0=id:data"),
                 Error::InvalidUtf8, 24),
        "UTF-8 surrogate encodings are rejected");
    expectations.expect(
        hasError(output::validateOutputFacetDescriptorV1(
                     Schema::Channels, "count=u:1;name-0=utf8:c3a9;role-0=id:data"),
                 Error::NormalizationUnavailable, 22),
        "valid non-ASCII reports unavailable NFC proof instead of being accepted optimistically");
}

void testInvalidSchema(Expectations& expectations) {
    expectations.expect(hasError(output::validateOutputFacetDescriptorV1(
                                     enumWithBits<Schema>(0xFFU), "profile=id:none"),
                                 Error::InvalidSchema, 0),
                        "an unknown schema representation fails closed");
}

} // namespace

int main() {
    Expectations expectations;
    testAllClosedSchemas(expectations);
    testAbsenceAndClosedKeys(expectations);
    testFieldSyntaxAndOrdering(expectations);
    testExactFieldValueForms(expectations);
    testGenericTaggedValueGrammar(expectations);
    testChannelShapeAndLexicalIndices(expectations);
    testUtf8HexBoundary(expectations);
    testInvalidSchema(expectations);
    return expectations.failures() == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
