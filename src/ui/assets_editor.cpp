#include <bloom/ui/assets_editor.hpp>

#include <bloom/ui/composition_session.hpp>

#include <bloom/document/project.hpp>

#include <QLabel>
#include <QListWidget>
#include <QListWidgetItem>
#include <QSignalBlocker>
#include <QVBoxLayout>
#include <QVariant>

namespace bloom::ui {
namespace {

constexpr int kCompositionIdRole = Qt::UserRole + 1;

} // namespace

AssetsEditor::AssetsEditor(CompositionSession& session, QWidget* parent)
    : QWidget(parent), session_(session) {
    setObjectName("assetsEditor");
    setAccessibleName(tr("Project assets editor"));

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(8, 8, 8, 8);
    layout->setSpacing(7);
    auto* title = new QLabel(tr("Project"), this);
    title->setObjectName("editorSectionTitle");
    compositions_ = new QListWidget(this);
    compositions_->setObjectName("compositionList");
    compositions_->setAccessibleName(tr("Project compositions"));
    compositions_->setAlternatingRowColors(true);
    layout->addWidget(title);
    layout->addWidget(compositions_, 1);

    connect(compositions_, &QListWidget::currentItemChanged, this,
            [this](QListWidgetItem* current) {
                if (!rebuilding_ && current != nullptr) {
                    (void)session_.setComposition(document::CompositionId::fromRaw(
                        current->data(kCompositionIdRole).toULongLong()));
                }
            });
    connect(&session_, &CompositionSession::snapshotChanged, this, &AssetsEditor::rebuild);
    connect(&session_, &CompositionSession::compositionChanged, this,
            &AssetsEditor::updateSelection);

    rebuild();
}

void AssetsEditor::rebuild() {
    rebuilding_ = true;
    compositions_->clear();
    for (const auto& composition : session_.snapshot().project().compositions()) {
        auto* item = new QListWidgetItem(QString::fromStdString(composition.name()), compositions_);
        item->setData(kCompositionIdRole,
                      QVariant::fromValue<qulonglong>(composition.id().value()));
        item->setToolTip(tr("Composition %1").arg(composition.id().value()));
    }
    updateSelection();
    rebuilding_ = false;
}

void AssetsEditor::updateSelection() {
    const QSignalBlocker blocker(compositions_);
    for (int index = 0; index < compositions_->count(); ++index) {
        auto* item = compositions_->item(index);
        if (item->data(kCompositionIdRole).toULongLong() == session_.compositionId().value()) {
            compositions_->setCurrentItem(item);
            return;
        }
    }
    compositions_->setCurrentItem(nullptr);
}

} // namespace bloom::ui
