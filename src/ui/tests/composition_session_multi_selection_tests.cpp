#include <bloom/commands/node_operations.hpp>
#include <bloom/document/new_project.hpp>
#include <bloom/ui/composition_session.hpp>

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
} // namespace

int main(int argc, char** argv) {
    QApplication application(argc, argv);
    try {
        multiSelection();
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    return failures == 0 ? 0 : 1;
}
