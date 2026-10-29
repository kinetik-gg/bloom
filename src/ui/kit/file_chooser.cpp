#include <bloom/ui/kit/file_chooser.hpp>

#include <bloom/ui/kit/icons.hpp>
#include <bloom/ui/kit/network_shares.hpp>
#include <bloom/ui/kit/painting.hpp>
#include <bloom/ui/kit/tokens.hpp>

#include <QAbstractFileIconProvider>
#include <QComboBox>
#include <QHeaderView>
#include <QIcon>
#include <QLineEdit>
#include <QListView>
#include <QPalette>
#include <QPushButton>
#include <QSplitter>
#include <QStyledItemDelegate>
#include <QToolButton>
#include <QTreeView>

#include <array>
#include <utility>

#include <QDialog>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QSettings>
#include <QWidget>

namespace bloom::ui::kit {
namespace {

[[nodiscard]] QString settingsKey(const QString& kind) {
    return QStringLiteral("dialogs/directories/") + kind;
}

[[nodiscard]] QString startDirectory(const KFileRequest& request) {
    if (!request.startPath.isEmpty())
        return request.startPath;
    return request.kind.isEmpty() ? QString{} : rememberedDirectory(request.kind);
}

// A colour as a QSS value.
[[nodiscard]] QString css(const QColor& value) {
    if (!value.isValid())
        return QStringLiteral("transparent");
    return value.alpha() == 255
               ? value.name(QColor::HexRgb)
               : QStringLiteral("rgba(%1, %2, %3, %4)")
                     .arg(value.red())
                     .arg(value.green())
                     .arg(value.blue())
                     .arg(QString::number(static_cast<double>(value.alphaF()), 'f', 3));
}

[[nodiscard]] QString colours(const ButtonColors& colors) {
    return QStringLiteral("color: %1; background: %2; border-color: %3;")
        .arg(css(colors.ink), css(colors.fill), css(colors.border));
}

// The file dialog's own sheet, scoped to it: its look-in and file-type combos take the
// Secondary button recipe (the global QComboBox rules were retired with KDropdown), its views
// sit in a Field well, and its sidebar on Surface. Everything comes from tokens and recipes.
[[nodiscard]] QString fileDialogSheet() {
    const auto button = [](const State state) {
        return colours(buttonColors(ButtonKind::Secondary, state, false));
    };
    const auto radius = radiusPx(Radius::Small, 0);
    const auto inset = px(Spacing::ControlInset);
    QString sheet;
    sheet += QStringLiteral("QFileDialog QComboBox { border: %1px solid transparent; "
                            "border-radius: %2px; margin: %3px; padding: 0px %4px; %5 }\n")
                 .arg(static_cast<int>(kHairlineWidth))
                 .arg(radius)
                 .arg(inset)
                 .arg(px(Spacing::S))
                 .arg(button(State::Normal));
    sheet += QStringLiteral("QFileDialog QComboBox:hover { %1 }\n").arg(button(State::Hover));
    sheet += QStringLiteral("QFileDialog QComboBox:on { %1 }\n").arg(button(State::Pressed));
    sheet += QStringLiteral("QFileDialog QComboBox:disabled { %1 }\n").arg(button(State::Disabled));
    sheet += QStringLiteral("QFileDialog QComboBox::drop-down { border: none; width: %1px; }\n")
                 .arg(px(Size::IconMedium) + px(Spacing::XS));
    sheet +=
        QStringLiteral("QFileDialog QComboBox QAbstractItemView { background: %1; color: %2; "
                       "border: %3px solid %4; selection-background-color: %5; "
                       "selection-color: %6; outline: none; }\n")
            .arg(css(color(Color::SurfaceRaised)), css(color(Color::Foreground)))
            .arg(static_cast<int>(kHairlineWidth))
            .arg(css(color(Color::Border)), css(color(Color::Accent)), css(color(Color::OnAccent)));
    sheet += QStringLiteral("QFileDialog QTreeView, QFileDialog QListView { background: %1; "
                            "border: none; }\n")
                 .arg(css(color(Color::Field)));
    sheet += QStringLiteral("QFileDialog QListView#sidebar { background: %1; }\n")
                 .arg(css(color(Color::Surface)));
    sheet +=
        QStringLiteral("QFileDialog QLabel { color: %1; }\n").arg(css(color(Color::Foreground)));
    return sheet;
}

// A place in the sidebar: a mounted network share reads as its host/share, the rest as Qt names
// them.
class PlacesDelegate final : public QStyledItemDelegate {
  public:
    using QStyledItemDelegate::QStyledItemDelegate;

  protected:
    void initStyleOption(QStyleOptionViewItem* option, const QModelIndex& index) const override {
        QStyledItemDelegate::initStyleOption(option, index);
        if (const auto label = networkShareLabel(option->text))
            option->text = *label;
    }
};

// Kit glyphs for the dialog's sidebar and file views, in the same muted ink as every list.
class KitFileIconProvider final : public QAbstractFileIconProvider {
  public:
    [[nodiscard]] QIcon icon(const IconType type) const override {
        switch (type) {
        case File:
            return kit::icon(IconId::Image, IconRole::Chrome);
        default:
            return kit::icon(IconId::Folder, IconRole::Chrome);
        }
    }
    [[nodiscard]] QIcon icon(const QFileInfo& info) const override {
        if (info.isDir() || info.isRoot())
            return kit::icon(IconId::Folder, IconRole::Chrome);
        const auto suffix = info.suffix().toLower();
        if (suffix == QStringLiteral("bloom"))
            return kit::icon(IconId::Composition, IconRole::Chrome);
        if (QStringList{QStringLiteral("mov"), QStringLiteral("mp4"), QStringLiteral("mkv"),
                        QStringLiteral("mxf"), QStringLiteral("m4v"), QStringLiteral("mts"),
                        QStringLiteral("m2ts"), QStringLiteral("ts")}
                .contains(suffix))
            return kit::icon(IconId::Clip, IconRole::Chrome);
        if (QStringList{QStringLiteral("wav"), QStringLiteral("mp3"), QStringLiteral("aac")}
                .contains(suffix))
            return kit::icon(IconId::Audio, IconRole::Chrome);
        return kit::icon(IconId::Image, IconRole::Chrome);
    }
};

[[nodiscard]] QAbstractFileIconProvider& fileIcons() {
    // Stateless: every icon comes from the kit's own icon cache on demand.
    static KitFileIconProvider provider;
    return provider;
}

// Kit look and height for every control inside the dialog: Control tall, kit icons on the
// navigation buttons, the kit text-entry recipe on the name field.
void styleFileDialog(QFileDialog& dialog) {
    auto surface = dialog.palette();
    surface.setColor(QPalette::Window, color(Color::SurfaceRaised));
    dialog.setPalette(surface);
    dialog.setAutoFillBackground(true);
    dialog.setStyleSheet(fileDialogSheet());
    dialog.setIconProvider(&fileIcons());
    // A settings-dialog footprint, with the places sidebar wide enough to read.
    dialog.resize(px(Size::DialogWideWidth), px(Size::DialogDocumentHeight));
    if (auto* splitter = dialog.findChild<QSplitter*>(QStringLiteral("splitter")))
        splitter->setSizes({px(Size::DialogSidebarWidth),
                            px(Size::DialogWideWidth) - px(Size::DialogSidebarWidth)});
    const int control = px(Size::Control);
    for (auto* combo : dialog.findChildren<QComboBox*>())
        combo->setFixedHeight(control);
    for (auto* line : dialog.findChildren<QLineEdit*>()) {
        if (qobject_cast<QComboBox*>(line->parentWidget()) != nullptr)
            continue;
        line->setProperty("kitControl", true);
        line->setFixedHeight(control);
    }
    for (auto* button : dialog.findChildren<QPushButton*>()) {
        // Kit buttons carry words only, never the style's standard Open/Save/Cancel pictures.
        button->setIcon(QIcon());
        button->setFixedHeight(control);
    }
    // The places sidebar names a mounted network share as host/share and elides, never scrolls
    // sideways.
    if (auto* sidebar = dialog.findChild<QListView*>(QStringLiteral("sidebar"))) {
        sidebar->setItemDelegate(new PlacesDelegate(sidebar));
        sidebar->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        sidebar->setTextElideMode(Qt::ElideMiddle);
    }
    // The detail view gives the name the room and sizes the other columns to what they hold.
    if (auto* tree = dialog.findChild<QTreeView*>(QStringLiteral("treeView"))) {
        auto* header = tree->header();
        header->setStretchLastSection(false);
        header->setSectionResizeMode(QHeaderView::ResizeToContents);
        header->setSectionResizeMode(0, QHeaderView::Stretch);
    }
    const std::array<std::pair<const char*, IconId>, 6> glyphs{
        {{"backButton", IconId::CaretLeft},
         {"forwardButton", IconId::CaretRight},
         {"toParentButton", IconId::CaretUp},
         {"newFolderButton", IconId::NewFolder},
         {"listModeButton", IconId::Menu},
         {"detailModeButton", IconId::Stack}}};
    for (const auto& [name, glyph] : glyphs)
        if (auto* tool = dialog.findChild<QToolButton*>(QString::fromLatin1(name))) {
            tool->setIcon(icon(glyph, IconRole::Chrome));
            tool->setIconSize(QSize(px(Size::IconMedium), px(Size::IconMedium)));
            tool->setFixedSize(control, control);
            tool->setAutoRaise(true);
        }
}

} // namespace

QString rememberedDirectory(const QString& kind) {
    const auto stored = QSettings().value(settingsKey(kind)).toString();
    return !stored.isEmpty() && QFileInfo(stored).isDir() ? stored : QString{};
}

void rememberDirectory(const QString& kind, const QString& path) {
    if (kind.isEmpty() || path.isEmpty())
        return;
    const QFileInfo info(path);
    QSettings().setValue(settingsKey(kind),
                         info.isDir() ? info.absoluteFilePath() : info.absolutePath());
}

QString withDefaultSuffix(const QString& path, const QString& suffix) {
    if (path.isEmpty() || suffix.isEmpty() || !QFileInfo(path).suffix().isEmpty())
        return path;
    return path + QLatin1Char('.') + suffix;
}

std::unique_ptr<QFileDialog> buildFileDialog(QWidget& owner, const KFileRequest& request) {
    auto dialog = std::make_unique<QFileDialog>(owner.window(), request.title,
                                                startDirectory(request), request.nameFilters);
    // Bloom's own dialog on every platform (owner decision, 2026-10-30): a platform or desktop
    // chooser (a GTK one through a Qt platform theme) can open on another workspace and leave
    // Bloom looking frozen. Qt's widget dialog stays a window-modal child of Bloom's window.
    dialog->setOption(QFileDialog::DontUseNativeDialog);
    dialog->setWindowModality(Qt::WindowModal);
    switch (request.mode) {
    case KFileRequest::Mode::Open:
        dialog->setAcceptMode(QFileDialog::AcceptOpen);
        dialog->setFileMode(QFileDialog::ExistingFile);
        break;
    case KFileRequest::Mode::OpenMany:
        dialog->setAcceptMode(QFileDialog::AcceptOpen);
        dialog->setFileMode(QFileDialog::ExistingFiles);
        break;
    case KFileRequest::Mode::Save:
        dialog->setAcceptMode(QFileDialog::AcceptSave);
        dialog->setFileMode(QFileDialog::AnyFile);
        break;
    case KFileRequest::Mode::Directory:
        dialog->setAcceptMode(QFileDialog::AcceptOpen);
        dialog->setFileMode(QFileDialog::Directory);
        dialog->setOption(QFileDialog::ShowDirsOnly);
        break;
    }
    if (!request.defaultSuffix.isEmpty())
        dialog->setDefaultSuffix(request.defaultSuffix);
    configureFileDialogSidebar(*dialog);
    styleFileDialog(*dialog);
    return dialog;
}

QStringList chooseFiles(QWidget& owner, const KFileRequest& request) {
    const auto dialog = buildFileDialog(owner, request);
    if (dialog->exec() != QDialog::Accepted)
        return {};
    auto files = dialog->selectedFiles();
    if (request.mode == KFileRequest::Mode::Save)
        for (auto& file : files)
            file = withDefaultSuffix(file, request.defaultSuffix);
    if (!files.isEmpty())
        rememberDirectory(request.kind, files.front());
    return files;
}

std::optional<QString> chooseFile(QWidget& owner, const KFileRequest& request) {
    const auto files = chooseFiles(owner, request);
    if (files.isEmpty())
        return std::nullopt;
    return files.front();
}

} // namespace bloom::ui::kit
