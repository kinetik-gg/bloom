// What the Export panel's rows, links and menus do: each gesture is one command through the
// adapter, each dialog's OK one undoable edit, and a Start that would replace a file asks first.

#include <bloom/ui/export_editor.hpp>

#include "export_detail_support.hpp"
#include "export_job_details.hpp"
#include "export_job_lines.hpp"
#include "export_output_module_dialog.hpp"
#include "export_panel_text.hpp"
#include "export_render_list.hpp"
#include "export_render_settings_dialog.hpp"

#include "assets_editor_internal.hpp"

#include <bloom/document/project.hpp>
#include <bloom/host/export_presets.hpp>
#include <bloom/ui/composition_session.hpp>
#include <bloom/ui/export_queue_adapter.hpp>
#include <bloom/ui/kit/controls.hpp>
#include <bloom/ui/kit/dialog_prompts.hpp>
#include <bloom/ui/kit/file_chooser.hpp>
#include <bloom/ui/kit/switch_control.hpp>

#include <QAction>
#include <QClipboard>
#include <QDir>
#include <QFileInfo>
#include <QGuiApplication>
#include <QMenu>
#include <QPointer>

#include <algorithm>
#include <utility>

namespace bloom::ui {

using exportpanel::qs;

namespace {

[[nodiscard]] const host::ExportJobView* findView(const host::ExportQueueSnapshot* snapshot,
                                                  const document::ExportJobId job) {
    if (snapshot == nullptr)
        return nullptr;
    for (const auto& view : snapshot->jobs)
        if (view.id == job)
            return &view;
    return nullptr;
}

[[nodiscard]] const host::ExportOutputView* findOutput(const host::ExportJobView* job,
                                                       const document::ExportOutputId output) {
    if (job == nullptr)
        return nullptr;
    for (const auto& view : job->outputs)
        if (view.id == output)
            return &view;
    return nullptr;
}

} // namespace

void ExportEditor::connectJob(QTreeWidgetItem* const item) {
    const auto job = exportpanel::rowJob(item);
    if (auto* check = renderCheck(job))
        connect(check->box(), &QAbstractButton::toggled, this, [this, job](const bool on) {
            if (!syncing_)
                report(adapter_->editJob(job, jobEdit([on](auto& edit) { edit.enabled = on; })));
        });
    if (auto* actions = qobject_cast<exportpanel::ActionsCell*>(list_->itemWidget(item, exportpanel::ActionsColumn))) {
        auto* button = actions->button();
        connect(button, &QAbstractButton::clicked, this, [this, job, button] {
            showJobMenu(job, button->mapToGlobal(QPoint(0, button->height())));
        });
    }
    if (auto* line = renderSettingsLine(job)) {
        connect(line, &exportpanel::RenderSettingsLine::openRequested, this,
                [this, job] { (void)openRenderSettings(job); });
        connect(line, &exportpanel::RenderSettingsLine::rangePicked, this,
                [this, job](const int kind) { quickRange(job, kind); });
    }
    for (int child = 1; child < item->childCount(); ++child) {
        const auto output = exportpanel::rowOutput(item->child(child));
        auto* line = outputLine(job, output);
        if (line == nullptr)
            continue;
        using Line = exportpanel::OutputModuleLine;
        connect(line, &Line::openRequested, this, [this, job, output] { (void)openOutputModule(job, output); });
        connect(line, &Line::presetPicked, this, [this, job, output](const QString& preset) {
            report(adapter_->applyPreset(job, output, preset.toStdString()));
        });
        connect(line, &Line::addRequested, this, [this, job] {
            const auto result = adapter_->addOutput(job);
            report(result);
            if (result.accepted)
                setJobExpanded(job, true);
        });
        connect(line, &Line::removeRequested, this,
                [this, job, output] { report(adapter_->removeOutput(job, output)); });
        connect(line, &Line::chooseRequested, this, [this, job, output] { chooseDestination(job, output); });
        connect(line, &Line::defaultNameRequested, this, [this, job, output] { useDefaultName(job, output); });
        connect(line, &Line::folderPicked, this,
                [this, job, output](const QString& folder) { useFolder(job, output, folder); });
    }
}

void ExportEditor::report(const ExportEditResult& result) {
    if (result.accepted || result.reason.isEmpty())
        return;
    kit::inform(*this, kit::KPromptTone::Error, tr("Export"), tr("That change wasn't made."), result.reason,
                QStringLiteral("exportRefusal"));
}

std::unique_ptr<QMenu> ExportEditor::buildJobMenu(const document::ExportJobId job) {
    std::unique_ptr<QMenu> menu(kit::makeMenu(this));
    menu->setObjectName(QStringLiteral("exportContextMenu"));
    menu->setToolTipsVisible(true);
    const auto snapshot = adapter_->snapshot();
    const auto* view = findView(snapshot.get(), job);
    if (view == nullptr)
        return menu;
    const auto add = [&](const QString& text, const char* name,
                         const host::ExportCommandAvailability& availability, auto&& slot) {
        auto* action = menu->addAction(text);
        action->setObjectName(QString::fromLatin1(name));
        action->setEnabled(availability.available);
        if (!availability.available)
            action->setToolTip(qs(availability.reason));
        connect(action, &QAction::triggered, this, std::forward<decltype(slot)>(slot));
        return action;
    };
    // The verbs that fit the job's state, then the list's own items.
    using State = host::ExportJobState;
    const auto& commands = view->commands;
    switch (view->state) {
    case State::Ready: {
        auto start = commands[host::ExportCommand::Start];
        // A replace question is asked by Start itself.
        if (start.code == host::ExportUnavailableCode::NeedsReplaceConfirmation)
            start.available = true;
        add(tr("Start"), "exportStartAction", start, [this, job] { startJob(job); });
        break;
    }
    case State::Rendering:
    case State::Pausing:
        add(tr("Pause"), "exportPauseAction", commands[host::ExportCommand::Pause],
            [this, job] { (void)adapter_->pause(job); });
        add(tr("Stop"), "exportStopAction", commands[host::ExportCommand::Stop],
            [this, job] { (void)adapter_->stop(job); });
        break;
    case State::Paused:
        add(tr("Resume"), "exportResumeAction", commands[host::ExportCommand::Resume],
            [this, job] { (void)adapter_->resume(job); });
        add(tr("Stop"), "exportStopAction", commands[host::ExportCommand::Stop],
            [this, job] { (void)adapter_->stop(job); });
        break;
    case State::Waiting:
    case State::Preparing:
    case State::Verifying:
    case State::Stopping:
        add(tr("Stop"), "exportStopAction", commands[host::ExportCommand::Stop],
            [this, job] { (void)adapter_->stop(job); });
        break;
    case State::Done:
    case State::DoneWithWarnings:
    case State::Failed:
    case State::Stopped:
        add(tr("Render again"), "exportRetryAction", commands[host::ExportCommand::Retry],
            [this, job] { (void)adapter_->retry(job); });
        break;
    }
    host::ExportCommandAvailability details;
    details.available = exportpanel::JobDetailsDialog::hasResults(*view);
    details.reason = tr("This job has not run yet.").toStdString();
    add(tr("Details…"), "exportDetailsAction", details, [this, job] { (void)openDetails(job); });
    menu->addSeparator();
    add(tr("Duplicate"), "exportDuplicateAction", commands[host::ExportCommand::DuplicateJob],
        [this, job] { report(adapter_->duplicateJob(job)); });
    add(tr("Remove"), "exportRemoveAction", commands[host::ExportCommand::RemoveJob],
        [this, job] { report(adapter_->removeJob(job)); });
    const auto order = jobOrder();
    const auto at = static_cast<std::size_t>(std::ranges::find(order, job) - order.begin());
    auto up = commands[host::ExportCommand::MoveJob];
    auto down = up;
    if (at == 0) {
        up.available = false;
        up.reason = tr("This job is already first.").toStdString();
    }
    if (at + 1 >= order.size()) {
        down.available = false;
        down.reason = tr("This job is already last.").toStdString();
    }
    add(tr("Move up"), "exportMoveUpAction", up, [this, job, at] { report(adapter_->moveJob(job, at - 1)); });
    add(tr("Move down"), "exportMoveDownAction", down,
        [this, job, at] { report(adapter_->moveJob(job, at + 1)); });
    menu->addSeparator();
    const auto copy = [this, job](const host::ExportScriptFormat format) {
        const auto latest = adapter_->snapshot();
        if (const auto* current = findView(latest.get(), job))
            QGuiApplication::clipboard()->setText(qs(adapter_->jobScript(current->record, format)));
    };
    menu->addAction(tr("Copy as bloom-cli command"), this, [copy] { copy(host::ExportScriptFormat::CliJson); })
        ->setObjectName(QStringLiteral("exportCopyCliAction"));
    menu->addAction(tr("Copy as Python"), this, [copy] { copy(host::ExportScriptFormat::Python); })
        ->setObjectName(QStringLiteral("exportCopyPythonAction"));
    return menu;
}

void ExportEditor::showJobMenu(const document::ExportJobId job, const QPoint globalPosition) {
    if (auto* item = jobItem(job))
        list_->setCurrentItem(item);
    const auto menu = buildJobMenu(job);
    menu->exec(globalPosition);
}

exportpanel::RenderSettingsDialog* ExportEditor::openRenderSettings(const document::ExportJobId job) {
    const auto snapshot = adapter_->snapshot();
    const auto* view = findView(snapshot.get(), job);
    if (view == nullptr)
        return nullptr;
    auto* dialog = new exportpanel::RenderSettingsDialog(session_.snapshot().project(), *view, *this);
    QPointer<ExportEditor> self(this);
    QPointer<exportpanel::RenderSettingsDialog> shown(dialog);
    dialog->open([self, shown, job](const kit::KDialog::Role role) {
        if (self == nullptr || shown == nullptr || role != kit::KDialog::Role::Accept || !shown->changed())
            return;
        const auto range = shown->range();
        self->report(self->adapter_->editJob(job, jobEdit([&range](auto& edit) { edit.range = range; })));
    });
    return dialog;
}

exportpanel::OutputModuleDialog* ExportEditor::openOutputModule(const document::ExportJobId job,
                                                                const document::ExportOutputId output) {
    const auto snapshot = adapter_->snapshot();
    const auto* view = findOutput(findView(snapshot.get(), job), output);
    if (view == nullptr)
        return nullptr;
    QPointer<ExportEditor> self(this);
    auto* dialog = new exportpanel::OutputModuleDialog(*view, adapter_->catalogue(), *colours_, *this);
    QPointer<exportpanel::OutputModuleDialog> shown(dialog);
    dialog->open([self, shown, job, output](const kit::KDialog::Role role) {
        if (self == nullptr || shown == nullptr || role != kit::KDialog::Role::Accept)
            return;
        // Every change of the dialog in one edit: one undo entry.
        if (const auto change = shown->edit())
            self->report(self->adapter_->editOutput(job, output, *change));
    });
    return dialog;
}

exportpanel::JobDetailsDialog* ExportEditor::openDetails(const document::ExportJobId job) {
    auto* dialog = new exportpanel::JobDetailsDialog(*adapter_, job, *this);
    dialog->open([](kit::KDialog::Role) {});
    return dialog;
}

void ExportEditor::quickRange(const document::ExportJobId job, const int kind) {
    const auto snapshot = adapter_->snapshot();
    const auto* view = findView(snapshot.get(), job);
    if (view == nullptr)
        return;
    auto range = view->record.render.range;
    range.kind = static_cast<document::ExportRangeKind>(kind);
    if (range.kind == document::ExportRangeKind::Frame) {
        // The playhead of the open composition, as a frame of it.
        const auto* composition = session_.snapshot().project().findComposition(view->record.composition);
        if (composition == nullptr || view->record.composition != session_.compositionId())
            return;
        range.first = assets::framesAt(session_.currentTime(), composition->format().frameRate());
        range.last = range.first;
    }
    if (range == view->record.render.range)
        return;
    report(adapter_->editJob(job, jobEdit([&range](auto& edit) { edit.range = range; })));
}

void ExportEditor::chooseDestination(const document::ExportJobId job, const document::ExportOutputId output) {
    const auto snapshot = adapter_->snapshot();
    const auto catalogue = adapter_->catalogue();
    const auto* view = findOutput(findView(snapshot.get(), job), output);
    if (view == nullptr)
        return;
    kit::KFileRequest request;
    request.mode = kit::KFileRequest::Mode::Save;
    request.title = tr("Output To");
    request.kind = QStringLiteral("export");
    if (catalogue != nullptr)
        for (const auto& preset : catalogue->presets)
            if (preset.id == view->record.presetId)
                request.defaultSuffix = qs(preset.capabilities.extension);
    // The name the tokens resolve to now is the starting suggestion.
    request.startPath = !view->destination.firstPath.empty()
                            ? QString::fromStdString(view->destination.firstPath.string())
                            : exportpanel::detail::displayPath(view->record.destination);
    const auto chosen = kit::chooseFile(*this, request);
    if (!chosen.has_value())
        return;
    const auto latest = adapter_->snapshot();
    const auto* current = findOutput(findView(latest.get(), job), output);
    const auto policy = current != nullptr ? current->record.destination.overwrite
                                           : document::ExportOverwritePolicy::AskBeforeReplacing;
    exportpanel::rememberExportFolder(QFileInfo(*chosen).absolutePath());
    report(adapter_->editOutput(job, output, outputEdit([&](auto& edit) {
        edit.chosenPath = std::pair{std::filesystem::path(chosen->toStdString()), policy};
    })));
}

void ExportEditor::useDefaultName(const document::ExportJobId job, const document::ExportOutputId output) {
    const auto snapshot = adapter_->snapshot();
    const auto* view = findOutput(findView(snapshot.get(), job), output);
    if (view == nullptr)
        return;
    auto destination = host::defaultExportDestination(view->record.settings.format);
    destination.overwrite = view->record.destination.overwrite;
    if (destination == view->record.destination)
        return;
    report(adapter_->editOutput(job, output, outputEdit([&](auto& edit) { edit.destination = destination; })));
}

void ExportEditor::useFolder(const document::ExportJobId job, const document::ExportOutputId output,
                             const QString& folder) {
    const auto snapshot = adapter_->snapshot();
    const auto* view = findOutput(findView(snapshot.get(), job), output);
    if (view == nullptr)
        return;
    // The default name, in that folder: the name still follows the composition.
    const auto name = QString::fromStdString(
        std::filesystem::path(host::defaultExportDestination(view->record.settings.format).path)
            .filename()
            .string());
    const auto path = QDir(QDir::fromNativeSeparators(folder)).filePath(name);
    exportpanel::rememberExportFolder(folder);
    report(adapter_->editOutput(job, output, outputEdit([&](auto& edit) {
        edit.chosenPath = std::pair{std::filesystem::path(path.toStdString()), view->record.destination.overwrite};
    })));
}

void ExportEditor::confirmReplacements(const std::vector<document::ExportJobId>& jobs,
                                       const std::function<void()>& then) {
    const auto snapshot = adapter_->snapshot();
    std::vector<std::pair<document::ExportJobId, document::ExportOutputId>> asking;
    QStringList names;
    for (const auto job : jobs)
        if (const auto* view = findView(snapshot.get(), job))
            for (const auto& output : view->outputs)
                if (exportpanel::awaitsReplaceConfirmation(output)) {
                    asking.emplace_back(job, output.id);
                    names.push_back(QFileInfo(exportpanel::destinationText(output)).fileName());
                }
    if (asking.empty()) {
        then();
        return;
    }
    kit::KConfirmSpec spec;
    spec.title = tr("Replace");
    spec.objectName = QStringLiteral("exportReplaceConfirm");
    spec.tone = kit::KPromptTone::Warning;
    spec.heading = names.size() == 1 ? tr("Replace %1?").arg(names.front())
                                     : tr("Replace %1 files that are already there?").arg(names.size());
    spec.detail = names.size() == 1 ? tr("A file with this name is already there. Replacing it can't be undone.")
                                    : names.join(QStringLiteral(", "));
    spec.destructiveText = tr("Replace");
    spec.alternateText = tr("Choose another name");
    spec.rejectText = tr("Cancel");
    spec.defaultRole = kit::KDialog::Role::Reject;
    QPointer<ExportEditor> self(this);
    kit::confirm(*this, spec, [self, asking, then](const kit::KDialog::Role role) {
        if (self == nullptr)
            return;
        if (role == kit::KDialog::Role::Destructive) {
            for (const auto& [job, output] : asking)
                (void)self->adapter_->confirmReplace(job, output);
            then();
        } else if (role == kit::KDialog::Role::Alternate) {
            self->chooseDestination(asking.front().first, asking.front().second);
        }
    });
}

void ExportEditor::startAll() {
    const auto snapshot = adapter_->snapshot();
    if (!snapshot)
        return;
    std::vector<document::ExportJobId> jobs;
    for (const auto& job : snapshot->jobs)
        if (!job.transient && job.record.enabled && job.state == host::ExportJobState::Ready)
            jobs.push_back(job.id);
    QPointer<ExportEditor> self(this);
    confirmReplacements(jobs, [self] {
        if (self != nullptr)
            (void)self->adapter_->startAll();
    });
}

void ExportEditor::startJob(const document::ExportJobId job) {
    QPointer<ExportEditor> self(this);
    confirmReplacements({job}, [self, job] {
        if (self != nullptr)
            (void)self->adapter_->start(job);
    });
}

} // namespace bloom::ui
