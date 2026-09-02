#pragma once

#include <QDialog>

class QListWidget;
class QPlainTextEdit;

namespace bloom::ui {

// "Open Source Licenses..." (task U2, issue #118, decision 3): a read-only, kit-styled dialog
// listing every shipped third-party component (bloom::ui::thirdPartyLicenseCatalog(), embedded at
// CMake configure time from the real license records -- see
// generate_third_party_license_catalog.cmake) with its complete license text. No network access.
class LicensesWindow final : public QDialog {
    Q_OBJECT

  public:
    explicit LicensesWindow(QWidget* parent = nullptr);

  private:
    QListWidget* componentList_ = nullptr;
    QPlainTextEdit* licenseText_ = nullptr;
};

} // namespace bloom::ui
