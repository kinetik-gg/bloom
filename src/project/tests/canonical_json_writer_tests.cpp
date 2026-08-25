#include <bloom/project/canonical_json_writer.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <limits>
#include <span>
#include <string_view>

namespace {

class Expectations final {
  public:
    void expect(const bool condition, const std::string_view message) {
        if (condition) {
            return;
        }
        ++failures_;
        std::cerr << "FAILED: " << message << '\n';
    }

    [[nodiscard]] int failures() const noexcept { return failures_; }

  private:
    int failures_ = 0;
};

using bloom::project::CanonicalJsonWriter;
using bloom::project::CanonicalJsonWriterError;
using bloom::project::CanonicalJsonWriterLimits;
using bloom::project::CanonicalJsonWriterResult;

static_assert(bloom::project::kCanonicalJsonMaximumDepth == 128);
static_assert(bloom::project::kCanonicalJsonMaximumValues == 4'000'000);
static_assert(bloom::project::kCanonicalJsonMaximumContainerEntries == 1'000'000);

void expectSuccess(Expectations& expectations, const CanonicalJsonWriterResult result,
                   const std::string_view message) {
    expectations.expect(result && result.error() == CanonicalJsonWriterError::None &&
                            !result.requiredCapacity().has_value(),
                        message);
}

void expectError(Expectations& expectations, const CanonicalJsonWriterResult result,
                 const CanonicalJsonWriterError error, const std::string_view message) {
    expectations.expect(!result && result.error() == error, message);
}

void testGoldenLayout(Expectations& expectations) {
    std::array<char, 1024> output{};
    CanonicalJsonWriter writer(output);
    expectSuccess(expectations, writer.beginObject(), "the golden root object begins");
    expectSuccess(expectations, writer.memberName("z-first"), "member call order begins with z");
    expectSuccess(expectations, writer.stringValue("Bloom\n\"UI\""),
                  "string values use canonical escaping");
    expectSuccess(expectations, writer.memberName("enabled"), "a boolean member begins");
    expectSuccess(expectations, writer.booleanValue(true), "a boolean value is written");
    expectSuccess(expectations, writer.memberName("nothing"), "a null member begins");
    expectSuccess(expectations, writer.nullValue(), "a null value is written");
    expectSuccess(expectations, writer.memberName("maximum"), "an integer member begins");
    expectSuccess(expectations, writer.integerValue(UINT32_MAX),
                  "the supported integer maximum is canonical");
    expectSuccess(expectations, writer.memberName("negativeZero"), "a Float64 member begins");
    expectSuccess(expectations, writer.float64Value(std::bit_cast<double>(0x8000000000000000ULL)),
                  "a finite Float64 value is written");
    expectSuccess(expectations, writer.memberName("nested"), "a nested object member begins");
    expectSuccess(expectations, writer.beginObject(), "the nested object begins");
    expectSuccess(expectations, writer.memberName("emptyObject"), "an empty object member begins");
    expectSuccess(expectations, writer.beginObject(), "an empty object begins");
    expectSuccess(expectations, writer.endObject(), "an empty object ends compactly");
    expectSuccess(expectations, writer.memberName("items"), "an array member begins");
    expectSuccess(expectations, writer.beginArray(), "a non-empty array begins");
    expectSuccess(expectations, writer.stringValue("one"), "the first array value is written");
    expectSuccess(expectations, writer.booleanValue(false), "the second array value is written");
    expectSuccess(expectations, writer.beginArray(), "an empty nested array begins");
    expectSuccess(expectations, writer.endArray(), "an empty nested array ends compactly");
    expectSuccess(expectations, writer.beginObject(), "an object array element begins");
    expectSuccess(expectations, writer.memberName("value"), "its member begins");
    expectSuccess(expectations, writer.integerValue(0), "canonical zero is written");
    expectSuccess(expectations, writer.endObject(), "the object array element ends");
    expectSuccess(expectations, writer.endArray(), "the non-empty array ends");
    expectSuccess(expectations, writer.endObject(), "the nested object ends");
    expectSuccess(expectations, writer.endObject(), "the root object ends");
    expectSuccess(expectations, writer.finish(), "the golden document finishes");

    constexpr std::string_view expected = "{\n"
                                          "  \"z-first\": \"Bloom\\n\\\"UI\\\"\",\n"
                                          "  \"enabled\": true,\n"
                                          "  \"nothing\": null,\n"
                                          "  \"maximum\": 4294967295,\n"
                                          "  \"negativeZero\": -0.0,\n"
                                          "  \"nested\": {\n"
                                          "    \"emptyObject\": {},\n"
                                          "    \"items\": [\n"
                                          "      \"one\",\n"
                                          "      false,\n"
                                          "      [],\n"
                                          "      {\n"
                                          "        \"value\": 0\n"
                                          "      }\n"
                                          "    ]\n"
                                          "  }\n"
                                          "}\n";
    expectations.expect(writer.written().size() == expected.size() &&
                            std::string_view(writer.written().data(), writer.written().size()) ==
                                expected,
                        "golden bytes use caller order, two-space multiline layout, and one LF");
}

void testFloat64FailuresAreTransactional(Expectations& expectations) {
    constexpr char sentinel = '?';
    std::array<char, 24> output{};
    output.fill(sentinel);
    CanonicalJsonWriter writer(output);
    expectSuccess(expectations, writer.beginObject(), "the Float64 failure object begins");
    expectSuccess(expectations, writer.memberName("value"), "its pending member begins");
    const auto beforeInvalid = writer.bytesWritten();
    expectError(expectations, writer.float64Value(std::numeric_limits<double>::infinity()),
                CanonicalJsonWriterError::NonFiniteNumber, "infinity is not a JSON number");
    expectError(expectations, writer.float64Value(std::numeric_limits<double>::quiet_NaN()),
                CanonicalJsonWriterError::NonFiniteNumber, "NaN is not a JSON number");
    expectations.expect(writer.bytesWritten() == beforeInvalid && output[beforeInvalid] == sentinel,
                        "non-finite Float64 rejection preserves the pending member and bytes");

    const auto finite = writer.float64Value(1.7976931348623157e+308);
    expectations.expect(!finite &&
                            finite.error() == CanonicalJsonWriterError::OutputCapacityExceeded &&
                            writer.bytesWritten() == beforeInvalid,
                        "Float64 capacity failure is transactional");
    expectSuccess(expectations, writer.float64Value(1.0),
                  "a smaller finite Float64 can complete the member");
    expectSuccess(expectations, writer.endObject(), "the Float64 failure object closes");
    expectSuccess(expectations, writer.finish(), "the Float64 failure object finishes");
}

void testEmptyAndRootValues(Expectations& expectations) {
    std::array<char, 32> objectOutput{};
    CanonicalJsonWriter objectWriter(objectOutput);
    expectSuccess(expectations, objectWriter.beginObject(), "an empty root object begins");
    expectSuccess(expectations, objectWriter.endObject(), "an empty root object ends");
    expectSuccess(expectations, objectWriter.finish(), "an empty root object finishes");
    expectations.expect(
        std::string_view(objectWriter.written().data(), objectWriter.written().size()) == "{}\n",
        "an empty object is compact with one final LF");

    std::array<char, 32> arrayOutput{};
    CanonicalJsonWriter arrayWriter(arrayOutput);
    expectSuccess(expectations, arrayWriter.beginArray(), "an empty root array begins");
    expectSuccess(expectations, arrayWriter.endArray(), "an empty root array ends");
    expectSuccess(expectations, arrayWriter.finish(), "an empty root array finishes");
    expectations.expect(
        std::string_view(arrayWriter.written().data(), arrayWriter.written().size()) == "[]\n",
        "an empty array is compact with one final LF");

    std::array<char, 32> scalarOutput{};
    CanonicalJsonWriter scalarWriter(scalarOutput);
    expectSuccess(expectations, scalarWriter.stringValue("root"), "a scalar root is accepted");
    expectSuccess(expectations, scalarWriter.finish(), "a scalar root finishes");
    expectations.expect(std::string_view(scalarWriter.written().data(),
                                         scalarWriter.written().size()) == "\"root\"\n",
                        "a scalar root receives exactly one final LF");
}

void testGrammarAndRecovery(Expectations& expectations) {
    std::array<char, 256> output{};
    CanonicalJsonWriter writer(output);
    expectError(expectations, writer.finish(), CanonicalJsonWriterError::InvalidState,
                "an empty document cannot finish");
    expectError(expectations, writer.memberName("outside"), CanonicalJsonWriterError::InvalidState,
                "a member name requires an object");
    expectSuccess(expectations, writer.beginObject(), "the recovery object begins");
    expectError(expectations, writer.stringValue("missing-name"),
                CanonicalJsonWriterError::InvalidState,
                "an object requires a member name before a value");
    expectError(expectations, writer.endArray(), CanonicalJsonWriterError::InvalidState,
                "a mismatched close is rejected without changing the stack");
    expectSuccess(expectations, writer.memberName("value"), "a valid member name follows");
    expectError(expectations, writer.memberName("second"), CanonicalJsonWriterError::InvalidState,
                "a second name cannot replace a pending value");
    expectError(expectations, writer.endObject(), CanonicalJsonWriterError::InvalidState,
                "an object cannot close with a missing value");
    expectSuccess(expectations, writer.nullValue(), "the pending value can still be supplied");
    expectSuccess(expectations, writer.endObject(), "the correctly matched object closes");
    expectError(expectations, writer.booleanValue(false), CanonicalJsonWriterError::InvalidState,
                "a second root value is rejected");
    expectSuccess(expectations, writer.finish(), "the recovered document finishes");
    const auto sizeAfterFinish = writer.bytesWritten();
    expectError(expectations, writer.finish(), CanonicalJsonWriterError::InvalidState,
                "finish cannot append a second LF");
    expectError(expectations, writer.beginArray(), CanonicalJsonWriterError::InvalidState,
                "writes after finish are rejected");
    expectations.expect(writer.bytesWritten() == sizeAfterFinish && writer.isFinished(),
                        "failed post-finish operations leave bytes and state unchanged");
}

void testCapacityIsTransactional(Expectations& expectations) {
    constexpr char sentinel = '?';
    std::array<char, 5> scalarOutput{};
    scalarOutput.fill(sentinel);
    CanonicalJsonWriter scalarWriter(scalarOutput);
    const auto scalar = scalarWriter.stringValue("four");
    expectations.expect(
        !scalar && scalar.error() == CanonicalJsonWriterError::OutputCapacityExceeded &&
            scalar.requiredCapacity() == 6 && scalarWriter.bytesWritten() == 0 &&
            std::ranges::all_of(scalarOutput,
                                [](const char character) { return character == sentinel; }),
        "a capacity failure reports the exact requirement and writes no token");

    std::array<char, 15> memberOutput{};
    memberOutput.fill(sentinel);
    CanonicalJsonWriter memberWriter(memberOutput);
    expectSuccess(expectations, memberWriter.beginObject(), "the capacity object begins");
    const auto beforeMember = memberWriter.bytesWritten();
    const auto member = memberWriter.memberName("a-name-too-long");
    expectations.expect(
        !member && member.error() == CanonicalJsonWriterError::OutputCapacityExceeded &&
            memberWriter.bytesWritten() == beforeMember && memberOutput.front() == '{' &&
            std::ranges::all_of(memberOutput.begin() + 1, memberOutput.end(),
                                [](const char character) { return character == sentinel; }),
        "a member capacity failure leaves both output and object state unchanged");
    expectSuccess(expectations, memberWriter.memberName("x"),
                  "a smaller member can follow a failed member operation");
    const auto beforeValue = memberWriter.bytesWritten();
    const auto value = memberWriter.stringValue("too-long");
    expectations.expect(!value &&
                            value.error() == CanonicalJsonWriterError::OutputCapacityExceeded &&
                            memberWriter.bytesWritten() == beforeValue,
                        "a value capacity failure preserves the pending-member state");
    expectSuccess(expectations, memberWriter.nullValue(),
                  "a smaller value can complete the pending member");
    expectSuccess(expectations, memberWriter.endObject(), "the capacity object closes");
    const auto beforeFinalLf = memberWriter.bytesWritten();
    const auto finish = memberWriter.finish();
    expectations.expect(!finish &&
                            finish.error() == CanonicalJsonWriterError::OutputCapacityExceeded &&
                            finish.requiredCapacity() == beforeFinalLf + 1 &&
                            memberWriter.bytesWritten() == beforeFinalLf,
                        "a missing final-LF byte is reported without changing output");
}

void testUtf8FailureIsTransactional(Expectations& expectations) {
    constexpr std::string_view invalid{"\xED\xA0\x80", 3};
    std::array<char, 128> output{};
    output.fill('?');
    CanonicalJsonWriter writer(output);
    expectSuccess(expectations, writer.beginObject(), "the UTF-8 object begins");
    const auto beforeName = writer.bytesWritten();
    expectError(expectations, writer.memberName(invalid), CanonicalJsonWriterError::InvalidUtf8,
                "an invalid UTF-8 member name is rejected");
    expectations.expect(writer.bytesWritten() == beforeName && output[beforeName] == '?',
                        "an invalid member token changes no output or state");
    expectSuccess(expectations, writer.memberName("valid"), "a valid member follows");
    const auto beforeValue = writer.bytesWritten();
    expectError(expectations, writer.stringValue(invalid), CanonicalJsonWriterError::InvalidUtf8,
                "an invalid UTF-8 value is rejected");
    expectations.expect(writer.bytesWritten() == beforeValue && output[beforeValue] == '?',
                        "an invalid value token changes no output or pending-member state");
    expectSuccess(expectations, writer.stringValue("ok"), "a valid retry succeeds");
    expectSuccess(expectations, writer.endObject(), "the UTF-8 object closes");
    expectSuccess(expectations, writer.finish(), "the UTF-8 object finishes");
}

void testResourceLimits(Expectations& expectations) {
    std::array<char, 256> depthOutput{};
    CanonicalJsonWriter depthWriter(
        depthOutput, CanonicalJsonWriterLimits{
                         .maximumDepth = 2, .maximumValues = 10, .maximumContainerEntries = 10});
    expectSuccess(expectations, depthWriter.beginArray(), "depth-one root begins");
    expectSuccess(expectations, depthWriter.beginArray(), "depth-two child begins");
    const auto depthBytes = depthWriter.bytesWritten();
    expectError(expectations, depthWriter.nullValue(), CanonicalJsonWriterError::DepthLimitExceeded,
                "a scalar beyond the configured nesting depth is rejected");
    expectError(expectations, depthWriter.beginObject(),
                CanonicalJsonWriterError::DepthLimitExceeded,
                "a container beyond the configured nesting depth is rejected");
    expectations.expect(depthWriter.bytesWritten() == depthBytes,
                        "depth failures leave output unchanged");
    expectSuccess(expectations, depthWriter.endArray(), "the depth-two empty child closes");
    expectSuccess(expectations, depthWriter.endArray(), "the depth-one root closes");
    expectSuccess(expectations, depthWriter.finish(), "the depth-limited document finishes");

    std::array<char, 256> valueOutput{};
    CanonicalJsonWriter valueWriter(
        valueOutput, CanonicalJsonWriterLimits{
                         .maximumDepth = 4, .maximumValues = 2, .maximumContainerEntries = 4});
    expectSuccess(expectations, valueWriter.beginArray(), "the value-limited root counts once");
    expectSuccess(expectations, valueWriter.nullValue(), "its first child reaches the value limit");
    const auto valueBytes = valueWriter.bytesWritten();
    expectError(expectations, valueWriter.booleanValue(true),
                CanonicalJsonWriterError::ValueLimitExceeded,
                "another child exceeds the total value limit");
    expectations.expect(valueWriter.bytesWritten() == valueBytes,
                        "a value-count failure leaves output unchanged");
    expectSuccess(expectations, valueWriter.endArray(), "the value-limited array closes");
    expectSuccess(expectations, valueWriter.finish(), "the value-limited document finishes");

    std::array<char, 256> containerOutput{};
    CanonicalJsonWriter containerWriter(
        containerOutput, CanonicalJsonWriterLimits{
                             .maximumDepth = 4, .maximumValues = 4, .maximumContainerEntries = 1});
    expectSuccess(expectations, containerWriter.beginObject(), "the entry-limited object begins");
    expectSuccess(expectations, containerWriter.memberName("one"), "its sole member begins");
    expectSuccess(expectations, containerWriter.nullValue(), "its sole member value is written");
    const auto containerBytes = containerWriter.bytesWritten();
    expectError(expectations, containerWriter.memberName("two"),
                CanonicalJsonWriterError::ContainerLimitExceeded,
                "another member exceeds the per-container limit");
    expectations.expect(containerWriter.bytesWritten() == containerBytes,
                        "a container-count failure leaves output unchanged");
    expectSuccess(expectations, containerWriter.endObject(), "the entry-limited object closes");
    expectSuccess(expectations, containerWriter.finish(), "the entry-limited document finishes");

    std::array<char, 256> arrayOutput{};
    CanonicalJsonWriter arrayWriter(
        arrayOutput, CanonicalJsonWriterLimits{
                         .maximumDepth = 4, .maximumValues = 4, .maximumContainerEntries = 1});
    expectSuccess(expectations, arrayWriter.beginArray(), "the entry-limited array begins");
    expectSuccess(expectations, arrayWriter.nullValue(), "its sole element is written");
    expectError(expectations, arrayWriter.nullValue(),
                CanonicalJsonWriterError::ContainerLimitExceeded,
                "another array element exceeds the per-container limit");
    expectSuccess(expectations, arrayWriter.endArray(), "the entry-limited array closes");
    expectSuccess(expectations, arrayWriter.finish(), "the entry-limited array finishes");
}

void testFixedDepthBoundary(Expectations& expectations) {
    std::array<char, 33'000> output{};
    CanonicalJsonWriter writer(output);
    bool openedMaximum = true;
    for (std::size_t depth = 0; depth < bloom::project::kCanonicalJsonMaximumDepth; ++depth) {
        openedMaximum = openedMaximum && static_cast<bool>(writer.beginArray());
    }
    expectations.expect(openedMaximum, "the fixed stack accepts exactly 128 nested containers");
    const auto bytesAtMaximum = writer.bytesWritten();
    expectError(expectations, writer.beginArray(), CanonicalJsonWriterError::DepthLimitExceeded,
                "the fixed stack rejects container depth 129");
    expectations.expect(writer.bytesWritten() == bytesAtMaximum,
                        "the hard depth failure leaves output and stack unchanged");
    bool closedMaximum = true;
    for (std::size_t depth = 0; depth < bloom::project::kCanonicalJsonMaximumDepth; ++depth) {
        closedMaximum = closedMaximum && static_cast<bool>(writer.endArray());
    }
    expectations.expect(closedMaximum, "all 128 containers remain closable after the failure");
    expectSuccess(expectations, writer.finish(), "the hard-depth boundary document finishes");
}

void testInvalidLimits(Expectations& expectations) {
    {
        std::array<char, 32> output{};
        CanonicalJsonWriter writer(
            output,
            CanonicalJsonWriterLimits{
                .maximumDepth = bloom::project::kCanonicalJsonMaximumDepth + 1,
                .maximumValues = bloom::project::kCanonicalJsonMaximumValues,
                .maximumContainerEntries = bloom::project::kCanonicalJsonMaximumContainerEntries});
        expectError(expectations, writer.nullValue(), CanonicalJsonWriterError::InvalidLimits,
                    "a caller cannot raise the fixed implementation depth ceiling");
        expectations.expect(writer.bytesWritten() == 0,
                            "an invalid depth limit cannot produce partial output");
    }
    {
        std::array<char, 32> output{};
        CanonicalJsonWriter writer(
            output,
            CanonicalJsonWriterLimits{
                .maximumDepth = bloom::project::kCanonicalJsonMaximumDepth,
                .maximumValues = bloom::project::kCanonicalJsonMaximumValues + 1,
                .maximumContainerEntries = bloom::project::kCanonicalJsonMaximumContainerEntries});
        expectError(expectations, writer.nullValue(), CanonicalJsonWriterError::InvalidLimits,
                    "a caller cannot raise the fixed total-value ceiling");
    }
    {
        std::array<char, 32> output{};
        CanonicalJsonWriter writer(
            output, CanonicalJsonWriterLimits{
                        .maximumDepth = bloom::project::kCanonicalJsonMaximumDepth,
                        .maximumValues = bloom::project::kCanonicalJsonMaximumValues,
                        .maximumContainerEntries =
                            bloom::project::kCanonicalJsonMaximumContainerEntries + 1});
        expectError(expectations, writer.nullValue(), CanonicalJsonWriterError::InvalidLimits,
                    "a caller cannot raise the fixed per-container ceiling");
    }
}

} // namespace

int main() {
    Expectations expectations;
    testGoldenLayout(expectations);
    testEmptyAndRootValues(expectations);
    testGrammarAndRecovery(expectations);
    testCapacityIsTransactional(expectations);
    testUtf8FailureIsTransactional(expectations);
    testResourceLimits(expectations);
    testFixedDepthBoundary(expectations);
    testInvalidLimits(expectations);
    testFloat64FailuresAreTransactional(expectations);
    return expectations.failures() == 0 ? 0 : 1;
}
