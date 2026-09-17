#include <QApplication>
#include <QClipboard>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFocusEvent>
#include <QInputMethodEvent>
#include <QKeyEvent>
#include <QLineEdit>
#include <QMouseEvent>
#include <QSettings>
#include <QTemporaryDir>
#include <QVBoxLayout>
#include <bloom/commands/command_stack.hpp>
#include <bloom/document/new_project.hpp>
#include <bloom/runtime/cpu_composition_evaluator.hpp>
#include <bloom/runtime/node_definition_registry.hpp>
#include <bloom/runtime/reference_display_preparation.hpp>
#include <bloom/ui/composition_preview_controller.hpp>
#include <bloom/ui/composition_preview_pipeline.hpp>
#include <bloom/ui/composition_session.hpp>
#include <bloom/ui/properties_editor.hpp>
#include <bloom/ui/task_ui_bridge.hpp>
#include <bloom/ui/viewer_editor.hpp>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <source_location>
#include <thread>

namespace {
using namespace bloom;
using namespace std::chrono_literals;
int failures = 0;
void expect(bool condition, const char* message,
            std::source_location location = std::source_location::current()) {
    if (!condition) {
        ++failures;
        std::cerr << location.line() << ": " << message << '\n';
    }
}
template <typename Predicate> bool waitUntil(Predicate predicate) {
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < 6000) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        if (predicate())
            return true;
        std::this_thread::yield();
    }
    return predicate();
}
document::NewProject project() {
    const auto format = document::CompositionFormat::create(400, 300);
    if (!format)
        std::abort();
    return document::makeNewProject("Text editing", "Main", core::RationalTime::fromInteger(10),
                                    *format);
}
runtime::TaskSchedulerConfig schedulerConfig() {
    auto config = runtime::TaskSchedulerConfig::defaults();
    config.cpuWorkerCount = 1;
    config.rowBandWorkerCount = runtime::kSerialRowBandWorkers;
    return config;
}
struct Fixture {
    document::NewProject initial = project();
    document::Document document{std::move(initial.project)};
    commands::CommandStack history{document};
    ui::CompositionSession session{document, history, initial.initialCompositionId};
    runtime::TaskScheduler scheduler{schedulerConfig()};
    ui::TaskUiBridge bridge{scheduler, nullptr, 1ms};
    struct Definitions {
        runtime::NodeDefinitionRegistry registry;
        Definitions() {
            expect(runtime::registerBuiltInNodeDefinitions(registry), "register node definitions");
            registry.freeze();
        }
    } definitions;
    runtime::SnapshotCompiler compiler{definitions.registry};
    runtime::CpuCompositionEvaluator evaluator;
    runtime::CpuReferenceDisplayPreparer display;
    runtime::QualifiedDisplayProcessorProvider qualified;
    ui::CompositionPreviewController preview{
        session, scheduler, bridge,
        ui::makeCompositionPreviewPipeline(compiler, evaluator, display, qualified)};
    QWidget host;
    QVBoxLayout layout{&host};
    ui::ViewerEditor viewer{session, preview};
    ui::PropertiesEditor properties{session};
    Fixture() {
        preview.setResolutionPolicy(runtime::PreviewResolutionPolicy::Full);
        layout.addWidget(&viewer);
        layout.addWidget(&properties);
        host.resize(900, 1000);
        host.show();
        expect(session.addTextLayer("Title", "Abc", 32, {1, 1, 1, 1}, document::Vec2d{200, 100}),
               "create text");
        ready();
        host.activateWindow();
        viewer.setFocus();
        QCoreApplication::processEvents();
    }
    ~Fixture() {
        key(Qt::Key_Escape);
        preview.beginShutdown();
        bridge.beginShutdown();
        expect(waitUntil([&] { return scheduler.isQuiescent(); }), "preview shuts down");
        viewer.setParent(nullptr);
        properties.setParent(nullptr);
    }
    void ready() {
        expect(waitUntil([&] {
                   return preview.state().activity == ui::PreviewActivity::Ready &&
                          preview.state().frame &&
                          preview.state().frame->desiredIdentity().sourceRevision ==
                              session.snapshot().revision();
               }),
               "preview ready");
    }
    void caretReady() {
        const bool prepared = waitUntil([&] { return !viewer.textCaretForTest().isNull(); });
        expect(prepared, "caret layout ready");
        if (!prepared)
            std::cerr << "edit=" << viewer.textEditing() << " live=" << session.valueEditActive()
                      << " focus=" << viewer.hasFocus()
                      << " status=" << viewer.accessibleDescription().toStdString() << '\n';
    }
    document::ParameterId parameter(std::string_view role = document::kTextParameterRole) {
        const auto* p = session.parameterForSelection(role);
        if (!p)
            std::abort();
        return p->id;
    }
    QString live() { return session.effectiveStringValue(parameter()).value_or(QString{}); }
    QString stored() { return session.constantStringValue(parameter()).value_or(QString{}); }
    void set(std::string_view role, document::ParameterValue value) {
        expect(session.setParameterValue(parameter(role), std::move(value), "Fixture"),
               "set fixture value");
    }
    void key(int code, Qt::KeyboardModifiers modifiers = Qt::NoModifier, const QString& text = {}) {
        QKeyEvent event(QEvent::KeyPress, code, modifiers, text);
        QCoreApplication::sendEvent(&viewer, &event);
    }
    void type(const QString& text) { key(0, Qt::NoModifier, text); }
    void mouse(QEvent::Type type, QPointF point, Qt::KeyboardModifiers modifiers = Qt::NoModifier) {
        QMouseEvent event(type, point, point, Qt::LeftButton, Qt::LeftButton, modifiers);
        QCoreApplication::sendEvent(&viewer, &event);
    }
    QTransform screenTransform() {
        const auto* c = session.composition();
        const auto extent = render::ImageExtent::create(c->format().width(), c->format().height());
        const auto displayRect = ui::viewTransformedDisplayRect(
            viewer.canvasRectForTest(), *extent.value(), c->format().pixelAspect(),
            viewer.viewTransformForTest());
        QTransform transform;
        transform.translate(displayRect.left(), displayRect.top());
        transform.scale(displayRect.width() / c->format().width(),
                        displayRect.height() / c->format().height());
        return transform;
    }
    QTransform worldTransform() {
        const auto bounds = preview.selectedLayerBounds();
        if (bounds.empty())
            std::abort();
        const auto& b = bounds.front();
        const QPointF origin(b.polygon[0].x, b.polygon[0].y);
        const auto x =
            (QPointF(b.polygon[1].x, b.polygon[1].y) - origin) / (b.local.right - b.local.left);
        const auto y =
            (QPointF(b.polygon[3].x, b.polygon[3].y) - origin) / (b.local.bottom - b.local.top);
        const auto translation = origin - x * b.local.left - y * b.local.top;
        return {x.x(), x.y(), y.x(), y.y(), translation.x(), translation.y()};
    }
};
void testClickTypingCommit() {
    Fixture f;
    const auto parameters = render::TextRasterParameters::create(32, 32);
    const auto query =
        render::layoutText(render::EmbeddedFace::DejaVuSans, "Abc", *parameters.value());
    const auto& glyph = query.value()->glyphs[1];
    const auto transform = f.worldTransform() * f.screenTransform();
    const auto click = transform.map(QPointF(glyph.advanceRect.x + 0.1, glyph.advanceRect.y + 16));
    const auto before = f.history.size();
    f.mouse(QEvent::MouseButtonDblClick, click);
    expect(f.viewer.textEditing(), "double click enters edit mode");
    QMetaObject::invokeMethod(&f.session, "snapshotChanged");
    auto* field = f.properties.findChild<QLineEdit*>("textContentEditor");
    QMetaObject::invokeMethod(field, "editingFinished");
    expect(f.viewer.textEditing(),
           "projection refresh and idle Properties field preserve canvas edit");
    f.caretReady();
    expect(f.viewer.inputMethodQuery(Qt::ImCursorPosition).toInt() == 1,
           "click selects nearest glyph boundary");
    expect(QLineF(f.viewer.textCaretForTest().p1(), transform.map(QPointF(glyph.advanceRect.x, 0)))
                   .length() < 0.01,
           "caret uses raster pen position");
    f.type("X");
    expect(f.live() == "AXbc" && f.stored() == "Abc", "typing uses live String override only");
    expect(field && field->text() == "AXbc", "Properties mirrors live canvas content");
    expect(f.history.size() == before, "typing creates no history");
    f.key(Qt::Key_Return);
    expect(!f.viewer.textEditing() && f.history.size() == before + 1 &&
               f.session.undoLabel() == "Edit Text",
           "Enter commits exactly one Edit Text transaction");
    expect(f.session.undo() && f.stored() == "Abc", "undo restores original content");
}
void testCancelAndNavigation() {
    Fixture f;
    const auto before = f.history.size();
    const auto layer = f.session.selection().contextualLayer;
    f.key(Qt::Key_Return);
    f.caretReady();
    QKeyEvent shortcut(QEvent::ShortcutOverride, Qt::Key_Delete, Qt::NoModifier);
    shortcut.ignore();
    QCoreApplication::sendEvent(&f.viewer, &shortcut);
    expect(shortcut.isAccepted(), "text editor reserves Delete before application shortcuts");
    f.key(Qt::Key_A, Qt::ControlModifier);
    f.type("hello world");
    f.key(Qt::Key_Left, Qt::ControlModifier | Qt::ShiftModifier);
    expect(f.viewer.inputMethodQuery(Qt::ImCurrentSelection).toString() == "world",
           "Ctrl Shift Left selects word");
    f.key(Qt::Key_Delete);
    expect(f.live() == "hello " && f.session.selection().contextualLayer == layer,
           "Delete edits text without deleting layer");
    QApplication::clipboard()->setText("é🙂");
    f.key(Qt::Key_V, Qt::ControlModifier);
    f.key(Qt::Key_Backspace);
    expect(f.live() == "hello é", "paste and Backspace preserve UTF-16 scalar boundaries");
    f.type("🙂");
    expect(f.live() == "hello é🙂", "typing accepts supplementary Unicode scalars");
    f.key(Qt::Key_Backspace);
    f.key(Qt::Key_Home);
    expect(f.viewer.inputMethodQuery(Qt::ImCursorPosition).toInt() == 0,
           "Home reaches line start while layout pending");
    f.key(Qt::Key_Escape);
    expect(!f.viewer.textEditing() && f.history.size() == before && f.live() == "Abc",
           "Escape restores without history");
}
void testIme() {
    Fixture f;
    f.key(Qt::Key_Return);
    const auto before = f.history.size();
    QInputMethodEvent compose(QString::fromUtf8("に"), {{QInputMethodEvent::Cursor, 1, 1, {}}});
    QCoreApplication::sendEvent(&f.viewer, &compose);
    expect(f.live() == QString::fromUtf8("Abcに") && f.stored() == "Abc",
           "IME preedit is preview only");
    f.caretReady();
    QInputMethodEvent commit;
    commit.setCommitString(QString::fromUtf8("日"));
    QCoreApplication::sendEvent(&f.viewer, &commit);
    expect(f.live() == QString::fromUtf8("Abc日"), "IME commit replaces preedit exactly once");
    QInputMethodEvent replacement;
    replacement.setCommitString(QString::fromUtf8("本"), -1, 1);
    QCoreApplication::sendEvent(&f.viewer, &replacement);
    expect(f.live() == QString::fromUtf8("Abc本"),
           "IME replacement range respects surrounding text");
    f.key(Qt::Key_Return);
    expect(f.history.size() == before + 1 && f.stored() == QString::fromUtf8("Abc本"),
           "IME and typing share transaction");
}
void testWrappedAndTransformed() {
    Fixture f;
    const auto child = f.session.selection().contextualLayer.value_or(document::LayerId{});
    expect(f.session.addTextLayer("Parent", "Parent", 24, {1, 1, 1, 1}, document::Vec2d{200, 100}),
           "parent layer");
    const auto parent = f.session.selection().contextualLayer.value_or(document::LayerId{});
    expect(f.session.setSelectedRotation(25) && f.session.setSelectedScale(1.2, 0.8),
           "parent transform");
    f.session.selectLayer(child);
    expect(f.session.setLayerParent(child, parent), "parent text layer");
    expect(f.session.setSelectedRotation(-15) && f.session.setSelectedScale(1.4, 0.7),
           "child transform");
    f.set(document::kTextBoxParameterRole, document::Vec2d{50, 140});
    f.set(document::kTextWrapParameterRole, true);
    f.set(document::kTextVerticalAlignmentParameterRole, std::int64_t{2});
    expect(f.session.setSelectedTextContent("A A A"), "wrapped content");
    f.ready();
    f.key(Qt::Key_Return);
    f.caretReady();
    const auto parameters = render::TextRasterParameters::create(32, 32);
    render::TextLayoutOptions options;
    options.boxWidth = 50;
    options.boxHeight = 140;
    options.wrap = true;
    options.verticalAlignment = render::TextLayoutOptions::VerticalAlignment::Bottom;
    const auto query =
        render::layoutText(render::EmbeddedFace::DejaVuSans, "A A A", *parameters.value(), options);
    expect(query && query.value()->lines.size() > 1, "box wraps");
    const auto transform = f.worldTransform() * f.screenTransform();
    const auto caret = transform.map(ui::textLayoutCaret(*query.value(), 5));
    expect(QLineF(f.viewer.textCaretForTest().p1(), caret.p1()).length() < 0.01 &&
               QLineF(f.viewer.textCaretForTest().p2(), caret.p2()).length() < 0.01,
           "wrapped caret follows world transform including rotated scaled parent");
    f.key(Qt::Key_Home);
    expect(f.viewer.inputMethodQuery(Qt::ImCursorPosition).toInt() == 4,
           "Home follows wrapped line");
    f.key(Qt::Key_Up);
    expect(f.viewer.inputMethodQuery(Qt::ImCursorPosition).toInt() < 4,
           "Up follows previous wrapped line");
    const auto before = f.history.size();
    f.type("Z");
    f.key(Qt::Key_Return);
    expect(f.viewer.textEditing() && f.live().contains('\n'), "Enter inserts newline in box text");
    f.key(Qt::Key_Return, Qt::ControlModifier);
    expect(!f.viewer.textEditing() && f.history.size() == before + 1,
           "Ctrl Enter commits multiline edit");
}
void testFocusAndProperties() {
    Fixture f;
    f.key(Qt::Key_Return);
    f.type("!");
    const auto before = f.history.size();
    auto* field = f.properties.findChild<QLineEdit*>("textContentEditor");
    field->setFocus();
    QCoreApplication::processEvents();
    expect(!f.viewer.textEditing() && f.history.size() == before + 1 && f.stored() == "Abc!",
           "focus loss commits once");
    field->setText("Property");
    QMetaObject::invokeMethod(field, "textEdited", Q_ARG(QString, QString("Property")));
    expect(f.live() == "Property" && f.stored() == "Abc!",
           "Properties also previews a String edit");
    QKeyEvent escape(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
    QCoreApplication::sendEvent(field, &escape);
    expect(f.live() == "Abc!" && field->text() == "Abc!", "Properties Escape restores mirror");
    field->setText("Cancelled externally");
    QMetaObject::invokeMethod(field, "textEdited", Q_ARG(QString, QString("Cancelled externally")));
    f.session.cancelValueEdit();
    expect(field->text() == "Abc!", "session cancellation restores the Properties mirror");
    f.key(Qt::Key_Return);
    QCoreApplication::processEvents();
    expect(f.viewer.textEditing(), "cancelled Properties edit cannot finish a new canvas edit");
    f.key(Qt::Key_Escape);
    field->setFocus();
    field->setText("Accepted");
    QMetaObject::invokeMethod(field, "textEdited", Q_ARG(QString, QString("Accepted")));
    QMetaObject::invokeMethod(field, "editingFinished");
    expect(f.history.size() == before + 2 && f.session.undoLabel() == "Edit Text",
           "Properties commits with same label and gesture");
}
void testReversedAdvances() {
    Fixture f;
    f.set(document::kTextLetterSpacingParameterRole, -48.0);
    f.ready();
    f.key(Qt::Key_Return);
    f.caretReady();
    f.key(Qt::Key_Home);
    expect(f.viewer.inputMethodQuery(Qt::ImCursorPosition).toInt() == 0,
           "Home reaches source start even with negative advances");
    f.key(Qt::Key_End);
    expect(f.viewer.inputMethodQuery(Qt::ImCursorPosition).toInt() == 3,
           "End reaches source end even with negative advances");
}
void testEmptyAndInvalidation() {
    Fixture f;
    expect(f.session.setSelectedTextContent(""), "empty text");
    f.ready();
    f.key(Qt::Key_Return);
    f.caretReady();
    expect(f.viewer.textEditing(), "empty text enters edit mode without ink bounds");
    f.type("A");
    expect(f.live() == "A", "empty text accepts input");
    expect(f.session.setCurrentTime(core::RationalTime::fromInteger(1)), "change time");
    expect(!f.viewer.textEditing() && f.live().isEmpty(), "time change cancels stale edit");
}
} // namespace
int main(int argc, char** argv) {
    QApplication app(argc, argv);
    QTemporaryDir settings;
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settings.path());
    QApplication::setOrganizationName("BloomTextEditTests");
    QApplication::setApplicationName("BloomTextEditTests");
    testClickTypingCommit();
    testCancelAndNavigation();
    testIme();
    testWrappedAndTransformed();
    testFocusAndProperties();
    testReversedAdvances();
    testEmptyAndInvalidation();
    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
