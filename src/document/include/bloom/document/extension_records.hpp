#pragma once

#include <bloom/document/ids.hpp>
#include <bloom/document/schema_version.hpp>
#include <bloom/document/validation.hpp>

#include <algorithm>
#include <cstddef>
#include <initializer_list>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <variant>
#include <vector>

namespace bloom::document {

class Project;

inline constexpr std::size_t kMaxOpaqueExtensionPayloadBytes = 67'108'864;
inline constexpr std::size_t kMaxAggregateOpaqueExtensionPayloadBytes = 134'217'728;

class OpaqueExtensionPayload final {
  public:
    OpaqueExtensionPayload() noexcept = default;
    explicit OpaqueExtensionPayload(std::vector<std::byte> bytes)
        : storage_(std::make_shared<const std::vector<std::byte>>(std::move(bytes))) {}
    OpaqueExtensionPayload(std::initializer_list<std::byte> bytes)
        : OpaqueExtensionPayload(std::vector<std::byte>(bytes)) {}

    [[nodiscard]] std::span<const std::byte> bytes() const noexcept {
        return storage_ == nullptr ? std::span<const std::byte>{} : *storage_;
    }
    [[nodiscard]] std::size_t size() const noexcept {
        return storage_ == nullptr ? 0 : storage_->size();
    }
    [[nodiscard]] bool empty() const noexcept { return size() == 0; }

    friend bool operator==(const OpaqueExtensionPayload& left,
                           const OpaqueExtensionPayload& right) noexcept {
        return left.storage_ == right.storage_ || std::ranges::equal(left.bytes(), right.bytes());
    }

  private:
    std::shared_ptr<const std::vector<std::byte>> storage_;
};

using ExtensionTarget = std::variant<ProjectId, CompositionId, NodeId, EdgeId, LayerId, LayerSlotId,
                                     ParameterId, AnimationCurveId, KeyframeId>;

struct ExtensionHostReference final {
    std::string key;
    ExtensionTarget target;

    friend bool operator==(const ExtensionHostReference&, const ExtensionHostReference&) = default;
};

struct NoExtensionReferences final {
    friend constexpr bool operator==(const NoExtensionReferences&,
                                     const NoExtensionReferences&) noexcept = default;
};

struct ExtensionHostReferenceTable final {
    std::vector<ExtensionHostReference> references;

    friend bool operator==(const ExtensionHostReferenceTable&,
                           const ExtensionHostReferenceTable&) = default;
};

struct ExtensionOwnerRemapper final {
    std::string remapperId;
    SchemaVersion version;

    friend bool operator==(const ExtensionOwnerRemapper&, const ExtensionOwnerRemapper&) = default;
};

using ExtensionReferencePolicy =
    std::variant<NoExtensionReferences, ExtensionHostReferenceTable, ExtensionOwnerRemapper>;

struct ExtensionRecord final {
    ExtensionRecordId id;
    std::string ownerId;
    std::string typeId;
    SchemaVersion schemaVersion;
    std::optional<ExtensionTarget> subject;
    std::string mediaType;
    ExtensionReferencePolicy referencePolicy;
    OpaqueExtensionPayload payload;

    friend bool operator==(const ExtensionRecord&, const ExtensionRecord&) = default;
};

[[nodiscard]] ValidationResult validateExtensionRecords(const Project& project);

} // namespace bloom::document
