#include "viewer_editor_text_layout.hpp"
#include <QApplication>
#include <QFocusEvent>
#include <QInputMethod>
#include <QMouseEvent>
#include <QPainter>
#include <algorithm>
#include <bloom/ui/composition_preview_controller.hpp>
#include <bloom/ui/kit/tokens.hpp>
#include <bloom/ui/viewer_editor.hpp>
#include <cmath>

namespace bloom::ui {
namespace {
template <typename T>
T textValue(const CompositionSession& session, std::string_view role, T fallback) {
    const auto* parameter = session.parameterForSelection(role);
    const auto value = parameter ? session.liveValue(parameter->id) : std::nullopt;
    const auto* typed = value ? std::get_if<T>(&*value) : nullptr;
    return typed ? *typed : fallback;
}
std::optional<QTransform> textWorld(const runtime::EvaluatedOperationBounds& bounds) {
    if (bounds.local.empty())
        return {};
    const auto point = [](document::Vec2d p) { return QPointF(p.x, p.y); };
    const auto x = (point(bounds.polygon[1]) - point(bounds.polygon[0])) /
                   (bounds.local.right - bounds.local.left);
    const auto y = (point(bounds.polygon[3]) - point(bounds.polygon[0])) /
                   (bounds.local.bottom - bounds.local.top);
    const auto origin = point(bounds.polygon[0]) - x * bounds.local.left - y * bounds.local.top;
    QTransform result(x.x(), x.y(), y.x(), y.y(), origin.x(), origin.y());
    if (!result.isInvertible() || !std::isfinite(result.determinant()) ||
        !std::isfinite(origin.x()) || !std::isfinite(origin.y()))
        return {};
    return result;
}
} // namespace

bool ViewerEditor::textEditing() const noexcept { return textEdit_ != nullptr; }

bool ViewerEditor::beginTextEditing(std::optional<QPointF> point, const bool selectAll) {
    const auto* parameter = session_.parameterForSelection(document::kTextParameterRole);
    const auto layer = session_.selection().contextualLayer;
    if (!parameter || !layer)
        return false;
    if (textEdit_)
        return true;
    playback_->pause();
    if (!session_.beginTextEdit(parameter->id))
        return false;
    auto edit = std::make_unique<ViewerTextEdit>();
    edit->parameter = parameter->id;
    edit->layer = *layer;
    edit->text = session_.effectiveStringValue(parameter->id).value_or(QString{});
    edit->cursor = static_cast<int>(edit->text.size());
    edit->anchor = selectAll ? 0 : edit->cursor;
    edit->pendingClick = point;
    const auto box = textValue(session_, document::kTextBoxParameterRole, document::Vec2d{});
    edit->multiline = box.x > 0 || edit->text.contains('\n');
    // Empty/new point text has no evaluated polygon. Its authored anchor remains editable.
    const auto position = textValue(session_, document::kPositionParameterRole, document::Vec2d{});
    const auto anchor = textValue(session_, document::kAnchorParameterRole, document::Vec2d{});
    const auto scale = textValue(session_, document::kScaleParameterRole, document::Vec2d{1, 1});
    edit->world.translate(position.x, position.y);
    edit->world.rotate(textValue(session_, document::kRotationParameterRole, 0.0));
    edit->world.scale(scale.x, scale.y);
    edit->world.translate(-anchor.x, -anchor.y);
    if (const auto& frame = previewController_.state().frame) {
        if (const auto parent = session_.parentOf(*layer)) {
            for (const auto& bounds : frame->evaluatedBounds())
                if (bounds.layerId == *parent)
                    if (const auto world = textWorld(bounds))
                        edit->world *= *world;
        }
        for (const auto& bounds : frame->evaluatedBounds())
            if (bounds.layerId == *layer)
                if (const auto world = textWorld(bounds))
                    edit->world = *world;
    }
    edit->emptyWorld = edit->world;
    if (const auto& frame = previewController_.state().frame) {
        for (const auto& bounds : frame->evaluatedBounds()) {
            if (bounds.layerId != *layer || bounds.local.empty())
                continue;
            const auto& w = edit->world;
            edit->emptyWorld =
                QTransform(w.m11(), w.m12(), w.m21(), w.m22(),
                           bounds.anchor.x - w.m11() * anchor.x - w.m21() * anchor.y,
                           bounds.anchor.y - w.m12() * anchor.x - w.m22() * anchor.y);
        }
    }
    if (!edit->world.isInvertible() || !std::isfinite(edit->world.determinant()) ||
        !std::isfinite(edit->world.dx()) || !std::isfinite(edit->world.dy())) {
        session_.cancelValueEdit();
        return false;
    }
    edit->worker = new ViewerTextLayout(this);
    edit->worker->ready = [this](ViewerTextLayout::Result result) {
        if (!textEdit_)
            return;
        textEdit_->layoutStatus = result.diagnostic;
        setAccessibleDescription(result.diagnostic);
        if (!result.diagnostic.isEmpty()) {
            update();
            return;
        }
        textEdit_->layout = std::move(result.layout);
        textEdit_->layoutContent = QString::fromStdString(result.content);
        textEdit_->layoutReady = true;
        if (const auto click = textEdit_->pendingClick) {
            textEdit_->pendingClick.reset();
            moveTextCaret(*click, false);
        }
        QGuiApplication::inputMethod()->update(Qt::ImQueryAll);
        update();
    };
    textEdit_ = std::move(edit);
    setAttribute(Qt::WA_InputMethodEnabled, true);
    setFocus(Qt::OtherFocusReason);
    setCursor(Qt::IBeamCursor);
    previewController_.beginInteractiveScrub();
    refreshTextLayout();
    return true;
}

void ViewerEditor::finishTextEditing(const bool commit) {
    if (!textEdit_)
        return;
    if (commit && !textEdit_->preedit.isEmpty()) {
        QGuiApplication::inputMethod()->commit();
        if (!textEdit_)
            return;
    }
    auto edit = std::move(textEdit_);
    setAttribute(Qt::WA_InputMethodEnabled, false);
    QGuiApplication::inputMethod()->reset();
    edit->worker->cancel();
    edit->worker->deleteLater();
    if (session_.isValueEditing(edit->parameter)) {
        if (commit) {
            (void)session_.updateValueEdit(edit->text.toStdString());
            (void)session_.commitValueEdit();
        } else {
            session_.cancelValueEdit();
        }
    }
    previewController_.notifyScrubEnded();
    unsetCursor();
    updatePreviewAccessibility();
    update();
}

void ViewerEditor::refreshTextLayout() {
    if (!textEdit_)
        return;
    render::TextLayoutOptions options;
    options.alignment = static_cast<render::TextAlignment>(
        textValue(session_, document::kTextAlignmentParameterRole, std::int64_t{0}));
    options.lineHeight = textValue(session_, document::kTextLineHeightParameterRole, 1.0);
    options.letterSpacing = textValue(session_, document::kTextLetterSpacingParameterRole, 0.0);
    const auto box = textValue(session_, document::kTextBoxParameterRole, document::Vec2d{});
    options.boxWidth = box.x;
    options.boxHeight = box.y;
    options.wrap = textValue(session_, document::kTextWrapParameterRole, false);
    options.verticalAlignment = static_cast<render::TextLayoutOptions::VerticalAlignment>(
        textValue(session_, document::kTextVerticalAlignmentParameterRole, std::int64_t{0}));
    options.anchorMode = static_cast<render::TextLayoutOptions::AnchorMode>(
        textValue(session_, document::kTextAnchorModeParameterRole, std::int64_t{0}));
    options.overflow = static_cast<render::TextLayoutOptions::Overflow>(
        textValue(session_, document::kTextOverflowParameterRole, std::int64_t{0}));
    const auto size = textValue(session_, document::kTextSizeParameterRole, 72.0);
    const auto parameters = render::TextRasterParameters::create(size, size);
    if (!parameters)
        return;
    const auto content = session_.effectiveStringValue(textEdit_->parameter).value_or(QString{});
    textEdit_->layoutReady = false;
    textEdit_->layoutStatus = tr("Preparing text layout");
    setAccessibleDescription(textEdit_->layoutStatus);
    textEdit_->worker->request({session_.snapshot(), session_.compositionId(), textEdit_->parameter,
                                content.toStdString(), *parameters.value(), options});
    update();
}

QTransform ViewerEditor::textToScreen() const {
    if (!textEdit_)
        return {};
    auto world = textEdit_->layoutContent.isEmpty() ? textEdit_->emptyWorld : textEdit_->world;
    const auto bounds = previewController_.selectedLayerBounds();
    for (const auto& bound : bounds)
        if (bound.layerId == textEdit_->layer && !textEdit_->layoutContent.isEmpty())
            if (const auto evaluated = textWorld(bound))
                world = *evaluated;
    const auto geometry = currentDisplayGeometry();
    const auto* composition = session_.composition();
    if (!geometry || !composition)
        return world;
    const auto display = viewTransformedDisplayRect(canvasRect(), geometry->extent,
                                                    geometry->pixelAspect, transform_);
    QTransform screen;
    screen.translate(display.left(), display.top());
    screen.scale(display.width() / composition->format().width(),
                 display.height() / composition->format().height());
    return world * screen;
}

std::size_t ViewerEditor::textCaretByte() const {
    const int cursor =
        textEdit_->preedit.isEmpty()
            ? textEdit_->cursor
            : std::min(textEdit_->cursor, textEdit_->anchor) + textEdit_->preeditCursor;
    return static_cast<std::size_t>(textEdit_->layoutContent.left(cursor).toUtf8().size());
}
QLineF ViewerEditor::textCaretForTest() const {
    if (!textEdit_ || !textEdit_->layoutReady)
        return {};
    return textToScreen().map(textLayoutCaret(textEdit_->layout, textCaretByte()));
}
void ViewerEditor::paintTextEditing(QPainter& painter) const {
    if (!textEdit_)
        return;
    if (!textEdit_->layoutReady) {
        painter.save();
        painter.setPen(kit::color(kit::Color::Muted));
        painter.setFont(kit::font(kit::TypeRole::Ui));
        painter.drawText(contentRect(), Qt::AlignBottom | Qt::AlignHCenter,
                         textEdit_->layoutStatus);
        painter.restore();
        return;
    }
    auto first = static_cast<std::size_t>(
        textEdit_->text.left(std::min(textEdit_->cursor, textEdit_->anchor)).toUtf8().size());
    auto last = static_cast<std::size_t>(
        textEdit_->text.left(std::max(textEdit_->cursor, textEdit_->anchor)).toUtf8().size());
    if (!textEdit_->preedit.isEmpty())
        last = first + static_cast<std::size_t>(textEdit_->preedit.toUtf8().size());
    paintViewerTextEdit(painter, contentRect(), textToScreen(), textEdit_->layout, textCaretByte(),
                        first, last, !textEdit_->preedit.isEmpty(),
                        textEdit_->preeditCursorVisible);
}
void ViewerEditor::moveTextCaret(const QPointF screenPoint, const bool extend) {
    if (!textEdit_ || !textEdit_->layoutReady)
        return;
    const auto byte =
        nearestTextLayoutBoundary(textEdit_->layout, textToScreen().inverted().map(screenPoint));
    const auto bytes = textEdit_->text.toUtf8();
    textEdit_->cursor =
        static_cast<int>(QString::fromUtf8(bytes.left(static_cast<qsizetype>(byte))).size());
    if (!extend)
        textEdit_->anchor = textEdit_->cursor;
    QGuiApplication::inputMethod()->update(Qt::ImQueryAll);
    update();
}
void ViewerEditor::mouseDoubleClickEvent(QMouseEvent* event) {
    if (tool_ == Tool::Select && event->button() == Qt::LeftButton) {
        endDrag(false);
        if (const auto mapping = currentMapping()) {
            const auto hit = hitAt(*mapping, event->position());
            if (hit.layerId.isValid()) {
                session_.selectLayer(hit.layerId);
                if (beginTextEditing(event->position())) {
                    event->accept();
                    return;
                }
            }
        }
    }
    QWidget::mouseDoubleClickEvent(event);
}
void ViewerEditor::focusOutEvent(QFocusEvent* event) {
    finishTextEditing(true);
    QWidget::focusOutEvent(event);
}
bool ViewerEditor::textEditPress(QMouseEvent* event) {
    if (!textEdit_ || event->button() != Qt::LeftButton)
        return false;
    if (!textEdit_->preedit.isEmpty()) {
        QGuiApplication::inputMethod()->commit();
        if (!textEdit_)
            return true;
    }
    moveTextCaret(event->position(), event->modifiers().testFlag(Qt::ShiftModifier));
    textEdit_->dragging = true;
    event->accept();
    return true;
}
} // namespace bloom::ui
