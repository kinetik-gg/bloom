#include <bloom/ui/licenses_window.hpp>

#include <bloom/ui/kit/button.hpp>
#include <bloom/ui/kit/dialog.hpp>
#include <bloom/ui/kit/tokens.hpp>
#include <bloom/ui/third_party_license_catalog.hpp>

#include <QHBoxLayout>
#include <QListWidget>
#include <QListWidgetItem>
#include <QPlainTextEdit>
#include <QString>
#include <QVBoxLayout>

namespace bloom::ui {
namespace {

[[nodiscard]] QString toQString(const std::string_view text) {
    return QString::fromUtf8(text.data(), static_cast<qsizetype>(text.size()));
}

} // namespace

LicensesWindow::LicensesWindow(QWidget& owner)
    : kit::KDialog(tr("Open Source Licenses"), QStringLiteral("licensesWindow"), owner,
                   Qt::NonModal, Width::Wide) {
    // A document to read, so it opens at a readable height rather than its smallest layout.
    setMinimumHeight(kit::px(kit::Size::DialogDocumentHeight));

    componentList_ = new QListWidget(this);
    componentList_->setObjectName(QStringLiteral("licensesComponentList"));
    kit::styleDocumentView(*componentList_);

    licenseText_ = new QPlainTextEdit(this);
    licenseText_->setObjectName(QStringLiteral("licensesTextView"));
    licenseText_->setReadOnly(true);
    kit::styleDocumentView(*licenseText_);
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

    auto* panes = new QHBoxLayout;
    panes->setContentsMargins(0, 0, 0, 0);
    panes->setSpacing(kit::px(kit::Spacing::M));
    panes->addWidget(componentList_, 1);
    panes->addWidget(licenseText_, 3);
    contentLayout()->addLayout(panes, 1);
    addButton(Role::Reject, tr("Close"))->setObjectName(QStringLiteral("licensesCloseButton"));
    setDefaultRole(Role::Reject);
    setInitialFocus(componentList_);

    if (componentList_->count() > 0) {
        componentList_->setCurrentRow(0);
    }
}

} // namespace bloom::ui
