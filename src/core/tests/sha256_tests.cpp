#include <bloom/core/sha256.hpp>

#include <array>
#include <cstddef>
#include <iostream>
#include <span>
#include <string>
#include <string_view>
#include <utility>

namespace {

template <typename Value>
concept HasRvalueDigestBytes = requires(Value&& value) { std::move(value).bytes(); };

static_assert(!HasRvalueDigestBytes<bloom::core::Sha256Digest>);
static_assert(requires(const bloom::core::Sha256Digest& digest) { digest.bytes(); });

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

[[nodiscard]] std::span<const std::byte> asBytes(const std::string_view text) noexcept {
    return std::as_bytes(std::span(text.data(), text.size()));
}

[[nodiscard]] bool hasDigest(const std::optional<bloom::core::Sha256Digest>& digest,
                             const std::string_view expected) {
    if (!digest.has_value()) {
        return false;
    }
    const auto hex = digest->toLowercaseHex();
    return std::string_view(hex.data(), hex.size()) == expected;
}

[[nodiscard]] bool hasDigest(const bloom::core::Sha256Digest& digest,
                             const std::string_view expected) {
    const auto hex = digest.toLowercaseHex();
    return std::string_view(hex.data(), hex.size()) == expected;
}

void testStandardVectors(Expectations& expectations) {
    expectations.expect(
        hasDigest(bloom::core::Sha256Hasher::hash(asBytes("")),
                  "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"),
        "the empty SHA-256 vector matches");
    expectations.expect(
        hasDigest(bloom::core::Sha256Hasher::hash(asBytes("abc")),
                  "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"),
        "the short SHA-256 vector matches");
    expectations.expect(
        hasDigest(bloom::core::Sha256Hasher::hash(
                      asBytes("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq")),
                  "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1"),
        "the multi-block SHA-256 vector matches");
}

void testStreamingAndRepeatableFinalization(Expectations& expectations) {
    bloom::core::Sha256Hasher hasher;
    expectations.expect(hasher.update(asBytes("a")) && hasher.update(asBytes("b")) &&
                            hasher.update(asBytes("c")),
                        "streamed updates are accepted");
    const auto first = hasher.finalize();
    const auto second = hasher.finalize();
    expectations.expect(
        hasDigest(first, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad") &&
            first == second,
        "finalization is repeatable and does not mutate streaming state");

    expectations.expect(hasher.update(asBytes("d")), "a finalized hasher may continue streaming");
    expectations.expect(
        hasDigest(hasher.finalize(),
                  "88d4266fd4e6338d13b845fcf289579d209c897823b9217da3e161936f031589"),
        "continued streaming produces the extended message digest");
}

void testBlockBoundaries(Expectations& expectations) {
    const std::array<std::size_t, 8> lengths{55, 56, 63, 64, 65, 119, 120, 128};
    const std::array<std::string_view, 8> expected{
        "9f4390f8d30c2dd92ec9f095b65e2b9ae9b0a925a5258e241c9f1e910f734318",
        "b35439a4ac6f0948b6d6f9e3c6af0f5f590ce20f1bde7090ef7970686ec6738a",
        "7d3e74a05d7db15bce4ad9ec0658ea98e3f06eeecf16b4c6fff2da457ddc2f34",
        "ffe054fe7ae0cb6dc65c3af9b61d5209f439851db43d0ba5997337df154668eb",
        "635361c48bb9eab14198e76ea8ab7f1a41685d6ad62aa9146d301d4f17eb0ae0",
        "31eba51c313a5c08226adf18d4a359cfdfd8d2e816b13f4af952f7ea6584dcfb",
        "2f3d335432c70b580af0e8e1b3674a7c020d683aa5f73aaaedfdc55af904c21c",
        "6836cf13bac400e9105071cd6af47084dfacad4e5e302c94bfed24e013afb73e",
    };
    for (std::size_t index = 0; index < lengths.size(); ++index) {
        const std::string message(lengths[index], 'a');
        expectations.expect(
            hasDigest(bloom::core::Sha256Hasher::hash(asBytes(message)), expected[index]),
            "padding and compression block boundaries match");
    }
}

void testMillionByteStreamingVector(Expectations& expectations) {
    bloom::core::Sha256Hasher hasher;
    const std::string chunk(1000, 'a');
    bool accepted = true;
    for (std::size_t index = 0; index < 1000; ++index) {
        accepted = hasher.update(asBytes(chunk)) && accepted;
    }
    expectations.expect(
        accepted && hasDigest(hasher.finalize(),
                              "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0"),
        "the million-byte streamed SHA-256 vector matches");
}

void testCanonicalHexCodec(Expectations& expectations) {
    constexpr std::string_view valid =
        "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad";
    const auto decoded = bloom::core::Sha256Digest::fromLowercaseHex(valid);
    expectations.expect(decoded.has_value(), "canonical lowercase digest text decodes");
    if (decoded.has_value()) {
        const auto encoded = decoded->toLowercaseHex();
        expectations.expect(std::string_view(encoded.data(), encoded.size()) == valid,
                            "digest text round-trips exactly");
    }
    expectations.expect(!bloom::core::Sha256Digest::fromLowercaseHex(valid.substr(1)).has_value(),
                        "a short digest is rejected");
    expectations.expect(!bloom::core::Sha256Digest::fromLowercaseHex(
                             "BA7816BF8F01CFEA414140DE5DAE2223B00361A396177A9CB410FF61F20015AD")
                             .has_value(),
                        "uppercase digest text is non-canonical");
    expectations.expect(!bloom::core::Sha256Digest::fromLowercaseHex(
                             "ga7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad")
                             .has_value(),
                        "non-hexadecimal digest text is rejected");
}

} // namespace

int main() {
    Expectations expectations;
    testStandardVectors(expectations);
    testStreamingAndRepeatableFinalization(expectations);
    testBlockBoundaries(expectations);
    testMillionByteStreamingVector(expectations);
    testCanonicalHexCodec(expectations);
    return expectations.failures() == 0 ? 0 : 1;
}
