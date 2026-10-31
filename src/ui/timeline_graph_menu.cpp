// The graph editor's context menu (T1, item 5). Right-clicking a key offers exactly:
// Interpolation, Easing, Easy Ease / In / Out, Handles (Auto / Aligned / Free) and
// Delete. Every entry is an existing command; nothing here edits a curve by itself.

#include <bloom/ui/timeline_graph_view.hpp>

#include "timeline_graph_easing.hpp"

#include <bloom/commands/transaction.hpp>
#include <bloom/document/project.hpp>
#include <bloom/ui/composition_session.hpp>
#include <bloom/ui/kit/controls.hpp>

#include <QAction>
#include <QActionGroup>
#include <QContextMenuEvent>
#include <QMenu>

#include <algorithm>
#include <memory>

namespace bloom::ui {
namespace {

[[nodiscard]] QString text(const std::string_view value) {
    return QString::fromUtf8(value.data(), static_cast<qsizetype>(value.size()));
}

} // namespace

void TimelineGraphView::contextMenuEvent(QContextMenuEvent* event) {
    const auto hit = hitTest(event->pos());
    cancelGesture();
    const std::unique_ptr<QMenu> owner(kit::makeMenu(this));
    auto& menu = *owner;
    menu.setObjectName(QStringLiteral("timelineGraphContextMenu"));
    if (hit.has_value() && hit->kind != GraphHit::Kind::Segment) {
        // A right-click on a key acts on the selection it belongs to; on an unselected key it
        // selects that key first, as a click would.
        const KeyframeSelection address{hit->curve.curveId, hit->keyframeId, hit->curve.component};
        if (std::ranges::find(session_.selection().keyframes, address) ==
            session_.selection().keyframes.end()) {
            if (address.component.has_value())
                session_.selectKeyframe(address.curveId, *address.component, address.keyframeId);
            else
                session_.selectKeyframe(address.curveId, address.keyframeId);
        }
        populateContextMenu(menu);
    } else {
        auto* fit = menu.addAction(tr("Fit Curves"));
        fit->setObjectName(QStringLiteral("graphFitAction"));
        connect(fit, &QAction::triggered, this, [this] { fitCurves(); });
    }
    menu.exec(event->globalPos());
    event->accept();
}

void TimelineGraphView::populateContextMenu(QMenu& menu) {
    // The selected keys that own an outgoing segment. A curve's final key is canonical Linear
    // (docs/architecture/animation-and-time.md), so a mode it cannot take is applied to the rest of
    // the selection rather than refused for all of it.
    std::vector<commands::KeyframeAddress> all;
    std::vector<commands::KeyframeAddress> segments;
    std::vector<document::KeyframeInterpolation> modes;
    std::vector<document::KeyframeHandleMode> handleModes;
    for (const auto& key : session_.selection().keyframes) {
        const commands::KeyframeAddress address{key.curveId, key.keyframeId, key.component};
        all.push_back(address);
        const auto curve = curveOf(key);
        if (!curve.has_value())
            continue;
        const auto keys = keysFor(*curve, false);
        const auto at = std::ranges::find(keys, key.keyframeId, &document::ScalarKeyframe::id);
        if (at != keys.end())
            handleModes.push_back(at->handleMode);
        if (at == keys.end() || at + 1 == keys.end())
            continue;
        segments.push_back(address);
        modes.push_back(at->outgoingInterpolation);
    }
    const auto apply = [this](std::vector<commands::KeyframeAddress> keys,
                              const document::KeyframeInterpolation mode, const char* label) {
        if (keys.empty())
            return;
        commands::Transaction transaction(label, session_.snapshot().revision());
        transaction.emplace<commands::SetKeyframesInterpolation>(session_.compositionId(),
                                                                 std::move(keys), mode);
        (void)session_.executeTransaction(std::move(transaction));
    };
    const auto targetsFor = [&](const document::KeyframeInterpolation mode) {
        return mode == document::KeyframeInterpolation::Linear ? all : segments;
    };

    auto* interpolation = kit::makeMenu(tr("Interpolation"), &menu);
    interpolation->setObjectName(QStringLiteral("graphInterpolationMenu"));
    auto* group = new QActionGroup(interpolation);
    group->setExclusive(true);
    const bool uniform =
        !modes.empty() && std::ranges::all_of(modes, [&](auto mode) { return mode == modes[0]; });
    for (const auto& [mode, label] :
         {std::pair{document::KeyframeInterpolation::Hold, tr("Constant")},
          std::pair{document::KeyframeInterpolation::Linear, tr("Linear")},
          std::pair{document::KeyframeInterpolation::EaseInOut, tr("Bezier")}}) {
        auto* action = interpolation->addAction(label);
        action->setObjectName(QStringLiteral("graphInterpolationAction"));
        action->setCheckable(true);
        action->setChecked(uniform && modes[0] == mode);
        action->setEnabled(!targetsFor(mode).empty());
        group->addAction(action);
        connect(action, &QAction::triggered, this, [apply, targets = targetsFor(mode), mode] {
            apply(targets, mode, "Set Keyframes Interpolation");
        });
    }
    menu.addMenu(interpolation);

    // Easing presets shape the segments BETWEEN keys, so they ask for two or more of them.
    auto* easing = kit::makeMenu(tr("Easing"), &menu);
    easing->setObjectName(QStringLiteral("graphEasingMenu"));
    easing->menuAction()->setEnabled(all.size() >= 2);
    for (const auto& [name, mode] : kGraphEasingBasics) {
        auto* action = easing->addAction(tr(name.data()));
        action->setObjectName(QStringLiteral("graphEasingAction"));
        action->setEnabled(!targetsFor(mode).empty());
        connect(action, &QAction::triggered, this,
                [apply, targets = targetsFor(mode), mode] { apply(targets, mode, "Set Easing"); });
    }
    easing->addSeparator();
    for (const auto& family : kGraphEasingFamilies) {
        auto* submenu = kit::makeMenu(text(family.name), easing);
        submenu->setObjectName(QStringLiteral("graphEasingFamilyMenu"));
        for (const auto& [modeName, mode] : kGraphEasingModes) {
            auto* action = submenu->addAction(tr(modeName.data()));
            action->setObjectName(QStringLiteral("graphEasingModeAction"));
            action->setEnabled(!segments.empty());
            // The equation and its ease end in ONE command, so the pick is one undo.
            connect(action, &QAction::triggered, this,
                    [this, targets = segments, mode, interpolation = family.interpolation] {
                        commands::Transaction transaction("Set Easing",
                                                          session_.snapshot().revision());
                        transaction.emplace<commands::SetKeyframesEasing>(
                            session_.compositionId(), targets,
                            commands::KeyframeEasingEdit{.easing = mode}, interpolation);
                        (void)session_.executeTransaction(std::move(transaction));
                    });
        }
        easing->addMenu(submenu);
    }
    menu.addMenu(easing);
    menu.addSeparator();

    // The timeline's own actions, so the menu shows -- and is -- the F9 family.
    for (auto* action : easeActions_)
        if (action != nullptr)
            menu.addAction(action);

    // Handles > Auto / Aligned / Free. The engine owns what a mode means (Auto recomputes handles,
    // Aligned mirrors them); the menu only issues the one command for the selected keys.
    auto* handles = kit::makeMenu(tr("Handles"), &menu);
    handles->setObjectName(QStringLiteral("graphHandlesMenu"));
    handles->menuAction()->setEnabled(!all.empty());
    auto* handleGroup = new QActionGroup(handles);
    handleGroup->setExclusive(false);
    const bool commonMode = !handleModes.empty() && std::ranges::all_of(handleModes, [&](auto mode) {
        return mode == handleModes[0];
    });
    for (const auto& [mode, label] :
         {std::pair{document::KeyframeHandleMode::Auto, tr("Auto")},
          std::pair{document::KeyframeHandleMode::Aligned, tr("Aligned")},
          std::pair{document::KeyframeHandleMode::Free, tr("Free")}}) {
        auto* action = handles->addAction(label);
        action->setObjectName(QStringLiteral("graphHandleModeAction"));
        action->setCheckable(true);
        action->setChecked(commonMode && handleModes[0] == mode);
        action->setEnabled(!all.empty());
        handleGroup->addAction(action);
        connect(action, &QAction::triggered, this, [this, targets = all, mode] {
            commands::Transaction transaction("Set Handle Mode", session_.snapshot().revision());
            transaction.emplace<commands::SetKeyframesHandleMode>(session_.compositionId(), targets,
                                                                  mode);
            (void)session_.executeTransaction(std::move(transaction));
        });
    }
    menu.addMenu(handles);
    menu.addSeparator();
    auto* remove = menu.addAction(tr("Delete"));
    remove->setObjectName(QStringLiteral("graphDeleteAction"));
    remove->setEnabled(!all.empty());
    connect(remove, &QAction::triggered, this,
            [this] { (void)session_.deleteSelectedKeyframes(); });
}

} // namespace bloom::ui
