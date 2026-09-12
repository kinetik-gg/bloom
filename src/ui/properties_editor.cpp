#include <bloom/ui/properties_editor.hpp>

#include "composition_editor_support.hpp"

#include <bloom/ui/composition_authoring.hpp>
#include <bloom/ui/composition_session.hpp>

#include <bloom/ui/kit/icons.hpp>
#include <bloom/ui/kit/painting.hpp>
#include <bloom/ui/kit/tokens.hpp>
#include <bloom/ui/kit/value_field.hpp>

#include <bloom/core/color.hpp>
#include <bloom/core/pixel_aspect_ratio.hpp>
#include <bloom/document/composition_settings.hpp>
#include <bloom/document/graph.hpp>
#include <bloom/document/parameter.hpp>
#include <bloom/document/project.hpp>

#include <QEnterEvent>
#include <QFontMetrics>
#include <QHBoxLayout>
#include <QLabel>
#include <QPainter>
#include <QPalette>
#include <QSignalBlocker>
#include <QVBoxLayout>

#include <algorithm>
#include <array>
#include <cmath>
#include <optional>
#include <variant>

namespace bloom::ui {
namespace {

const document::NodeRecord* selectedPresentationSource(const CompositionSession& session) {
    if (const auto* layerId = std::get_if<document::LayerId>(&session.selection().primary)) {
        return directSourceNode(session, *layerId);
    }
    return session.selectedNode();
}

QString selectionName(const CompositionSession& session) {
    const auto* composition = session.composition();
    if (composition == nullptr) {
        return QStringLiteral("No composition");
    }
    if (const auto* layerId = std::get_if<document::LayerId>(&session.selection().primary)) {
        return layerName(*composition, *layerId);
    }
    if (const auto* nodeId = std::get_if<document::NodeId>(&session.selection().primary)) {
        const auto* node = composition->graph().findNode(*nodeId);
        return node == nullptr ? QStringLiteral("Node unavailable")
                               : QString::fromStdString(node->typeId);
    }
    if (const auto* parameterId =
            std::get_if<document::ParameterId>(&session.selection().primary)) {
        const auto* parameter = composition->parameters().find(*parameterId);
        return parameter == nullptr ? QStringLiteral("Parameter unavailable")
                                    : QString::fromStdString(parameter->schemaKey);
    }
    return QStringLiteral("Nothing selected");
}

// Issue #120 (task U5), decisions 1/2: the properties panel's kit field grid. A row is
// [right-aligned Muted label][optional gold/dimmed Keyframe indicator][value widget], and every
// row lives inside a PropertiesRow so hovering anywhere across the row -- label, indicator, or
// value -- paints the SAME States-recipe highlight (docs/ux/visual-language.md, "State": "Hover:
// one surface step up, plus BorderHover"). Qt delivers Enter/Leave to the common ancestor of the
// previously- and newly-hovered leaf widgets only when that ancestor's OWN membership in the
// "currently entered" chain changes, so moving the pointer between a row's own label and its value
// cell never toggles this row's hover off: the row is entered once and left once, exactly as a
// single hoverable unit.
class PropertiesRow final : public QWidget {
  public:
    explicit PropertiesRow(QWidget* parent) : QWidget(parent) {
        setObjectName(QStringLiteral("propertiesRow"));
        setMinimumHeight(kit::px(kit::Size::Control));
    }

  protected:
    void enterEvent(QEnterEvent* event) override {
        hovered_ = true;
        update();
        QWidget::enterEvent(event);
    }

    void leaveEvent(QEvent* event) override {
        hovered_ = false;
        update();
        QWidget::leaveEvent(event);
    }

    void paintEvent(QPaintEvent* event) override {
        Q_UNUSED(event)
        if (!hovered_) {
            return;
        }
        QPainter painter(this);
        // The panel itself paints no surface of its own (PropertiesEditor sits directly on the
        // editor area's Background), so the row's resting surface for the recipe is Background --
        // hover steps it to Surface, exactly one step up the ladder.
        kit::fillRoundedSurface(
            painter, rect(),
            kit::color(kit::surfaceForState(kit::Color::Background, kit::State::Hover)),
            kit::color(kit::borderForState(kit::State::Hover)), kit::Radius::Small);
    }

  private:
    bool hovered_ = false;
};

// The fixed right-aligned label column every row in the panel shares (decision 1), sized once from
// the widest label this panel can ever show rather than a spelled pixel width -- KValueField's own
// internal sub-label column (kLabelColumnWidth in value_field.cpp) is private to that widget and
// exists for a narrower purpose (a field-local "X"/"Y" prefix inside the cell itself), so the row's
// OUTER label column, which names the whole parameter, is measured independently here.
int propertyLabelColumnWidth() {
    static const std::array<QString, 10> kLabels{
        PropertiesEditor::tr("Position"), PropertiesEditor::tr("Opacity"),
        PropertiesEditor::tr("RGBA"),     PropertiesEditor::tr("Alpha"),
        PropertiesEditor::tr("Encoding"), PropertiesEditor::tr("Name"),
        PropertiesEditor::tr("Format"),   PropertiesEditor::tr("Frame Rate"),
        PropertiesEditor::tr("Duration"), PropertiesEditor::tr("Pixel Aspect"),
    };
    const QFontMetrics metrics(kit::font(kit::TypeRole::Ui));
    int widest = 0;
    for (const auto& label : kLabels) {
        widest = std::max(widest, metrics.horizontalAdvance(label));
    }
    return widest;
}

QLabel* makePropertyRowLabel(const QString& text, const int columnWidth, QWidget* parent) {
    auto* label = new QLabel(text, parent);
    label->setObjectName(QStringLiteral("propertiesRowLabel"));
    label->setFont(kit::font(kit::TypeRole::Ui));
    QPalette palette = label->palette();
    palette.setColor(QPalette::WindowText, kit::color(kit::Color::Muted));
    label->setPalette(palette);
    label->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    label->setFixedWidth(columnWidth);
    return label;
}

QLabel* makeKeyframeIndicator(QWidget* parent) {
    auto* indicator = new QLabel(parent);
    indicator->setObjectName(QStringLiteral("propertiesKeyframeIndicator"));
    indicator->setFixedSize(kit::px(kit::Size::IconSmall), kit::px(kit::Size::IconSmall));
    return indicator;
}

// A read-only value cell's text: `role` is Value (Geist Mono) for numeric-looking content --
// RGBA, format, frame rate, duration, pixel aspect -- and Ui for prose -- the alpha association
// sentence, the composition name. None of these are editable through the current session API
// (issue #120, decision 1's read-only carve-out: "do not add editing capability that doesn't
// exist today"), so they stay plain selectable text rather than kit::KValueField, which has no way
// to carry a string and would otherwise misrepresent them as steppable controls.
QLabel* makeReadOnlyValueLabel(const kit::TypeRole role, QWidget* parent) {
    auto* label = new QLabel(parent);
    label->setFont(kit::font(role));
    QPalette palette = label->palette();
    palette.setColor(QPalette::WindowText, kit::color(kit::Color::Foreground));
    label->setPalette(palette);
    label->setTextInteractionFlags(Qt::TextSelectableByMouse | Qt::TextSelectableByKeyboard);
    return label;
}

// Wraps `label` (+ optional `indicator`) and `value` in one PropertiesRow, added to `section`'s
// layout. `section` also parents label/indicator/value at construction, but QBoxLayout::addWidget
// below reparents each into the row -- the same "construct with `this`, let the layout reparent"
// idiom the rest of this file already uses for its form rows.
PropertiesRow* addPropertyRow(QVBoxLayout* section, QWidget* sectionParent, QLabel* label,
                              QLabel* indicator, QWidget* value) {
    auto* row = new PropertiesRow(sectionParent);
    auto* layout = new QHBoxLayout(row);
    layout->setContentsMargins(kit::px(kit::Spacing::XS), kit::px(kit::Spacing::XXS),
                               kit::px(kit::Spacing::XS), kit::px(kit::Spacing::XXS));
    layout->setSpacing(kit::px(kit::Spacing::S));
    layout->addWidget(label);
    if (indicator != nullptr) {
        layout->addWidget(indicator);
    }
    layout->addWidget(value, 1);
    section->addWidget(row);
    return row;
}

// A UISmall uppercase group header (decision 1: "Transform", "Appearance", a source-specific
// group) plus its hairline divider (decision 1: "section dividers as hairlines"), appended to
// `section`.
void addSectionHeader(QVBoxLayout* section, QWidget* parent, const QString& title) {
    // "editorSectionTitle" already names TimelineEditor's "Layers" title and MediaEditor's
    // "Project" title (unchanged by this task): reused here rather than a new name so every group
    // header in the workspace -- Transform/Appearance/Solid Source included -- is the same logical
    // widget kind, per decision 4's "existing objectNames preserved."
    auto* header = new QLabel(title.toUpper(), parent);
    header->setObjectName(QStringLiteral("editorSectionTitle"));
    header->setFont(kit::font(kit::TypeRole::UiSmall));
    QPalette headerPalette = header->palette();
    headerPalette.setColor(QPalette::WindowText, kit::color(kit::Color::Muted));
    header->setPalette(headerPalette);
    section->addWidget(header);

    auto* divider = new QWidget(parent);
    divider->setObjectName(QStringLiteral("propertiesSectionDivider"));
    divider->setFixedHeight(static_cast<int>(std::lround(kit::kHairlineWidth)));
    QPalette dividerPalette = divider->palette();
    dividerPalette.setColor(QPalette::Window, kit::color(kit::Color::Border));
    divider->setPalette(dividerPalette);
    divider->setAutoFillBackground(true);
    section->addWidget(divider);
}

// True when `parameter` carries an animation curve source (issue #120, decision 2: "truth from
// the session snapshot, no new session API") -- the same std::holds_alternative check
// parameterSourceDescription() below already uses to report "Animated", read directly rather than
// by string-comparing that tooltip text.
bool isAnimatedParameter(const document::ParameterRecord* parameter) {
    return parameter != nullptr &&
           std::holds_alternative<document::AnimationCurveSource>(parameter->source);
}

// Paints `indicator` gold-filled when `parameter` is animation-sourced, Muted-dimmed-outline
// otherwise (decision 2). The dim opacity reuses tokens::kDisabledOpacity rather than a new
// literal: "dimmed" and "disabled ink" are the same fade recipe applied to a different ink. The
// weight switch follows docs/ux/visual-language.md's own iconography rule verbatim ("regular is
// the default visual weight and fill for selected or toggled states"): an animated parameter is
// this indicator's "on" state, so it takes the solid diamond-fill glyph rather than the outline
// one static rows show.
void updateKeyframeIndicator(QLabel* indicator, const document::ParameterRecord* parameter) {
    const bool animated = isAnimatedParameter(parameter);
    const QColor tint =
        animated ? kit::color(kit::Color::Keyframe)
                 : kit::withOpacity(kit::color(kit::Color::Muted), kit::kDisabledOpacity);
    const auto weight = animated ? kit::IconWeight::Fill : kit::IconWeight::Regular;
    indicator->setPixmap(
        kit::iconPixmap(kit::IconId::Keyframe, kit::Size::IconSmall, tint, 0.0, weight));
    indicator->setToolTip(animated ? PropertiesEditor::tr("Animated")
                                   : PropertiesEditor::tr("Static"));
}

// Frame rate as an exact rational (decision 3: "exact rational shown honestly"): numerator and
// denominator are exact std::uint32_t, so this never rounds -- it only omits the denominator when
// it is exactly 1 (the common whole-fps case) rather than always spelling "24/1 fps".
QString formatFrameRate(const document::FrameRate rate) {
    if (rate.denominator() == 1) {
        return PropertiesEditor::tr("%1 fps").arg(rate.numerator());
    }
    return PropertiesEditor::tr("%1/%2 fps").arg(rate.numerator()).arg(rate.denominator());
}

QString formatPixelAspect(const core::PixelAspectRatio pixelAspect) {
    return QStringLiteral("%1:%2").arg(pixelAspect.numerator()).arg(pixelAspect.denominator());
}

QString formatCompositionFormat(const document::CompositionFormat format) {
    return PropertiesEditor::tr("%1 × %2 px").arg(format.width()).arg(format.height());
}

// Duration as frame count + exact seconds (decision 3: "duration (frames + seconds via the exact
// formatting rule)"), reusing formatExactSeconds() above verbatim -- the SAME truncated-rational
// formatter TimelineEditor::updateTimeReadout() uses for the current-time readout, rather than a
// second, possibly-inconsistent formatting rule for duration. Frame count is maxFrameIndex + 1
// (the greatest valid index is 0-based).
QString formatDuration(const TimelineFrameContext& context) {
    return PropertiesEditor::tr("%1 frames · %2")
        .arg(context.maxFrameIndexValue + 1)
        .arg(formatExactSeconds(context.duration));
}

} // namespace

PropertiesEditor::PropertiesEditor(CompositionSession& session, QWidget* parent)
    : QWidget(parent), session_(session) {
    setObjectName("propertiesEditor");
    setAccessibleName(tr("Properties editor"));

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(kit::px(kit::Spacing::M), kit::px(kit::Spacing::M),
                               kit::px(kit::Spacing::M), kit::px(kit::Spacing::M));
    layout->setSpacing(kit::px(kit::Spacing::S));

    selectionLabel_ = new QLabel(this);
    selectionLabel_->setObjectName("propertiesSelectionTitle");
    selectionLabel_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    layout->addWidget(selectionLabel_);

    // Issue #120 (task U5), decision 1: the kit field grid. Every row's label lives in ONE fixed,
    // right-aligned column shared across the whole panel -- Transform, Appearance, the
    // source-specific group, and the no-selection Composition group alike -- computed once from
    // the widest label text this panel can ever show (propertyLabelColumnWidth() above), not from
    // a spelled pixel width.
    const int labelColumnWidth = propertyLabelColumnWidth();

    // --- Selection-driven groups (Transform / Appearance / source-specific) --------------------
    selectionSection_ = new QWidget(this);
    selectionSection_->setObjectName(QStringLiteral("propertiesSelectionSection"));
    auto* selectionLayout = new QVBoxLayout(selectionSection_);
    selectionLayout->setContentsMargins(0, 0, 0, 0);
    selectionLayout->setSpacing(kit::px(kit::Spacing::XS));

    addSectionHeader(selectionLayout, selectionSection_, tr("Transform"));

    positionX_ = new kit::KValueField(selectionSection_);
    positionY_ = new kit::KValueField(selectionSection_);
    for (auto* field : {positionX_, positionY_}) {
        field->setRange(-1'000'000.0, 1'000'000.0);
        field->setDecimals(2);
        field->setSingleStep(1.0);
        field->setUnit(QStringLiteral("px"));
    }
    positionX_->setObjectName("positionXEditor");
    positionX_->setAccessibleName(tr("Position X"));
    positionX_->setLabel(QStringLiteral("X"));
    positionY_->setObjectName("positionYEditor");
    positionY_->setAccessibleName(tr("Position Y"));
    positionY_->setLabel(QStringLiteral("Y"));

    auto* positionFields = new QWidget(selectionSection_);
    positionFields->setObjectName(QStringLiteral("positionFieldGroup"));
    auto* positionFieldsLayout = new QHBoxLayout(positionFields);
    positionFieldsLayout->setContentsMargins(0, 0, 0, 0);
    positionFieldsLayout->setSpacing(kit::px(kit::Spacing::S));
    positionFieldsLayout->addWidget(positionX_);
    positionFieldsLayout->addWidget(positionY_);

    positionKeyframe_ = makeKeyframeIndicator(selectionSection_);
    addPropertyRow(selectionLayout, selectionSection_,
                   makePropertyRowLabel(tr("Position"), labelColumnWidth, selectionSection_),
                   positionKeyframe_, positionFields);

    addSectionHeader(selectionLayout, selectionSection_, tr("Appearance"));

    opacity_ = new kit::KValueField(selectionSection_);
    opacity_->setObjectName("opacityEditor");
    opacity_->setAccessibleName(tr("Opacity"));
    opacity_->setRange(0.0, 100.0);
    opacity_->setDecimals(1);
    opacity_->setSingleStep(1.0);
    opacity_->setUnit(QStringLiteral("%"));

    opacityKeyframe_ = makeKeyframeIndicator(selectionSection_);
    addPropertyRow(selectionLayout, selectionSection_,
                   makePropertyRowLabel(tr("Opacity"), labelColumnWidth, selectionSection_),
                   opacityKeyframe_, opacity_);

    solidColorPanel_ = new QWidget(selectionSection_);
    solidColorPanel_->setObjectName("solidColorProperties");
    auto* solidColorLayout = new QVBoxLayout(solidColorPanel_);
    solidColorLayout->setContentsMargins(0, 0, 0, 0);
    solidColorLayout->setSpacing(kit::px(kit::Spacing::XS));
    addSectionHeader(solidColorLayout, solidColorPanel_, tr("Solid Source"));

    solidColorValue_ = makeReadOnlyValueLabel(kit::TypeRole::Value, solidColorPanel_);
    solidColorValue_->setObjectName("solidColorValue");
    solidColorValue_->setAccessibleName(tr("Solid RGBA value"));
    solidColorValue_->setWordWrap(true);
    solidColorKeyframe_ = makeKeyframeIndicator(solidColorPanel_);
    addPropertyRow(solidColorLayout, solidColorPanel_,
                   makePropertyRowLabel(tr("RGBA"), labelColumnWidth, solidColorPanel_),
                   solidColorKeyframe_, solidColorValue_);

    solidAlphaAssociation_ = makeReadOnlyValueLabel(kit::TypeRole::Ui, solidColorPanel_);
    solidAlphaAssociation_->setObjectName("solidAlphaAssociation");
    solidAlphaAssociation_->setAccessibleName(tr("Solid alpha association"));
    addPropertyRow(solidColorLayout, solidColorPanel_,
                   makePropertyRowLabel(tr("Alpha"), labelColumnWidth, solidColorPanel_), nullptr,
                   solidAlphaAssociation_);

    solidColorEncoding_ = makeReadOnlyValueLabel(kit::TypeRole::Ui, solidColorPanel_);
    solidColorEncoding_->setObjectName("solidColorEncoding");
    solidColorEncoding_->setAccessibleName(tr("Solid color encoding"));
    addPropertyRow(solidColorLayout, solidColorPanel_,
                   makePropertyRowLabel(tr("Encoding"), labelColumnWidth, solidColorPanel_),
                   nullptr, solidColorEncoding_);

    selectionLayout->addWidget(solidColorPanel_);
    selectionLayout->addStretch(1);
    layout->addWidget(selectionSection_);

    // --- No-selection document/composition view (decision 3) ----------------------------------
    documentSection_ = new QWidget(this);
    documentSection_->setObjectName(QStringLiteral("propertiesDocumentSection"));
    auto* documentLayout = new QVBoxLayout(documentSection_);
    documentLayout->setContentsMargins(0, 0, 0, 0);
    documentLayout->setSpacing(kit::px(kit::Spacing::XS));
    addSectionHeader(documentLayout, documentSection_, tr("Composition"));

    documentName_ = makeReadOnlyValueLabel(kit::TypeRole::Ui, documentSection_);
    documentName_->setObjectName(QStringLiteral("documentName"));
    documentName_->setAccessibleName(tr("Composition name"));
    addPropertyRow(documentLayout, documentSection_,
                   makePropertyRowLabel(tr("Name"), labelColumnWidth, documentSection_), nullptr,
                   documentName_);

    documentFormat_ = makeReadOnlyValueLabel(kit::TypeRole::Value, documentSection_);
    documentFormat_->setObjectName(QStringLiteral("documentFormat"));
    documentFormat_->setAccessibleName(tr("Composition format"));
    addPropertyRow(documentLayout, documentSection_,
                   makePropertyRowLabel(tr("Format"), labelColumnWidth, documentSection_), nullptr,
                   documentFormat_);

    documentFrameRate_ = makeReadOnlyValueLabel(kit::TypeRole::Value, documentSection_);
    documentFrameRate_->setObjectName(QStringLiteral("documentFrameRate"));
    documentFrameRate_->setAccessibleName(tr("Composition frame rate"));
    addPropertyRow(documentLayout, documentSection_,
                   makePropertyRowLabel(tr("Frame Rate"), labelColumnWidth, documentSection_),
                   nullptr, documentFrameRate_);

    documentDuration_ = makeReadOnlyValueLabel(kit::TypeRole::Value, documentSection_);
    documentDuration_->setObjectName(QStringLiteral("documentDuration"));
    documentDuration_->setAccessibleName(tr("Composition duration"));
    addPropertyRow(documentLayout, documentSection_,
                   makePropertyRowLabel(tr("Duration"), labelColumnWidth, documentSection_),
                   nullptr, documentDuration_);

    documentPixelAspect_ = makeReadOnlyValueLabel(kit::TypeRole::Value, documentSection_);
    documentPixelAspect_->setObjectName(QStringLiteral("documentPixelAspect"));
    documentPixelAspect_->setAccessibleName(tr("Composition pixel aspect ratio"));
    addPropertyRow(documentLayout, documentSection_,
                   makePropertyRowLabel(tr("Pixel Aspect"), labelColumnWidth, documentSection_),
                   nullptr, documentPixelAspect_);

    // Color settings (process space + config name) are read from ProjectSession, not from
    // anything CompositionSession exposes (src/host/include/bloom/host/project_session.hpp) --
    // document::Composition/Snapshot carry no ColorSettings at all (verified: grep finds
    // ColorSettings only under src/host and src/project, never src/document). Per decision 3 ("if
    // a listed fact is not reachable via existing read-only API, omit it and report rather than
    // adding API"), the color settings summary row is omitted here; see this task's raw report.

    documentLayout->addStretch(1);
    layout->addWidget(documentSection_);

    const auto commitPosition = [this] {
        if (!rebuilding_) {
            (void)session_.setSelectedPosition(positionX_->value(), positionY_->value());
        }
    };
    connect(positionX_, &kit::KValueField::valueChanged, this, commitPosition);
    connect(positionY_, &kit::KValueField::valueChanged, this, commitPosition);
    connect(opacity_, &kit::KValueField::valueChanged, this, [this](const double value) {
        if (!rebuilding_) {
            (void)session_.setSelectedOpacity(value / 100.0);
        }
    });
    connect(&session_, &CompositionSession::snapshotChanged, this, &PropertiesEditor::rebuild);
    connect(&session_, &CompositionSession::compositionChanged, this, &PropertiesEditor::rebuild);
    connect(&session_, &CompositionSession::selectionChanged, this, &PropertiesEditor::rebuild);

    rebuild();
}

void PropertiesEditor::rebuild() {
    rebuilding_ = true;
    selectionLabel_->setText(selectionName(session_));

    configurePosition();
    configureOpacity();
    configureSolidColor();
    configureDocumentProperties();
    rebuilding_ = false;
}

void PropertiesEditor::configurePosition() {
    const auto* position = session_.parameterForSelection(document::kPositionParameterRole);
    const auto positionValue =
        position == nullptr ? std::nullopt : session_.constantVec2Value(position->id);
    const bool canEditPosition = positionValue.has_value();
    positionX_->setEnabled(canEditPosition);
    positionY_->setEnabled(canEditPosition);
    if (canEditPosition) {
        const QSignalBlocker blockX(positionX_);
        const QSignalBlocker blockY(positionY_);
        positionX_->setValue(positionValue->x);
        positionY_->setValue(positionValue->y);
    }
    const QString positionTip = position == nullptr
                                    ? tr("Position is not exposed by this selection")
                                    : parameterSourceDescription(*position);
    positionX_->setToolTip(positionTip);
    positionY_->setToolTip(positionTip);
    updateKeyframeIndicator(positionKeyframe_, position);
}

void PropertiesEditor::configureOpacity() {
    const auto* parameter = session_.parameterForSelection(document::kOpacityParameterRole);
    const auto value = parameter == nullptr ? std::nullopt : session_.constantValue(parameter->id);
    opacity_->setEnabled(value.has_value());
    const QSignalBlocker blocker(opacity_);
    opacity_->setValue(value.has_value() ? *value * 100.0 : 100.0);
    opacity_->setToolTip(parameter == nullptr ? tr("Opacity is not exposed by this selection")
                                              : parameterSourceDescription(*parameter));
    updateKeyframeIndicator(opacityKeyframe_, parameter);
}

void PropertiesEditor::configureSolidColor() {
    const auto* parameter = session_.parameterForSelection(document::kSolidColorParameterRole);
    const auto* sourceNode = selectedPresentationSource(session_);
    const bool isSolid = isKnownSource(sourceNode, document::kSolidSourceNodeType,
                                       document::kSolidSourceNodeSchemaVersion) &&
                         parameter != nullptr &&
                         parameter->schemaKey == document::kSolidColorParameterSchemaKey;
    solidColorPanel_->setVisible(isSolid);
    if (!isSolid) {
        return;
    }

    updateKeyframeIndicator(solidColorKeyframe_, parameter);
    const auto value = session_.constantColorValue(parameter->id);
    solidColorValue_->setText(value.has_value() ? exactColorText(*value)
                                                : parameterSourceDescription(*parameter));
    solidColorValue_->setToolTip(
        tr("Straight scene-linear authoring values; negative and HDR RGB are not clipped"));
    solidAlphaAssociation_->setText(tr("Straight (unassociated)"));
    solidColorEncoding_->setText(
        QString::fromUtf8(document::kSolidColorEncoding.data(),
                          static_cast<qsizetype>(document::kSolidColorEncoding.size())));
}

void PropertiesEditor::configureDocumentProperties() {
    const auto* composition = session_.composition();
    const bool hasSelection = !std::holds_alternative<std::monostate>(session_.selection().primary);
    const bool showDocument = composition != nullptr && !hasSelection;
    documentSection_->setVisible(showDocument);
    selectionSection_->setVisible(!showDocument);
    if (!showDocument) {
        return;
    }

    documentName_->setText(QString::fromStdString(composition->name()));
    const auto format = composition->format();
    documentFormat_->setText(formatCompositionFormat(format));
    documentFrameRate_->setText(formatFrameRate(format.frameRate()));
    documentPixelAspect_->setText(formatPixelAspect(format.pixelAspect()));
    const auto context = frameContextFor(session_);
    documentDuration_->setText(context.has_value() ? formatDuration(*context)
                                                   : QStringLiteral("—"));
}

} // namespace bloom::ui
