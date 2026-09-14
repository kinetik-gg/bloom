#pragma once
#include <QWidget>
#include <array>
#include <bloom/document/node_definition_registry.hpp>
#include <bloom/ui/kit/value_field.hpp>
class QLineEdit;
class QPlainTextEdit;
namespace bloom::ui {
class CompositionSession;
void resetPropertiesParameter(CompositionSession& session, document::ParameterId parameter);
void jumpToPropertiesNode(CompositionSession& session, document::NodeId node, QWidget* panel);
class KeyframeDiamond;
namespace kit {
class KColorChip;
class KDropdown;
class KSwitch;
class KRadioGroup;
} // namespace kit
enum class PropertiesRowControl : std::uint8_t { Automatic, SegmentedEnum, Stepper };
[[nodiscard]] PropertiesRowControl propertiesRowControl(std::string_view schemaKey);
QList<std::pair<QString, std::int64_t>> propertiesSelectorItems(std::string_view schemaKey);
class PropertiesRegistryRow final : public QWidget {
  public:
    PropertiesRegistryRow(CompositionSession& session, document::NodeId node,
                          document::ParameterId parameter, document::ParameterDefinition definition,
                          QWidget* parent);
    void refresh();
    void reset();

  private:
    void commit();
    [[nodiscard]] double displayScale() const;
    bool eventFilter(QObject* watched, QEvent* event) override;
    CompositionSession& session_;
    document::NodeId node_;
    document::ParameterId parameter_;
    document::ParameterDefinition definition_;
    std::array<kit::KValueField*, 4> fields_{};
    kit::KRadioGroup* segments_ = nullptr;
    kit::KDropdown* selector_ = nullptr;
    kit::KSwitch* toggle_ = nullptr;
    kit::KColorChip* color_ = nullptr;
    QLineEdit* text_ = nullptr;
    QLineEdit* integer_ = nullptr;
    QPlainTextEdit* multiline_ = nullptr;
    KeyframeDiamond* diamond_ = nullptr;
    bool refreshing_ = false;
    bool scrubbing_ = false;
};
} // namespace bloom::ui
