#include "preferences_window.hpp"

#include "properties_sections.hpp"

#include <bloom/ui/kit/button.hpp>
#include <bloom/ui/kit/controls.hpp>
#include <bloom/ui/kit/row.hpp>
#include <bloom/ui/kit/section.hpp>
#include <bloom/ui/kit/slider.hpp>
#include <bloom/ui/kit/switch_control.hpp>
#include <bloom/ui/kit/tokens.hpp>

#include <QFrame>
#include <QLabel>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QVBoxLayout>
#include <QWidget>

#include <cmath>

namespace bloom::ui {

// The Sounds page lives in its own translation unit so the dialog construction file stays under
// the size budget. It uses the same row vocabulary as the other pages and adopts whatever form the
// dialog gives them. Everything here edits the draft; nothing plays until Apply except the test
// button, which previews the draft's volume.
QWidget* PreferencesWindow::buildSoundsPage() {
    auto* content = new QWidget;
    auto* layout = new QVBoxLayout(content);
    layout->setContentsMargins(kit::px(kit::Spacing::M), kit::px(kit::Spacing::M),
                               kit::px(kit::Spacing::M), kit::px(kit::Spacing::M));
    layout->setSpacing(kit::px(kit::Spacing::S));

    auto* section = properties::makeSection(
        layout, content, QStringLiteral("preferencesSoundsSection"), tr("Sounds"),
        QStringLiteral("preferences/sections/sounds/collapsed"));
    auto* rows = section->bodyLayout();

    const auto addSwitch = [&](const QString& objectName, const QString& label,
                               bool ApplicationPreferences::*member, const QString& tip) {
        auto* control = addSwitchRow(rows, content, objectName, label, false);
        control->setToolTip(tip);
        control->setChecked(draft_.*member);
        connect(control, &QAbstractButton::toggled, this, [this, member](const bool enabled) {
            draft_.*member = enabled;
            updateApplyEnabled();
        });
        refreshCallbacks_.push_back([this, control, member] {
            const QSignalBlocker blocker(control);
            control->setChecked(draft_.*member);
        });
        return control;
    };

    auto* master = addSwitch(QStringLiteral("preferencesSoundsEnabledSwitch"), tr("Play sounds"),
                             &ApplicationPreferences::soundsEnabled,
                             tr("Turns every Bloom sound on or off."));

    auto* volumeSlider = new kit::KSlider(content);
    volumeSlider->setObjectName(QStringLiteral("preferencesSoundVolumeSlider"));
    volumeSlider->setAccessibleName(tr("Volume"));
    volumeSlider->setRange(0.0, 100.0);
    volumeSlider->setValue(draft_.soundVolumePercent);
    auto* volumeLabel = new kit::KLabel(QString{}, content, kit::TypeRole::UiSmall);
    volumeLabel->setObjectName(QStringLiteral("preferencesSoundVolumeValue"));
    volumeLabel->setTone(kit::Color::Muted);
    const auto showVolume = [this, volumeLabel] {
        volumeLabel->setText(tr("%1%").arg(draft_.soundVolumePercent));
    };
    showVolume();
    {
        auto* row = new kit::KPropertyRow(properties::makeRowLabel(tr("Volume"), content), nullptr,
                                          {volumeSlider, volumeLabel}, content);
        row->setLabelWidth(kit::px(kit::Size::PreferencesLabelWidth));
        rows->addWidget(row);
    }
    connect(volumeSlider, &kit::KSlider::valueChanged, this,
            [this, showVolume](const double value) {
                draft_.soundVolumePercent = static_cast<int>(std::lround(value));
                showVolume();
                updateApplyEnabled();
            });
    refreshCallbacks_.push_back([this, volumeSlider, showVolume] {
        const QSignalBlocker blocker(volumeSlider);
        volumeSlider->setValue(draft_.soundVolumePercent);
        showVolume();
    });

    auto* events = addSwitch(
        QStringLiteral("preferencesEventSoundsSwitch"), tr("Event sounds"),
        &ApplicationPreferences::eventSoundsEnabled,
        tr("Render and import results, warnings and refused commands."));
    auto* interaction = addSwitch(
        QStringLiteral("preferencesInteractionSoundsSwitch"), tr("Interaction sounds"),
        &ApplicationPreferences::interactionSoundsEnabled,
        tr("Small sounds for switches, choices, dialogs and tools. Never while scrubbing."));
    addSwitch(QStringLiteral("preferencesRenderNotificationsSwitch"),
              tr("Notify when a render finishes"),
              &ApplicationPreferences::renderNotificationsEnabled,
              tr("Shows a system notification when a render finishes or fails while Bloom is not "
                 "the active window."));

    // The category and volume rows follow the master switch.
    const auto syncEnabled = [this, volumeSlider, events, interaction] {
        const bool on = draft_.soundsEnabled;
        volumeSlider->setEnabled(on);
        events->setEnabled(on);
        interaction->setEnabled(on);
    };
    syncEnabled();
    connect(master, &QAbstractButton::toggled, this, [syncEnabled](bool) { syncEnabled(); });
    refreshCallbacks_.push_back(syncEnabled);

    auto* test = new kit::KButton(tr("Play test sound"), content);
    test->setObjectName(QStringLiteral("preferencesPlayTestSoundButton"));
    connect(test, &QAbstractButton::clicked, this, [this] { emit soundTestRequested(draft_); });
    {
        auto* row = new kit::KPropertyRow(properties::makeRowLabel(QString{}, content), nullptr,
                                          {test}, content);
        row->setLabelWidth(kit::px(kit::Size::PreferencesLabelWidth));
        rows->addWidget(row);
    }

    layout->addStretch(1);
    auto* scroll = new QScrollArea;
    scroll->setObjectName(QStringLiteral("preferencesSoundsPage"));
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->viewport()->setAutoFillBackground(false);
    scroll->setWidget(content);
    content->setAutoFillBackground(false);
    return scroll;
}

} // namespace bloom::ui
