#include <QAction>
#include <QApplication>
#include <QElapsedTimer>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QSettings>
#include <QThread>
#include <QVBoxLayout>
#include <bloom/commands/command_stack.hpp>
#include <bloom/commands/node_operations.hpp>
#include <bloom/commands/transaction.hpp>
#include <bloom/document/document.hpp>
#include <bloom/document/new_project.hpp>
#include <bloom/document/project.hpp>
#include <bloom/document/value_nodes.hpp>
#include <bloom/ui/composition_session.hpp>
#include <bloom/ui/kit/button.hpp>
#include <bloom/ui/kit/dropdown.hpp>
#include <bloom/ui/kit/section.hpp>
#include <bloom/ui/node_editor.hpp>
#include <bloom/ui/properties_editor.hpp>
#include <iostream>
using namespace bloom;
namespace {
int failures = 0;
void expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
        ++failures;
    }
}
QWidget* row(QWidget& panel, const char* role) {
    for (auto* candidate : panel.findChildren<QWidget*>("propertiesRegistryRow"))
        if (candidate->property("role").toString() == role)
            return candidate;
    return nullptr;
}
document::NodeId addNode(ui::CompositionSession& session, std::string_view type) {
    commands::Transaction transaction("Add", session.snapshot().revision());
    transaction.emplace<commands::AddNode>(session.compositionId(), std::string(type),
                                           document::Vec2d{});
    const auto result = session.executeNodeTransaction(std::move(transaction));
    return result.outputId<document::NodeId>(commands::kAddNodeOutput).value_or(document::NodeId{});
}
void connectNodes(ui::CompositionSession& session, document::NodeId source, document::NodeId target,
                  const char* port) {
    commands::Transaction transaction("Connect", session.snapshot().revision());
    transaction.emplace<commands::ConnectPorts>(session.compositionId(),
                                                document::OutputPortRef{source, "value"},
                                                document::NodeInputRef{target, port});
    expect(session.executeNodeTransaction(std::move(transaction)).changed(),
           "connect upstream fixture");
}
void upstreamRows() {
    auto project =
        document::makeNewProject("Upstream", "Main", core::RationalTime::fromInteger(10));
    auto id = project.initialCompositionId;
    document::Document document(std::move(project.project));
    commands::CommandStack stack(document);
    ui::CompositionSession session(document, stack, id);
    const auto root = addNode(session, document::kScalarMathNodeType);
    const auto first = addNode(session, document::kScalarMathNodeType);
    const auto second = addNode(session, document::kScalarMathNodeType);
    const auto third = addNode(session, document::kScalarMathNodeType);
    const auto fourth = addNode(session, document::kScalarValueNodeType);
    connectNodes(session, first, root, "a");
    connectNodes(session, first, root, "b");
    connectNodes(session, second, first, "a");
    connectNodes(session, third, second, "a");
    connectNodes(session, fourth, third, "a");
    session.selectNode(root);
    QWidget window;
    auto* layout = new QVBoxLayout(&window);
    auto* panel = new ui::PropertiesEditor(session, &window);
    auto* canvas = new ui::NodeGraphEditor(session, &window);
    layout->addWidget(panel);
    layout->addWidget(canvas);
    window.resize(900, 800);
    window.show();
    QCoreApplication::processEvents();
    auto* drivenRow = row(*panel, "a");
    auto* value = drivenRow ? drivenRow->findChild<QLabel*>("propertiesDrivenValue") : nullptr;
    QElapsedTimer wait;
    wait.start();
    while (value && value->text() == "Resolving…" && wait.elapsed() < 5000) {
        QCoreApplication::processEvents();
        QThread::msleep(5);
    }
    expect(value && value->text() == "0",
           "detached driven graph resolves through background evaluator");
    expect(drivenRow &&
               drivenRow->findChild<ui::kit::KButton*>("propertiesDriverLink")->text() == "Math",
           "driven row names the driver node");
    if (drivenRow) {
        Q_EMIT drivenRow->customContextMenuRequested(QPoint{});
        auto* reset = drivenRow->findChild<QAction*>("propertiesResetToDefault");
        expect(reset && reset->isEnabled(), "driven row offers Reset to default");
        const auto before = stack.size();
        if (reset)
            reset->trigger();
        expect(stack.size() == before + 1,
               "driven reset disconnects to registry default in one command");
        expect(session.undo(), "driven reset is undoable");
        for (auto* menu : panel->findChildren<QMenu*>())
            menu->close();
    }
    auto* upstream = panel->findChild<QWidget*>("propertiesUpstreamPanel");
    expect(upstream && upstream->findChildren<ui::kit::KSection*>().size() == 3,
           "BFS deduplicates paths and limits sections to depth three");
    auto* more = panel->findChild<QLabel*>("propertiesMoreUpstream");
    expect(more && more->text() == "and 1 more upstream", "one exact beyond-depth count");
    if (upstream) {
        const auto sections = upstream->findChildren<ui::kit::KSection*>();
        expect(sections.front()->property("nodeId").toULongLong() == first.value(),
               "BFS nearest node first");
        auto* operand = row(*sections.front(), "b");
        if (operand) {
            operand->findChild<ui::kit::KValueField*>()->setValue(5);
            expect(session.selectedNode()->id == root,
                   "editing an upstream operand preserves selection");
        }
        sections.front()->findChild<ui::kit::KButton*>("propertiesJumpToNode")->click();
        expect(session.selectedNode()->id == first, "Jump selects its own node");
        auto* item = canvas->graphScene()->findNodeItem(first);
        const auto center =
            canvas->graphView()->mapToScene(canvas->graphView()->viewport()->rect().center());
        expect(item && QLineF(center, item->sceneBoundingRect().center()).length() < 3,
               "Jump centers the canvas on its node");
    }
}
void registryRows() {
    auto project =
        document::makeNewProject("Registry", "Main", core::RationalTime::fromInteger(10));
    auto id = project.initialCompositionId;
    document::Document document(std::move(project.project));
    commands::CommandStack stack(document);
    ui::CompositionSession session(document, stack, id);
    expect(session.addSolidLayer("Solid", {1, 0, 0, 1}), "add solid");
    ui::PropertiesEditor panel(session);
    auto* width = row(panel, "width");
    auto* height = row(panel, "height");
    expect(width && height, "registry adds both solid dimensions");
    if (width) {
        auto* field = width->findChild<ui::kit::KValueField*>();
        const auto parameter =
            document::ParameterId::fromRaw(width->property("parameterId").toULongLong());
        const auto selection = session.selection().primary;
        const auto before = stack.size();
        Q_EMIT field->scrubStarted();
        field->setValue(731);
        expect(stack.size() == before, "scrubbing publishes no intermediate commands");
        Q_EMIT field->scrubFinished();
        expect(stack.size() == before + 1 && session.effectiveScalarValue(parameter) == 731,
               "scrub commits one exact parameter edit");
        expect(session.selection().primary == selection, "registry edit preserves selection");
        expect(session.undo(), "registry edit is undoable");
        auto* section = panel.findChild<ui::kit::KSection*>("propertiesSection_solid");
        field->setValue(872);
        Q_EMIT section->resetRequested();
        expect(session.effectiveScalarValue(parameter) == 1,
               "source reset includes registry defaults");
    }
    expect(session.addTextLayer("Text", "Hello", 48, {1, 1, 1, 1}), "add text");
    panel.show();
    QCoreApplication::processEvents();
    auto* search = panel.findChild<QLineEdit*>("propertiesSearchField");
    expect(search != nullptr, "search field exists");
    search->setText("HeIgHt");
    expect(row(panel, "line-height")->isVisible() && !row(panel, "letter-spacing")->isVisible(),
           "search is case-insensitive substring over labels");
    expect(!panel.findChild<ui::kit::KSection*>("propertiesSection_transform")->isVisible(),
           "unmatched sections hide");
    search->setText("not a property");
    expect(!panel.findChild<ui::kit::KSection*>("propertiesSection_text")->isVisible(),
           "empty sections hide");
    search->setFocus();
    QKeyEvent escape(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
    QCoreApplication::sendEvent(search, &escape);
    expect(search->text().isEmpty() && panel.hasFocus() &&
               row(panel, "letter-spacing")->isVisible(),
           "Escape clears, restores rows, and returns panel focus");
    auto* alignment = row(panel, "alignment");
    expect(alignment && row(panel, "line-height") && row(panel, "letter-spacing"),
           "registry adds every text layout parameter");
    if (alignment) {
        auto* dropdown = alignment->findChild<ui::kit::KDropdown*>();
        expect(dropdown && dropdown->count() == 3, "alignment is a closed dropdown");
        if (dropdown)
            dropdown->setCurrentIndex(2);
        expect(session.canUndo(), "enum edit uses command history");
    }
}
} // namespace
int main(int argc, char** argv) {
    QApplication application(argc, argv);
    QCoreApplication::setOrganizationName("BloomPropertiesRegistryTest");
    QSettings().clear();
    registryRows();
    upstreamRows();
    QSettings().clear();
    return failures ? 1 : 0;
}
