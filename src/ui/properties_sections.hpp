#pragma once

// The Properties panel's row and section vocabulary (task PROPS-1, deliverable 1).
//
// properties_editor.cpp used to carry every one of these in its own anonymous namespace, which is
// most of why it had grown past a thousand lines. They are factories, not policy: nothing here
// reads the session or the document, so the panel keeps every decision about WHAT to show and this
// file owns only HOW a row, a label, a paired cell group, or a section is built.
//
// Internal to bloom_ui on purpose (src/ui, not src/ui/include/bloom/ui): exactly
// timeline_property_rows.hpp's precedent -- no target outside bloom_ui has any business
// constructing a Properties row.

#include <bloom/core/color.hpp>
#include <bloom/ui/kit/tokens.hpp>
#include <optional>

#include <QString>
#include <initializer_list>
#include <string_view>

class QLabel;
class QVBoxLayout;
class QWidget;

namespace bloom::ui {

class CompositionSession;
class KeyframeDiamond;

namespace kit {
class KColorChip;
class KButton;
class KSection;
class KValueField;
} // namespace kit

namespace properties {

// Project colour presentation/authoring shared by Properties and node-card adapters.
void refreshColor(CompositionSession& session, std::string_view schemaKey, core::Color4d value,
                  kit::KColorChip* chip, std::initializer_list<kit::KValueField*> fields = {});
[[nodiscard]] std::optional<core::Color4d>
colorFromFields(CompositionSession& session, std::string_view schemaKey,
                std::initializer_list<kit::KValueField*> fields);

// The token-sized, right-aligned label column shared by every Properties row.
[[nodiscard]] int labelColumnWidth();

// A row's outer label: the shared column width as its PREFERRED width, elided with Qt::ElideRight
// when the panel is narrower than that, and always carrying the untruncated name in its tooltip
// (task WIDTH-1).
[[nodiscard]] QLabel* makeRowLabel(const QString& text, QWidget* parent);

// [right-aligned Muted label][value widgets...][fixed keyframe slot], appended to
// `section`. Every value widget is a DIRECT child of the returned row, so a single-cell row's value
// widget can always find its row through one parentWidget() hop -- which is exactly what the
// projection tests rely on.
QWidget* addRow(QVBoxLayout* section, QWidget* sectionParent, QLabel* label, QWidget* indicator,
                std::initializer_list<QWidget*> values);

QWidget* addRow(QVBoxLayout* section, QWidget* sectionParent, QLabel* label, QWidget* indicator,
                QWidget* value);

// The clickable keyframe indicator for `role`. objectName "propertiesKeyframeIndicator".
[[nodiscard]] KeyframeDiamond* makeKeyframeDiamond(CompositionSession& session,
                                                   std::string_view role, QWidget* parent);

// A read-only value cell's text: `role` is Value (Geist Mono) for numeric-looking content and Ui
// for prose.
[[nodiscard]] QLabel* makeReadOnlyValueLabel(kit::TypeRole role, QWidget* parent);

// One configured numeric cell. `subLabel` is the cell-local "X"/"Y"/"R" prefix, empty for none.
struct ValueCellSpec {
    QString objectName;
    QString accessibleName;
    QString subLabel;
    double minimum = -1'000'000.0;
    double maximum = 1'000'000.0;
    int decimals = 2;
    double singleStep = 1.0;
    QString unit;
};

[[nodiscard]] kit::KValueField* makeValueCell(const ValueCellSpec& spec, QWidget* parent);

// The [X][Y] (optionally [X][link][Y]) container a paired row puts in its value column. Named so
// the existing positionFieldGroup/anchorFieldGroup/scaleFieldGroup/solidColorFieldGroup objectNames
// keep resolving.
[[nodiscard]] QWidget* makeCellGroup(const QString& objectName,
                                     std::initializer_list<QWidget*> cells, QWidget* parent);

// Append a swatch row and a single-line RGBA disclosure. Existing fields retain their names.
[[nodiscard]] QWidget* addColorRow(QVBoxLayout* rows, QWidget* parent, kit::KColorChip* chip,
                                   QWidget* diamond, std::initializer_list<QWidget*> fields,
                                   const QString& expandName, const QString& groupName);

// A proportional/axis link toggle for a paired row. Checkable, Ghost, IconId::Link.
[[nodiscard]] QWidget* makeLinkToggle(const QString& objectName, const QString& tooltip,
                                      QWidget* parent);

// One kit::KSection wired to `properties/sections/<id>/collapsed` and appended to `parent`'s
// layout. Collapse all / Expand all are answered by the panel, which is the only thing that knows
// the full set of sections.
[[nodiscard]] kit::KSection* addSection(QVBoxLayout* layout, QWidget* parent, const QString& id,
                                        const QString& title);

} // namespace properties
} // namespace bloom::ui
