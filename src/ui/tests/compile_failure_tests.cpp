// The compile-failure details (docs/ux/ui-grammar.md, "Compile failure details"): the status bar's
// Details disclosure, the popup's words, Select, Copy details, and the absence of raw codes in
// anything visible. Set BLOOM_COMPILE_FAILURE_CAPTURE_DIR to also write PNGs of the bar and popup.
#include <bloom/commands/command_stack.hpp>
#include <bloom/core/color.hpp>
#include <bloom/document/document.hpp>
#include <bloom/document/new_project.hpp>
#include <bloom/ui/compile_failure.hpp>
#include <bloom/ui/composition_preview_controller.hpp>
#include <bloom/ui/composition_session.hpp>
#include <bloom/ui/kit/button.hpp>
#include <bloom/ui/kit/controls.hpp>
#include <bloom/ui/kit/popup_window.hpp>
#include <bloom/ui/kit/theme.hpp>
#include <bloom/ui/window_status_bar.hpp>

#include <QApplication>
#include <QClipboard>
#include <QDir>
#include <QTest>
#include <QVBoxLayout>

#include <iostream>
#include <source_location>

namespace {

namespace ui = bloom::ui;
namespace doc = bloom::document;

int gFailures = 0;
void expect(const bool condition, const std::string_view message,
            const std::source_location where = std::source_location::current()) {
    if (condition)
        return;
    ++gFailures;
    std::cerr << where.file_name() << ':' << where.line() << ": " << message << '\n';
}

struct Fixture {
    explicit Fixture()
        : project(doc::makeNewProject("Diag", "Main", bloom::core::RationalTime::fromInteger(10))),
          compositionId(project.initialCompositionId), document(std::move(project.project)),
          stack(document), session(document, stack, compositionId) {
        expect(session.addSolidLayer(QStringLiteral("Title"), bloom::core::Color4d{1, 1, 1, 1}),
               "a solid layer is added");
        const auto* composition = session.composition();
        for (const auto& node : composition->graph().nodes()) {
            if (!node.parameters.empty() && session.layerForNode(node.id)) {
                nodeId = node.id;
                parameterId = node.parameters.front().parameterId;
                role = QString::fromStdString(node.parameters.front().role);
                layerId = *session.layerForNode(node.id);
                break;
            }
        }
        expect(nodeId.isValid(), "a layer node with a parameter exists");
    }

    // The preview state a failed compile publishes: a compile TaskDiagnostic with a plain detail
    // and the subject's ids as data.
    ui::CompositionPreviewState failedState() const {
        ui::CompositionPreviewState state;
        state.activity = ui::PreviewActivity::Failed;
        state.diagnostics.push_back(
            {.code = "bloom.runtime.compile.topology-invariant",
             .severity = bloom::runtime::DiagnosticSeverity::Error,
             .summary = "Validated graph could not be lowered",
             .detail = "A reachable node lost its registered definition.",
             .suggestedAction = {},
             .subject = bloom::runtime::TaskDiagnosticSubject{.node = nodeId.value(),
                                                              .parameter = parameterId.value()}});
        state.message = QStringLiteral("Validated graph could not be lowered");
        return state;
    }

    doc::NewProject project;
    doc::CompositionId compositionId;
    doc::Document document;
    bloom::commands::CommandStack stack;
    ui::CompositionSession session;
    doc::NodeId nodeId;
    doc::ParameterId parameterId;
    doc::LayerId layerId;
    QString role;
};

bool hasRawCode(const QString& text) {
    return text.contains(QStringLiteral("bloom.runtime")) || text.contains(QStringLiteral("node="))
           || text.contains(QStringLiteral("parameter="));
}

void capture(QWidget& widget, const QString& name) {
    const auto directory = qEnvironmentVariable("BLOOM_COMPILE_FAILURE_CAPTURE_DIR");
    if (directory.isEmpty())
        return;
    QDir().mkpath(directory);
    const QString scale = qEnvironmentVariable("QT_SCALE_FACTOR", QStringLiteral("1")).remove('.');
    const bool ok = widget.grab().toImage().save(
        directory + QStringLiteral("/%1-dpr%2.png").arg(name, scale));
    expect(ok, "the capture is written");
}

void testAdapter() {
    Fixture f;
    const auto failure = ui::compileFailureFromPreview(f.failedState());
    expect(failure.has_value(), "a compile failure is recognised");
    if (!failure)
        return;
    expect(failure->headline == QStringLiteral("Validated graph could not be lowered"), "headline");
    expect(failure->detail == QStringLiteral("A reachable node lost its registered definition."),
           "detail has no ids");
    expect(failure->subject.node == f.nodeId.value() && failure->subject.parameter == f.parameterId.value(),
           "subject ids carried");
    expect(failure->raw.contains(QStringLiteral("bloom.runtime.compile.topology-invariant")) &&
               failure->raw.contains(QStringLiteral("node=")),
           "raw keeps the code and ids");
    expect(!hasRawCode(failure->headline) && !hasRawCode(failure->detail), "no raw codes in prose");
    const auto words = ui::compileFailureSubjectText(*failure, f.session);
    expect(words.startsWith(QStringLiteral("Layer 'Title'")), "subject names the layer");

    auto ready = f.failedState();
    ready.activity = ui::PreviewActivity::Ready;
    expect(!ui::compileFailureFromPreview(ready).has_value(), "a ready preview has no failure");
    auto other = f.failedState();
    other.diagnostics.front().code = "bloom.preview.pipeline.invalid-result";
    expect(!ui::compileFailureFromPreview(other).has_value(), "a non-compile failure is not a compile failure");
}

void testStatusBarPopup() {
    Fixture f;
    QWidget host;
    auto* layout = new QVBoxLayout(&host);
    layout->addStretch(1);
    ui::WindowStatusBar bar(f.session, nullptr);
    layout->addWidget(&bar);
    host.resize(900, 240);
    host.show();
    QTest::qWait(20);

    expect(!bar.compileDetailsVisibleForTest(), "no failure: no disclosure");
    bar.applyPreviewStateForTest(f.failedState());
    expect(bar.compileDetailsVisibleForTest(), "failure: the disclosure appears");
    expect(bar.previewStateTextForTest() == QStringLiteral("Validated graph could not be lowered"),
           "the headline stays a plain sentence");
    expect(!hasRawCode(bar.previewStateTextForTest()), "no raw code in the status text");
    capture(host, QStringLiteral("status-bar-failed"));

    auto* details = bar.findChild<ui::kit::KButton*>(QStringLiteral("windowStatusBarCompileDetails"));
    expect(details != nullptr, "the disclosure button exists");
    if (details == nullptr)
        return;
    details->click();
    auto* popup = qobject_cast<ui::kit::KPopupWindow*>(bar.compileFailurePopupForTest());
    expect(popup != nullptr && popup->isVisible(), "clicking Details opens the popup");
    if (popup == nullptr)
        return;
    QTest::qWait(20);
    expect(popup->anchorRect().isValid(), "the popup is anchored to the bar");
    const auto texts = [&] {
        QStringList out;
        for (auto* label : popup->findChildren<QLabel*>())
            out << label->text();
        for (auto* button : popup->findChildren<ui::kit::KButton*>())
            out << button->text();
        return out;
    }();
    const auto joined = texts.join(QLatin1Char('\n'));
    expect(joined.contains(QStringLiteral("Validated graph could not be lowered")), "popup headline");
    expect(joined.contains(QStringLiteral("A reachable node lost its registered definition.")), "popup detail");
    expect(joined.contains(QStringLiteral("Layer 'Title'")), "popup subject in words");
    expect(!hasRawCode(joined), "no raw codes in any visible popup text");
    capture(*popup, QStringLiteral("popup"));

    // Copy details puts the raw diagnostic on the clipboard.
    QGuiApplication::clipboard()->clear();
    popup->findChild<ui::kit::KButton*>(QStringLiteral("compileFailureCopy"))->click();
    const auto copied = QGuiApplication::clipboard()->text();
    expect(copied.contains(QStringLiteral("bloom.runtime.compile.topology-invariant")) &&
               copied.contains(QStringLiteral("node=%1").arg(f.nodeId.value())),
           "Copy details copies the raw diagnostic");
    QTest::qWait(20);
    expect(bar.compileFailurePopupForTest() == nullptr, "Copy details closes the popup");

    // Select picks the failing layer through the session's selection.
    f.session.clearSelection();
    details->click();
    popup = qobject_cast<ui::kit::KPopupWindow*>(bar.compileFailurePopupForTest());
    expect(popup != nullptr, "the popup reopens");
    if (popup != nullptr) {
        popup->findChild<ui::kit::KButton*>(QStringLiteral("compileFailureSelect"))->click();
        expect(f.session.selection().contextualLayer == f.layerId, "Select picks the failing layer");
        QTest::qWait(20);
        expect(bar.compileFailurePopupForTest() == nullptr, "Select closes the popup");
    }

    // A recovered preview removes the disclosure.
    auto ready = f.failedState();
    ready.activity = ui::PreviewActivity::Ready;
    bar.applyPreviewStateForTest(ready);
    expect(!bar.compileDetailsVisibleForTest(), "recovered: the disclosure goes away");
}

void testGoneSubject() {
    Fixture f;
    auto state = f.failedState();
    state.diagnostics.front().detail = "Gone.";
    state.diagnostics.front().subject = bloom::runtime::TaskDiagnosticSubject{.node = 999999};
    const auto failure = ui::compileFailureFromPreview(state);
    expect(failure.has_value() && ui::compileFailureSubjectText(*failure, f.session).isEmpty(),
           "a vanished node has no subject text");
    expect(failure && !ui::selectCompileFailureSubject(f.session, *failure),
           "Select reports a vanished node");
}

} // namespace

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    app.setOrganizationName("BloomCompileFailureTests");
    app.setApplicationName("CompileFailure");
    ui::kit::installKinetikTheme(app);
    testAdapter();
    testStatusBarPopup();
    testGoneSubject();
    if (gFailures != 0)
        std::cerr << gFailures << " failure(s)\n";
    return gFailures == 0 ? 0 : 1;
}
