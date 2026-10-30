#pragma once

#include <bloom/ui/application_preferences.hpp>
#include <bloom/ui/kit/dialog.hpp>

#include <functional>
#include <initializer_list>
#include <vector>

class QListWidget;
class QScrollArea;
class QStackedWidget;
class QVBoxLayout;
class QWidget;
class QLabel;
class QTimer;
class QShowEvent;
class QHideEvent;

namespace bloom::ui {

class AccelerationStatusProvider;

namespace kit {
class KButton;
class KDropdown;
class KSection;
class KSwitch;
class KValueField;
} // namespace kit

// The application's Preferences window: a left category rail over a stack of scrollable pages, each
// built from the same kit::KSection and Properties row vocabulary the rest of the application uses.
//
// The window is a pure editor of an ApplicationPreferences value. It never reads or writes
// QSettings itself; the caller supplies the current value and persists what preferences() returns.
// That keeps the dialog testable without a preferences file and keeps persistence ownership in one
// place (MainWindow, which broadcasts the result to live panels). A kit::KDialog shown with exec()
// by MainWindow (the commit path is its preferencesApplied signal).
class PreferencesWindow final : public kit::KDialog {
    Q_OBJECT

  public:
    // `accelerationStatus` may be null, in which case the Performance page reports the CPU-only
    // truth. The injected provider is borrowed and must outlive the dialog.
    PreferencesWindow(QWidget& owner, const ApplicationPreferences& current,
                      const AccelerationStatusProvider* accelerationStatus = nullptr);

    [[nodiscard]] ApplicationPreferences preferences() const;
    // Pushes a value into every control. Signals are blocked while refreshing, so this never
    // writes back into the draft as a side effect.
    void setPreferences(const ApplicationPreferences& preferences);

  signals:
    // Emitted when Apply or OK commits the edited value. Apply keeps the window open; OK accepts
    // after emitting.
    void preferencesApplied(const ApplicationPreferences& preferences);
    // The Sounds page's "Play test sound": carries the edited (unsaved) values so the volume the
    // artist hears is the one they are choosing.
    void soundTestRequested(const ApplicationPreferences& draft);
    // The General page's link to the MCP Server panel, where the port and auto-start are edited.
    void mcpSettingsRequested();

  protected:
    // The Performance page follows the cached acceleration status only while the dialog is
    // visible; the timer is stopped on hide and owned by this dialog, so it never runs after
    // destruction.
    void showEvent(QShowEvent* event) override;
    void hideEvent(QHideEvent* event) override;

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
    QWidget* buildSoundsPage();
    QWidget* buildPerformancePage();
    [[nodiscard]] QWidget* makeScrollPage();

    // One Properties row (KPropertyRow) with its label in the label column all pages share.
    QWidget* addPreferencesRow(QVBoxLayout* layout, QWidget* parent, const QString& label,
                               std::initializer_list<QWidget*> controls);
    // A value whose zero means Automatic: an Automatic / Custom mode and the value with its unit.
    kit::KValueField* addAutomaticRow(QVBoxLayout* layout, QWidget* parent,
                                      const QString& objectName, const QString& label,
                                      double maximum, int decimals, const QString& unit,
                                      double customDefault);

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
    void fitPagesToContent();
    // A page is a Properties panel body: its margins and section spacing, on Background, scrolling.
    static void setPageMargins(QVBoxLayout& layout);
    [[nodiscard]] static QScrollArea* wrapPage(QWidget* content, const QString& objectName);
    // Wires a section's Reset to put back the defaults `assign` copies from the second argument.
    void resetSection(
        kit::KSection& section,
        std::function<void(ApplicationPreferences&, const ApplicationPreferences&)> assign);
    void commitPreferences();
    void updateApplyEnabled();
    // Restore Defaults asks first; the answer arrives asynchronously.
    void confirmRestoreDefaults();
    // Checks a chosen or typed cache directory once (never per keystroke): a file, a folder Bloom
    // cannot write, or one it cannot create is refused under the field.
    void checkCacheDirectory(const QString& directory);

    // Performance page (defined in preferences_window_performance.cpp).
    void refreshPerformancePage();

    ApplicationPreferences draft_;
    ApplicationPreferences applied_;
    const AccelerationStatusProvider* accelerationStatus_ = nullptr;
    QStackedWidget* pages_ = nullptr;
    bool fitted_ = false; // the first show sizes the window to its content
    QListWidget* categoryRail_ = nullptr;
    kit::KButton* applyButton_ = nullptr;
    QWidget* cacheDirectoryField_ = nullptr;
    QString cacheDirectoryIssue_;
    std::vector<std::function<void()>> refreshCallbacks_;
    // Run after the refresh callbacks: mode controls that follow a refreshed value.
    std::vector<std::function<void()>> syncCallbacks_;

    QLabel* performanceBackendValue_ = nullptr;
    QLabel* performanceStateValue_ = nullptr;
    QLabel* performancePresentationValue_ = nullptr;
    QLabel* performanceDeviceValue_ = nullptr;
    QLabel* performanceDriverValue_ = nullptr;
    QWidget* performanceDeviceRow_ = nullptr;
    QWidget* performanceDriverRow_ = nullptr;
    QTimer* performanceRefreshTimer_ = nullptr;
};

} // namespace bloom::ui
