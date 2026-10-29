#include "safe_area_fields.hpp"

#include <bloom/ui/kit/value_field.hpp>

#include <QObject>

namespace bloom::ui {
namespace {

[[nodiscard]] kit::KValueField* percentField(QWidget& parent, const QString& name,
                                             const QString& accessible) {
    auto* field = new kit::KValueField(&parent);
    field->setObjectName(name);
    field->setAccessibleName(accessible);
    field->setDecimals(1);
    field->setSingleStep(1.0);
    field->setRange(0.1, 100.0);
    field->setUnit(QStringLiteral("%"));
    return field;
}

} // namespace

SafeAreaFields SafeAreaFields::create(QWidget& parent, const QString& actionName,
                                      const QString& titleName) {
    return {percentField(parent, actionName, QObject::tr("Action safe")),
            percentField(parent, titleName, QObject::tr("Title safe"))};
}

void SafeAreaFields::setSettings(const document::SafeAreaSettings& settings) {
    action->setValue(settings.action * 100.0);
    title->setValue(settings.title * 100.0);
}

document::SafeAreaSettings SafeAreaFields::settings() const {
    return {action->value() / 100.0, title->value() / 100.0};
}

std::optional<QString> SafeAreaFields::issue() const {
    if (title->value() > action->value())
        return QObject::tr("Title safe must be inside action safe.");
    return std::nullopt;
}

} // namespace bloom::ui
