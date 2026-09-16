#include <bloom/commands/node_operations.hpp>
#include <bloom/document/new_project.hpp>
#include <bloom/runtime/cpu_composition_evaluator.hpp>
#include <bloom/runtime/snapshot_compiler.hpp>
#include <bloom/ui/composition_session.hpp>
#include <bloom/ui/window_status_bar.hpp>

#include <QApplication>

#include <iostream>
#include <set>
#include <stdexcept>

namespace {
using namespace bloom;
int failures = 0;
void expect(const bool condition, const char* message) {
    if (!condition) {
        ++failures;
        std::cerr << message << '\n';
    }
}

void multiSelection() {
    auto project =
        document::makeNewProject("Selection", "Main", core::RationalTime::fromInteger(10));
    const auto compositionId = project.initialCompositionId;
    document::Document document(std::move(project.project));
    commands::CommandStack stack(document);
    ui::CompositionSession session(document, stack, compositionId);
    if (!session.addSolidLayer(QStringLiteral("A"), {1, 0, 0, 1}) ||
        !session.addSolidLayer(QStringLiteral("B"), {0, 1, 0, 1}))
        throw std::logic_error("selection fixture");
    const auto& graph = session.composition()->graph();
    const auto layerA = graph.layerOutputs()[0].layerId;
    const auto layerB = graph.layerOutputs()[1].layerId;
    const auto sourceA = session.directSourceNodeForLayer(layerA);
    const auto sourceB = session.directSourceNodeForLayer(layerB);
    if (!sourceA || !sourceB)
        throw std::logic_error("selection sources");
    const auto a = *sourceA;
    const auto b = *sourceB;
    const auto revision = session.snapshot().revision();
    const auto history = stack.size();
    int changes = 0;
    int refusals = 0;
    QObject::connect(&session, &ui::CompositionSession::selectionChanged, &session,
                     [&] { ++changes; });
    QObject::connect(&session, &ui::CompositionSession::commandRejected, &session,
                     [&](const QString&) { ++refusals; });
    session.selectNodes({a, b}, a);
    expect(session.selectedNodes() == std::set{a, b} &&
               session.selection().primary == ui::SelectionTarget{a} &&
               session.selection().contextualLayer == layerA && changes == 1,
           "selectNodes preserves primary and its unique layer context");
    session.selectNodes({a, b}, a);
    expect(changes == 1, "identical multi-selection emits no change");
    session.selectNodes({a}, b);
    session.selectNodes({a, document::NodeId::fromRaw(9999)}, a);
    session.toggleNodeSelection(document::NodeId::fromRaw(9999));
    expect(refusals == 3 && changes == 1 && session.selectedNodes() == std::set{a, b},
           "invalid primary, absent node and invalid toggle are atomic");
    session.toggleNodeSelection(b);
    expect(session.selectedNodes() == std::set{a} &&
               session.selection().primary == ui::SelectionTarget{a},
           "removing secondary retains primary");
    session.toggleNodeSelection(b);
    expect(session.selectedNodes() == std::set{a, b} &&
               session.selection().primary == ui::SelectionTarget{b},
           "adding makes toggled node primary");
    session.toggleNodeSelection(b);
    expect(session.selection().primary == ui::SelectionTarget{a},
           "removing primary chooses lowest remaining stable ID");
    session.selectNodes({a, b}, a);
    session.selectNode(a);
    expect(session.selectedNodes() == std::set{a},
           "single-select replaces the multi-set even when primary is unchanged");
    session.selectNodes({a, b}, a);
    session.selectLayer(layerB);
    const auto boundary = session.boundaryNodeForLayer(layerB);
    expect(boundary && session.selectedNodes() == std::set{*boundary} &&
               session.selection().primary == ui::SelectionTarget{layerB},
           "layer primary stays LayerId while its boundary is selected");
    const auto* parameter = session.parameterForSelection(document::kOpacityParameterRole);
    if (!parameter)
        throw std::logic_error("selection opacity");
    session.selectParameter(parameter->id);
    expect(session.selectedNodes().empty() &&
               session.selection().primary == ui::SelectionTarget{parameter->id},
           "parameter primary keeps its existing tagged contract");
    session.selectNodes({a, b}, a);
    session.clearSelection();
    const auto clearedChanges = changes;
    session.clearSelection();
    expect(session.selectedNodes().empty() && session.selection() == ui::CompositionSelection{} &&
               changes == clearedChanges,
           "clearSelection clears both states once");
    expect(session.snapshot().revision() == revision && stack.size() == history,
           "selection remains session-only with no dirty/history edit");
    session.selectNodes({a, b}, a);
    commands::Transaction remove("Remove primary");
    remove.emplace<commands::RemoveNodes>(compositionId, std::set{a});
    if (!stack.execute(std::move(remove)).changed() || !stack.undo().changed() || !session.redo())
        throw std::logic_error("selection removal history");
    expect(session.selectedNodes() == std::set{b} &&
               session.selection().primary == ui::SelectionTarget{b} &&
               session.selection().contextualLayer == layerB,
           "publication prunes removed primary and preserves surviving selection");
    expect(session.undo() && session.selectedNodes() == std::set{b},
           "undo restores nodes without resurrecting deleted session selection");
    session.selectNodes({a, b}, a);
    session.rebind(document, stack, compositionId);
    expect(session.selectedNodes().empty() && session.selection() == ui::CompositionSelection{},
           "rebind clears the complete set atomically");
}
void parentingBounds() {
    const auto format = document::CompositionFormat::create(32, 32);
    if (!format)
        throw std::logic_error("bounds fixture format");
    auto project = document::makeNewProject("Parent Bounds", "Main",
                                            core::RationalTime::fromInteger(10), *format);
    const auto compositionId = project.initialCompositionId;
    document::Document document(std::move(project.project));
    commands::CommandStack stack(document);
    ui::CompositionSession session(document, stack, compositionId);
    if (!session.addSolidLayer("Parent", {1, 0, 0, 1}) || !session.setSelectedPosition(7, 9))
        throw std::logic_error("parent bounds fixture");
    const auto parent = session.selection().contextualLayer;
    if (!session.addSolidLayer("Child", {0, 0, 1, 1}) || !session.setSelectedPosition(3, 4))
        throw std::logic_error("child bounds fixture");
    const auto child = session.selection().contextualLayer;
    if (!parent || !child)
        throw std::logic_error("bounds fixture layers");
    const auto evaluate = [&]() -> runtime::EvaluatedOperationBounds {
        const runtime::SnapshotCompiler compiler(document::builtInNodeDefinitions());
        const auto compiled = compiler.compile({session.snapshot(), compositionId}, {});
        if (!compiled.plan)
            throw std::logic_error("compile parent bounds");
        const runtime::CpuCompositionEvaluator evaluator;
        const runtime::EvaluationRequest request{
            .time = session.currentTime(),
            .output = compiled.plan->output(),
            .resolution = runtime::CompositionFormatResolution{},
            .quality = runtime::EvaluationQuality::Reference,
            .colorIntent = runtime::EvaluationColorIntent::LinearRec709Scene,
            .pixelStorageByteLimit = std::size_t{1} << 24U};
        const auto result = evaluator.evaluate(compiled.plan, request, {});
        if (!result.frame())
            throw std::logic_error("evaluate parent bounds");
        for (const auto& bounds : result.frame()->evaluatedBounds())
            if (bounds.layerId == *child)
                return bounds;
        throw std::logic_error("child bounds missing");
    };
    const auto before = evaluate();
    expect(session.setLayerParent(*child, *parent), "choose parent through the session");
    const auto after = evaluate();
    expect(after.anchor == document::Vec2d{before.anchor.x + 7, before.anchor.y + 9} &&
               after.output.left == before.output.left + 7 &&
               after.output.top == before.output.top + 9,
           "parenting moves the child viewer overlay anchor and bounds by the evaluated parent "
           "transform");
    expect(session.undo() && evaluate() == before, "parent undo restores exact viewer bounds");
}

void parenting() {
    auto project =
        document::makeNewProject("Parenting", "Main", core::RationalTime::fromInteger(10));
    const auto compositionId = project.initialCompositionId;
    document::Document document(std::move(project.project));
    commands::CommandStack stack(document);
    ui::CompositionSession session(document, stack, compositionId);
    std::vector<document::LayerId> layers;
    for (const auto* name : {"Grandparent", "Parent", "Child", "Sibling"}) {
        if (!session.addSolidLayer(QString::fromLatin1(name), {1, 0, 0, 1}))
            throw std::logic_error("parenting layer fixture");
        const auto selected = session.selection().contextualLayer;
        if (!selected)
            throw std::logic_error("parenting layer selection fixture");
        layers.push_back(*selected);
    }
    expect(session.setLayerParent(layers[1], layers[0]) &&
               session.setLayerParent(layers[2], layers[1]),
           "session issues parent commands");
    expect(session.parentOf(layers[2]) == layers[1] &&
               session.childrenOf(layers[0]) == std::vector{layers[1]},
           "session exposes immediate links");
    expect(session.candidateParents(layers[0]) == std::vector{layers[3]},
           "candidate parents exclude all descendants and self");
    expect(session.candidateParents(layers[2]).size() == 3,
           "ancestors and unrelated layers are candidate parents");
    ui::WindowStatusBar status(session, nullptr);
    expect(!session.setLayerParent(layers[0], layers[2]), "session rejects parent cycle");
    expect(status.messageTextForTest().contains("cycle", Qt::CaseInsensitive),
           "cycle refusal reaches the application status line with its reason");
    expect(session.setLayerParent(layers[2], std::nullopt) && !session.parentOf(layers[2]),
           "session clears parent through command");
    expect(session.undo() && session.parentOf(layers[2]) == layers[1], "parent edit undoes");
    expect(session.redo() && !session.parentOf(layers[2]), "parent edit redoes");
    const auto unknown = document::LayerId::fromRaw(999999);
    expect(!session.parentOf(unknown) && session.childrenOf(unknown).empty() &&
               session.candidateParents(unknown).empty(),
           "unknown layer accessors are empty");
}

} // namespace

int main(int argc, char** argv) {
    QApplication application(argc, argv);
    try {
        multiSelection();
        parenting();
        parentingBounds();
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    return failures == 0 ? 0 : 1;
}
