// Task FIX2, the owner's known case (2026-09-14): "timeline Blending dropdown does nothing on
// click". The S6 wiring behind it is real -- a pooled row reads whichever layer it is bound to and
// calls CompositionSession::setLayerBlendMode() -- so this file states the GESTURE rather than the
// wiring: a real click on the row's Blending cell opens the popup, and picking a mode commits it on
// the layer that row draws, with the pooled binding still correct after a scroll.
#include "surface_harness.hpp"

#include <bloom/ui/composition_authoring.hpp>
#include <bloom/ui/kit/controls.hpp>
#include <bloom/ui/kit/row.hpp>

using namespace bloom;
using namespace bloom::ui;
using namespace bloom::ui::surface_test;

namespace {

[[nodiscard]] std::optional<core::BlendMode> modeOfRow(const Surfaces& surfaces, const int index) {
    const auto* stack = surfaces.layerStack();
    if (stack == nullptr || index < 0 || index >= stack->rowCount()) {
        return std::nullopt;
    }
    return surfaces.session.blendModeForLayer(
        stack->entries()[static_cast<std::size_t>(index)].layerId);
}

[[nodiscard]] std::optional<document::LayerId> layerOfRow(const Surfaces& surfaces,
                                                          const int index) {
    const auto* stack = surfaces.layerStack();
    if (stack == nullptr || index < 0 || index >= stack->rowCount()) {
        return std::nullopt;
    }
    return stack->entries()[static_cast<std::size_t>(index)].layerId;
}

} // namespace

namespace {
int run(int argc, char** argv) {
    QApplication application(argc, argv);
    Surfaces surfaces;
    for (int layer = 0; layer < 12; ++layer) {
        expect(addDefaultSolidLayer(surfaces.session), "the fixture adds its solid layers");
    }
    QCoreApplication::processEvents();

    auto* stack = surfaces.layerStack();
    expect(stack != nullptr && stack->rowCount() == 12, "the column draws one row per layer");
    if (stack == nullptr || stack->rowCount() != 12) {
        return 1;
    }

    // The row the owner clicks is not necessarily the selected one, which is the whole point of the
    // row carrying its own control: row 1 here is a layer the selection is not on.
    const int targetRow = 1;
    auto* dropdown = surfaces.blendingDropdown(targetRow);
    expect(dropdown != nullptr, "the row carries its Blending dropdown");
    if (dropdown == nullptr) {
        return 1;
    }
    const auto targetLayer = layerOfRow(surfaces, targetRow);
    const auto otherLayer = layerOfRow(surfaces, 0);
    expect(targetLayer.has_value() && otherLayer.has_value(), "both rows name their layer");
    if (!targetLayer.has_value() || !otherLayer.has_value()) {
        return 1;
    }
    const auto before = modeOfRow(surfaces, targetRow);
    const auto otherBefore = modeOfRow(surfaces, 0);
    const auto selectionBefore = surfaces.session.selection();

    click(dropdown);
    expect(dropdown->isPopupVisible(), "clicking a timeline row's Blending cell opens its popup");
    if (!dropdown->isPopupVisible()) {
        return failures == 0 ? 0 : 1;
    }

    const int target = dropdown->currentIndex() == 0 ? 1 : 0;
    const auto expected =
        core::blendModeFromStoredValue(dropdown->itemData(target).value<std::int64_t>());
    pickPopupRow(*dropdown, target);
    expect(!dropdown->isPopupVisible(), "picking a mode closes the popup");
    expect(expected.has_value() && modeOfRow(surfaces, targetRow) == expected,
           "picking a mode commits it on the layer that row draws");
    expect(modeOfRow(surfaces, 0) == otherBefore, "and on no other layer");
    expect(surfaces.session.undoLabel() == QStringLiteral("Set Blend Mode"),
           "through the layer-keyed Set Blend Mode command");
    expect(surfaces.session.selection() == selectionBefore,
           "authoring a row's blending does not move the selection");
    expect(before != modeOfRow(surfaces, targetRow), "the mode actually changed");

    auto* kitRow = qobject_cast<kit::KRow*>(surfaces.layerRow(targetRow));
    expect(kitRow != nullptr, "timeline uses the shared KRow");
    auto* visibility =
        kitRow ? kitRow->findChild<kit::KIconToggle*>("timelineLayerToggle0") : nullptr;
    auto* lock = kitRow ? kitRow->findChild<kit::KIconToggle*>("timelineLayerToggle3") : nullptr;
    expect(visibility && lock, "visibility and lock are real kit controls");
    if (visibility && lock) {
        const auto enabled = [&] {
            return surfaces.session.composition()->graph().findLayer(*targetLayer)->enabled;
        };
        const bool wasEnabled = enabled();
        click(visibility);
        expect(enabled() != wasEnabled && visibility->isChecked() == enabled(),
               "window-level toggle click authors and projects visibility");
        (void)surfaces.session.undo();
        expect(enabled() == wasEnabled && visibility->isChecked() == wasEnabled,
               "undo projects back into the same toggle");
        lock->setFocus(Qt::TabFocusReason);
        QTest::keyClick(lock, Qt::Key_Space);
        QCoreApplication::processEvents();
        expect(surfaces.session.composition()->graph().findLayer(*targetLayer)->locked &&
                   !dropdown->isEnabled(),
               "keyboard activation locks the bound layer and disables its blending");
        (void)surfaces.session.undo();
        expect(dropdown->isEnabled(), "undo restores the row controls");
    }

    // Every other surface agrees immediately: the Layer card's own Blending row and the Properties
    // Appearance row both read the same session answer.
    surfaces.session.selectLayer(*targetLayer);
    QCoreApplication::processEvents();
    const auto boundary = surfaces.session.boundaryNodeForLayer(*targetLayer);
    expect(boundary.has_value(), "the layer resolves its boundary node");
    if (boundary.has_value()) {
        auto* cardDropdown =
            qobject_cast<kit::KDropdown*>(surfaces.nodes->graphScene()->nodeFieldForTest(
                *boundary, QStringLiteral("nodeBlendModeDropdown")));
        expect(cardDropdown != nullptr && cardDropdown->currentIndex() == target,
               "the Layer card shows the mode the timeline row committed");
    }
    auto* panelDropdown =
        surfaces.properties->findChild<kit::KDropdown*>(QStringLiteral("blendModeEditor"));
    expect(panelDropdown == nullptr || panelDropdown->currentIndex() == target,
           "the Properties Appearance row shows it too");

    // A scrolled column re-points its pooled rows; the control must author whatever the row now
    // draws, never the layer it drew when the widget was created.
    auto* scrollBar = surfaces.timeline->verticalScrollBarForTest();
    if (scrollBar != nullptr && scrollBar->maximum() > 0) {
        scrollBar->setValue(scrollBar->maximum());
        QCoreApplication::processEvents();
        const int lastRow = stack->rowCount() - 1;
        auto* scrolled = surfaces.blendingDropdown(lastRow);
        const auto scrolledLayer = layerOfRow(surfaces, lastRow);
        if (scrolled != nullptr && scrolledLayer.has_value()) {
            auto* toggle =
                surfaces.layerRow(lastRow)->findChild<kit::KIconToggle*>("timelineLayerToggle0");
            expect(toggle != nullptr, "scrolled row retains real toggle cells");
            if (toggle) {
                const bool enabled =
                    surfaces.session.composition()->graph().findLayer(*scrolledLayer)->enabled;
                click(toggle);
                expect(surfaces.session.composition()->graph().findLayer(*scrolledLayer)->enabled !=
                           enabled,
                       "pooled toggle authors its rebound layer");
                (void)surfaces.session.undo();
            }
            scrollBar->setValue(scrollBar->maximum());
            QCoreApplication::processEvents();
            scrolled = surfaces.blendingDropdown(lastRow);
            expect(scrolled != nullptr, "last row is visible after refresh");
            if (!scrolled)
                return 1;
            const auto scrolledBefore = surfaces.session.blendModeForLayer(*scrolledLayer);
            click(scrolled);
            const int pick = scrolled->currentIndex() == 0 ? 1 : 0;
            const auto picked =
                core::blendModeFromStoredValue(scrolled->itemData(pick).value<std::int64_t>());
            pickPopupRow(*scrolled, pick);
            expect(picked.has_value() &&
                       surfaces.session.blendModeForLayer(*scrolledLayer) == picked &&
                       scrolledBefore != picked,
                   "a pooled row scrolled onto another layer authors the layer it now draws");
        }
    }

    if (scrollBar != nullptr) {
        scrollBar->setValue(0);
        QCoreApplication::processEvents();
    }

    // The column's one tooltip table still answers for the row's painted cells: the row itself
    // carries no tooltip, so a help event on it is ignored and propagates to the column exactly as
    // a press does. Only the two dropdowns -- real controls -- carry their own.
    if (auto* toolTipRow = surfaces.layerRow(0); toolTipRow != nullptr) {
        expect(toolTipRow->toolTip().isEmpty(), "a row shadows none of the column's tooltips");
        expect(!stack->toolTipAt(QPoint(4, stack->rowTop(0) + 4)).isEmpty(),
               "the column still answers for the cells the row paints");
        auto* rowBlending = surfaces.blendingDropdown(0);
        expect(rowBlending != nullptr && !rowBlending->toolTip().isEmpty(),
               "the Blending control keeps its own");
    }

    // The column's own gestures still belong to the column: a press on the row's blank area
    // selects that row, exactly as it did when the row was transparent to the pointer.
    auto* row = surfaces.layerRow(0);
    expect(row != nullptr, "row zero is on screen");
    if (row != nullptr) {
        click(row, QPoint(TimelineEditor::layerColumnWidth() - 4, row->height() / 2));
        expect(stack->currentRow() == 0, "clicking a row still selects it");
        expect(surfaces.session.selection().primary == SelectionTarget{*otherLayer},
               "and selection is still the column's one truth");
    }

    return failures == 0 ? 0 : 1;
}
} // namespace

int main(int argc, char** argv) {
    try {
        return run(argc, argv);
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
