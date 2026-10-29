#pragma once

// The import progress modal (#377). Every batch import the AssetController prepares (File >
// Import, Add > Import…, the Assets footer, file and folder drops) is shown here: overall progress,
// "3 of 12", the item in progress and Cancel, then a summary when anything was skipped, cancelled
// or refused. The work itself stays on the import worker; the dialog only reads its reports. It is
// the kit task dialog (kit::KTaskDialog) with the import's own wording and automation names.

#include <bloom/ui/asset_controller.hpp>

#include <bloom/ui/kit/task_dialog.hpp>

#include <QElapsedTimer>
#include <QPointer>

#include <chrono>
#include <memory>
#include <utility>

class QWidget;

namespace bloom::ui {

class ImportProgressDialog final : public kit::KTaskDialog {
    Q_OBJECT

  public:
    // A file read as one unit (a still, a video, audio, a LUT) has no measurable progress of its
    // own. The bar is busy while such a file is read and nothing in the batch has finished yet, or
    // once the same file has been read for this long; a batch of quick files stays determinate.
    static constexpr std::chrono::milliseconds kItemBusyAfter{400};

    explicit ImportProgressDialog(QWidget& owner);

    void showProgress(const AssetController::ImportProgressState& state);
    void showSummary(const AssetController::ImportReport& report);

  private:
    // The item the last report was reading, and since when, for kItemBusyAfter.
    std::pair<std::size_t, QString> readingItem_;
    QElapsedTimer readingSince_;
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
    kit::KTaskTiming timing_;
    QPointer<ImportProgressDialog> dialog_;
    std::unique_ptr<QObject> inputHold_;
};

} // namespace bloom::ui
