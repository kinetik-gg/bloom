// The Export panel's Outputs section: one output's preset, format settings, destination and
// overwrite policy, with the host's preflight reasons inline under the control they concern.

#include "export_detail_fields.hpp"
#include "export_detail_support.hpp"
#include "export_panel_text.hpp"

#include "properties_sections.hpp"

#include <bloom/ui/composition_session.hpp>
#include <bloom/ui/export_queue_adapter.hpp>
#include <bloom/ui/kit/button.hpp>
#include <bloom/ui/kit/controls.hpp>
#include <bloom/ui/kit/dropdown.hpp>
#include <bloom/ui/kit/file_chooser.hpp>
#include <bloom/ui/kit/row.hpp>
#include <bloom/ui/kit/section.hpp>
#include <bloom/ui/kit/switch_control.hpp>
#include <bloom/ui/kit/text_field.hpp>
#include <bloom/ui/kit/value_field.hpp>

#include <QDir>
#include <QSignalBlocker>
#include <QUrl>
#include <QHBoxLayout>
#include <QVBoxLayout>

#include <algorithm>

namespace bloom::ui::exportpanel {

using detail::displayPath;
using detail::fill;
using detail::isH26x;
using detail::isImage;
using detail::isMovie;
using detail::named;
using detail::choicesOf;
using detail::reasonLabel;
using detail::select;
using document::ExportFormat;

void ExportDetailPane::buildOutputs(QVBoxLayout& layout) {
    auto* section = properties::makeSection(&layout, this, QStringLiteral("exportOutputsSection"),
                                            tr("Outputs"),
                                            QStringLiteral("export/sections/outputs/collapsed"));
    section->setBodyPadding(kit::px(kit::Spacing::XS));
    section->setResetEnabled(false);
    auto* rows = section->bodyLayout();

    f_->outputPicker = named(new kit::KDropdown(this), QStringLiteral("exportOutputPicker"),
                             tr("Output"));
    f_->outputPickerRow = addRow(*rows, tr("Output"), {f_->outputPicker});
    f_->preset = named(new kit::KDropdown(this), QStringLiteral("exportPresetPicker"), tr("Preset"));
    addRow(*rows, tr("Preset"), {f_->preset});
    f_->presetReason = reasonLabel(this, QStringLiteral("exportPresetReason"), kit::Color::Warn);
    f_->remedy = new kit::KButton(this);
    f_->remedy->setObjectName(QStringLiteral("exportRemedyButton"));
    f_->presetReasonRow = addRow(*rows, QString{}, {f_->presetReason, f_->remedy});

    f_->profile = named(new kit::KDropdown(this), QStringLiteral("exportProfile"), tr("Profile"));
    f_->profileRow = addRow(*rows, tr("Profile"), {f_->profile});
    f_->bitrate = named(new kit::KValueField(this), QStringLiteral("exportBitrate"), tr("Bit rate"));
    f_->bitrate->setIntegerMode(true);
    f_->bitrate->setIntegerRange(0, 1'000'000);
    f_->bitrate->setUnit(tr("kbps"));
    f_->bitrate->setSpecialValueText(tr("Preset default"));
    f_->bitrateRow = addRow(*rows, tr("Bit rate"), {f_->bitrate});
    f_->compression = named(new kit::KDropdown(this), QStringLiteral("exportCompression"),
                            tr("Compression"));
    f_->compressionRow = addRow(*rows, tr("Compression"), {f_->compression});
    f_->colourSpace = named(new kit::KLineEdit(this), QStringLiteral("exportColourSpace"),
                            tr("Output colour space"));
    f_->colourSpace->setPlaceholderText(tr("Working colour space"));
    f_->colourSpaceRow = addRow(*rows, tr("Colour space"), {f_->colourSpace});
    f_->display = named(new kit::KLineEdit(this), QStringLiteral("exportDisplay"), tr("Display"));
    f_->display->setPlaceholderText(tr("Project default"));
    f_->displayRow = addRow(*rows, tr("Display"), {f_->display});
    f_->view = named(new kit::KLineEdit(this), QStringLiteral("exportView"), tr("View"));
    f_->view->setPlaceholderText(tr("Project default"));
    f_->viewRow = addRow(*rows, tr("View"), {f_->view});
    f_->bakeLook = named(new kit::KSwitch(this), QStringLiteral("exportBakeLook"),
                         tr("Bake look-tagged effects"));
    f_->bakeLookRow = addRow(*rows, tr("Bake look"), {f_->bakeLook});
    f_->useCachedFrames = named(new kit::KSwitch(this), QStringLiteral("exportUseCachedFrames"),
                                tr("Use cached frames"));
    f_->useCachedFramesRow = addRow(*rows, tr("Cached frames"), {f_->useCachedFrames});
    f_->includeAudio = named(new kit::KSwitch(this), QStringLiteral("exportIncludeAudio"),
                             tr("Include audio"));
    f_->includeAudioRow = addRow(*rows, tr("Audio"), {f_->includeAudio});
    f_->sampleRate = named(new kit::KDropdown(this), QStringLiteral("exportSampleRate"),
                           tr("Audio sample rate"));
    fill(*f_->sampleRate, {{tr("44.1 kHz"), "44100"}, {tr("48 kHz"), "48000"}, {tr("96 kHz"), "96000"}});
    f_->sampleRateRow = addRow(*rows, tr("Sample rate"), {f_->sampleRate});
    f_->encoder = named(new kit::KDropdown(this), QStringLiteral("exportEncoder"), tr("Encoder"));
    f_->encoder->addItem(tr("Automatic"), static_cast<int>(document::ExportEncoderPreference::Automatic));
    f_->encoder->addItem(tr("Software"), static_cast<int>(document::ExportEncoderPreference::Software));
    f_->encoder->addItem(tr("Hardware"), static_cast<int>(document::ExportEncoderPreference::Hardware));
    f_->encoderRow = addRow(*rows, tr("Encoder"), {f_->encoder});

    const auto numberField = [this](const QString& name, const QString& label) {
        auto* field = named(new kit::KValueField(this), name, label);
        field->setIntegerMode(true);
        return field;
    };
    f_->sequenceStart = numberField(QStringLiteral("exportSequenceStart"), tr("Start frame number"));
    f_->sequenceStart->setIntegerRange(0, 1'000'000'000);
    f_->sequenceStartMode = named(new kit::KDropdown(this), QStringLiteral("exportSequenceStartMode"),
                                  tr("Start number"));
    f_->sequenceStartMode->addItem(tr("First frame"), 0);
    f_->sequenceStartMode->addItem(tr("Custom"), 1);
    f_->sequenceStart->setSpecialValueText(QString{});
    f_->sequenceStartRow =
        addRow(*rows, tr("Numbering"), {f_->sequenceStartMode});
    f_->sequenceStartNumberRow = addRow(*rows, tr("Start number"), {f_->sequenceStart});
    f_->padding = numberField(QStringLiteral("exportPadding"), tr("Frame number padding"));
    f_->padding->setIntegerRange(1, 20);
    f_->padding->setUnit(tr("digits"));
    f_->paddingRow = addRow(*rows, tr("Padding"), {f_->padding});
    f_->pattern = named(new kit::KLineEdit(this), QStringLiteral("exportPattern"), tr("File name pattern"));
    f_->patternRow = addRow(*rows, tr("File names"), {f_->pattern});

    f_->destination = named(new kit::KPathField(this), QStringLiteral("exportDestination"),
                            tr("Destination"));
    f_->browse = new kit::KButton(tr("Browse…"), this);
    f_->browse->setObjectName(QStringLiteral("exportBrowseButton"));
    addRow(*rows, tr("Destination"), {f_->destination, f_->browse});
    f_->destinationReason = reasonLabel(this, QStringLiteral("exportDestinationReason"), kit::Color::Warn);
    f_->destinationReasonRow = addRow(*rows, QString{}, {f_->destinationReason});
    f_->tokenHelp = reasonLabel(this, QStringLiteral("exportTokenHelp"), kit::Color::Muted);
    f_->tokenHelp->setText(
        tr("Tokens: {name} composition, {job} job, {format}, {date}, <base> and <ext> from the "
           "file name, #### or {frame} for sequence frames."));
    addRow(*rows, QString{}, {f_->tokenHelp});
    f_->overwrite = named(new kit::KDropdown(this), QStringLiteral("exportOverwrite"),
                          tr("If the file exists"));
    f_->overwrite->addItem(tr("Ask before replacing"),
                           static_cast<int>(document::ExportOverwritePolicy::AskBeforeReplacing));
    f_->overwrite->addItem(tr("Replace"), static_cast<int>(document::ExportOverwritePolicy::Replace));
    f_->overwrite->addItem(tr("Refuse"), static_cast<int>(document::ExportOverwritePolicy::Refuse));
    addRow(*rows, tr("If it exists"), {f_->overwrite});
    f_->replaceText = reasonLabel(this, QStringLiteral("exportReplaceText"), kit::Color::Warn);
    f_->replaceText->setText(tr("Replace existing files?"));
    f_->replace = new kit::KButton(tr("Replace"), this);
    f_->replace->setObjectName(QStringLiteral("exportReplaceButton"));
    f_->replaceRow = addRow(*rows, QString{}, {f_->replaceText, f_->replace});
    f_->estimate = reasonLabel(this, QStringLiteral("exportEstimate"), kit::Color::Muted);
    f_->estimateRow = addRow(*rows, QString{}, {f_->estimate});
    f_->outputEnabled = named(new kit::KSwitch(this), QStringLiteral("exportOutputEnabled"),
                              tr("Render this output"));
    addRow(*rows, tr("Enabled"), {f_->outputEnabled});

    auto* buttons = new QWidget(this);
    auto* buttonLayout = new QHBoxLayout(buttons);
    buttonLayout->setContentsMargins(0, 0, 0, 0);
    buttonLayout->setSpacing(kit::px(kit::Spacing::XS));
    f_->addOutput = new kit::KButton(tr("Add output"), buttons);
    f_->addOutput->setObjectName(QStringLiteral("exportAddOutputButton"));
    f_->removeOutput = new kit::KButton(tr("Remove output"), buttons);
    f_->removeOutput->setObjectName(QStringLiteral("exportRemoveOutputButton"));
    buttonLayout->addWidget(f_->addOutput);
    buttonLayout->addWidget(f_->removeOutput);
    buttonLayout->addStretch(1);
    rows->addWidget(buttons);

    f_->outputEditors = {f_->sequenceStartMode, f_->preset,    f_->profile,      f_->bitrate,  f_->compression,
                         f_->colourSpace, f_->display,    f_->view,     f_->bakeLook, f_->useCachedFrames,
                         f_->includeAudio, f_->sampleRate, f_->encoder,  f_->sequenceStart,
                         f_->padding,   f_->pattern,      f_->destination, f_->browse,
                         f_->overwrite, f_->outputEnabled};

    connect(f_->outputPicker, &kit::KDropdown::activated, this, [this] {
        if (!refreshing_)
            setSelection(job_, document::ExportOutputId::fromRaw(
                                   f_->outputPicker->currentData().toULongLong()));
    });
    connect(f_->preset, &kit::KDropdown::activated, this, [this] { applyPreset(); });
    connect(f_->remedy, &kit::KButton::clicked, this, [this] {
        if (const auto* output = currentOutput())
            adapter_.requestRemedy(output->availability.remedy);
    });
    connect(f_->profile, &kit::KDropdown::activated, this, [this] {
        commitSettings([this](document::ExportOutputSettings& s) {
            s.codecProfile = f_->profile->currentData().toString().toStdString();
        });
    });
    connect(f_->bitrate, &kit::KValueField::editFinished, this, [this] {
        commitSettings([this](document::ExportOutputSettings& s) {
            const auto kbps = f_->bitrate->integerValue();
            s.videoBitrateKbps = kbps > 0 ? std::optional<std::uint32_t>(static_cast<std::uint32_t>(kbps))
                                          : std::nullopt;
        });
    });
    connect(f_->compression, &kit::KDropdown::activated, this, [this] {
        commitSettings([this](document::ExportOutputSettings& s) {
            s.compression = f_->compression->currentData().toString().toStdString();
        });
    });
    const auto textSetting = [this](kit::KLineEdit* edit, std::string document::ExportOutputSettings::*member) {
        connect(edit, &QLineEdit::editingFinished, this, [this, edit, member] {
            commitSettings([edit, member](document::ExportOutputSettings& s) {
                s.*member = edit->text().toStdString();
            });
        });
    };
    textSetting(f_->colourSpace, &document::ExportOutputSettings::outputColorSpaceId);
    textSetting(f_->display, &document::ExportOutputSettings::displayName);
    textSetting(f_->view, &document::ExportOutputSettings::viewName);
    connect(f_->bakeLook, &QAbstractButton::toggled, this, [this](const bool on) {
        commitSettings([on](document::ExportOutputSettings& s) { s.bakeLook = on; });
    });
    connect(f_->useCachedFrames, &QAbstractButton::toggled, this, [this](const bool on) {
        commitSettings([on](document::ExportOutputSettings& s) { s.useCachedFrames = on; });
    });
    connect(f_->includeAudio, &QAbstractButton::toggled, this, [this](const bool on) {
        commitSettings([on](document::ExportOutputSettings& s) { s.includeAudio = on; });
    });
    connect(f_->sampleRate, &kit::KDropdown::activated, this, [this] {
        commitSettings([this](document::ExportOutputSettings& s) {
            s.audioSampleRate = f_->sampleRate->currentData().toUInt();
        });
    });
    connect(f_->encoder, &kit::KDropdown::activated, this, [this] {
        commitSettings([this](document::ExportOutputSettings& s) {
            s.encoder = static_cast<document::ExportEncoderPreference>(
                f_->encoder->currentData().toInt());
        });
    });
    connect(f_->sequenceStartMode, &kit::KDropdown::activated, this, [this] {
        commitSettings([this](document::ExportOutputSettings& s) {
            if (f_->sequenceStartMode->currentData().toInt() == 0)
                s.naming.startFrame = std::nullopt;
            else if (!s.naming.startFrame.has_value())
                s.naming.startFrame = s.naming.startFrame.value_or(1);
        });
    });
    connect(f_->sequenceStart, &kit::KValueField::editFinished, this, [this] {
        commitSettings([this](document::ExportOutputSettings& s) {
            s.naming.startFrame = static_cast<std::uint64_t>(f_->sequenceStart->integerValue());
        });
    });
    connect(f_->padding, &kit::KValueField::editFinished, this, [this] {
        commitSettings([this](document::ExportOutputSettings& s) {
            s.naming.framePadding = static_cast<std::uint32_t>(f_->padding->integerValue());
        });
    });
    connect(f_->pattern, &QLineEdit::editingFinished, this, [this] {
        commitSettings([this](document::ExportOutputSettings& s) {
            s.naming.pattern = f_->pattern->text().toStdString();
        });
    });
    connect(f_->destination, &QLineEdit::editingFinished, this, [this] {
        const auto text = f_->destination->text();
        if (QDir::isAbsolutePath(text))
            commitChosenPath(text);
        else
            commitDestination([&text](document::ExportDestination& d) {
                d.portability = "project-relative";
                d.path = text.toStdString();
            });
    });
    connect(f_->browse, &kit::KButton::clicked, this, [this] { chooseDestination(); });
    connect(f_->overwrite, &kit::KDropdown::activated, this, [this] {
        commitDestination([this](document::ExportDestination& d) {
            d.overwrite = static_cast<document::ExportOverwritePolicy>(
                f_->overwrite->currentData().toInt());
        });
    });
    connect(f_->replace, &kit::KButton::clicked, this, [this] {
        if (job_ && output_)
            (void)adapter_.confirmReplace(*job_, *output_);
    });
    connect(f_->outputEnabled, &QAbstractButton::toggled, this, [this](const bool on) {
        if (!refreshing_ && job_ && output_)
            (void)adapter_.editOutput(*job_, *output_, outputEdit([&](auto& edit) { edit.enabled = on; }));
    });
    connect(f_->addOutput, &kit::KButton::clicked, this, [this] {
        if (job_)
            (void)adapter_.addOutput(*job_);
    });
    connect(f_->removeOutput, &kit::KButton::clicked, this, [this] {
        if (job_ && output_)
            (void)adapter_.removeOutput(*job_, *output_);
    });
}

void ExportDetailPane::fillPresets() {
    f_->preset->clearItems();
    if (!catalogue_)
        return;
    struct Group {
        host::ExportPresetCategory category;
        QString title;
    };
    // Video first, then VFX / sequences (ADR 0033, "Presets and defaults").
    const Group groups[] = {{host::ExportPresetCategory::Video, tr("Video")},
                            {host::ExportPresetCategory::VfxSequences, tr("VFX / sequences")},
                            {host::ExportPresetCategory::Audio, tr("Audio")},
                            {host::ExportPresetCategory::User, tr("My presets")}};
    for (const auto& group : groups) {
        bool header = false;
        for (const auto& preset : catalogue_->presets) {
            if (preset.category != group.category)
                continue;
            if (!header) {
                // A group title: shown, never chosen.
                const int index = f_->preset->addItem(group.title, QVariant{});
                f_->preset->setItemEnabled(index, false);
                header = true;
            }
            auto text = qs(preset.displayName);
            if (!preset.platformAvailability.available)
                text += tr(" (unavailable)");
            const int index = f_->preset->addItem(text, qs(preset.id));
            f_->preset->setItemToolTip(index, qs(preset.description));
        }
    }
    catalogueRevision_ = catalogue_->revision;
}

void ExportDetailPane::refreshOutput(const host::ExportJobView& job,
                                     const host::ExportOutputView* const output) {
    const bool has = output != nullptr;
    for (auto* widget : {static_cast<QWidget*>(f_->outputPickerRow)})
        widget->setVisible(job.outputs.size() > 1);
    {
        const QSignalBlocker block(f_->outputPicker);
        f_->outputPicker->clearItems();
        for (const auto& candidate : job.outputs)
            f_->outputPicker->addItem(presetText(candidate, catalogue_.get()) + QStringLiteral(" · ") +
                                          destinationText(candidate),
                                      static_cast<qulonglong>(candidate.id.value()));
        if (has)
            f_->outputPicker->setCurrentIndex(
                std::max(0, f_->outputPicker->findData(static_cast<qulonglong>(output->id.value()))));
    }
    const auto& addAvailable = job.commands[host::ExportCommand::AddOutput];
    f_->addOutput->setEnabled(addAvailable.available);
    f_->addOutput->setToolTip(addAvailable.available ? QString{} : qs(addAvailable.reason));
    if (!has) {
        f_->removeOutput->setEnabled(false);
        return;
    }
    const auto& record = output->record;
    const auto& settings = record.settings;
    const auto& edit = output->commands[host::ExportCommand::EditOutput];
    for (auto* widget : f_->outputEditors) {
        widget->setEnabled(edit.available);
        widget->setToolTip(edit.available ? widget->toolTip() : qs(edit.reason));
    }
    const auto& remove = output->commands[host::ExportCommand::RemoveOutput];
    f_->removeOutput->setEnabled(remove.available);
    f_->removeOutput->setToolTip(remove.available ? QString{} : qs(remove.reason));

    if (!catalogue_ || f_->preset->count() == 0 || catalogueRevision_ != catalogue_->revision) {
        const QSignalBlocker block(f_->preset);
        fillPresets();
    }
    {
        const QSignalBlocker block(f_->preset);
        const int index = f_->preset->findData(qs(record.presetId));
        f_->preset->setCurrentIndex(index);
        if (index < 0)
            f_->preset->setMutedValue(true);
        else
            f_->preset->setMutedValue(false);
    }
    // Why the preset cannot run here, with the remedy when there is one.
    const bool blocked = !output->availability.available;
    f_->presetReason->setText(blocked ? qs(output->availability.reason) : QString{});
    f_->remedy->setVisible(blocked && output->availability.remedy != host::ExportRemedy::None);
    switch (output->availability.remedy) {
    case host::ExportRemedy::InstallOpenH264:
        f_->remedy->setText(tr("Install encoder…"));
        break;
    case host::ExportRemedy::LocateOpenH264:
        f_->remedy->setText(tr("Locate encoder…"));
        break;
    case host::ExportRemedy::OpenProjectSettings:
        f_->remedy->setText(tr("Project settings…"));
        break;
    case host::ExportRemedy::ChooseSoftwareEncoder:
        f_->remedy->setText(tr("Use software encoder"));
        break;
    case host::ExportRemedy::None:
        break;
    }
    f_->presetReasonRow->setVisible(blocked);

    const auto format = settings.format;
    const bool image = isImage(format);
    const bool movie = isMovie(format);
    const auto& description = host::exportFormatDescription(format);
    const auto profiles = choicesOf(description.codecProfiles);
    fill(*f_->profile, profiles);
    select(*f_->profile, qs(settings.codecProfile.empty() ? std::string(description.defaultCodecProfile)
                                                          : settings.codecProfile));
    f_->profileRow->setVisible(!profiles.empty() && profiles.size() > 1);
    f_->bitrate->setIntegerValue(settings.videoBitrateKbps.value_or(0));
    f_->bitrateRow->setVisible(isH26x(format));
    fill(*f_->compression, choicesOf(description.compressions));
    select(*f_->compression, qs(settings.compression.empty() ? std::string(description.defaultCompression)
                                                             : settings.compression));
    f_->compressionRow->setVisible(!description.compressions.empty());
    f_->colourSpace->setText(qs(settings.outputColorSpaceId));
    f_->colourSpaceRow->setVisible(format == ExportFormat::OpenExr || format == ExportFormat::Dpx);
    const bool displayReferred = format == ExportFormat::Png || format == ExportFormat::Tiff || movie;
    f_->display->setText(qs(settings.displayName));
    f_->view->setText(qs(settings.viewName));
    f_->displayRow->setVisible(displayReferred);
    f_->viewRow->setVisible(displayReferred);
    f_->bakeLook->setChecked(settings.bakeLook);
    f_->bakeLookRow->setVisible(format != ExportFormat::Wav);
    f_->useCachedFrames->setChecked(settings.useCachedFrames);
    f_->useCachedFramesRow->setVisible(format != ExportFormat::Wav);
    f_->includeAudio->setChecked(settings.includeAudio);
    f_->includeAudioRow->setVisible(movie);
    select(*f_->sampleRate, QString::number(settings.audioSampleRate));
    f_->sampleRateRow->setVisible(movie || format == ExportFormat::Wav);
    f_->encoder->setCurrentIndex(
        std::max(0, f_->encoder->findData(static_cast<int>(settings.encoder))));
    f_->encoderRow->setVisible(isH26x(format));
    f_->sequenceStart->setIntegerValue(
        static_cast<std::int64_t>(settings.naming.startFrame.value_or(0)));
    f_->sequenceStartMode->setCurrentIndex(settings.naming.startFrame.has_value() ? 1 : 0);
    f_->sequenceStartNumberRow->setVisible(image && settings.naming.startFrame.has_value());
    f_->padding->setIntegerValue(settings.naming.framePadding);
    f_->pattern->setText(qs(settings.naming.pattern));
    f_->sequenceStartRow->setVisible(image);
    f_->paddingRow->setVisible(image);
    f_->patternRow->setVisible(image);

    f_->destination->setText(displayPath(record.destination));
    f_->destination->setToolTip(destinationToolTip(*output));
    f_->overwrite->setCurrentIndex(
        std::max(0, f_->overwrite->findData(static_cast<int>(record.destination.overwrite))));
    // Preflight: Destination and Naming problems sit under the destination, quiet while the
    // worker has not sampled it yet.
    QStringList destinationReasons;
    bool pending = false;
    bool exists = false;
    for (const auto& issue : orderedIssues(output->preflight)) {
        if (issue.code == host::ExportPreflightCode::DestinationCheckPending) {
            pending = true;
        } else if (issue.code == host::ExportPreflightCode::DestinationExists &&
                   issue.severity == host::ExportPreflightSeverity::NeedsConfirmation) {
            exists = true;
        } else if (issue.target == host::ExportPreflightTarget::Destination ||
                   issue.target == host::ExportPreflightTarget::Naming) {
            destinationReasons.push_back(qs(issue.reason));
        }
    }
    if (pending)
        destinationReasons.push_back(tr("Checking destination…"));
    f_->destinationReason->setText(destinationReasons.join(QLatin1Char(' ')));
    f_->destinationReason->setTone(pending && destinationReasons.size() == 1 ? kit::Color::Muted
                                                                             : kit::Color::Warn);
    f_->destinationReasonRow->setVisible(!destinationReasons.isEmpty());
    f_->destination->setError(pending || destinationReasons.isEmpty() ? QString{}
                                                                      : destinationReasons.front());
    const auto& confirm = output->commands[host::ExportCommand::ConfirmReplace];
    f_->replaceRow->setVisible(exists && !output->replaceConfirmed);
    f_->replace->setEnabled(confirm.available);
    f_->replace->setToolTip(confirm.available ? QString{} : qs(confirm.reason));
    // Only what the host could estimate: an absent size shows nothing, never a guess.
    QStringList facts;
    if (output->estimatedBytes.has_value())
        facts.push_back(tr("About %1").arg(bytesText(*output->estimatedBytes)));
    if (output->freeBytesAtDestination.has_value())
        facts.push_back(tr("%1 free").arg(bytesText(*output->freeBytesAtDestination)));
    f_->estimate->setText(facts.join(QStringLiteral(" · ")));
    f_->estimateRow->setVisible(!facts.isEmpty());
    f_->outputEnabled->setChecked(record.enabled);
    f_->importIntoAssets->setChecked(record.importIntoAssets);
    f_->importIntoAssets->setEnabled(edit.available);
}

void ExportDetailPane::commitSettings(
    const std::function<void(document::ExportOutputSettings&)>& change) {
    const auto* output = currentOutput();
    if (refreshing_ || !job_ || output == nullptr)
        return;
    auto settings = output->record.settings;
    change(settings);
    if (settings == output->record.settings)
        return;
    const auto result = adapter_.editOutput(*job_, output->id, outputEdit([&](auto& edit) { edit.settings = settings; }));
    f_->refusal->setVisible(!result.accepted && !result.reason.isEmpty());
    f_->refusal->setText(result.reason);
}

void ExportDetailPane::commitDestination(
    const std::function<void(document::ExportDestination&)>& change) {
    const auto* output = currentOutput();
    if (refreshing_ || !job_ || output == nullptr)
        return;
    auto destination = output->record.destination;
    change(destination);
    if (destination == output->record.destination)
        return;
    const auto result = adapter_.editOutput(*job_, output->id, outputEdit([&](auto& edit) { edit.destination = destination; }));
    f_->refusal->setVisible(!result.accepted && !result.reason.isEmpty());
    f_->refusal->setText(result.reason);
}

void ExportDetailPane::commitChosenPath(const QString& path) {
    const auto* output = currentOutput();
    if (refreshing_ || !job_ || output == nullptr)
        return;
    const auto result = adapter_.editOutput(
        *job_, output->id, outputEdit([&](auto& edit) {
            edit.chosenPath = std::pair{std::filesystem::path(path.toStdString()),
                                        output->record.destination.overwrite};
        }));
    f_->refusal->setVisible(!result.accepted && !result.reason.isEmpty());
    f_->refusal->setText(result.reason);
}

void ExportDetailPane::chooseDestination() {
    const auto* output = currentOutput();
    if (output == nullptr)
        return;
    QString suffix;
    if (catalogue_)
        for (const auto& preset : catalogue_->presets)
            if (preset.id == output->record.presetId)
                suffix = qs(preset.capabilities.extension);
    kit::KFileRequest request;
    request.mode = kit::KFileRequest::Mode::Save;
    request.title = tr("Export Destination");
    request.kind = QStringLiteral("export");
    request.defaultSuffix = suffix;
    request.startPath = displayPath(output->record.destination);
    if (const auto chosen = kit::chooseFile(*this, request)) {
        commitChosenPath(*chosen);
    }
}

void ExportDetailPane::applyPreset() {
    const auto* output = currentOutput();
    if (refreshing_ || !job_ || output == nullptr)
        return;
    const auto id = f_->preset->currentData().toString();
    if (id.isEmpty() || id.toStdString() == output->record.presetId)
        return;
    const auto result = adapter_.applyPreset(*job_, output->id, id.toStdString());
    f_->refusal->setVisible(!result.accepted && !result.reason.isEmpty());
    f_->refusal->setText(result.reason);
}

} // namespace bloom::ui::exportpanel
