#include <bloom/ui/licenses_window.hpp>

#include <bloom/ui/kit/tokens.hpp>
#include <bloom/ui/third_party_license_catalog.hpp>

#include <QHBoxLayout>
#include <QListWidget>
#include <QListWidgetItem>
#include <QPlainTextEdit>
#include <QString>

namespace bloom::ui {
namespace {

[[nodiscard]] QString toQString(const std::string_view text) {
    return QString::fromUtf8(text.data(), static_cast<qsizetype>(text.size()));
}

} // namespace

LicensesWindow::LicensesWindow(QWidget* parent) : QDialog(parent) {
    setObjectName(QStringLiteral("licensesWindow"));
    setWindowTitle(QStringLiteral("Open Source Licenses"));
    // A whole-dialog default extent, not a chrome token: mirrors MainWindow's own
    // resize(1600, 1000) precedent for top-level window sizing -- the token vocabulary governs
    // control chrome (spacing, radii, control heights), not an arbitrary dialog's overall size.
    resize(760, 520);

    componentList_ = new QListWidget(this);
    componentList_->setObjectName(QStringLiteral("licensesComponentList"));

    licenseText_ = new QPlainTextEdit(this);
    licenseText_->setObjectName(QStringLiteral("licensesTextView"));
    licenseText_->setReadOnly(true);
    licenseText_->setFont(kit::font(kit::TypeRole::Value));

    for (const auto& entry : thirdPartyLicenseCatalog()) {
        auto* item = new QListWidgetItem(toQString(entry.component), componentList_);
        item->setData(Qt::UserRole, toQString(entry.licenseText));
    }

    connect(componentList_, &QListWidget::currentRowChanged, this, [this](const int row) {
        if (row < 0) {
            licenseText_->clear();
            return;
        }
        licenseText_->setPlainText(componentList_->item(row)->data(Qt::UserRole).toString());
    });

    auto* layout = new QHBoxLayout(this);
    layout->setContentsMargins(kit::px(kit::Spacing::M), kit::px(kit::Spacing::M),
                               kit::px(kit::Spacing::M), kit::px(kit::Spacing::M));
    layout->setSpacing(kit::px(kit::Spacing::M));
    layout->addWidget(componentList_, 1);
    layout->addWidget(licenseText_, 3);

    if (componentList_->count() > 0) {
        componentList_->setCurrentRow(0);
    }
}

} // namespace bloom::ui
