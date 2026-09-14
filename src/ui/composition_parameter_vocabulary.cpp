#include "node_editor_items.hpp"
#include "properties_registry_row.hpp"
#include <bloom/core/scalar_primitives.hpp>
namespace bloom::ui {
QList<std::pair<QString, std::int64_t>> propertiesSelectorItems(std::string_view schemaKey) {
    QList<std::pair<QString, std::int64_t>> items;
    const auto add = [&items](const QString& text, const std::int64_t stored) {
        items.append({text, stored});
    };
    if (schemaKey == document::kTextAlignmentParameterSchemaKey) {
        add(QObject::tr("Left"), 0);
        add(QObject::tr("Center"), 1);
        add(QObject::tr("Right"), 2);
        return items;
    }
    if (schemaKey == document::kScalarOperationParameterSchemaKey) {
        for (const auto operation : document::kScalarOperations) {
            const auto* signature = core::primitives::scalarPrimitiveSignature(operation);
            // The frozen signature's own id is the name, with its namespace trimmed: the card
            // must not invent a second spelling for an operation the kernel already names.
            const auto id = signature == nullptr ? std::string_view{} : signature->id;
            add(node_editor::displayTypeName(id.subsQObject::tr(id.rfind('.') + 1)),
                document::scalarOperationStoredValue(operation));
        }
        return items;
    }
    if (schemaKey == document::kVectorOperationParameterSchemaKey) {
        static constexpr std::array kNames{"Add",   "Subtract",  "Multiply",     "Divide",
                                           "Scale", "Normalize", "Cross Product"};
        for (std::size_t index = 0; index < document::kVectorOperations.size(); ++index) {
            add(QString::fromUtf8(kNames[index]),
                document::vectorOperationStoredValue(document::kVectorOperations[index]));
        }
        return items;
    }
    if (schemaKey == document::kVectorReductionParameterSchemaKey) {
        static constexpr std::array kNames{"Length", "Dot Product", "Distance"};
        for (std::size_t index = 0; index < document::kVectorReductions.size(); ++index) {
            add(QString::fromUtf8(kNames[index]),
                document::vectorReductionStoredValue(document::kVectorReductions[index]));
        }
        return items;
    }
    if (schemaKey == document::kRangeInterpolationParameterSchemaKey) {
        static constexpr std::array kNames{"Linear", "Smoothstep", "Smootherstep"};
        for (std::size_t index = 0; index < document::kRangeInterpolations.size(); ++index) {
            add(QString::fromUtf8(kNames[index]),
                document::rangeInterpolationStoredValue(document::kRangeInterpolations[index]));
        }
        return items;
    }
    if (schemaKey == document::kCompareOperationParameterSchemaKey) {
        static constexpr std::array kNames{"Equal",         "Not Equal", "Less",
                                           "Less Or Equal", "Greater",   "Greater Or Equal"};
        for (std::size_t index = 0; index < document::kCompareOperations.size(); ++index) {
            add(QString::fromUtf8(kNames[index]),
                document::compareOperationStoredValue(document::kCompareOperations[index]));
        }
        return items;
    }
    return items;
}
} // namespace bloom::ui
