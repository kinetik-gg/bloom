#pragma once

#include <bloom/ui/kit/dialog.hpp>

class QListWidget;
class QPlainTextEdit;

namespace bloom::ui {

// "Open Source Licenses..." (task U2, issue #118, decision 3): a read-only, kit-styled dialog
// listing every shipped third-party component (bloom::ui::thirdPartyLicenseCatalog(), embedded at
// CMake configure time from the real license records -- see
// generate_third_party_license_catalog.cmake) with its complete license text. No network access.
// A non-modal kit::KDialog with a Close button; MainWindow keeps one and raises it when asked
// again.
class LicensesWindow final : public kit::KDialog {
    Q_OBJECT

  public:
    explicit LicensesWindow(QWidget& owner);

  private:
    QListWidget* componentList_ = nullptr;
    QPlainTextEdit* licenseText_ = nullptr;
};

} // namespace bloom::ui
