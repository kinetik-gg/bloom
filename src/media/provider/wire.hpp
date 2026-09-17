#pragma once
#include <bit>
#include <bloom/media/provider/contract.hpp>
#include <stdexcept>
#include <type_traits>

namespace bloom::media::provider::wire {
struct Failure {
    Error code;
};
inline void require(bool condition, Error code = Error::InvalidValue) {
    if (!condition)
        throw Failure{code};
}
[[nodiscard]] bool validText(std::string_view value);
class Writer {
  public:
    Bytes bytes;
    template <typename T> void number(T value) {
        using U = std::make_unsigned_t<T>;
        auto bits = static_cast<U>(value);
        for (std::size_t i = 0; i < sizeof(T); ++i) {
            bytes.push_back(static_cast<std::byte>(bits & 255U));
            bits = static_cast<U>(bits >> 8U);
        }
    }
    template <typename T> void enumeration(T value, std::uint8_t low, std::uint8_t high) {
        const auto n = static_cast<std::uint8_t>(value);
        require(n >= low && n <= high, Error::BadEnum);
        number(n);
    }
    void text(const std::string& value) {
        require(value.size() <= Limits::stringBytes, Error::Oversized);
        require(validText(value));
        number(static_cast<std::uint32_t>(value.size()));
        for (char c : value)
            bytes.push_back(static_cast<std::byte>(c));
    }
    void hash(const Digest& value) {
        for (auto b : value.bytes())
            bytes.push_back(static_cast<std::byte>(b));
    }
    void count(std::size_t size, std::uint32_t limit = Limits::entries) {
        require(size <= limit, Error::Oversized);
        number(static_cast<std::uint32_t>(size));
    }
    void rational(const Rational& value) {
        require(valid(value));
        number(value.numerator);
        number(value.denominator);
    }
};
class Reader {
  public:
    explicit Reader(std::span<const std::byte> input) : bytes(input) {}
    std::span<const std::byte> bytes;
    template <typename T> T number() {
        require(bytes.size() >= sizeof(T), Error::Truncated);
        using U = std::make_unsigned_t<T>;
        U bits = 0;
        for (std::size_t i = 0; i < sizeof(T); ++i)
            bits |=
                static_cast<U>(static_cast<U>(std::to_integer<std::uint8_t>(bytes[i])) << (8U * i));
        bytes = bytes.subspan(sizeof(T));
        return std::bit_cast<T>(bits);
    }
    template <typename T> T enumeration(std::uint8_t low, std::uint8_t high) {
        const auto n = number<std::uint8_t>();
        require(n >= low && n <= high, Error::BadEnum);
        return static_cast<T>(n);
    }
    std::uint32_t count(std::uint32_t limit = Limits::entries) {
        const auto n = number<std::uint32_t>();
        require(n <= limit, Error::Oversized);
        return n;
    }
    std::string text() {
        const auto n = count(Limits::stringBytes);
        require(n <= bytes.size(), Error::BadLength);
        std::string out;
        out.reserve(n);
        for (auto b : bytes.first(n))
            out.push_back(static_cast<char>(b));
        bytes = bytes.subspan(n);
        require(validText(out));
        return out;
    }
    Digest hash() {
        Digest::Bytes out{};
        for (auto& b : out)
            b = number<std::uint8_t>();
        return Digest::fromBytes(out);
    }
    Rational rational() {
        Rational out;
        out.numerator = number<std::int64_t>();
        out.denominator = number<std::int64_t>();
        require(valid(out));
        return out;
    }
    void end() { require(bytes.empty(), Error::BadLength); }
};
void write(Writer&, const MediaCapabilityKeyV1&);
void write(Writer&, const ProviderExecutionKeyV1&);
void write(Writer&, const QualificationEvidenceV1&);
void write(Writer&, const PipelineQualificationV1&);
void write(Writer&, const MediaQcEvidenceV1&);
MediaCapabilityKeyV1 capability(Reader&);
ProviderExecutionKeyV1 execution(Reader&);
QualificationEvidenceV1 evidence(Reader&);
} // namespace bloom::media::provider::wire
