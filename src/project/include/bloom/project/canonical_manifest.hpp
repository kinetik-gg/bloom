#pragma once

#include <bloom/document/schema_version.hpp>
#include <bloom/project/canonical_json_writer.hpp>
#include <bloom/project/manifest_requirements.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <type_traits>

namespace bloom::project {

inline constexpr std::string_view kCanonicalManifestFormat = "org.kinetik.bloom.project";
inline constexpr std::string_view kCanonicalManifestDocumentPath = "document.json";
inline constexpr document::SchemaVersion kCanonicalManifestContainerVersionV1{1, 0};
inline constexpr document::SchemaVersion kCanonicalManifestDocumentSchemaVersionV1{1, 0};
inline constexpr std::size_t kCanonicalManifestMaximumBytes = 1U << 20U;
inline constexpr std::size_t kCanonicalManifestNoIndex = static_cast<std::size_t>(-1);

struct CanonicalManifestV1 final {
    std::string_view format = kCanonicalManifestFormat;
    document::SchemaVersion containerVersion = kCanonicalManifestContainerVersionV1;
    std::string_view documentPath = kCanonicalManifestDocumentPath;
    document::SchemaVersion documentSchemaVersion = kCanonicalManifestDocumentSchemaVersionV1;
    std::span<const ManifestRequirement> requirements{};
};

struct CanonicalManifestLimits final {
    std::size_t maximumRequirements = kMaxManifestRequirementCount;
    std::size_t maximumProvidedNodeTypes = kMaxProvidedNodeTypeCount;
    std::size_t maximumValues = kCanonicalJsonMaximumValues;
    std::size_t maximumOutputBytes = kCanonicalManifestMaximumBytes;
};

enum class CanonicalManifestError : std::uint8_t {
    None,
    InvalidLimits,
    InvalidFormat,
    InvalidContainerVersion,
    InvalidDocumentPath,
    InvalidDocumentSchemaVersion,
    RequirementCountExceeded,
    InvalidProviderId,
    InvalidCapabilityId,
    InvalidRequirementSchemaVersion,
    DuplicateRequirementIdentity,
    InvalidRequirementOrder,
    ProvidedNodeTypeCountExceeded,
    InvalidProvidedNodeTypeId,
    DuplicateProvidedNodeTypeId,
    InvalidProvidedNodeTypeOrder,
    ValueCountExceeded,
    SizeOverflow,
    ManifestSizeExceeded,
    OutputCapacityExceeded,
};

class [[nodiscard]] CanonicalManifestSizeResult final {
  public:
    [[nodiscard]] static constexpr CanonicalManifestSizeResult
    success(const std::size_t size) noexcept {
        return CanonicalManifestSizeResult(size);
    }
    [[nodiscard]] static constexpr CanonicalManifestSizeResult
    failure(const CanonicalManifestError error,
            const std::size_t requirementIndex = kCanonicalManifestNoIndex,
            const std::size_t nodeTypeIndex = kCanonicalManifestNoIndex) noexcept {
        return CanonicalManifestSizeResult(error, requirementIndex, nodeTypeIndex);
    }

    [[nodiscard]] constexpr bool hasValue() const noexcept { return size_.has_value(); }
    [[nodiscard]] explicit constexpr operator bool() const noexcept { return hasValue(); }
    [[nodiscard]] constexpr const std::size_t* value() const& noexcept {
        return size_.has_value() ? &*size_ : nullptr;
    }
    [[nodiscard]] constexpr const std::size_t* value() const&& = delete;
    [[nodiscard]] constexpr CanonicalManifestError error() const noexcept { return error_; }
    [[nodiscard]] constexpr std::size_t requirementIndex() const noexcept {
        return requirementIndex_;
    }
    [[nodiscard]] constexpr std::size_t nodeTypeIndex() const noexcept { return nodeTypeIndex_; }

  private:
    constexpr explicit CanonicalManifestSizeResult(const std::size_t size) noexcept : size_(size) {}
    constexpr CanonicalManifestSizeResult(const CanonicalManifestError error,
                                          const std::size_t requirementIndex,
                                          const std::size_t nodeTypeIndex) noexcept
        : error_(error), requirementIndex_(requirementIndex), nodeTypeIndex_(nodeTypeIndex) {}

    std::optional<std::size_t> size_;
    CanonicalManifestError error_ = CanonicalManifestError::None;
    std::size_t requirementIndex_ = kCanonicalManifestNoIndex;
    std::size_t nodeTypeIndex_ = kCanonicalManifestNoIndex;
};

class [[nodiscard]] CanonicalManifestWriteResult final {
  public:
    [[nodiscard]] static constexpr CanonicalManifestWriteResult
    success(const std::size_t bytesWritten) noexcept {
        return CanonicalManifestWriteResult(CanonicalManifestError::None, bytesWritten,
                                            bytesWritten, kCanonicalManifestNoIndex,
                                            kCanonicalManifestNoIndex);
    }
    [[nodiscard]] static constexpr CanonicalManifestWriteResult
    failure(const CanonicalManifestError error,
            const std::optional<std::size_t> requiredSize = std::nullopt,
            const std::size_t requirementIndex = kCanonicalManifestNoIndex,
            const std::size_t nodeTypeIndex = kCanonicalManifestNoIndex) noexcept {
        return CanonicalManifestWriteResult(error, requiredSize, 0, requirementIndex,
                                            nodeTypeIndex);
    }

    [[nodiscard]] constexpr bool succeeded() const noexcept {
        return error_ == CanonicalManifestError::None;
    }
    [[nodiscard]] explicit constexpr operator bool() const noexcept { return succeeded(); }
    [[nodiscard]] constexpr CanonicalManifestError error() const noexcept { return error_; }
    [[nodiscard]] constexpr std::optional<std::size_t> requiredSize() const noexcept {
        return requiredSize_;
    }
    [[nodiscard]] constexpr std::size_t bytesWritten() const noexcept { return bytesWritten_; }
    [[nodiscard]] constexpr std::size_t requirementIndex() const noexcept {
        return requirementIndex_;
    }
    [[nodiscard]] constexpr std::size_t nodeTypeIndex() const noexcept { return nodeTypeIndex_; }

  private:
    constexpr CanonicalManifestWriteResult(const CanonicalManifestError error,
                                           const std::optional<std::size_t> requiredSize,
                                           const std::size_t bytesWritten,
                                           const std::size_t requirementIndex,
                                           const std::size_t nodeTypeIndex) noexcept
        : requiredSize_(requiredSize), bytesWritten_(bytesWritten), error_(error),
          requirementIndex_(requirementIndex), nodeTypeIndex_(nodeTypeIndex) {}

    std::optional<std::size_t> requiredSize_;
    std::size_t bytesWritten_ = 0;
    CanonicalManifestError error_ = CanonicalManifestError::None;
    std::size_t requirementIndex_ = kCanonicalManifestNoIndex;
    std::size_t nodeTypeIndex_ = kCanonicalManifestNoIndex;
};

// Validates the context-free v1 manifest shape, lexical forms, limits, and canonical ordering.
// Exact requirement coverage against document truth remains the caller's responsibility through
// validateManifestRequirements().
[[nodiscard]] CanonicalManifestSizeResult
canonicalManifestSize(const CanonicalManifestV1& manifest,
                      CanonicalManifestLimits limits = CanonicalManifestLimits{}) noexcept;

// Validation and exact sizing complete before output is touched. Capacity and validation failures
// report zero bytes written and leave the destination unchanged. Extra destination capacity is
// allowed and remains untouched.
[[nodiscard]] CanonicalManifestWriteResult
encodeCanonicalManifest(const CanonicalManifestV1& manifest, std::span<char> output,
                        CanonicalManifestLimits limits = CanonicalManifestLimits{}) noexcept;

static_assert(std::is_trivially_copyable_v<CanonicalManifestV1>);
static_assert(std::is_trivially_copyable_v<CanonicalManifestLimits>);
static_assert(std::is_trivially_copyable_v<CanonicalManifestSizeResult>);
static_assert(std::is_trivially_copyable_v<CanonicalManifestWriteResult>);

} // namespace bloom::project
