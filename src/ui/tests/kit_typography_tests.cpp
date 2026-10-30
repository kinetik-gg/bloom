// Bloom's type roles win over the platform theme everywhere (visual-language.md, "Type roles").
//
// A platform theme hands Qt a font for the application and one for each widget class it knows
// (QPlatformTheme::font(); qt6ct returns its "general" font for every class, CoreText and Windows
// return smaller item-view and label fonts). This test plays a deliberately hostile theme against
// the real main window and every surface that can be built offscreen, and then records two things
// for each surface: the font every widget resolves, and the font of every run of text actually
// painted (through a recording paint engine, so painted chrome -- rulers, node cards, item-view
// delegates, menus -- is covered as well as widget fonts). Chrome must only ever use the bundled
// faces; text-layer content is the user's and never passes through a Qt font.
//
// BLOOM_TYPOGRAPHY_TABLE=<file> writes the class-to-font table; BLOOM_TYPOGRAPHY_CAPTURE=<prefix>
// saves the Assets panel under the hostile theme as <prefix>-dpr<ratio>.png.
#include "import_progress_dialog.hpp"
#include "preferences_window.hpp"
#include "window_fixture.hpp"

#include <bloom/commands/asset_operations.hpp>
#include <bloom/ui/assets_editor.hpp>
#include <bloom/ui/composition_commands.hpp>
#include <bloom/ui/jobs_editor.hpp>
#include <bloom/ui/kit/dialog_prompts.hpp>
#include <bloom/ui/kit/dropdown.hpp>
#include <bloom/ui/kit/dropdown_popup.hpp>
#include <bloom/ui/kit/fonts.hpp>
#include <bloom/ui/kit/theme.hpp>
#include <bloom/ui/licenses_window.hpp>
#include <bloom/ui/task_monitor_model.hpp>

#include <QAbstractItemView>
#include <QAction>
#include <QApplication>
#include <QDialog>
#include <QDirIterator>
#include <QElapsedTimer>
#include <QFile>
#include <QFontDatabase>
#include <QFontInfo>
#include <QImage>
#include <QLabel>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QPaintDevice>
#include <QPaintEngine>
#include <QRawFont>
#include <QTemporaryDir>
#include <QTextItem>
#include <QTimer>
#include <QToolTip>
#include <QTreeWidget>

#include <filesystem>
#include <functional>
#include <iostream>
#include <map>
#include <memory>
#include <set>

namespace {

using namespace bloom;

// Every class name Qt 6 fills from QPlatformTheme::font() (QApplicationPrivate::
// initializeWidgetFontHash) -- the per-class entries a platform theme can push.
constexpr auto kPlatformFontClasses = std::to_array<const char*>(
    {"QMenu",       "QMenuBar",    "QMenuItem",    "QMessageBox",           "QLabel",
     "QTipLabel",   "QTitleBar",   "QStatusBar",   "QMdiSubWindowTitleBar", "QDockWidgetTitle",
     "QPushButton", "QCheckBox",   "QRadioButton", "QToolButton",           "QAbstractItemView",
     "QListView",   "QHeaderView", "QListBox",     "QComboMenuItem",        "QComboLineEdit",
     "QSmallFont",  "QMiniFont"});

// The families chrome may resolve to: the bundled interface and monospaced families.
const QStringList kChromeFamilies{QStringLiteral("Inter Variable"), QStringLiteral("Geist Mono")};

// An obviously wrong face at an obviously wrong size, in the role a hostile theme plays.
QFont hostileFont() {
    QFont font;
    font.setFamilies({QStringLiteral("Comic Neue"), QStringLiteral("DejaVu Serif")});
    font.setStyleHint(QFont::Serif);
    font.setPointSizeF(15.0);
    return font;
}

// What qt6ct's applySettings() does once the event loop runs (QGuiApplication::setFont, then
// QApplication::setFont with its general font), plus the per-class entries CoreText and Windows
// install, followed by the ThemeChange qt6ct sends every widget.
void playHostileTheme() {
    const QFont hostile = hostileFont();
    QGuiApplication::setFont(hostile);
    QApplication::setFont(hostile);
    for (const char* name : kPlatformFontClasses)
        QApplication::setFont(hostile, name);
    for (QWidget* widget : QApplication::allWidgets()) {
        QEvent change(QEvent::ThemeChange);
        QApplication::sendEvent(widget, &change);
    }
    QApplication::processEvents();
    QApplication::processEvents();
}

// A paint engine that keeps the font of every text item drawn through it and draws nothing.
class TextRecorder final : public QPaintEngine {
  public:
    TextRecorder() : QPaintEngine(QPaintEngine::AllFeatures) {}
    bool begin(QPaintDevice*) override { return true; }
    bool end() override { return true; }
    void updateState(const QPaintEngineState&) override {}
    void drawPixmap(const QRectF&, const QPixmap&, const QRectF&) override {}
    void drawImage(const QRectF&, const QImage&, const QRectF&, Qt::ImageConversionFlags) override {
    }
    void drawTiledPixmap(const QRectF&, const QPixmap&, const QPointF&) override {}
    void drawPath(const QPainterPath&) override {}
    void drawPolygon(const QPointF*, int, PolygonDrawMode) override {}
    void drawPolygon(const QPoint*, int, PolygonDrawMode) override {}
    void drawRects(const QRectF*, int) override {}
    void drawRects(const QRect*, int) override {}
    void drawLines(const QLineF*, int) override {}
    void drawLines(const QLine*, int) override {}
    void drawEllipse(const QRectF&) override {}
    void drawPoints(const QPointF*, int) override {}
    void drawTextItem(const QPointF&, const QTextItem& item) override {
        if (!item.text().trimmed().isEmpty())
            fonts.push_back(item.font());
    }
    [[nodiscard]] Type type() const override { return QPaintEngine::User; }
    std::vector<QFont> fonts;
};

class RecordingDevice final : public QPaintDevice {
  public:
    [[nodiscard]] QPaintEngine* paintEngine() const override { return &engine; }
    mutable TextRecorder engine;

  protected:
    [[nodiscard]] int metric(const PaintDeviceMetric metric) const override {
        switch (metric) {
        case PdmWidth:
        case PdmHeight:
            return 8192;
        case PdmWidthMM:
        case PdmHeightMM:
            return 2167;
        case PdmDpiX:
        case PdmDpiY:
        case PdmPhysicalDpiX:
        case PdmPhysicalDpiY:
            return 96;
        case PdmDepth:
            return 32;
        case PdmNumColors:
            return INT_MAX;
        case PdmDevicePixelRatio:
            return 1;
        case PdmDevicePixelRatioScaled:
            return static_cast<int>(devicePixelRatioFScale());
        case PdmDevicePixelRatioF_EncodedA:
        case PdmDevicePixelRatioF_EncodedB:
            return QPaintDevice::metric(metric);
        }
        return 0;
    }
};

// The bundled faces' 'head' tables: the proof that a resolved family is the asset Bloom ships and
// not an identically named system install (this workstation has its own "Geist Mono").
const std::vector<QByteArray>& bundledHeads() {
    static const auto heads = [] {
        std::vector<QByteArray> tables;
        QDirIterator it(QStringLiteral(":/bloom/kit"), {QStringLiteral("*.ttf")}, QDir::Files,
                        QDirIterator::Subdirectories);
        while (it.hasNext()) {
            QFile file(it.next());
            if (file.open(QIODevice::ReadOnly))
                tables.push_back(QRawFont(file.readAll(), 12.0).fontTable("head"));
        }
        return tables;
    }();
    return heads;
}

struct Resolved {
    QString family;
    int pixels = 0;
    // The resolved face is one of the bundled asset files.
    bool shipped = false;
    // ... and it is one of the chrome families.
    bool bundled = false;
};

Resolved resolve(const QFont& font) {
    static std::map<QString, Resolved> cache;
    const QString key = font.key();
    if (const auto found = cache.find(key); found != cache.end())
        return found->second;
    const QFontInfo info(font);
    const QByteArray head = QRawFont::fromFont(font).fontTable("head");
    const bool shipped = std::ranges::find(bundledHeads(), head) != bundledHeads().end();
    Resolved resolved{info.family(), info.pixelSize(), shipped,
                      shipped && kChromeFamilies.contains(info.family())};
    cache.emplace(key, resolved);
    return resolved;
}

class Audit {
  public:
    void expect(const bool condition, const std::string& message) {
        if (!condition) {
            ++failures_;
            std::cerr << "FAIL: " << message << '\n';
        }
    }

    // Records every visible widget under `root` and every text run each one paints.
    void surface(const QString& name, QWidget& root) {
        QApplication::processEvents();
        std::vector<QWidget*> widgets{&root};
        for (QWidget* child : root.findChildren<QWidget*>())
            if (child->isVisibleTo(&root))
                widgets.push_back(child);
        for (QWidget* widget : widgets) {
            if (widget->property("typographyUserFont").toBool())
                continue;
            note(name, className(*widget), QStringLiteral("widget"), widget->font());
            if (widget->width() <= 0 || widget->height() <= 0)
                continue;
            RecordingDevice device;
            widget->render(&device, QPoint(), QRegion(), QWidget::RenderFlags());
            for (const QFont& font : device.engine.fonts)
                note(name, className(*widget), QStringLiteral("painted"), font);
        }
    }

    // Item views: no model hands its delegate a platform font through Qt::FontRole.
    void itemFonts(const QString& name, QAbstractItemView& view) {
        auto* model = view.model();
        if (model == nullptr)
            return;
        std::function<void(const QModelIndex&)> walk = [&](const QModelIndex& parent) {
            for (int row = 0; row < model->rowCount(parent); ++row)
                for (int column = 0; column < model->columnCount(parent); ++column) {
                    const QModelIndex index = model->index(row, column, parent);
                    if (const QVariant data = index.data(Qt::FontRole); data.isValid())
                        note(name, className(view), QStringLiteral("FontRole"),
                             data.value<QFont>());
                    if (column == 0)
                        walk(index);
                }
        };
        walk({});
    }

    void writeTable(const QString& path) const {
        QFile file(path);
        if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate))
            return;
        file.write("| Surface | Class | Source | Resolved family | px | Count | Bundled file | "
                   "Chrome family |\n");
        file.write("| --- | --- | --- | --- | --- | --- | --- | --- |\n");
        for (const auto& [key, count] : rows_)
            file.write(
                QStringLiteral("| %1 | %2 | %3 | %4 | %5 | %6 | %7 | %8 |\n")
                    .arg(std::get<0>(key), std::get<1>(key), std::get<2>(key), std::get<3>(key))
                    .arg(std::get<4>(key))
                    .arg(count)
                    .arg(std::get<5>(key) ? QStringLiteral("yes") : QStringLiteral("no"),
                         std::get<6>(key) ? QStringLiteral("yes") : QStringLiteral("NO"))
                    .toUtf8());
    }

    [[nodiscard]] int failures() const noexcept { return failures_; }
    [[nodiscard]] int leaks() const noexcept { return leaks_; }
    [[nodiscard]] int observations() const noexcept { return observations_; }

  private:
    static QString className(const QWidget& widget) {
        // A scroll area's viewport paints on behalf of its view.
        const QWidget* owner = &widget;
        if (widget.objectName() == QStringLiteral("qt_scrollarea_viewport") &&
            widget.parentWidget() != nullptr)
            owner = widget.parentWidget();
        return QString::fromLatin1(owner->metaObject()->className());
    }

    void note(const QString& surface, const QString& cls, const QString& source,
              const QFont& font) {
        const Resolved resolved = resolve(font);
        ++observations_;
        ++rows_[{surface, cls, source, resolved.family, resolved.pixels, resolved.shipped,
                 resolved.bundled}];
        if (!resolved.bundled) {
            ++leaks_;
            if (reported_.insert({surface, cls, source, resolved.family}).second)
                expect(false, (surface + QStringLiteral(": ") + cls + QStringLiteral(" (") +
                               source + QStringLiteral(") resolves to ") + resolved.family)
                                  .toStdString());
        }
    }

    using Key = std::tuple<QString, QString, QString, QString, int, bool, bool>;
    std::map<Key, int> rows_;
    std::set<std::tuple<QString, QString, QString, QString>> reported_;
    int failures_ = 0;
    int leaks_ = 0;
    int observations_ = 0;
};

// Runs `open` and audits the modal surface it shows, then dismisses it. `open` may block in a
// modal exec() or return at once with a window-modal kit dialog shown asynchronously; the poll
// keeps running until the modal appears either way.
void auditModal(Audit& audit, const QString& name, const std::function<void()>& open) {
    bool seen = false;
    QTimer poll;
    poll.setInterval(10);
    QObject::connect(&poll, &QTimer::timeout, [&] {
        QWidget* modal = QApplication::activeModalWidget();
        if (modal == nullptr || !modal->isVisible())
            return;
        poll.stop();
        audit.surface(name, *modal);
        seen = true;
        if (auto* dialog = qobject_cast<QDialog*>(modal))
            dialog->reject();
        else
            modal->close();
    });
    poll.start();
    open();
    QElapsedTimer waited;
    waited.start();
    while (!seen && waited.elapsed() < 3000)
        QApplication::processEvents(QEventLoop::AllEvents, 10);
    poll.stop();
    audit.expect(seen, (name + QStringLiteral(" opened for the audit")).toStdString());
}

QTreeWidgetItem* findRow(QTreeWidget& tree, const QString& text) {
    for (QTreeWidgetItemIterator it(&tree); *it != nullptr; ++it)
        if ((*it)->text(0) == text)
            return *it;
    return nullptr;
}

// The main window's own Assets panel with two imported stills, one filed in a folder; its context
// menu and the modals it opens (Edit Tags, Input Colour, the kit delete
// confirmation).
void auditAssets(Audit& audit, ui::test::WindowFixture& fixture, const QString& capture) {
    QTemporaryDir directory;
    QImage still(64, 48, QImage::Format_RGBA8888);
    still.fill(Qt::gray);
    const QString plate = directory.filePath(QStringLiteral("Plate.png"));
    const QString reference = directory.filePath(QStringLiteral("Reference.png"));
    audit.expect(directory.isValid() && still.save(plate) && still.save(reference),
                 "the stills to import");
    auto& session = fixture.session;
    commands::Transaction create("Folder", session.snapshot().revision());
    create.emplace<commands::CreateAssetFolder>("Plates");
    const auto folder = session.executeTransaction(std::move(create))
                            .outputId<document::AssetFolderId>(commands::kCreateAssetFolderOutput);
    const auto import = [&](const QString& path, std::optional<document::AssetFolderId> into) {
        commands::Transaction transaction("Import", session.snapshot().revision());
        transaction
            .emplace<commands::ImportAssets>(std::vector<std::filesystem::path>{path.toStdString()},
                                             directory.path().toStdString())
            .setFolder(into);
        return session.executeTransaction(std::move(transaction)).succeeded();
    };
    audit.expect(folder.has_value() && import(plate, {}) && import(reference, folder),
                 "the Assets fixture imports");
    auto* editor = fixture.window->findChild<ui::AssetsEditor*>();
    audit.expect(editor != nullptr, "the main window shows the Assets panel");
    if (editor == nullptr)
        return;
    QApplication::processEvents();
    auto* tree = editor->findChild<QTreeWidget*>(QStringLiteral("assetsTree"));
    audit.expect(tree != nullptr, "the Assets tree exists");
    if (tree == nullptr)
        return;
    tree->expandAll();
    QApplication::processEvents();
    audit.surface(QStringLiteral("Assets"), *editor);
    audit.itemFonts(QStringLiteral("Assets"), *tree);
    if (!capture.isEmpty())
        (void)editor->grab().save(
            capture + QStringLiteral("-dpr%1.png")
                          .arg(QString::number(editor->devicePixelRatioF()).remove('.')));

    const auto viaMenu = [&](const QString& row, const QString& action, const QString& modal) {
        QTreeWidgetItem* item = findRow(*tree, row);
        audit.expect(item != nullptr, ("the Assets row " + row).toStdString());
        if (item == nullptr)
            return;
        // The menu is deleted once its exec() returns, so the action fires from inside it, as a
        // click would, and the modal it opens is audited from there.
        bool offered = false;
        QTimer::singleShot(0, tree, [&] {
            auto* menu = qobject_cast<QMenu*>(QApplication::activePopupWidget());
            if (menu == nullptr)
                return;
            audit.surface(QStringLiteral("Assets context menu"), *menu);
            QAction* chosen = nullptr;
            for (QAction* candidate : menu->actions())
                if (candidate->objectName() == action)
                    chosen = candidate;
            menu->close();
            offered = chosen != nullptr;
            if (chosen != nullptr)
                auditModal(audit, modal, [chosen] { chosen->trigger(); });
        });
        emit tree->customContextMenuRequested(tree->visualItemRect(item).center());
        QApplication::processEvents();
        audit.expect(offered, ("the context menu offers " + action).toStdString());
    };
    viaMenu(QStringLiteral("Plate"), QStringLiteral("assetsEditTagsAction"),
            QStringLiteral("Edit Tags"));
    viaMenu(QStringLiteral("Plate"), QStringLiteral("assetsInputColorAction"),
            QStringLiteral("Input Color"));
    // Deleting a folder that holds items asks first, through a kit confirmation.
    viaMenu(QStringLiteral("Plates"), QStringLiteral("assetsDeleteAction"),
            QStringLiteral("Delete confirmation"));
}

void auditWindowPopups(Audit& audit, ui::MainWindow& window) {
    // Every top-level menu of the menu bar.
    if (auto* bar = window.findChild<QMenuBar*>()) {
        for (QAction* action : bar->actions()) {
            QMenu* menu = action->menu();
            if (menu == nullptr)
                continue;
            menu->popup(window.mapToGlobal(QPoint(0, 0)));
            QApplication::processEvents();
            audit.surface(QStringLiteral("Menu ") + action->text().remove('&'), *menu);
            menu->hide();
        }
    }
    // A KDropdown's KPopupList.
    for (auto* dropdown : window.findChildren<ui::kit::KDropdown*>()) {
        if (!dropdown->isVisible() || !dropdown->isEnabled() || dropdown->count() == 0)
            continue;
        dropdown->showPopup();
        QApplication::processEvents();
        if (auto* popup = dropdown->popup(); popup != nullptr && popup->isVisible()) {
            audit.surface(QStringLiteral("KPopupList"), *popup);
            popup->hide();
            break;
        }
    }
    // A tooltip.
    QToolTip::showText(window.mapToGlobal(QPoint(40, 40)), QStringLiteral("Tooltip text"), &window);
    QApplication::processEvents();
    for (QWidget* top : QApplication::topLevelWidgets())
        if (top->inherits("QTipLabel") && top->isVisible())
            audit.surface(QStringLiteral("Tooltip"), *top);
    QToolTip::hideText();
}

void auditDialogs(Audit& audit, ui::test::WindowFixture& fixture) {
    ui::PreferencesWindow preferences(*fixture.window, ui::ApplicationPreferences{});
    preferences.show();
    audit.surface(QStringLiteral("Preferences"), preferences);
    preferences.hide();

    ui::LicensesWindow licenses(*fixture.window);
    licenses.show();
    audit.surface(QStringLiteral("Licenses"), licenses);
    licenses.hide();

    ui::ImportProgressDialog progress(*fixture.window);
    progress.show();
    audit.surface(QStringLiteral("Import progress"), progress);
    progress.dismiss();

    QMessageBox box(QMessageBox::Warning, QStringLiteral("Delete"),
                    QStringLiteral("Delete the selected items?"),
                    QMessageBox::Yes | QMessageBox::No | QMessageBox::Cancel, fixture.window.get());
    box.setInformativeText(QStringLiteral("This cannot be undone."));
    box.show();
    audit.surface(QStringLiteral("QMessageBox"), box);
    box.hide();

    ui::kit::KConfirmSpec confirmSpec;
    confirmSpec.title = QStringLiteral("Delete");
    confirmSpec.heading = QStringLiteral("Delete the selected items?");
    std::unique_ptr<ui::kit::KDialog> confirm(ui::kit::buildConfirm(*fixture.window, confirmSpec));
    confirm->show();
    audit.surface(QStringLiteral("Kit confirm"), *confirm);
    confirm->hide();

    ui::TaskMonitorModel tasks(fixture.bridge);
    ui::JobsEditor jobs(tasks);
    jobs.resize(480, 240);
    jobs.show();
    audit.surface(QStringLiteral("Jobs"), jobs);
    for (auto* view : jobs.findChildren<QAbstractItemView*>())
        audit.itemFonts(QStringLiteral("Jobs"), *view);
    jobs.hide();

    auditModal(audit, QStringLiteral("Composition Settings"), [&] {
        (void)ui::showCompositionSettingsDialog(fixture.session, fixture.session.compositionId(),
                                                *fixture.window);
    });
}

int run(int argc, char** argv) {
    QApplication app(argc, argv);
    app.setOrganizationName(QStringLiteral("BloomTypographyTests"));
    app.setApplicationName(QStringLiteral("Typography"));
    // A theme that is already hostile when Bloom starts: qt6ct sets its general font while the
    // platform theme is created, before main() installs anything.
    QGuiApplication::setFont(hostileFont());
    for (const char* name : kPlatformFontClasses)
        QApplication::setFont(hostileFont(), name);
    ui::kit::installKinetikTheme(app);
    Audit audit;

    // The application font and every platform class entry resolve to the bundled face.
    for (const char* name : kPlatformFontClasses)
        audit.expect(resolve(QApplication::font(name)).bundled,
                     std::string{"after the theme installs, the class font for "} + name +
                         " is a bundled face (got " +
                         resolve(QApplication::font(name)).family.toStdString() + ')');

    ui::test::WindowFixture fixture;
    // A label carrying a user's own face (a text-layer font preview) keeps it: role enforcement
    // never overrides a font that was set explicitly.
    QFont userFace;
    userFace.setFamilies({QStringLiteral("DejaVu Serif")});
    auto* userLabel = new QLabel(QStringLiteral("User face"), fixture.window.get());
    userLabel->setProperty("typographyUserFont", true);
    userLabel->setFont(userFace);
    userLabel->show();

    playHostileTheme();
    for (const char* name : kPlatformFontClasses)
        audit.expect(resolve(QApplication::font(name)).bundled,
                     std::string{"after a runtime theme change, the class font for "} + name +
                         " is a bundled face again (got " +
                         resolve(QApplication::font(name)).family.toStdString() + ')');
    audit.expect(resolve(QApplication::font()).bundled,
                 "after a runtime theme change, the application font is a bundled face again");
    audit.expect(userLabel->font().families() == userFace.families(),
                 "an explicitly set user face survives role enforcement");

    fixture.clearInteraction();
    audit.surface(QStringLiteral("Main window"), *fixture.window);
    for (auto* view : fixture.window->findChildren<QAbstractItemView*>())
        audit.itemFonts(QStringLiteral("Main window"), *view);
    auditWindowPopups(audit, *fixture.window);
    auditDialogs(audit, fixture);
    auditAssets(audit, fixture, qEnvironmentVariable("BLOOM_TYPOGRAPHY_CAPTURE"));

    // A later ApplicationFontChange (the platform font changing under a running Bloom) is
    // answered the same way.
    const int leaksBefore = audit.leaks();
    QApplication::setFont(hostileFont());
    QApplication::processEvents();
    QApplication::processEvents();
    audit.surface(QStringLiteral("Main window after ApplicationFontChange"), *fixture.window);
    audit.expect(audit.leaks() == leaksBefore,
                 "an application font change re-applies the type roles");

    if (const QString table = qEnvironmentVariable("BLOOM_TYPOGRAPHY_TABLE"); !table.isEmpty())
        audit.writeTable(table);
    std::cout << "Typography audit: " << audit.observations() << " observations, " << audit.leaks()
              << " platform-font leaks\n";
    return audit.failures() == 0 ? 0 : 1;
}

} // namespace

int main(int argc, char** argv) {
    try {
        return run(argc, argv);
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
