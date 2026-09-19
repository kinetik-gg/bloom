#pragma once

#include <bloom/ui/application_preferences.hpp>

#include <QDialog>

#include <functional>
#include <vector>

class QPushButton;
class QStackedWidget;
class QVBoxLayout;
class QWidget;

namespace bloom::ui {

class AccelerationStatusProvider;

namespace kit {
class KDropdown;
class KRadioGroup;
class KSwitch;
class KValueField;
} // namespace kit

// The application's Preferences window: a left category rail over a stack of scrollable pages, each
// built from the same kit::KSection and Properties row vocabulary the rest of the application uses.
//
// The window is a pure editor of an ApplicationPreferences value. It never reads or writes
// QSettings itself; the caller supplies the current value and persists what preferences() returns.
// That keeps the dialog testable without a preferences file and keeps persistence ownership in one
// place (MainWindow, which broadcasts the result to live panels).
class PreferencesWindow final : public QDialog {
    Q_OBJECT

  public:
    // `accelerationStatus` may be null, in which case the Performance page reports the CPU-only
    // truth. It is borrowed and must outlive the dialog.
    explicit PreferencesWindow(const ApplicationPreferences& current,
                               const AccelerationStatusProvider* accelerationStatus = nullptr,
                               QWidget* parent = nullptr);

    [[nodiscard]] ApplicationPreferences preferences() const;
    // Pushes a value into every control. Signals are blocked while refreshing, so this never
    // writes back into the draft as a side effect.
    void setPreferences(const ApplicationPreferences& preferences);

  signals:
    // Emitted when Apply or OK commits the edited value. Apply keeps the window open; OK accepts
    // after emitting.
    void preferencesApplied(const ApplicationPreferences& preferences);

  private:
    struct Page final {
        QString id;
        QString title;
        QWidget* widget = nullptr;
    };

    [[nodiscard]] std::vector<Page> buildPages();
    QWidget* buildGeneralPage();
    QWidget* buildMemoryPage();
    QWidget* buildTimelinePage();
    QWidget* buildNodeGraphPage();
    QWidget* buildViewerPage();
    QWidget* buildPerformancePage();
    [[nodiscard]] QWidget* makeScrollPage();

    // Row helpers. Each returns the control it built and registers a refresh callback that
    // re-reads the draft with signals blocked.
    kit::KValueField* addValueFieldRow(QVBoxLayout* layout, QWidget* parent,
                                       const QString& objectName, const QString& label,
                                       double minimum, double maximum, int decimals,
                                       const QString& unit, bool restartRequired);
    kit::KDropdown* addDropdownRow(QVBoxLayout* layout, QWidget* parent, const QString& objectName,
                                   const QString& label);
    kit::KSwitch* addSwitchRow(QVBoxLayout* layout, QWidget* parent, const QString& objectName,
                               const QString& label, bool restartRequired);

    void refreshControls();
    void commitPreferences();
    void updateApplyEnabled();

    ApplicationPreferences draft_;
    ApplicationPreferences applied_;
    const AccelerationStatusProvider* accelerationStatus_ = nullptr;
    QStackedWidget* pages_ = nullptr;
    kit::KRadioGroup* categoryRail_ = nullptr;
    QPushButton* applyButton_ = nullptr;
    std::vector<std::function<void()>> refreshCallbacks_;
};

} // namespace bloom::ui
