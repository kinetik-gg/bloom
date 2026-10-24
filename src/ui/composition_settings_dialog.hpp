#pragma once

// The one modal for New Composition and Composition Settings (#376). It edits a
// commands::CompositionSettings value and, in settings mode, the resize anchor; the caller turns
// the result into exactly one command. Every value is exact: a frame rate is a rational, a duration
// and a start timecode are rational times, and a unit switch only re-displays them.

#include "composition_editor_support.hpp"

#include <bloom/commands/composition_settings_operations.hpp>
#include <bloom/ui/composition_presets.hpp>

#include <QDialog>
#include <QString>

#include <optional>
#include <vector>

class QDialogButtonBox;
class QFormLayout;
class QLabel;
class QLineEdit;

namespace bloom::ui {
namespace kit {
class KAnchorGrid;
class KCheckBox;
class KColorChip;
class KDropdown;
class KIconButton;
class KIconToggle;
class KSection;
class KValueField;
} // namespace kit

class CompositionSettingsDialog final : public QDialog {
  public:
    enum class Mode : std::uint8_t { Create, Edit };
    // The unit the Start and Duration fields are entered in. Indices are the unit dropdown's.
    enum class TimeUnit : std::uint8_t { Frames = 0, Seconds = 1, Timecode = 2 };

    CompositionSettingsDialog(Mode mode, commands::CompositionSettings initial,
                              std::vector<WorkingColorSpaceChoice> workingSpaces,
                              QWidget* parent = nullptr);

    [[nodiscard]] Mode mode() const noexcept { return mode_; }
    // The settings the fields describe, or nullopt while any of them is invalid.
    [[nodiscard]] std::optional<commands::CompositionSettings> settings() const;
    [[nodiscard]] commands::ResizeAnchor anchor() const;
    // The inline reason the dialog cannot be accepted, empty when it can.
    [[nodiscard]] QString issue() const;
    // The one-line summary under the preset, e.g. "1920 × 1080 · 16:9 · Square pixels · 24 fps ·
    // 10 s (240 frames)"; parts that are currently invalid are left out.
    [[nodiscard]] QString summary() const;

    // Preset management, the seams the preset buttons use (and tests drive without a prompt).
    // Each returns a plain reason when refused.
    [[nodiscard]] std::optional<QString> savePreset(const QString& name);
    [[nodiscard]] std::optional<QString> renameSelectedPreset(const QString& name);
    [[nodiscard]] std::optional<QString> deleteSelectedPreset();
    [[nodiscard]] const std::vector<CompositionPreset>& userPresets() const noexcept {
        return userPresets_;
    }

  private:
    // The field an inline reason belongs under.
    enum class Field : std::uint8_t { None, Name, Size, Rate, Start, Duration, Safe };
    struct Issue final {
        Field field = Field::None;
        QString reason;
    };
    [[nodiscard]] Issue currentIssue() const;
    [[nodiscard]] QLabel* formLabel(const QString& text, QWidget* parent);
    [[nodiscard]] QWidget* withError(QWidget* field, QLabel*& error, const QString& objectName);
    void syncRatePreset();
    void showPresetRefusal(const QString& reason);
    void buildBasic(kit::KSection& section);
    void buildAdvanced(kit::KSection& section);
    void rebuildPresetItems();
    void selectMatchingPreset();
    void applySelectedPreset();
    void applySettings(const commands::CompositionSettings& settings);
    void displayDuration();
    void displayStart();
    void readDuration();
    void readStart();
    void refresh();
    [[nodiscard]] std::optional<document::FrameRate> rate() const;
    [[nodiscard]] TimeUnit unit() const;
    [[nodiscard]] std::optional<core::PixelAspectRatio> pixelAspect() const;
    void persistPresets();

    Mode mode_;
    commands::CompositionSettings initial_;
    std::vector<CompositionPreset> builtIns_;
    std::vector<CompositionPreset> userPresets_;
    std::optional<core::RationalTime> duration_;
    std::optional<core::RationalTime> start_;
    double aspectLockRatio_ = 1.0;
    bool updating_ = false;

    QLineEdit* name_ = nullptr;
    kit::KDropdown* preset_ = nullptr;
    kit::KIconButton* savePreset_ = nullptr;
    kit::KIconButton* renamePreset_ = nullptr;
    kit::KIconButton* deletePreset_ = nullptr;
    kit::KValueField* width_ = nullptr;
    kit::KValueField* height_ = nullptr;
    kit::KIconToggle* lockAspect_ = nullptr;
    kit::KDropdown* pixelAspect_ = nullptr;
    QLineEdit* frameRate_ = nullptr;
    kit::KDropdown* ratePresets_ = nullptr;
    kit::KCheckBox* dropFrame_ = nullptr;
    QLineEdit* startField_ = nullptr;
    QLineEdit* durationField_ = nullptr;
    kit::KDropdown* unit_ = nullptr;
    kit::KColorChip* background_ = nullptr;
    kit::KSection* advanced_ = nullptr;
    kit::KDropdown* workingSpace_ = nullptr;
    kit::KAnchorGrid* anchorGrid_ = nullptr;
    kit::KValueField* actionSafe_ = nullptr;
    kit::KValueField* titleSafe_ = nullptr;
    QLabel* summary_ = nullptr;
    QLabel* backgroundHex_ = nullptr;
    std::vector<QLabel*> formLabels_;
    QLabel* nameError_ = nullptr;
    QLabel* presetError_ = nullptr;
    QLabel* sizeError_ = nullptr;
    QLabel* rateError_ = nullptr;
    QLabel* startError_ = nullptr;
    QLabel* durationError_ = nullptr;
    QLabel* safeError_ = nullptr;
    QDialogButtonBox* buttons_ = nullptr;
};

} // namespace bloom::ui
