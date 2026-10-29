#pragma once

class QAccessibleInterface;
class QObject;
class QString;

namespace bloom::ui::kit {

// Installs the kit's accessibility interfaces once per application: KDropdown reads as a combo box
// (its value, whether its list is open, and an action that opens it) and KRadioGroup as a group
// of radio buttons, one accessible child per option carrying its checked and disabled states.
// Every kit control that has one calls this from its constructor.
void installKitAccessibility();

// The factory installKitAccessibility() registers, exposed so a test can ask it directly.
[[nodiscard]] QAccessibleInterface* kitAccessibleFactory(const QString& className, QObject* object);

} // namespace bloom::ui::kit
