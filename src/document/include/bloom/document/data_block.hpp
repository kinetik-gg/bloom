#pragma once

#include <bloom/core/color.hpp>
#include <bloom/core/sha256.hpp>
#include <bloom/document/asset.hpp>
#include <bloom/document/extension_records.hpp>
#include <bloom/document/parameter.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace bloom::document {

// A DataBlockId is deliberately a tagged union. Existing assets and extension records keep their
// historic allocator namespaces and storage, while authored DATA blocks receive their own stable
// ids. The three alternatives are never interchangeable even when their numeric values match.
using DataBlockId = std::variant<AssetId, ExtensionRecordId, DataBlockRecordId>;

enum class DataBlockKind : std::uint8_t {
    Image,
    Sequence,
    Video,
    Audio,
    Font,
    Curve,
    Ramp,
    Table,
    PointSet,
    Path,
    Mask,
    Analysis,
    Opaque,
};

struct DataBlockProducer final {
    std::string name;
    std::string version;
    core::Sha256Digest parametersDigest;

    friend bool operator==(const DataBlockProducer&, const DataBlockProducer&) = default;
};

struct DataBlockProvenance final {
    // Asset locators are also the source-locator form used by imported media-backed blocks.
    std::variant<AssetLocator, DataBlockProducer> source;
    std::string createdAt;
    core::Sha256Digest contentDigest;

    friend bool operator==(const DataBlockProvenance&, const DataBlockProvenance&) = default;
};

struct DataBlockCurve final {
    double domainStart = 0.0;
    double domainEnd = 1.0;
    std::vector<double> samples;

    friend bool operator==(const DataBlockCurve&, const DataBlockCurve&) = default;
};

struct DataBlockRampStop final {
    double position = 0.0;
    core::Color4d color;

    friend bool operator==(const DataBlockRampStop&, const DataBlockRampStop&) = default;
};

struct DataBlockRamp final {
    std::vector<DataBlockRampStop> stops;

    friend bool operator==(const DataBlockRamp&, const DataBlockRamp&) = default;
};

enum class DataBlockValueKind : std::uint8_t {
    Boolean,
    Integer,
    Float64,
    Vec2,
    Vec3,
    Color4,
    String,
    Rational,
    Path,
};

struct DataBlockTableColumn final {
    std::string name;
    DataBlockValueKind valueKind = DataBlockValueKind::Float64;
    std::vector<ParameterValue> values;

    friend bool operator==(const DataBlockTableColumn&, const DataBlockTableColumn&) = default;
};

struct DataBlockTable final {
    std::vector<DataBlockTableColumn> columns;

    friend bool operator==(const DataBlockTable&, const DataBlockTable&) = default;
};

struct DataBlockPointSet final {
    std::uint8_t dimensions = 2;
    std::vector<Vec3d> points;
    std::vector<DataBlockTableColumn> attributes;

    friend bool operator==(const DataBlockPointSet&, const DataBlockPointSet&) = default;
};

struct DataBlockMask final {
    AssetId coverageAsset;

    friend bool operator==(const DataBlockMask&, const DataBlockMask&) = default;
};

using DataBlockPayload =
    std::variant<DataBlockCurve, DataBlockRamp, DataBlockTable, DataBlockPointSet, PathValue,
                 DataBlockMask, OpaqueExtensionPayload>;

struct DataBlockRecord final {
    DataBlockId id;
    DataBlockKind kind = DataBlockKind::Opaque;
    std::optional<ExtensionTarget> owner;
    std::string typeId;
    SchemaVersion schemaVersion;
    std::string mediaType;
    DataBlockProvenance provenance;
    std::optional<ExtensionTarget> subject;
    DataBlockPayload payload = OpaqueExtensionPayload{};
    std::vector<std::string> tags;

    [[nodiscard]] ValidationResult validate(const Project* project = nullptr) const;
    [[nodiscard]] std::size_t payloadBytes() const;

    [[nodiscard]] static DataBlockRecord fromAsset(const AssetRecord& asset);
    [[nodiscard]] static DataBlockRecord fromExtension(const ExtensionRecord& record);

    friend bool operator==(const DataBlockRecord&, const DataBlockRecord&) = default;
};

[[nodiscard]] bool isMediaDataBlockKind(DataBlockKind kind) noexcept;
[[nodiscard]] std::string_view dataBlockKindName(DataBlockKind kind) noexcept;

inline constexpr std::size_t kMaxDataBlockPayloadBytes = 67'108'864;
inline constexpr std::size_t kMaxAggregateDataBlockPayloadBytes = 134'217'728;

} // namespace bloom::document
