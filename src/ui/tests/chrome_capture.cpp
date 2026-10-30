// Real-window captures of the window chrome (lane C2): the status bar idle, busy and narrow, the
// context bar, each workspace preset's whole window and the open Workspace dropdown. Writes PNGs to
// the directory given as the first argument (or BLOOM_GRAMMAR_ARTIFACT_DIR), suffixed with the
// device pixel ratio.
#include "window_fixture.hpp"

#include <QDir>
#include <QImage>
#include <bloom/ui/context_bar.hpp>
#include <bloom/ui/kit/dropdown.hpp>
#include <bloom/ui/kit/dropdown_popup.hpp>
#include <bloom/ui/kit/theme.hpp>
#include <bloom/ui/viewer_editor.hpp>
#include <bloom/ui/window_status_bar.hpp>
#include <bloom/ui/workspace_presets.hpp>

#include <iostream>

namespace {
using namespace bloom::ui;

QString directory;
QString suffix;

void save(const QPixmap& pixmap, const QString& name) {
    if (!pixmap.save(directory + QLatin1Char('/') + name + QLatin1Char('-') + suffix +
                     QStringLiteral(".png")))
        std::cerr << "could not write " << name.toStdString() << '\n';
}

void settle() {
    for (int pass = 0; pass < 4; ++pass) {
        QCoreApplication::processEvents();
        QTest::qWait(30);
    }
}

} // namespace

int main(int argc, char** argv) {
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication application(argc, argv);
    kit::installKinetikTheme(application);
    directory =
        argc > 1 ? QString::fromLocal8Bit(argv[1]) : QStringLiteral(BLOOM_GRAMMAR_ARTIFACT_DIR);
    QDir().mkpath(directory);
    suffix = QGuiApplication::primaryScreen()->devicePixelRatio() == 1.0 ? QStringLiteral("dpr1")
                                                                         : QStringLiteral("dpr15");
    try {
        test::WindowFixture fixture;
        auto& window = *fixture.window;
        auto* status = window.statusStrip();
        auto* presets = window.workspacePresets();
        // Status bar: idle, then busy (an export notice and a RAM preview run), then narrow.
        settle();
        save(status->grab(), QStringLiteral("status-idle"));
        status->setPersistentMessage(QStringLiteral("Exporting SC020_SH010 · frame 120 of 386"));
        fixture.preview->beginRamPreviewProgress(386);
        fixture.preview->setRamPreviewProgress(120, 120, 120);
        settle();
        save(status->grab(), QStringLiteral("status-busy"));
        // The activity line shows the persistent notice first; show the RAM preview run alone too.
        status->setPersistentMessage({});
        settle();
        save(status->grab(), QStringLiteral("status-busy-ram-preview"));
        fixture.preview->endRamPreviewProgress();
        status->setPersistentMessage({});
        const auto size = window.size();
        window.resize(900, size.height());
        settle();
        save(status->grab(), QStringLiteral("status-narrow"));
        window.resize(size);
        settle();
        // Context bar with the Select tool (no path selected: the handle strip is hidden).
        save(window.contextBar()->grab(), QStringLiteral("context-bar-select"));
        // Each preset's whole window.
        for (const auto& preset : presets->builtInPresets()) {
            presets->apply(preset.name);
            settle();
            save(window.grab(), QStringLiteral("preset-") + preset.name.toLower());
        }
        presets->apply(QStringLiteral("Default"));
        settle();
        presets->saveCurrentAs(QStringLiteral("My layout"));
        settle();
        // The open Workspace dropdown, with a saved layout and the actions.
        auto* field = window.contextBar()->workspaceDropdownForTest();
        field->showPopup();
        settle();
        QPixmap popup = field->popup() != nullptr ? field->popup()->grab() : QPixmap{};
        save(popup, QStringLiteral("workspace-dropdown-popup"));
        save(window.contextBar()->grab(), QStringLiteral("context-bar-workspace"));
        field->hidePopup();
        presets->removeUser(QStringLiteral("My layout"));
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    return 0;
}
