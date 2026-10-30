#pragma once

// Internal building blocks of WindowStatusBar (not installed): a labelled segment of the bar, a
// mono value whose width does not move as its digits change, and the clickable MCP text.

#include <bloom/ui/kit/controls.hpp>
#include <bloom/ui/kit/surfaces.hpp>
#include <bloom/ui/kit/tokens.hpp>

#include <QCoreApplication>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QMouseEvent>
#include <functional>

namespace bloom::ui::statusbar {

// Bytes with the unit that keeps the number readable: MB below one gibibyte, GB from there on, one
// decimal either way, so a machine-sized budget reads "45.3 GB" rather than "46370.2 MB".
[[nodiscard]] inline QString formatBytes(const std::size_t bytes) {
    constexpr double kMebibyte = 1024.0 * 1024.0;
    constexpr double kGibibyte = kMebibyte * 1024.0;
    const auto value = static_cast<double>(bytes);
    return value >= kGibibyte ? QCoreApplication::translate("bloom::ui::WindowStatusBar", "%1 GB")
                                    .arg(value / kGibibyte, 0, 'f', 1)
                              : QCoreApplication::translate("bloom::ui::WindowStatusBar", "%1 MB")
                                    .arg(value / kMebibyte, 0, 'f', 1);
}

// A value in the monospaced face. `widthSample` is the widest text the cell shows; its width is
// the cell's minimum, so neighbouring segments stay put while the number changes.
class ValueLabel : public kit::KLabel {
  public:
    ValueLabel(const QString& objectName, const QString& widthSample, kit::Color tone,
               QWidget* parent)
        : KLabel(parent) {
        setObjectName(objectName);
        setTypeRole(kit::TypeRole::Value);
        setTone(tone);
        setMinimumWidth(fontMetrics().horizontalAdvance(widthSample));
    }
};

// MCP's text segment: a mono label that opens the MCP Server panel. Keyboard reachable.
class ClickableValue final : public ValueLabel {
  public:
    using ValueLabel::ValueLabel;
    std::function<void()> onClick;

  protected:
    void mouseReleaseEvent(QMouseEvent* event) override {
        if (event->button() == Qt::LeftButton && rect().contains(event->position().toPoint()))
            if (onClick)
                onClick();
        ValueLabel::mouseReleaseEvent(event);
    }
    void keyPressEvent(QKeyEvent* event) override {
        if (event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter ||
            event->key() == Qt::Key_Space) {
            if (onClick)
                onClick();
            return;
        }
        ValueLabel::keyPressEvent(event);
    }
};

// One segment: [label] [value] [label] [value] ... in a row. Labels use the UI font in Muted ink.
class Segment final : public QWidget {
  public:
    Segment(const QString& objectName, QWidget* parent) : QWidget(parent) {
        setObjectName(objectName);
        layout_ = new QHBoxLayout(this);
        layout_->setContentsMargins(0, 0, 0, 0);
        layout_->setSpacing(kit::px(kit::Spacing::XS));
    }
    kit::KLabel* addLabel(const QString& text) {
        auto* label = new kit::KLabel(text, this, kit::TypeRole::UiSmall);
        label->setTone(kit::Color::Muted);
        layout_->addWidget(label);
        return label;
    }
    void addGap() { layout_->addSpacing(kit::px(kit::Spacing::S)); }
    void add(QWidget* widget, int stretch = 0) { layout_->addWidget(widget, stretch); }
    // The segment's explanation, set on the whole segment and on every child that has none.
    void setSegmentToolTip(const QString& text) {
        setToolTip(text);
        for (auto* child : findChildren<QWidget*>())
            if (child->toolTip().isEmpty() || child->property("segmentToolTip").toBool()) {
                child->setToolTip(text);
                child->setProperty("segmentToolTip", true);
            }
    }

  private:
    QHBoxLayout* layout_ = nullptr;
};

} // namespace bloom::ui::statusbar
