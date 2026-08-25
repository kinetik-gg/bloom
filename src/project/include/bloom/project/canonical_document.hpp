#pragma once

#include <bloom/document/schema_version.hpp>
#include <bloom/project/canonical_json_writer.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <type_traits>

namespace bloom::document {

struct ColorSettings;
class Snapshot;

} // namespace bloom::document

namespace bloom::project {

inline constexpr document::SchemaVersion kCanonicalDocumentSchemaVersionV1{1, 0};
// The v1 expanded document.json resource limit from docs/architecture/project-format.md.
inline constexpr std::size_t kCanonicalDocumentMaximumBytes = 268'435'456;
// Deepest canonical document emission is nine containers (root through a vec2 keyframe value
// object). The budget keeps manifest-style headroom over that fixed shape while still bounding
// every write path.
inline constexpr std::size_t kCanonicalDocumentMaximumDepth = 12;
inline constexpr std::size_t kCanonicalDocumentNoIndex = static_cast<std::size_t>(-1);

// One canonical encode request. The snapshot and color settings must outlive the call; Project
// does not own color settings, so the caller supplies the complete durable value explicitly.
//
// Both scratch spans keep the writer allocation-free. payloadScratch receives one base64 spelling
// at a time and must hold 4 * ceil(n / 3) bytes for the largest opaque extension payload in the
// snapshot (zero is enough when every payload is empty). sortScratch is partitioned into three
// disjoint windows so nested index orders never overlap: window zero holds the composition order,
// window one holds one composition-scoped collection at a time (parameters, animation curves,
// graph nodes, edges, layer output boundaries) plus the extension records, and window two holds
// one nested sub-order at a time (node parameter bindings, curve keyframes). The required total is
// measured from live collection sizes; store-ordered collections (animation curves, keyframes,
// extension records) are re-sorted defensively through the same windows rather than trusted.
struct CanonicalDocumentV1 final {
    const bloom::document::Snapshot* snapshot = nullptr;
    const bloom::document::ColorSettings* colorSettings = nullptr;
    std::span<char> payloadScratch{};
    std::span<std::size_t> sortScratch{};
};

struct CanonicalDocumentLimits final {
    std::size_t maximumValues = kCanonicalJsonMaximumValues;
    std::size_t maximumContainerEntries = kCanonicalJsonMaximumContainerEntries;
    std::size_t maximumOutputBytes = kCanonicalDocumentMaximumBytes;
};

enum class CanonicalDocumentError : std::uint8_t {
    None,
    MissingInput,
    InvalidLimits,
    InvalidName,
    InvalidCollectionIdentity,
    MissingAnimationCurve,
    InvalidCompositionDuration,
    InvalidCompositionFormat,
    InvalidParameter,
    InvalidAnimationCurve,
    InvalidGraph,
    InvalidExtensionRecord,
    InvalidColorSettings,
    InvalidProcessColorSpaceId,
    OcioPortabilityMismatch,
    InvalidOcioRevisionAlgorithm,
    UnsupportedDriverBindingSource,
    SortBufferTooSmall,
    PayloadBufferTooSmall,
    ValueCountExceeded,
    ContainerEntryCountExceeded,
    DocumentSizeExceeded,
    OutputCapacityExceeded,
};

class [[nodiscard]] CanonicalDocumentSizeResult final {
  public:
    [[nodiscard]] static constexpr CanonicalDocumentSizeResult success(const std::size_t size) noexcept {
        return CanonicalDocumentSizeResult(size);
    }
    [[nodiscard]] static constexpr CanonicalDocumentSizeResult
    failure(const CanonicalDocumentError error, const std::size_t compositionIndex =
                                                     kCanonicalDocumentNoIndex,
            const std::size_t elementIndex = kCanonicalDocumentNoIndex) noexcept {
        return CanonicalDocumentSizeResult(error, compositionIndex, elementIndex);
    }

    [[nodiscard]] constexpr bool hasValue() const noexcept { return size_.has_value(); }
    [[nodiscard]] explicit constexpr operator bool() const noexcept { return hasValue(); }
    [[nodiscard]] constexpr const std::size_t* value() const& noexcept {
        return size_.has_value() ? &*size_ : nullptr;
    }
    [[nodiscard]] constexpr const std::size_t* value() const&& = delete;
    [[nodiscard]] constexpr CanonicalDocumentError error() const noexcept { return error_; }
    [[nodiscard]] constexpr std::size_t compositionIndex() const noexcept {
        return compositionIndex_;
    }
    [[nodiscard]] constexpr std::size_t elementIndex() const noexcept { return elementIndex_; }

  private:
    constexpr explicit CanonicalDocumentSizeResult(const std::size_t size) noexcept : size_(size) {}
    constexpr CanonicalDocumentSizeResult(const CanonicalDocumentError error,
                                          const std::size_t compositionIndex,
                                          const std::size_t elementIndex) noexcept
        : error_(error), compositionIndex_(compositionIndex), elementIndex_(elementIndex) {}

    std::optional<std::size_t> size_;
    CanonicalDocumentError error_ = CanonicalDocumentError::None;
    std::size_t compositionIndex_ = kCanonicalDocumentNoIndex;
    std::size_t elementIndex_ = kCanonicalDocumentNoIndex;
};

class [[nodiscard]] CanonicalDocumentWriteResult final {
  public:
    [[nodiscard]] static constexpr CanonicalDocumentWriteResult
    success(const std::size_t bytesWritten) noexcept {
        return CanonicalDocumentWriteResult(CanonicalDocumentError::None, bytesWritten, bytesWritten,
                                            kCanonicalDocumentNoIndex, kCanonicalDocumentNoIndex);
    }
    [[nodiscard]] static constexpr CanonicalDocumentWriteResult
    failure(const CanonicalDocumentError error,
            const std::optional<std::size_t> requiredSize = std::nullopt,
            const std::size_t compositionIndex = kCanonicalDocumentNoIndex,
            const std::size_t elementIndex = kCanonicalDocumentNoIndex) noexcept {
        return CanonicalDocumentWriteResult(error, requiredSize, 0, compositionIndex, elementIndex);
    }

    [[nodiscard]] constexpr bool succeeded() const noexcept {
        return error_ == CanonicalDocumentError::None;
    }
    [[nodiscard]] explicit constexpr operator bool() const noexcept { return succeeded(); }
    [[nodiscard]] constexpr CanonicalDocumentError error() const noexcept { return error_; }
    [[nodiscard]] constexpr std::optional<std::size_t> requiredSize() const noexcept {
        return requiredSize_;
    }
    [[nodiscard]] constexpr std::size_t bytesWritten() const noexcept { return bytesWritten_; }
    [[nodiscard]] constexpr std::size_t compositionIndex() const noexcept {
        return compositionIndex_;
    }
    [[nodiscard]] constexpr std::size_t elementIndex() const noexcept { return elementIndex_; }

  private:
    constexpr CanonicalDocumentWriteResult(const CanonicalDocumentError error,
                                           const std::optional<std::size_t> requiredSize,
                                           const std::size_t bytesWritten,
                                           const std::size_t compositionIndex,
                                           const std::size_t elementIndex) noexcept
        : requiredSize_(requiredSize), bytesWritten_(bytesWritten), error_(error),
          compositionIndex_(compositionIndex), elementIndex_(elementIndex) {}

    std::optional<std::size_t> requiredSize_;
    std::size_t bytesWritten_ = 0;
    CanonicalDocumentError error_ = CanonicalDocumentError::None;
    std::size_t compositionIndex_ = kCanonicalDocumentNoIndex;
    std::size_t elementIndex_ = kCanonicalDocumentNoIndex;
};

// Validates the complete v1 document shape against the format contract and returns the exact
// canonical byte count. Document-owned lexical and domain rules for color settings are delegated to
// ColorSettings::validate(); live DriverBindingSource parameters are rejected here because native
// v1 Save is a restricted supported-subset encoder. No destination byte is touched and no memory is
// allocated beyond the callers' provided scratch spans.
[[nodiscard]] CanonicalDocumentSizeResult
canonicalDocumentSize(const CanonicalDocumentV1& document,
                      CanonicalDocumentLimits limits = CanonicalDocumentLimits{}) noexcept;

// Validation and exact sizing complete before output is touched. Capacity failures report the exact
// byte requirement, zero bytes written, and leave the destination unchanged. Extra destination
// capacity is allowed and remains untouched.
[[nodiscard]] CanonicalDocumentWriteResult
encodeCanonicalDocument(const CanonicalDocumentV1& document, std::span<char> output,
                        CanonicalDocumentLimits limits = CanonicalDocumentLimits{}) noexcept;

static_assert(std::is_trivially_copyable_v<CanonicalDocumentV1>);
static_assert(std::is_trivially_copyable_v<CanonicalDocumentLimits>);
static_assert(std::is_trivially_copyable_v<CanonicalDocumentSizeResult>);
static_assert(std::is_trivially_copyable_v<CanonicalDocumentWriteResult>);

} // namespace bloom::project
