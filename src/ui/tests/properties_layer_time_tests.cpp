// ADR 0024: the Properties Time section's Before/After mode dropdowns and Time Remap switch.

#include <QApplication>
#include <bloom/commands/command_stack.hpp>
#include <bloom/commands/operations.hpp>
#include <bloom/commands/transaction.hpp>
#include <bloom/document/document.hpp>
#include <bloom/document/graph.hpp>
#include <bloom/document/new_project.hpp>
#include <bloom/document/project.hpp>
#include <bloom/ui/composition_session.hpp>
#include <bloom/ui/kit/dropdown.hpp>
#include <bloom/ui/kit/switch_control.hpp>
#include <bloom/ui/properties_editor.hpp>

#include <iostream>

using namespace bloom;
namespace {
int failures = 0;
void expect(const bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
        ++failures;
    }
}
const document::LayerOutputBoundary* boundary(ui::CompositionSession& s, document::LayerId id) {
    return s.composition()->graph().findLayer(id);
}
bool remapBound(ui::CompositionSession& s, document::LayerId id) {
    const auto* node = s.composition()->graph().findNode(boundary(s, id)->nodeId);
    for (const auto& binding : node->parameters)
        if (binding.role == document::kTimeRemapParameterRole)
            return true;
    return false;
}
} // namespace

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    auto project = document::makeNewProject("Time", "Main", core::RationalTime::fromInteger(10));
    document::Document document(std::move(project.project));
    commands::CommandStack stack(document);
    ui::CompositionSession session(document, stack, project.initialCompositionId);
    commands::Transaction add("Add", session.snapshot().revision());
    add.emplace<commands::AddSolidLayer>(project.initialCompositionId, "Solid",
                                         core::Color4d{0.2, 0.3, 0.4, 1.0});
    const auto layer = session.executeTransaction(std::move(add))
                           .outputId<document::LayerId>(commands::kAddSolidLayerLayerOutput);
    expect(layer.has_value(), "solid layer added");
    if (!layer)
        return 1;
    session.selectLayer(*layer);
    ui::PropertiesEditor properties(session);
    properties.show();

    auto* before = properties.findChild<ui::kit::KDropdown*>("layerBeforeModeDropdown");
    auto* after = properties.findChild<ui::kit::KDropdown*>("layerAfterModeDropdown");
    auto* remap = properties.findChild<ui::kit::KSwitch*>("layerTimeRemapSwitch");
    expect(before && after && remap, "widgets exist");
    if (!before || !after || !remap)
        return 1;
    expect(before->count() == 4 && before->currentText() == "None", "default None, four items");
    expect(!remap->isChecked(), "remap off by default");

    before->setCurrentIndex(2); // Loop
    expect(boundary(session, *layer)->timing.beforeMode == document::LayerTimeMode::Loop &&
               boundary(session, *layer)->timing.afterMode == document::LayerTimeMode::None,
           "before Loop committed");
    after->setCurrentIndex(3); // Ping-pong
    expect(boundary(session, *layer)->timing.afterMode == document::LayerTimeMode::PingPong &&
               boundary(session, *layer)->timing.beforeMode == document::LayerTimeMode::Loop,
           "after Ping-pong committed");
    expect(session.undo(), "undo after");
    expect(boundary(session, *layer)->timing.afterMode == document::LayerTimeMode::None &&
               after->currentIndex() == 0,
           "undo restores after and dropdown");
    expect(boundary(session, *layer)->timing.beforeMode == document::LayerTimeMode::Loop,
           "before still Loop");

    remap->setChecked(true);
    expect(remapBound(session, *layer) && remap->isChecked(), "remap enabled");
    expect(session.undo() && !remapBound(session, *layer) && !remap->isChecked(),
           "undo removes remap and switch follows");
    expect(session.redo() && remapBound(session, *layer) && remap->isChecked(), "redo remap");
    remap->setChecked(false);
    expect(!remapBound(session, *layer), "remap disabled");

    commands::Transaction lock("Lock", session.snapshot().revision());
    lock.emplace<commands::SetLayerLocked>(project.initialCompositionId, *layer, true);
    (void)session.executeTransaction(std::move(lock));
    expect(!before->isEnabled() && !after->isEnabled() && !remap->isEnabled(),
           "controls disabled when locked");

    if (failures == 0)
        std::cout << "properties layer time: all checks passed\n";
    return failures == 0 ? 0 : 1;
}
