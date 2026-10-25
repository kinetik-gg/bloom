#pragma once

// The import progress modal (#377). Every batch import the AssetController prepares (File >
// Import, Add > Import…, the Assets footer, file and folder drops) is shown here: overall progress,
// "3 of 12", the item in progress and Cancel, then a summary when anything was skipped, cancelled
// or refused. The work itself stays on the import worker; the dialog only reads its reports.

#include <bloom/ui/asset_controller.hpp>

#include <QDialog>
#include <QPointer>
#include <QTimer>

#include <chrono>
#include <memory>

class QVBoxLayout;
class QWidget;

namespace bloom::ui {
namespace kit {
class KButton;
class KLabel;
class KProgressBar;
} // namespace kit

class ImportProgressDialog final : public QDialog {
    Q_OBJECT

  public:
    explicit ImportProgressDialog(QWidget* parent = nullptr);

    void showProgress(const AssetController::ImportProgressState& state);
    void showSummary(const AssetController::ImportReport& report);
    [[nodiscard]] bool summarizing() const noexcept { return summarizing_; }
    // Closes the dialog whatever it shows; the artist's own close only stops an import.
    void dismiss();

    // What the dialog says, for tests and accessibility checks.
    [[nodiscard]] QString headingText() const;
    [[nodiscard]] QString detailText() const;
    [[nodiscard]] kit::KProgressBar* progressBar() const noexcept { return bar_; }
    [[nodiscard]] kit::KButton* cancelButton() const noexcept { return cancel_; }
    [[nodiscard]] kit::KButton* closeButton() const noexcept { return close_; }

  Q_SIGNALS:
    // Cancel, Escape or closing the window while the import runs.
    void stopRequested();

  public Q_SLOTS:
    void reject() override;

  protected:
    void closeEvent(QCloseEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;

  private:
    void requestStop();

    kit::KLabel* heading_ = nullptr;
    kit::KLabel* detail_ = nullptr;
    kit::KProgressBar* bar_ = nullptr;
    kit::KLabel* counts_ = nullptr;
    kit::KLabel* note_ = nullptr;
    QWidget* skippedPanel_ = nullptr;
    QVBoxLayout* skippedRows_ = nullptr;
    kit::KButton* cancel_ = nullptr;
    kit::KButton* close_ = nullptr;
    QString detailFull_;
    bool summarizing_ = false;
    bool dismissing_ = false;
};

// Shows the dialog for the controller's batch imports over `window`. An import that finishes
// within kShowDelay shows nothing unless it skipped, cancelled or refused something, so a quick
// import cannot flicker; window input is held back during that delay so the import still blocks.
// An import that added everything closes the dialog and reports through notice().
class ImportProgressPresenter final : public QObject {
    Q_OBJECT

  public:
    static constexpr std::chrono::milliseconds kShowDelay{300};
    static constexpr std::chrono::milliseconds kRefreshInterval{50};

    ImportProgressPresenter(AssetController& controller, QWidget& window);
    ~ImportProgressPresenter() override;

    [[nodiscard]] ImportProgressDialog* dialog() const noexcept { return dialog_; }

  Q_SIGNALS:
    // A one-line outcome for the status bar ("Imported 12 assets").
    void notice(const QString& message);

  private:
    void started();
    void finished(const AssetController::ImportReport& report);
    void showDialog();
    void refresh();
    void holdInput(bool held);
    ImportProgressDialog* ensureDialog();

    QPointer<AssetController> controller_;
    QWidget& window_;
    QTimer showTimer_;
    QTimer refreshTimer_;
    QPointer<ImportProgressDialog> dialog_;
    std::unique_ptr<QObject> inputHold_;
};

} // namespace bloom::ui
