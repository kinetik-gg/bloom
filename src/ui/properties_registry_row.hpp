#pragma once
#include <QWidget>
#include <array>
#include <bloom/document/asset.hpp>
#include <bloom/document/node_definition_registry.hpp>
#include <bloom/document/project.hpp>
#include <bloom/document/shape.hpp>
#include <bloom/platform/font_catalog.hpp>
#include <bloom/ui/kit/dropdown.hpp>
#include <bloom/ui/kit/value_field.hpp>
#include <vector>
class QLineEdit;
class QPlainTextEdit;
namespace bloom::ui {
class CompositionSession;
QString imageDimensionsText(const document::AssetRecord* asset);
QString imageRangeText(const document::AssetRecord* asset);
QString imageAssetDisplayName(const document::AssetRecord& asset);
QString mediaLayerDisplayName(const CompositionSession& session, document::LayerId id);
void refreshVideoAssetSelector(kit::KDropdown& selector, const CompositionSession& session,
                               const QString& stored);
void refreshImageAssetSelector(kit::KDropdown& selector, const CompositionSession& session,
                               const QString& stored);
void refreshAudioAssetSelector(kit::KDropdown& selector, const CompositionSession& session,
                               const QString& stored);
void resetPropertiesParameter(CompositionSession& session, document::ParameterId parameter);
void jumpToPropertiesNode(CompositionSession& session, document::NodeId node, QWidget* panel);
[[nodiscard]] document::ShapeKind nodeShapeKind(const document::Composition& composition,
                                                const document::NodeRecord& node);
class KeyframeDiamond;
namespace kit {
class KColorChip;
class KDropdown;
class KSwitch;
class KRadioGroup;
class KSlider;
} // namespace kit
enum class PropertiesRowControl : std::uint8_t { Automatic, SegmentedEnum, Stepper };
enum class PropertiesRowVisibility : std::uint8_t { Visible, Hidden };
[[nodiscard]] PropertiesRowVisibility
propertiesRowVisibility(std::string_view role, std::string_view schemaKey,
                        std::optional<document::ShapeKind> shape = std::nullopt) noexcept;
[[nodiscard]] PropertiesRowControl propertiesRowControl(std::string_view schemaKey);
QList<std::pair<QString, std::int64_t>> propertiesSelectorItems(std::string_view schemaKey);
class PropertiesRegistryRow final : public QWidget {
  public:
    PropertiesRegistryRow(CompositionSession& session, document::NodeId node,
                          document::ParameterId parameter, document::ParameterDefinition definition,
                          QWidget* parent);
    void refresh();
    void reset();
    // CRASH-2: a font row's catalogue poll re-arms itself via QTimer::singleShot for as long as
    // the (process-wide) font scan stays in flight. configureRegistryRows()/configureUpstream()
    // tear a stale row down with setParent(nullptr) + deleteLater(), and deleteLater() only
    // destroys the row once the event loop gets back to its DeferredDelete event -- there is no
    // guarantee that wins the race against the row's own pending poll. Callers MUST call this
    // before orphaning a row for deferred deletion, so a poll that still fires in that window
    // can no longer touch session_ (which may, by then, refer to a destroyed CompositionSession).
    void detachFromSession();

  private:
    void commit();
    void populateFontSelector();
    void pollFontCatalogue();
    [[nodiscard]] double displayScale() const;
    bool eventFilter(QObject* watched, QEvent* event) override;
    CompositionSession& session_;
    document::NodeId node_;
    document::ParameterId parameter_;
    document::ParameterDefinition definition_;
    std::array<kit::KValueField*, 4> fields_{};
    kit::KRadioGroup* segments_ = nullptr;
    kit::KDropdown* selector_ = nullptr;
    std::vector<platform::FontFace> fontFaces_;
    kit::KSlider* slider_ = nullptr;
    kit::KSwitch* toggle_ = nullptr;
    kit::KColorChip* color_ = nullptr;
    QLineEdit* text_ = nullptr;
    QLineEdit* integer_ = nullptr;
    QPlainTextEdit* multiline_ = nullptr;
    KeyframeDiamond* diamond_ = nullptr;
    bool refreshing_ = false;
    bool detached_ = false;
};
} // namespace bloom::ui
