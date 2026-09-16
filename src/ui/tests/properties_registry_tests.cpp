#include <QAction>
#include <QApplication>
#include <QElapsedTimer>
#include <QGraphicsItem>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QPlainTextEdit>
#include <QScrollArea>
#include <QScrollBar>
#include <QSettings>
#include <QStringList>
#include <QThread>
#include <QVBoxLayout>
#include <bloom/commands/command_stack.hpp>
#include <bloom/commands/node_operations.hpp>
#include <bloom/commands/operations.hpp>
#include <bloom/commands/transaction.hpp>
#include <bloom/document/document.hpp>
#include <bloom/document/new_project.hpp>
#include <bloom/document/project.hpp>
#include <bloom/document/value_nodes.hpp>
#include <bloom/ui/composition_session.hpp>
#include <bloom/ui/kit/button.hpp>
#include <bloom/ui/kit/color_chip.hpp>
#include <bloom/ui/kit/dropdown.hpp>
#include <bloom/ui/kit/radio_group.hpp>
#include <bloom/ui/kit/section.hpp>
#include <bloom/ui/kit/switch_control.hpp>
#include <bloom/ui/node_editor.hpp>
#include <bloom/ui/properties_editor.hpp>
#include <iostream>
#include <limits>
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
    const auto* node = session.composition()->graph().findNode(source);
    const auto* definition =
        document::builtInNodeDefinitions().find(node->typeId, node->schemaVersion);
    transaction.emplace<commands::ConnectPorts>(
        session.compositionId(), document::OutputPortRef{source, definition->outputs.front().name},
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
    expect(drivenRow && drivenRow->findChild<ui::kit::KButton*>("propertiesDriverLink") &&
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
    const auto vector = addNode(session, document::kVector2MathNodeType);
    connectNodes(session, fourth, vector, "a");
    session.selectNode(vector);
    value = panel->findChild<QLabel*>("propertiesDrivenValue");
    wait.restart();
    while (value && value->text() == "Resolving…" && wait.elapsed() < 5000) {
        QCoreApplication::processEvents();
        QThread::msleep(5);
    }
    expect(value && value->text() == "0, 0", "driver display applies scalar-to-vector promotion");
    const auto string = addNode(session, document::kStringValueNodeType);
    const auto stringSwitch = addNode(session, document::kStringSwitchNodeType);
    const auto stringParameter =
        session.composition()->graph().findNode(string)->parameters.front().parameterId;
    expect(session.setParameterValue(stringParameter, std::string("Exact string"), "String"),
           "string driver fixture");
    connectNodes(session, string, stringSwitch, "ifFalse");
    connectNodes(session, string, stringSwitch, "ifTrue");
    const auto stringConsumer = addNode(session, document::kStringSwitchNodeType);
    connectNodes(session, stringSwitch, stringConsumer, "ifFalse");
    session.selectNode(stringConsumer);
    value = panel->findChild<QLabel*>("propertiesDrivenValue");
    wait.restart();
    while (value && value->text() == "Resolving…" && wait.elapsed() < 5000) {
        QCoreApplication::processEvents();
        QThread::msleep(5);
    }
    if (value && value->text() != "Exact string")
        std::cerr << "STRING RESULT " << value->text().toStdString() << "\n";
    expect(value && value->text() == "Exact string",
           "detached String driver resolves without image lowering");
}
// Task DRIVE-1. The headline case at the panel: a text layer whose WORDS come from a String node.
// The content row has no authored value any more, so it shows the driver and the resolved text
// read-only rather than an editor holding a stale string -- the same shape every other driven kind
// already had, now that every kind can actually be driven.
void drivenTextContent() {
    auto project =
        document::makeNewProject("Driven text", "Main", core::RationalTime::fromInteger(10));
    const auto id = project.initialCompositionId;
    document::Document document(std::move(project.project));
    commands::CommandStack stack(document);
    ui::CompositionSession session(document, stack, id);
    expect(session.addTextLayer("Title", "Authored"), "driven-content fixture adds its text layer");
    const auto* content = session.parameterForSelection(document::kTextParameterRole);
    if (content == nullptr) {
        expect(false, "the text layer exposes its content parameter");
        return;
    }
    const auto contentId = content->id;
    const auto string = addNode(session, document::kStringValueNodeType);
    const auto stringParameter =
        session.composition()->graph().findNode(string)->parameters.front().parameterId;
    expect(session.setParameterValue(stringParameter, std::string("Driven words"), "String"),
           "the String node carries the words");
    commands::Transaction drive("Drive content", session.snapshot().revision());
    drive.emplace<commands::SetParameterSource>(
        id, contentId,
        document::DriverBindingSource{string, std::string(document::kValuePortName)});
    expect(session.executeNodeTransaction(std::move(drive)).changed(),
           "the text content becomes driven");

    ui::PropertiesEditor panel(session);
    panel.resize(420, 700);
    panel.show();
    QCoreApplication::processEvents();
    QWidget* contentRow = nullptr;
    for (auto* candidate : panel.findChildren<QWidget*>("propertiesRow"))
        if (candidate->property("parameterId").toULongLong() == contentId.value())
            contentRow = candidate;
    if (contentRow == nullptr) {
        expect(false, "the content row is realized");
        return;
    }
    auto* editor = contentRow->findChild<QWidget*>("textContentEditor");
    auto* link = contentRow->findChild<ui::kit::KButton*>("propertiesDriverLink");
    auto* value = contentRow->findChild<QLabel*>("propertiesDrivenValue");
    expect(editor != nullptr && !editor->isVisible(),
           "a driven String shows no text editor, because there is no authored string to edit");
    expect(link != nullptr && link->isVisible() && !link->text().isEmpty(),
           "and names the node its words come from");
    QElapsedTimer wait;
    wait.start();
    while (value && value->text() == "Resolving…" && wait.elapsed() < 5000) {
        QCoreApplication::processEvents();
        QThread::msleep(5);
    }
    expect(value != nullptr && value->text() == "Driven words",
           "and shows the resolved text itself, read-only");
    expect(value != nullptr && value->text() == session.drivenValueText(contentId),
           "read from the session, so the timeline row for this parameter shows the same string");
}

void widthRule() {
    auto project = document::makeNewProject("Width", "Main", core::RationalTime::fromInteger(10));
    auto id = project.initialCompositionId;
    document::Document document(std::move(project.project));
    commands::CommandStack stack(document);
    ui::CompositionSession session(document, stack, id);
    expect(session.addSolidLayer("A very long source name for a narrow viewport", {1, 0, 0, 1}),
           "width fixture");
    ui::PropertiesEditor panel(session);
    panel.resize(ui::kit::px(ui::kit::Size::PanelMinWidth), 260);
    panel.show();
    QCoreApplication::processEvents();
    auto* scroll = panel.findChild<QScrollArea*>("propertiesScrollArea");
    expect(panel.width() == ui::kit::px(ui::kit::Size::PanelMinWidth),
           "panel stays at the unchanged 300-design-pixel minimum");
    expect(scroll && scroll->verticalScrollBar()->maximum() > 0,
           "body scrolls vertically at narrow size");
    if (!scroll)
        return;
    if (scroll->widget()->width() > scroll->viewport()->width())
        std::cerr << "WIDTH viewport=" << scroll->viewport()->width()
                  << " body=" << scroll->widget()->width()
                  << " min=" << scroll->widget()->minimumSizeHint().width() << '\n';
    expect(scroll->widget()->width() <= scroll->viewport()->width(),
           "body never exceeds viewport width");
    bool elided = false;
    for (auto* child : scroll->widget()->findChildren<QWidget*>()) {
        if (!child->isVisible() || child->isWindow())
            continue;
        if (child->minimumSizeHint().width() > scroll->viewport()->width())
            std::cerr << "WIDE " << child->objectName().toStdString() << " width=" << child->width()
                      << " min=" << child->minimumSizeHint().width() << '\n';
        expect(child->width() <= scroll->viewport()->width(),
               "no visible child is wider than viewport");
        if (auto* field = qobject_cast<ui::kit::KValueField*>(child))
            expect(field->width() >= field->minimumSizeHint().width(),
                   "numeric field keeps its legible floor");
        if (auto* label = qobject_cast<QLabel*>(child);
            label && label->objectName() == "propertiesRowLabel")
            elided = elided || label->text().endsWith(QChar(0x2026));
    }
    expect(elided, "narrow labels elide instead of stretching viewport");
}
void genericKinds() {
    auto project = document::makeNewProject("Kinds", "Main", core::RationalTime::fromInteger(10));
    const auto id = project.initialCompositionId;
    document::Document document(std::move(project.project));
    commands::CommandStack stack(document);
    ui::CompositionSession session(document, stack, id);
    ui::PropertiesEditor panel(session);
    for (const auto type : {document::kScalarValueNodeType, document::kIntegerValueNodeType,
                            document::kBooleanValueNodeType, document::kStringValueNodeType,
                            document::kVector2ValueNodeType, document::kVector3ValueNodeType,
                            document::kColorValueNodeType, document::kVector2MathNodeType}) {
        const auto nodeId = addNode(session, type);
        session.selectNode(nodeId);
        const auto* node = session.selectedNode();
        const auto* definition =
            document::builtInNodeDefinitions().find(node->typeId, node->schemaVersion);
        expect(panel.findChildren<QWidget*>("propertiesRegistryRow").size() ==
                   static_cast<qsizetype>(definition->parameters.size()),
               "every value-node registry parameter renders, including scale operands");
        auto* valueRow = row(panel, "value");
        if (!valueRow)
            continue;
        const auto parameter =
            document::ParameterId::fromRaw(valueRow->property("parameterId").toULongLong());
        if (type == document::kIntegerValueNodeType) {
            auto* field = valueRow->findChild<QLineEdit*>("propertiesRegistryInteger");
            expect(field != nullptr, "integer has an exact decimal field");
            if (field) {
                field->setText("9223372036854775807");
                Q_EMIT field->editingFinished();
            }
            const auto* record = session.composition()->parameters().find(parameter);
            const auto& constant = std::get<document::ConstantValueSource>(record->source);
            expect(std::get<std::int64_t>(constant.value) ==
                       std::numeric_limits<std::int64_t>::max(),
                   "integer field preserves all int64 digits");
        } else if (type == document::kBooleanValueNodeType) {
            auto* toggle = valueRow->findChild<ui::kit::KSwitch*>();
            expect(toggle != nullptr, "boolean renders a switch");
            if (toggle)
                toggle->setChecked(true);
        } else if (type == document::kColorValueNodeType) {
            expect(valueRow->findChild<ui::kit::KColorChip*>() &&
                       valueRow->findChildren<ui::kit::KValueField*>().size() == 4,
                   "color has a picker chip and four expanded components");
            expect(session.setParameterValue(parameter, core::Color4d{2, -1, 0.5, 0.25}, "Color"),
                   "HDR generic color authors through session");
            const auto fields = valueRow->findChildren<ui::kit::KValueField*>();
            expect(fields[0]->value() == 2 && fields[1]->value() == -1,
                   "generic components retain HDR and negative values");
        } else if (type == document::kVector2ValueNodeType ||
                   type == document::kVector3ValueNodeType) {
            expect(valueRow->findChildren<ui::kit::KValueField*>().size() ==
                       (type == document::kVector2ValueNodeType ? 2 : 3),
                   "vectors render every component");
        }
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
    auto* solidSection = panel.findChild<ui::kit::KSection*>("propertiesSection_solid");
    QStringList solidLabels;
    if (solidSection) {
        for (int index = 0; index < solidSection->bodyLayout()->count(); ++index) {
            auto* sourceRow = solidSection->bodyLayout()->itemAt(index)->widget();
            if (!sourceRow)
                continue;
            if (sourceRow->objectName() == "propertiesRegistryRow")
                sourceRow = sourceRow->findChild<QWidget*>("propertiesRow");
            else if (sourceRow->objectName() != "propertiesRow")
                continue;
            if (sourceRow && !sourceRow->property("rowLabel").toString().isEmpty())
                solidLabels.push_back(sourceRow->property("rowLabel").toString());
        }
    }
    expect(solidLabels == QStringList{"Color", "Width", "Height"},
           "Solid Properties exposes only Color, Width, and Height source rows");
    expect(panel.findChild<QLabel*>("solidAlphaAssociation") == nullptr &&
               panel.findChild<QLabel*>("solidColorEncoding") == nullptr,
           "Solid technical alpha and encoding metadata stays out of Properties rows");
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
    panel.activateWindow();
    QCoreApplication::processEvents();
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
        auto* dropdown = alignment->findChild<ui::kit::KRadioGroup*>();
        expect(dropdown && dropdown->count() == 3, "alignment is a closed segmented icon control");
        if (dropdown)
            dropdown->setCurrentIndex(2);
        expect(session.canUndo(), "enum edit uses command history");
    }
    auto* font = row(panel, "font");
    expect(font != nullptr, "registry adds the text font parameter");
    if (font) {
        auto* dropdown = font->findChild<ui::kit::KDropdown*>("propertiesRegistryEnum");
        expect(dropdown != nullptr && dropdown->count() == 4 &&
                   dropdown->currentText() == QStringLiteral("DejaVu Sans"),
               "Font is a generic Properties dropdown with the four faces in face order");
    }
    const auto layer = std::get<document::LayerId>(session.selection().primary);
    const auto boundary = session.boundaryNodeForLayer(layer);
    expect(boundary.has_value(), "text layer has a boundary node");
    if (!boundary)
        return;
    session.selectNode(*boundary);
    auto* upstream = panel.findChild<QWidget*>("propertiesUpstreamPanel");
    expect(upstream && upstream->findChildren<ui::kit::KSection*>().size() == 1,
           "image input exposes one source section when its boundary node is selected");
    expect(upstream && upstream->findChild<QPlainTextEdit*>("propertiesRegistryMultiline"),
           "upstream Text content uses the generic multiline editor");
    std::vector<document::NodeId> terminals;
    for (const auto& node : session.composition()->graph().nodes())
        if (node.typeId == document::kLayerStackNodeType ||
            node.typeId == document::kCompositionOutputNodeType)
            terminals.push_back(node.id);
    for (auto terminal : terminals) {
        session.selectNode(terminal);
        upstream = panel.findChild<QWidget*>("propertiesUpstreamPanel");
        expect(!upstream || upstream->findChildren<ui::kit::KSection*>().empty(),
               "Merge and Output stop upstream traversal");
    }
}
} // namespace
int main(int argc, char** argv) try {
    QApplication application(argc, argv);
    QCoreApplication::setOrganizationName("BloomPropertiesRegistryTest");
    QSettings().clear();
    genericKinds();
    registryRows();
    upstreamRows();
    drivenTextContent();
    widthRule();
    QSettings().clear();
    return failures ? 1 : 0;
}

catch (const std::exception& error) {
    std::cerr << error.what() << "\n";
    return 1;
}
