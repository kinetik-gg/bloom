#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <type_traits>

namespace bloom::project {

class UnknownJsonNumber;

inline constexpr std::size_t kCanonicalJsonMaximumDepth = 128;
inline constexpr std::size_t kCanonicalJsonMaximumValues = 4'000'000;
inline constexpr std::size_t kCanonicalJsonMaximumContainerEntries = 1'000'000;

struct CanonicalJsonWriterLimits final {
    std::size_t maximumDepth = kCanonicalJsonMaximumDepth;
    std::size_t maximumValues = kCanonicalJsonMaximumValues;
    std::size_t maximumContainerEntries = kCanonicalJsonMaximumContainerEntries;
};

enum class CanonicalJsonWriterError : std::uint8_t {
    None,
    InvalidState,
    InvalidUtf8,
    SizeOverflow,
    OutputCapacityExceeded,
    DepthLimitExceeded,
    ValueLimitExceeded,
    ContainerLimitExceeded,
    InvalidLimits,
    NonFiniteNumber,
};

class [[nodiscard]] CanonicalJsonWriterResult final {
  public:
    [[nodiscard]] static constexpr CanonicalJsonWriterResult success() noexcept {
        return CanonicalJsonWriterResult(CanonicalJsonWriterError::None, std::nullopt);
    }

    [[nodiscard]] static constexpr CanonicalJsonWriterResult
    failure(const CanonicalJsonWriterError error,
            const std::optional<std::size_t> requiredCapacity = std::nullopt) noexcept {
        return CanonicalJsonWriterResult(error, requiredCapacity);
    }

    [[nodiscard]] constexpr bool succeeded() const noexcept {
        return error_ == CanonicalJsonWriterError::None;
    }
    [[nodiscard]] explicit constexpr operator bool() const noexcept { return succeeded(); }
    [[nodiscard]] constexpr CanonicalJsonWriterError error() const noexcept { return error_; }
    [[nodiscard]] constexpr std::optional<std::size_t> requiredCapacity() const noexcept {
        return requiredCapacity_;
    }

  private:
    constexpr CanonicalJsonWriterResult(const CanonicalJsonWriterError error,
                                        const std::optional<std::size_t> requiredCapacity) noexcept
        : requiredCapacity_(requiredCapacity), error_(error) {}

    std::optional<std::size_t> requiredCapacity_;
    CanonicalJsonWriterError error_ = CanonicalJsonWriterError::None;
};

// Writes or counts one complete canonical JSON value with the same fixed state machine. Both modes
// use a fixed depth stack and never allocate. Destination capacity is intentionally a write-only
// concern; all grammar, token, resource-limit, and size-overflow validation is shared. Known-schema
// object ordering remains the caller's responsibility: memberName() preserves call order exactly
// and performs no sorting.
class CanonicalJsonWriter final {
  public:
    explicit CanonicalJsonWriter(std::span<char> output, CanonicalJsonWriterLimits limits =
                                                             CanonicalJsonWriterLimits{}) noexcept;

    // Count-only mode validates the same operation sequence and tokens as write mode while touching
    // no destination or applying a destination-capacity limit. After a successful finish(),
    // bytesRequired() is the exact span size for a second pass.
    [[nodiscard]] static CanonicalJsonWriter
    counting(CanonicalJsonWriterLimits limits = CanonicalJsonWriterLimits{}) noexcept;

    CanonicalJsonWriter(const CanonicalJsonWriter&) = delete;
    CanonicalJsonWriter& operator=(const CanonicalJsonWriter&) = delete;
    CanonicalJsonWriter(CanonicalJsonWriter&&) = delete;
    CanonicalJsonWriter& operator=(CanonicalJsonWriter&&) = delete;

    [[nodiscard]] CanonicalJsonWriterResult beginObject() noexcept;
    [[nodiscard]] CanonicalJsonWriterResult endObject() noexcept;
    [[nodiscard]] CanonicalJsonWriterResult beginArray() noexcept;
    [[nodiscard]] CanonicalJsonWriterResult endArray() noexcept;

    [[nodiscard]] CanonicalJsonWriterResult memberName(std::string_view name) noexcept;
    [[nodiscard]] CanonicalJsonWriterResult stringValue(std::string_view value) noexcept;
    [[nodiscard]] CanonicalJsonWriterResult booleanValue(bool value) noexcept;
    [[nodiscard]] CanonicalJsonWriterResult nullValue() noexcept;
    // Schema integers remain unsigned 32-bit JSON numbers. Semantic 64-bit integers and IDs are
    // emitted by callers as canonical decimal strings.
    [[nodiscard]] CanonicalJsonWriterResult integerValue(std::uint32_t value) noexcept;
    // Float64 values use Bloom's typed RFC 8785/ECMAScript-derived spelling. NaN and infinities are
    // rejected without changing the output or writer state.
    [[nodiscard]] CanonicalJsonWriterResult float64Value(double value) noexcept;
    // Unknown additive members use their closed lossless numeric subset. This deliberately avoids
    // exposing a raw-token writer that could bypass canonical JSON and owning-schema rules.
    [[nodiscard]] CanonicalJsonWriterResult
    unknownNumberValue(const UnknownJsonNumber& value) noexcept;

    // Completes the document with exactly one LF. A second call or any later write is invalid.
    [[nodiscard]] CanonicalJsonWriterResult finish() noexcept;

    [[nodiscard]] bool isCounting() const noexcept { return mode_ == Mode::Count; }
    // Before a successful finish this is the logical byte count accepted so far, not a complete
    // document size. In write mode it equals bytesWritten(). If a write operation fails for
    // capacity, the space that operation needed is result.requiredCapacity().
    [[nodiscard]] std::size_t bytesRequired() const noexcept { return offset_; }
    [[nodiscard]] std::size_t bytesWritten() const noexcept { return isCounting() ? 0 : offset_; }
    [[nodiscard]] std::span<const char> written() const noexcept {
        return isCounting() ? std::span<const char>{}
                            : std::span<const char>{output_.data(), offset_};
    }
    [[nodiscard]] bool isFinished() const noexcept { return finished_; }

  private:
    enum class Mode : std::uint8_t {
        Write,
        Count,
    };

    struct CountOnlyTag final {};

    explicit CanonicalJsonWriter(CountOnlyTag, CanonicalJsonWriterLimits limits) noexcept;

    enum class ContainerKind : std::uint8_t {
        Object,
        Array,
    };

    struct Frame final {
        std::size_t entryCount = 0;
        ContainerKind kind = ContainerKind::Object;
        bool awaitingValue = false;
    };

    struct ValuePrefix final {
        std::size_t size = 0;
    };

    [[nodiscard]] CanonicalJsonWriterResult validateLimits() const noexcept;
    [[nodiscard]] CanonicalJsonWriterResult prepareValue(ValuePrefix& prefix) const noexcept;
    [[nodiscard]] CanonicalJsonWriterResult
    ensureAdditionalCapacity(std::size_t additionalBytes) const noexcept;
    [[nodiscard]] CanonicalJsonWriterResult writeContainerStart(ContainerKind kind,
                                                                char opening) noexcept;
    [[nodiscard]] CanonicalJsonWriterResult writeContainerEnd(ContainerKind kind,
                                                              char closing) noexcept;
    [[nodiscard]] CanonicalJsonWriterResult writeToken(std::string_view token) noexcept;
    void writeValuePrefix(const ValuePrefix& prefix) noexcept;
    void completeValue() noexcept;

    std::span<char> output_;
    CanonicalJsonWriterLimits limits_;
    std::array<Frame, kCanonicalJsonMaximumDepth> frames_{};
    std::size_t offset_ = 0;
    std::size_t depth_ = 0;
    std::size_t valueCount_ = 0;
    bool rootWritten_ = false;
    bool finished_ = false;
    Mode mode_ = Mode::Write;
};

static_assert(std::is_trivially_copyable_v<CanonicalJsonWriterLimits>);
static_assert(std::is_trivially_copyable_v<CanonicalJsonWriterResult>);

} // namespace bloom::project
