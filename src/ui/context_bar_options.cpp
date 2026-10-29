#include "context_bar_options.hpp"

#include "properties_registry_row.hpp"
#include "properties_sections.hpp"
#include "properties_value_edits.hpp"
#include "text_font_choices.hpp"

#include <bloom/commands/asset_operations.hpp>
#include <bloom/commands/transaction.hpp>
#include <bloom/document/graph.hpp>
#include <bloom/document/parameter.hpp>
#include <bloom/document/shape.hpp>
#include <bloom/ui/composition_session.hpp>
#include <bloom/ui/kit/color_chip.hpp>
#include <bloom/ui/kit/controls.hpp>
#include <bloom/ui/kit/dropdown.hpp>
#include <bloom/ui/kit/radio_group.hpp>
#include <bloom/ui/kit/switch_control.hpp>
#include <bloom/ui/kit/tokens.hpp>
#include <bloom/ui/kit/value_field.hpp>
#include <bloom/ui/tool_controller.hpp>
#include <bloom/ui/viewer_editor.hpp>

#include <QHBoxLayout>
#include <QSignalBlocker>
#include <QTimer>

#include <array>

namespace bloom::ui {

namespace {

constexpr std::array kAnchorModes{document::PathAnchorMode::Corner,
                                  document::PathAnchorMode::Mirrored,
                                  document::PathAnchorMode::Aligned,
                                  document::PathAnchorMode::Free, document::PathAnchorMode::Auto};

QHBoxLayout* rowLayout(QWidget* group) {
    auto* layout = new QHBoxLayout(group);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(kit::px(kit::Spacing::ChromeGap));
    return layout;
}

// The node a context-bar option edits: the primary selected layer's own content node, when it is
// of `type`.
const document::NodeRecord* selectedSource(const CompositionSession& session,
                                           const std::string_view type) {
    const auto* node = session.selectedNode();
    if (node == nullptr)
        return nullptr;
    if (node->typeId == type)
        return node;
    const auto* composition = session.composition();
    if (const auto layer = session.layerForNode(node->id); layer && composition != nullptr)
        if (const auto sourceId = session.directSourceNodeForLayer(*layer))
            if (const auto* source = composition->graph().findNode(*sourceId);
                source != nullptr && source->typeId == type)
                return source;
    return nullptr;
}

std::optional<document::ParameterId> roleParameter(const document::NodeRecord* node,
                                                   const std::string_view role) {
    if (node == nullptr)
        return std::nullopt;
    for (const auto& binding : node->parameters)
        if (binding.role == role)
            return binding.parameterId;
    return std::nullopt;
}

core::Color4d referenceColor(const kit::KColor& color) {
    return {static_cast<double>(color.red), static_cast<double>(color.green),
            static_cast<double>(color.blue), static_cast<double>(color.alpha)};
}

// Bar controls never take keyboard focus on a click: the Viewer keeps the keys.
template <typename Widget> Widget* noFocus(Widget* widget) {
    widget->setFocusPolicy(Qt::NoFocus);
    return widget;
}

} // namespace

// ---- Anchor handle modes ----------------------------------------------------------------------

AnchorModeOptions::AnchorModeOptions(std::function<ViewerEditor*()> viewer, QWidget* parent)
    : QWidget(parent), viewer_(std::move(viewer)) {
    setObjectName(QStringLiteral("contextBarAnchorOptions"));
    auto* layout = rowLayout(this);
    switcher_ = noFocus(new kit::KRadioGroup(this));
    switcher_->setObjectName(QStringLiteral("contextBarAnchorMode"));
    switcher_->setAccessibleName(tr("Anchor handle mode"));
    for (const auto mode : kAnchorModes) {
        switch (mode) {
        case document::PathAnchorMode::Corner:
            switcher_->addOption(tr("Corner"));
            break;
        case document::PathAnchorMode::Mirrored:
            switcher_->addOption(tr("Mirrored"));
            break;
        case document::PathAnchorMode::Aligned:
            switcher_->addOption(tr("Aligned"));
            break;
        case document::PathAnchorMode::Free:
            switcher_->addOption(tr("Free"));
            break;
        case document::PathAnchorMode::Auto:
            switcher_->addOption(tr("Auto"));
            break;
        }
    }
    layout->addWidget(switcher_);
    connect(switcher_, &kit::KRadioGroup::currentIndexChanged, this, [this](const int index) {
        auto* viewer = viewer_();
        if (viewer != nullptr && index >= 0 && index < static_cast<int>(kAnchorModes.size()))
            (void)viewer->setAnchorHandleMode(kAnchorModes[static_cast<std::size_t>(index)]);
        refresh();
    });
    refresh();
}

void AnchorModeOptions::refresh() {
    auto* viewer = viewer_();
    const auto state = viewer != nullptr ? viewer->anchorModeState() : ViewerEditor::AnchorModeState{};
    const QSignalBlocker blocker(switcher_);
    switcher_->setEnabled(state.editing);
    if (state.editing && state.common)
        switcher_->setCurrentIndex(static_cast<int>(*state.common));
    else
        switcher_->setMixed();
    switcher_->setToolTip(
        state.editing
            ? tr("Handle mode of the selected anchors: Corner, Mirrored, Aligned, Free or Auto")
            : tr("Handle mode of the selected anchors\nSelect anchors on a path or mask to set "
                 "their handle mode"));
}

// ---- Shape fill and stroke --------------------------------------------------------------------

ShapeStyleOptions::ShapeStyleOptions(CompositionSession& session, std::function<void()> committed,
                                     QWidget* parent)
    : QWidget(parent), session_(session), committed_(std::move(committed)) {
    setObjectName(QStringLiteral("contextBarShapeOptions"));
    auto* layout = rowLayout(this);
    fill_ = noFocus(new kit::KCheckBox(this));
    fill_->setObjectName(QStringLiteral("contextBarShapeFill"));
    fill_->setText(tr("Fill"));
    fill_->setAccessibleName(tr("Fill"));
    fillColor_ = noFocus(new kit::KColorChip(this));
    fillColor_->setObjectName(QStringLiteral("contextBarShapeFillColor"));
    fillColor_->setAccessibleName(tr("Fill colour"));
    stroke_ = noFocus(new kit::KCheckBox(this));
    stroke_->setObjectName(QStringLiteral("contextBarShapeStroke"));
    stroke_->setText(tr("Stroke"));
    stroke_->setAccessibleName(tr("Stroke"));
    strokeColor_ = noFocus(new kit::KColorChip(this));
    strokeColor_->setObjectName(QStringLiteral("contextBarShapeStrokeColor"));
    strokeColor_->setAccessibleName(tr("Stroke colour"));
    strokeWidth_ = new kit::KValueField(this);
    strokeWidth_->setObjectName(QStringLiteral("contextBarShapeStrokeWidth"));
    strokeWidth_->setAccessibleName(tr("Stroke width"));
    strokeWidth_->setToolTip(tr("Stroke width"));
    strokeWidth_->setRange(0.0, 1000.0);
    strokeWidth_->setDecimals(1);
    strokeWidth_->setUnit(QStringLiteral("px"));
    strokeWidth_->setFixedWidth(kit::px(kit::Size::ViewerZoomWidth));
    // The check boxes paint only their box; each is named by the label beside it.
    const auto label = [this](const QString& text) {
        auto* name = new kit::KLabel(text, this, kit::TypeRole::UiSmall);
        name->setTone(kit::Color::Muted);
        return name;
    };
    layout->addWidget(fill_);
    layout->addWidget(label(tr("Fill")));
    layout->addWidget(fillColor_);
    layout->addSpacing(kit::px(kit::Spacing::S));
    layout->addWidget(stroke_);
    layout->addWidget(label(tr("Stroke")));
    layout->addWidget(strokeColor_);
    layout->addWidget(strokeWidth_);

    // A selected shape layer is edited with one value edit per gesture, as in Properties; with no
    // shape layer selected the change is the new-shape style and authors nothing.
    properties::bindValueEdit(session_, *fillColor_,
                              [this] { return parameter("fillColor").value_or(document::ParameterId{}); });
    properties::bindValueEdit(session_, *strokeColor_, [this] {
        return parameter("strokeColor").value_or(document::ParameterId{});
    });
    properties::bindValueEdit(session_, *strokeWidth_, [this] {
        return parameter("strokeWidth").value_or(document::ParameterId{});
    });
    connect(fill_, &QAbstractButton::toggled, this, [this] { write("fillEnabled", tr("Set Fill")); });
    connect(stroke_, &QAbstractButton::toggled, this,
            [this] { write("strokeEnabled", tr("Set Stroke")); });
    connect(fillColor_, &kit::KColorChip::colorChanged, this,
            [this] { write("fillColor", tr("Set Fill Colour")); });
    connect(strokeColor_, &kit::KColorChip::colorChanged, this,
            [this] { write("strokeColor", tr("Set Stroke Colour")); });
    connect(strokeWidth_, &kit::KValueField::valueChanged, this,
            [this] { write("strokeWidth", tr("Set Stroke Width")); });
    connect(strokeWidth_, &kit::KValueField::editFinished, this, [this] {
        if (committed_)
            committed_();
    });
    const auto later = [this] { refresh(); };
    connect(&session_, &CompositionSession::selectionChanged, this, later);
    connect(&session_, &CompositionSession::snapshotChanged, this, later);
    connect(&session_, &CompositionSession::currentTimeChanged, this, later);
    connect(&session_.tools(), &ToolController::newShapeStyleChanged, this, later);
    connect(&session_.tools(), &ToolController::toolChanged, this, later);
    refresh();
}

const document::NodeRecord* ShapeStyleOptions::target() const {
    return selectedSource(session_, document::kShapeSourceNodeType);
}

std::optional<document::ParameterId> ShapeStyleOptions::parameter(const std::string_view role) const {
    return roleParameter(target(), role);
}

void ShapeStyleOptions::write(const std::string_view role, const QString& label) {
    if (refreshing_)
        return;
    if (const auto parameterId = parameter(role)) {
        document::ParameterValue value;
        if (role == "fillEnabled")
            value = fill_->isChecked();
        else if (role == "strokeEnabled")
            value = stroke_->isChecked();
        else if (role == "fillColor")
            value = referenceColor(fillColor_->color());
        else if (role == "strokeColor")
            value = referenceColor(strokeColor_->color());
        else
            value = strokeWidth_->value();
        (void)session_.setParameterValue(*parameterId, std::move(value), label);
    } else {
        auto style = session_.tools().newShapeStyle();
        style.fillEnabled = fill_->isChecked();
        style.strokeEnabled = stroke_->isChecked();
        style.fillColor = referenceColor(fillColor_->color());
        style.strokeColor = referenceColor(strokeColor_->color());
        style.strokeWidth = strokeWidth_->value();
        session_.tools().setNewShapeStyle(style);
    }
    refresh();
}

void ShapeStyleOptions::refresh() {
    refreshing_ = true;
    const auto* node = target();
    const auto& defaults = session_.tools().newShapeStyle();
    auto fill = defaults.fillEnabled;
    auto stroke = defaults.strokeEnabled;
    auto fillColor = defaults.fillColor;
    auto strokeColor = defaults.strokeColor;
    auto width = defaults.strokeWidth;
    bool line = session_.tools().tool() == ViewerTool::Line;
    if (node != nullptr) {
        const auto read = [node](const std::string_view role) {
            return roleParameter(node, role);
        };
        if (const auto id = read("fillEnabled"))
            fill = session_.effectiveBooleanValue(*id).value_or(fill);
        if (const auto id = read("strokeEnabled"))
            stroke = session_.effectiveBooleanValue(*id).value_or(stroke);
        if (const auto id = read("fillColor"))
            fillColor = session_.effectiveColorValue(*id).value_or(fillColor);
        if (const auto id = read("strokeColor"))
            strokeColor = session_.effectiveColorValue(*id).value_or(strokeColor);
        if (const auto id = read("strokeWidth"))
            width = session_.effectiveScalarValue(*id).value_or(width);
        if (const auto* composition = session_.composition())
            line = nodeShapeKind(*composition, *node) == document::ShapeKind::Line;
    }
    {
        const QSignalBlocker a(fill_), b(stroke_), c(strokeWidth_);
        fill_->setChecked(fill && !line);
        stroke_->setChecked(stroke || line);
        strokeWidth_->setValue(width);
    }
    properties::refreshColor(session_, "bloom.shape.fill-color", fillColor,
                             fillColor_);
    properties::refreshColor(session_, "bloom.shape.stroke-color", strokeColor,
                             strokeColor_);
    // A line has a stroke and never a fill: its fill slot stays, disabled with the reason.
    const QString subject = node != nullptr ? tr("the selected shape") : tr("new shapes");
    fill_->setEnabled(!line);
    fillColor_->setEnabled(!line);
    stroke_->setEnabled(!line);
    const auto lineReason = tr("A line has a stroke and no fill");
    fill_->setToolTip(line ? lineReason : tr("Fill %1").arg(subject));
    fillColor_->setToolTip(line ? lineReason : tr("Fill colour of %1").arg(subject));
    stroke_->setToolTip(line ? lineReason : tr("Stroke %1").arg(subject));
    strokeColor_->setToolTip(tr("Stroke colour of %1").arg(subject));
    strokeWidth_->setToolTip(tr("Stroke width of %1").arg(subject));
    refreshing_ = false;
}

// ---- Text font, size and alignment ------------------------------------------------------------

TextStyleOptions::TextStyleOptions(CompositionSession& session, std::function<void()> committed,
                                   QWidget* parent)
    : QWidget(parent), session_(session), committed_(std::move(committed)) {
    setObjectName(QStringLiteral("contextBarTextOptions"));
    auto* layout = rowLayout(this);
    font_ = new kit::KDropdown(this);
    font_->setObjectName(QStringLiteral("contextBarTextFont"));
    font_->setAccessibleName(tr("Font"));
    font_->setSearchable(true);
    font_->setFocusPolicy(Qt::ClickFocus);
    font_->setWidthFloor(kit::px(kit::Size::DropdownWidthWide));
    font_->setMaximumWidth(kit::px(kit::Size::DropdownWidthExpanded));
    size_ = new kit::KValueField(this);
    size_->setObjectName(QStringLiteral("contextBarTextSize"));
    size_->setAccessibleName(tr("Font size"));
    size_->setRange(1.0, document::kMaximumTextSizePixels);
    size_->setDecimals(1);
    size_->setUnit(QStringLiteral("px"));
    size_->setFixedWidth(kit::px(kit::Size::ViewerZoomWidth));
    alignment_ = noFocus(new kit::KRadioGroup(this));
    alignment_->setObjectName(QStringLiteral("contextBarTextAlignment"));
    alignment_->setAccessibleName(tr("Text alignment"));
    alignment_->addOption(tr("Left"), kit::IconId::AlignLeft);
    alignment_->addOption(tr("Centre"), kit::IconId::AlignCenter);
    alignment_->addOption(tr("Right"), kit::IconId::AlignRight);
    alignment_->setIconsOnly(true);
    layout->addWidget(font_);
    layout->addWidget(size_);
    layout->addWidget(alignment_);

    properties::bindValueEdit(session_, *size_, [this] {
        return parameter(document::kTextSizeParameterRole).value_or(document::ParameterId{});
    });
    connect(font_, &kit::KDropdown::activated, this, [this](const int index) {
        chooseFont(index);
        if (committed_)
            committed_();
    });
    connect(size_, &kit::KValueField::valueChanged, this, [this](const double value) {
        if (refreshing_)
            return;
        if (const auto id = parameter(document::kTextSizeParameterRole)) {
            (void)session_.setParameterValue(*id, value, tr("Set Text Size"));
        } else {
            auto style = session_.tools().newTextStyle();
            style.size = value;
            session_.tools().setNewTextStyle(style);
        }
    });
    connect(size_, &kit::KValueField::editFinished, this, [this] {
        refresh();
        if (committed_)
            committed_();
    });
    connect(alignment_, &kit::KRadioGroup::currentIndexChanged, this, [this](const int index) {
        if (refreshing_ || index < 0)
            return;
        if (const auto id = parameter(document::kTextAlignmentParameterRole)) {
            (void)session_.setParameterValue(*id, std::int64_t{index}, tr("Set Text Alignment"));
        } else {
            auto style = session_.tools().newTextStyle();
            style.alignment = index;
            session_.tools().setNewTextStyle(style);
        }
        refresh();
    });
    const auto later = [this] { refresh(); };
    connect(&session_, &CompositionSession::selectionChanged, this, later);
    connect(&session_, &CompositionSession::snapshotChanged, this, later);
    connect(&session_, &CompositionSession::currentTimeChanged, this, later);
    connect(&session_.tools(), &ToolController::newTextStyleChanged, this, later);
    populateFonts();
    refresh();
    // The installed fonts arrive from a scan off the UI thread.
    QTimer::singleShot(50, this, &TextStyleOptions::pollFonts);
}

const document::NodeRecord* TextStyleOptions::target() const {
    return selectedSource(session_, document::kTextSourceNodeType);
}

std::optional<document::ParameterId> TextStyleOptions::parameter(const std::string_view role) const {
    return roleParameter(target(), role);
}

void TextStyleOptions::populateFonts() {
    auto& cache = fontCatalogueCache();
    (void)cache.poll();
    const auto snapshot = cache.snapshot();
    if (!snapshot)
        return;
    // Building the list is not a choice: activated() alone reaches chooseFont().
    const QSignalBlocker blocker(font_);
    font_->clearItems();
    faces_ = snapshot->faces;
    for (const auto& face : faces_) {
        const auto label =
            QString::fromStdString(face.family) +
            (face.style.empty() ? QString{}
                                : QStringLiteral(" ") + QString::fromStdString(face.style));
        font_->addItem(label, fontReference(face));
    }
}

void TextStyleOptions::pollFonts() {
    auto& cache = fontCatalogueCache();
    if (cache.poll()) {
        populateFonts();
        refresh();
    }
    if (cache.pending())
        QTimer::singleShot(50, this, &TextStyleOptions::pollFonts);
}

void TextStyleOptions::chooseFont(const int index) {
    if (refreshing_ || index < 0 || index >= static_cast<int>(faces_.size()))
        return;
    const auto& face = faces_[static_cast<std::size_t>(index)];
    if (const auto id = parameter(document::kTextFontParameterRole)) {
        // Properties' own two steps: the face becomes a Font asset, then the layer points at it.
        commands::Transaction transaction("Choose Font", session_.snapshot().revision());
        transaction.emplace<commands::EnsureFontAsset>(fontAssetRecord(face));
        const auto result = session_.executeTransaction(std::move(transaction));
        if (const auto asset = result.outputId<document::AssetId>("asset"))
            (void)session_.setParameterValue(*id, std::to_string(asset->value()),
                                             tr("Set Text Font"));
    } else {
        auto style = session_.tools().newTextStyle();
        style.fontReference = fontReference(face);
        session_.tools().setNewTextStyle(style);
    }
    refresh();
}

void TextStyleOptions::refresh() {
    refreshing_ = true;
    const auto* node = target();
    const auto& defaults = session_.tools().newTextStyle();
    double size = defaults.size > 0.0 ? defaults.size : document::kDefaultTextSizePixels;
    std::int64_t alignment = defaults.alignment;
    // The bundled default face is what new text uses until a font is chosen.
    QString font = defaults.fontReference;
    if (node != nullptr) {
        if (const auto id = roleParameter(node, document::kTextSizeParameterRole))
            size = session_.effectiveScalarValue(*id).value_or(size);
        if (const auto id = roleParameter(node, document::kTextAlignmentParameterRole))
            alignment = session_.effectiveIntegerValue(*id).value_or(alignment);
        if (const auto id = roleParameter(node, document::kTextFontParameterRole))
            font = storedFontReference(session_,
                                       session_.effectiveStringValue(*id).value_or(QString{}));
    }
    {
        const QSignalBlocker a(size_), b(alignment_), c(font_);
        size_->setValue(size);
        alignment_->setCurrentIndex(static_cast<int>(alignment));
        int current = -1;
        for (int index = 0; index < font_->count(); ++index)
            if (font_->itemData(index).toString() == font ||
                (font.isEmpty() && font_->itemText(index).startsWith(QStringLiteral("DejaVu Sans"))))
                current = index;
        if (current >= 0)
            font_->setCurrentIndex(current);
    }
    const QString subject = node != nullptr ? tr("the selected text") : tr("new text");
    font_->setToolTip(tr("Font of %1").arg(subject));
    size_->setToolTip(tr("Font size of %1").arg(subject));
    alignment_->setToolTip(tr("Alignment of %1").arg(subject));
    refreshing_ = false;
}

} // namespace bloom::ui
