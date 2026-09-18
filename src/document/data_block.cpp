#include <bloom/document/data_block.hpp>

#include <bloom/core/utf8.hpp>
#include <bloom/document/persisted_text.hpp>
#include <bloom/document/project.hpp>
#include <cmath>
#include <limits>
#include <ranges>
#include <type_traits>

namespace bloom::document {
namespace {

bool finite(const double value) { return std::isfinite(value); }
bool finite(const Vec2d& value) { return std::isfinite(value.x) && std::isfinite(value.y); }
bool finite(const Vec3d& value) {
    return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
}
bool finite(const core::Color4d& value) { return value.isValid(); }
bool finite(const core::RationalTime& value) { return value.denominator() != 0; }
bool finite(const PathValue& value) { return value.isValid(); }
template <typename Value> bool finite(const Value&) { return true; }

void validateParameterValue(const ParameterValue& value, const std::string& path,
                            ValidationResult& result) {
    std::visit(
        [&](const auto& item) {
            using Value = std::decay_t<decltype(item)>;
            if constexpr (!std::is_same_v<Value, bool> && !std::is_same_v<Value, std::int64_t> &&
                          !std::is_same_v<Value, std::string>) {
                if (!finite(item))
                    result.add(ValidationCode::InvalidValue, path,
                               "Data block table value is not finite or valid");
            }
        },
        value);
}

void validateTable(const std::vector<DataBlockTableColumn>& columns, const std::string& path,
                   ValidationResult& result) {
    if (columns.size() > 4096)
        result.add(ValidationCode::InvalidValue, path, "Data block has too many columns");
    std::size_t rows = 0;
    bool hasRows = false;
    std::string previous;
    for (std::size_t columnIndex = 0; columnIndex < columns.size(); ++columnIndex) {
        const auto& column = columns[columnIndex];
        const auto columnPath = path + "[" + std::to_string(columnIndex) + "]";
        validateStructuralText(column.name, columnPath + ".name", "Data block column name", result);
        if (column.name <= previous && columnIndex != 0)
            result.add(ValidationCode::InvalidOrder, columnPath + ".name",
                       "Data block columns must be unique and sorted");
        previous = column.name;
        if (!hasRows) {
            rows = column.values.size();
            hasRows = true;
        } else if (column.values.size() != rows) {
            result.add(ValidationCode::InvalidValue, columnPath + ".values",
                       "Data block table columns must have equal row counts");
        }
        if (column.values.size() > 1'000'000)
            result.add(ValidationCode::InvalidValue, columnPath + ".values",
                       "Data block table has too many rows");
        for (std::size_t row = 0; row < column.values.size(); ++row)
            validateParameterValue(column.values[row],
                                   columnPath + ".values[" + std::to_string(row) + "]", result);
        for (const auto& value : column.values) {
            const bool matches = std::visit(
                [&column](const auto& item) {
                    using Value = std::decay_t<decltype(item)>;
                    switch (column.valueKind) {
                    case DataBlockValueKind::Boolean:
                        return std::is_same_v<Value, bool>;
                    case DataBlockValueKind::Integer:
                        return std::is_same_v<Value, std::int64_t>;
                    case DataBlockValueKind::Float64:
                        return std::is_same_v<Value, double>;
                    case DataBlockValueKind::Vec2:
                        return std::is_same_v<Value, Vec2d>;
                    case DataBlockValueKind::Vec3:
                        return std::is_same_v<Value, Vec3d>;
                    case DataBlockValueKind::Color4:
                        return std::is_same_v<Value, core::Color4d>;
                    case DataBlockValueKind::String:
                        return std::is_same_v<Value, std::string>;
                    case DataBlockValueKind::Rational:
                        return std::is_same_v<Value, core::RationalTime>;
                    case DataBlockValueKind::Path:
                        return std::is_same_v<Value, PathValue>;
                    }
                    return false;
                },
                value);
            if (!matches)
                result.add(ValidationCode::TypeMismatch, columnPath + ".values",
                           "Table value does not match its declared kind");
        }
    }
}

bool targetExists(const Project& project, const ExtensionTarget& target) {
    return std::visit(
        [&project](const auto value) {
            using Id = std::decay_t<decltype(value)>;
            if constexpr (std::is_same_v<Id, ProjectId>) {
                return value == project.id();
            } else if constexpr (std::is_same_v<Id, AssetId>) {
                return project.findAsset(value) != nullptr;
            } else {
                for (const auto& composition : project.compositions()) {
                    if constexpr (std::is_same_v<Id, CompositionId>) {
                        if (composition.id() == value)
                            return true;
                    } else if constexpr (std::is_same_v<Id, NodeId>) {
                        if (composition.graph().findNode(value) != nullptr)
                            return true;
                    } else if constexpr (std::is_same_v<Id, NodeGroupId>) {
                        if (composition.nodeGroups().contains(value))
                            return true;
                    } else if constexpr (std::is_same_v<Id, ParameterId>) {
                        if (composition.parameters().find(value) != nullptr)
                            return true;
                    } else if constexpr (std::is_same_v<Id, AnimationCurveId>) {
                        if (composition.animationCurves().find(value) != nullptr)
                            return true;
                    } else if constexpr (std::is_same_v<Id, EdgeId>) {
                        if (std::ranges::any_of(
                                composition.graph().edges(),
                                [value](const auto& edge) { return edge.id == value; }))
                            return true;
                    } else if constexpr (std::is_same_v<Id, LayerId>) {
                        if (std::ranges::any_of(composition.graph().layerOutputs(),
                                                [value](const auto& boundary) {
                                                    return boundary.layerId == value;
                                                }))
                            return true;
                    } else if constexpr (std::is_same_v<Id, LayerSlotId>) {
                        for (const auto& stack : composition.graph().merges())
                            if (std::ranges::any_of(stack.entries(), [value](const auto& entry) {
                                    return entry.slotId == value;
                                }))
                                return true;
                    } else if constexpr (std::is_same_v<Id, KeyframeId>) {
                        for (const auto& record : composition.animationCurves().records())
                            if (std::visit(
                                    [value](const auto& curve) {
                                        if constexpr (requires { curve.keyframes; })
                                            return std::ranges::any_of(curve.keyframes,
                                                                       [value](const auto& key) {
                                                                           return key.id == value;
                                                                       });
                                        else
                                            return std::ranges::any_of(
                                                curve.components, [value](const auto& component) {
                                                    return std::ranges::any_of(
                                                        component.keyframes,
                                                        [value](const auto& key) {
                                                            return key.id == value;
                                                        });
                                                });
                                    },
                                    record))
                                return true;
                    }
                }
                return false;
            }
        },
        target);
}

} // namespace

bool isMediaDataBlockKind(const DataBlockKind kind) noexcept {
    return kind == DataBlockKind::Image || kind == DataBlockKind::Sequence ||
           kind == DataBlockKind::Video || kind == DataBlockKind::Audio ||
           kind == DataBlockKind::Font;
}

std::string_view dataBlockKindName(const DataBlockKind kind) noexcept {
    switch (kind) {
    case DataBlockKind::Image:
        return "Image";
    case DataBlockKind::Sequence:
        return "Sequence";
    case DataBlockKind::Video:
        return "Video";
    case DataBlockKind::Audio:
        return "Audio";
    case DataBlockKind::Font:
        return "Font";
    case DataBlockKind::Curve:
        return "Curve";
    case DataBlockKind::Ramp:
        return "Ramp";
    case DataBlockKind::Table:
        return "Table";
    case DataBlockKind::PointSet:
        return "PointSet";
    case DataBlockKind::Path:
        return "Path";
    case DataBlockKind::Mask:
        return "Mask";
    case DataBlockKind::Analysis:
        return "Analysis";
    case DataBlockKind::Opaque:
        return "Opaque";
    }
    return "Opaque";
}

std::size_t DataBlockRecord::payloadBytes() const {
    return std::visit(
        [](const auto& payload) -> std::size_t {
            using Payload = std::decay_t<decltype(payload)>;
            if constexpr (std::is_same_v<Payload, OpaqueExtensionPayload>) {
                return payload.size();
            } else if constexpr (std::is_same_v<Payload, DataBlockCurve>) {
                return payload.samples.size() * sizeof(double);
            } else if constexpr (std::is_same_v<Payload, DataBlockRamp>) {
                return payload.stops.size() * (sizeof(double) + sizeof(core::Color4d));
            } else if constexpr (std::is_same_v<Payload, DataBlockTable>) {
                std::size_t bytes = 0;
                for (const auto& column : payload.columns)
                    bytes += column.values.size() * sizeof(ParameterValue);
                return bytes;
            } else if constexpr (std::is_same_v<Payload, DataBlockPointSet>) {
                std::size_t bytes = payload.points.size() * sizeof(Vec3d);
                for (const auto& column : payload.attributes)
                    bytes += column.values.size() * sizeof(ParameterValue);
                return bytes;
            } else if constexpr (std::is_same_v<Payload, PathValue>) {
                return payload.anchors.size() * sizeof(PathAnchor);
            } else {
                return sizeof(AssetId);
            }
        },
        payload);
}

ValidationResult DataBlockRecord::validate(const Project* project) const {
    ValidationResult result;
    if (std::visit([](const auto& id) { return !id.isValid(); }, id))
        result.add(ValidationCode::InvalidId, "id", "Data block ID must not be zero");
    if (typeId.empty())
        result.add(ValidationCode::EmptyKey, "typeId", "Data block type ID must not be empty");
    else
        validateNamespacedIdentifier(typeId, "typeId", "Data block type ID", result);
    if (!schemaVersion.isValid())
        result.add(ValidationCode::InvalidValue, "schemaVersion",
                   "Data block schema major version must not be zero");
    validateStructuralText(mediaType, "mediaType", "Data block media type", result);
    if (project != nullptr) {
        if (owner.has_value() && !targetExists(*project, *owner))
            result.add(ValidationCode::MissingReference, "owner",
                       "Data block owner does not exist");
        if (subject.has_value() && !targetExists(*project, *subject))
            result.add(ValidationCode::MissingReference, "subject",
                       "Data block subject does not exist");
    }
    if (provenance.createdAt.size() > 128 ||
        (!provenance.createdAt.empty() && !core::isValidUtf8(provenance.createdAt)))
        result.add(ValidationCode::InvalidValue, "provenance.createdAt",
                   "Data block creation time is not valid UTF-8 or is too long");
    std::visit(
        [&](const auto& source) {
            if constexpr (std::is_same_v<std::decay_t<decltype(source)>, AssetLocator>) {
                if (source.path.empty())
                    result.add(ValidationCode::InvalidValue, "provenance.source.path",
                               "Data block source locator must have a path");
            } else {
                validateNamespacedIdentifier(source.name, "provenance.source.name",
                                             "Data block producer name", result);
                validateStructuralText(source.version, "provenance.source.version",
                                       "Data block producer version", result);
            }
        },
        provenance.source);
    if (tags.size() > 64 || !std::ranges::is_sorted(tags) ||
        std::ranges::adjacent_find(tags) != tags.end())
        result.add(ValidationCode::InvalidValue, "tags",
                   "Data block tags must be sorted, unique and at most 64");
    for (const auto& tag : tags)
        validateStructuralText(tag, "tags", "Data block tag", result);

    if (payloadBytes() > kMaxDataBlockPayloadBytes)
        result.add(ValidationCode::InvalidValue, "payload",
                   "Data block payload exceeds the 64 MiB bound");

    switch (kind) {
    case DataBlockKind::Curve: {
        const auto* curve = std::get_if<DataBlockCurve>(&payload);
        if (curve == nullptr || !std::isfinite(curve->domainStart) ||
            !std::isfinite(curve->domainEnd) || curve->domainStart >= curve->domainEnd ||
            curve->samples.empty() || curve->samples.size() > 1'000'000 ||
            !std::ranges::all_of(curve->samples,
                                 [](const auto value) { return std::isfinite(value); }))
            result.add(ValidationCode::InvalidValue, "payload", "Invalid sampled curve payload");
        break;
    }
    case DataBlockKind::Ramp: {
        const auto* ramp = std::get_if<DataBlockRamp>(&payload);
        if (ramp == nullptr || ramp->stops.empty() || ramp->stops.size() > 4096)
            result.add(ValidationCode::InvalidValue, "payload", "Invalid colour ramp payload");
        if (ramp != nullptr) {
            double previous = -std::numeric_limits<double>::infinity();
            for (const auto& stop : ramp->stops) {
                if (!std::isfinite(stop.position) || stop.position < 0.0 || stop.position > 1.0 ||
                    stop.position < previous || !stop.color.isValid())
                    result.add(ValidationCode::InvalidValue, "payload.stops",
                               "Ramp stops must be sorted, finite and unit-ranged");
                previous = stop.position;
            }
        }
        break;
    }
    case DataBlockKind::Table: {
        const auto* table = std::get_if<DataBlockTable>(&payload);
        if (table == nullptr)
            result.add(ValidationCode::TypeMismatch, "payload", "Table block needs table payload");
        else
            validateTable(table->columns, "payload.columns", result);
        break;
    }
    case DataBlockKind::PointSet: {
        const auto* points = std::get_if<DataBlockPointSet>(&payload);
        if (points == nullptr || (points->dimensions != 2 && points->dimensions != 3) ||
            points->points.size() > 1'000'000)
            result.add(ValidationCode::InvalidValue, "payload", "Invalid point-set payload");
        if (points != nullptr) {
            for (const auto& point : points->points)
                if (!finite(point))
                    result.add(ValidationCode::InvalidValue, "payload.points",
                               "Point-set coordinates must be finite");
            validateTable(points->attributes, "payload.attributes", result);
            if (!points->attributes.empty() &&
                points->attributes.front().values.size() != points->points.size())
                result.add(ValidationCode::InvalidValue, "payload.attributes",
                           "Point-set attributes must match point count");
        }
        break;
    }
    case DataBlockKind::Path:
        if (!std::holds_alternative<PathValue>(payload))
            result.add(ValidationCode::TypeMismatch, "payload", "Path block needs path payload");
        break;
    case DataBlockKind::Mask: {
        const auto* mask = std::get_if<DataBlockMask>(&payload);
        if (mask == nullptr || !mask->coverageAsset.isValid() ||
            (project != nullptr && project->findAsset(mask->coverageAsset) == nullptr))
            result.add(ValidationCode::MissingReference, "payload.coverageAsset",
                       "Mask coverage image must reference an existing asset");
        break;
    }
    case DataBlockKind::Analysis:
    case DataBlockKind::Opaque:
        if (!std::holds_alternative<OpaqueExtensionPayload>(payload))
            result.add(ValidationCode::TypeMismatch, "payload", "Opaque block needs byte payload");
        break;
    case DataBlockKind::Image:
    case DataBlockKind::Sequence:
    case DataBlockKind::Video:
    case DataBlockKind::Audio:
    case DataBlockKind::Font:
        if (!isMediaDataBlockKind(kind))
            result.add(ValidationCode::InvalidValue, "kind", "Invalid media block kind");
        break;
    }
    return result;
}

DataBlockRecord DataBlockRecord::fromAsset(const AssetRecord& asset) {
    DataBlockRecord block;
    block.id = asset.id;
    block.kind = asset.kind == AssetKind::Lut        ? DataBlockKind::Opaque
                 : asset.kind == AssetKind::Image    ? DataBlockKind::Image
                 : asset.kind == AssetKind::Sequence ? DataBlockKind::Sequence
                 : asset.kind == AssetKind::Video    ? DataBlockKind::Video
                 : asset.kind == AssetKind::Audio    ? DataBlockKind::Audio
                                                     : DataBlockKind::Font;
    if (asset.kind == AssetKind::Lut)
        block.payload = OpaqueExtensionPayload{};
    block.typeId = asset.kind == AssetKind::Lut ? "bloom.color.lut-asset" : "bloom.media.asset";
    block.schemaVersion =
        asset.kind == AssetKind::Lut ? SchemaVersion{1, 21} : SchemaVersion{1, 18};
    block.mediaType = "application/vnd.bloom.asset";
    block.provenance.source = asset.locator;
    block.provenance.contentDigest = asset.contentDigest;
    block.tags = asset.tags;
    return block;
}

DataBlockRecord DataBlockRecord::fromExtension(const ExtensionRecord& record) {
    DataBlockRecord block;
    block.id = record.id;
    block.kind = DataBlockKind::Opaque;
    block.typeId = record.typeId;
    block.schemaVersion = record.schemaVersion;
    block.mediaType = record.mediaType;
    block.provenance.source = DataBlockProducer{record.ownerId, "extension", {}};
    block.subject = record.subject;
    block.payload = record.payload;
    return block;
}

} // namespace bloom::document
