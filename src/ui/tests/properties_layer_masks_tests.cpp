// ADR 0028: the Properties Masks and Track Matte sections.

#include <QApplication>
#include <bloom/commands/command_stack.hpp>
#include <bloom/commands/layer_mask_operations.hpp>
#include <bloom/commands/operations.hpp>
#include <bloom/commands/transaction.hpp>
#include <bloom/document/document.hpp>
#include <bloom/document/graph.hpp>
#include <bloom/document/new_project.hpp>
#include <bloom/document/project.hpp>
#include <bloom/ui/composition_session.hpp>
#include <bloom/ui/kit/button.hpp>
#include <bloom/ui/kit/controls.hpp>
#include <bloom/ui/kit/dropdown.hpp>
#include <bloom/ui/kit/switch_control.hpp>
#include <bloom/ui/kit/value_field.hpp>
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
document::PathValue square() {
    document::PathValue path;
    path.closed = true;
    path.anchors = {{{0, 0}, {}, {}}, {{10, 0}, {}, {}}, {{10, 10}, {}, {}}, {{0, 10}, {}, {}}};
    return path;
}
const document::LayerOutputBoundary* layerOf(ui::CompositionSession& s, document::LayerId id) {
    return s.composition()->graph().findLayer(id);
}
} // namespace

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    auto project = document::makeNewProject("Masks", "Main", core::RationalTime::fromInteger(10));
    document::Document document(std::move(project.project));
    commands::CommandStack stack(document);
    ui::CompositionSession session(document, stack, project.initialCompositionId);
    const auto addSolid = [&] {
        commands::Transaction add("Add", session.snapshot().revision());
        add.emplace<commands::AddSolidLayer>(project.initialCompositionId, "Solid",
                                             core::Color4d{0.2, 0.3, 0.4, 1.0});
        return session.executeTransaction(std::move(add))
            .outputId<document::LayerId>(commands::kAddSolidLayerLayerOutput);
    };
    const auto first = addSolid();
    const auto second = addSolid();
    if (!first || !second) {
        std::cerr << "layers not added\n";
        return 1;
    }
    session.selectLayer(*first);
    ui::PropertiesEditor properties(session);
    properties.show();

    expect(properties.findChild<ui::kit::KValueField*>("layerMaskFeatherField") == nullptr,
           "no mask groups without masks");
    expect(session.addLayerMask(*first, square()), "mask added through the session");
    const auto mask = layerOf(session, *first)->masks.at(0).id;
    expect(session.selectedMask() && session.selectedMask()->mask == mask, "new mask is selected");
    QApplication::processEvents();

    auto* name = properties.findChild<ui::kit::KLineEdit*>("layerMaskNameEdit");
    auto* mode = properties.findChild<ui::kit::KDropdown*>("layerMaskModeDropdown");
    auto* inverted = properties.findChild<ui::kit::KSwitch*>("layerMaskInvertedSwitch");
    auto* feather = properties.findChild<ui::kit::KValueField*>("layerMaskFeatherField");
    auto* opacity = properties.findChild<ui::kit::KValueField*>("layerMaskOpacityField");
    expect(name && mode && inverted && feather && opacity, "mask group widgets exist");
    if (!name || !mode || !inverted || !feather || !opacity)
        return 1;
    expect(name->text() == "Mask 1" && mode->currentData().toInt() == 1 && !inverted->isChecked(),
           "group shows name, Add mode, not inverted");
    expect(opacity->value() == 1.0 && feather->value() == 0.0, "group shows parameter defaults");

    mode->chooseIndex(mode->findData(3)); // Intersect
    expect(layerOf(session, *first)->masks[0].mode == document::LayerMaskMode::Intersect,
           "mode dropdown commits SetLayerMaskMode");
    inverted->setChecked(true);
    expect(layerOf(session, *first)->masks[0].inverted, "inverted switch commits");
    name->setText("Hero");
    name->editingFinished();
    expect(layerOf(session, *first)->masks[0].name == "Hero", "name field commits rename");

    feather->setValue(12.0);
    {
        const auto* record =
            session.composition()->parameters().find(layerOf(session, *first)->masks[0].feather);
        const auto* constant = std::get_if<document::ConstantValueSource>(&record->source);
        expect(constant && std::get<double>(constant->value) == 12.0,
               "feather row edits the mask's own parameter");
    }
    expect(session.undo(), "undo feather");
    expect(session.composition()->parameters().find(layerOf(session, *first)->masks[0].feather) !=
               nullptr,
           "parameter survives undo");
    QApplication::processEvents();
    expect(properties.findChild<ui::kit::KValueField*>("layerMaskFeatherField")->value() == 0.0,
           "feather field follows undo");
    expect(session.undo() && session.undo() && session.undo(), "undo rename, invert, mode");
    QApplication::processEvents();
    expect(layerOf(session, *first)->masks[0].mode == document::LayerMaskMode::Add &&
               !layerOf(session, *first)->masks[0].inverted,
           "undo restored mode and inversion");

    // Second mask, then move and remove through the buttons.
    expect(session.addLayerMask(*first, square(), document::LayerMaskMode::Subtract),
           "second mask");
    QApplication::processEvents();
    auto buttons = properties.findChildren<ui::kit::KButton*>("layerMaskMoveUpButton");
    expect(buttons.size() == 2, "two mask groups");
    const auto secondId = layerOf(session, *first)->masks[1].id;
    buttons.at(1)->click();
    expect(layerOf(session, *first)->masks[0].id == secondId, "move up reorders");
    QApplication::processEvents();
    auto removers = properties.findChildren<ui::kit::KButton*>("layerMaskRemoveButton");
    removers.at(0)->click();
    expect(layerOf(session, *first)->masks.size() == 1, "remove button removes one mask");
    expect(session.undo() && layerOf(session, *first)->masks.size() == 2, "undo restores mask");

    // Track matte.
    auto* source = properties.findChild<ui::kit::KDropdown*>("layerTrackMatteSourceDropdown");
    auto* channel = properties.findChild<ui::kit::KDropdown*>("layerTrackMatteChannelDropdown");
    auto* invertMatte = properties.findChild<ui::kit::KSwitch*>("layerTrackMatteInvertedSwitch");
    expect(source && channel && invertMatte, "matte widgets exist");
    if (!source || !channel || !invertMatte)
        return 1;
    expect(source->count() == 2 && !channel->isEnabled(), "None plus one candidate; channel off");
    source->chooseIndex(1);
    expect(layerOf(session, *first)->trackMatte &&
               layerOf(session, *first)->trackMatte->source == *second,
           "source dropdown sets the matte");
    channel->chooseIndex(1);
    invertMatte->setChecked(true);
    expect(layerOf(session, *first)->trackMatte->channel == document::TrackMatteChannel::Luma &&
               layerOf(session, *first)->trackMatte->inverted,
           "channel and invert commit");
    source->chooseIndex(0);
    expect(!layerOf(session, *first)->trackMatte, "None clears the matte");

    commands::Transaction lock("Lock", session.snapshot().revision());
    lock.emplace<commands::SetLayerLocked>(project.initialCompositionId, *first, true);
    (void)session.executeTransaction(std::move(lock));
    QApplication::processEvents();
    expect(!source->isEnabled() &&
               !properties.findChild<ui::kit::KDropdown*>("layerMaskModeDropdown")->isEnabled(),
           "locked layer disables mask and matte controls");

    if (failures == 0)
        std::cout << "properties layer masks: all checks passed\n";
    return failures == 0 ? 0 : 1;
}
