#pragma once

// The export queue's project-truth edits as undoable commands (ADR 0033, section 12): each call is
// one transaction, so one undo entry however many members it changes. The desktop ExportQueuePort
// forwards its edit members here; the panel never builds a transaction itself.

#include <bloom/document/export_queue.hpp>
#include <bloom/host/export_presets.hpp>
#include <bloom/ui/export_queue_adapter.hpp>

#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <utility>

namespace bloom::ui {

class CompositionSession;

// What a new output starts from: the catalogue's default preset and the settings it stands for.
struct ExportPresetChoice final {
    std::string presetId;
    document::ExportOutputSettings settings;
};

class ExportQueueEdits final {
  public:
    // The catalogue's default preset with its default destination (host::defaultExportJob builds
    // the new record from it).
    using DefaultPreset = std::function<host::ExportDefaultPreset()>;
    using PresetById = std::function<std::optional<ExportPresetChoice>(const std::string&)>;

    ExportQueueEdits(CompositionSession& session, DefaultPreset defaultPreset, PresetById preset);

    // The saved project's folder (absent while unsaved), for project-relative destinations.
    using ProjectFolder = std::function<std::optional<std::filesystem::path>()>;
    void setProjectFolder(ProjectFolder folder) { projectFolder_ = std::move(folder); }

    ExportEditResult addJob(document::CompositionId composition);
    ExportEditResult duplicateJob(document::ExportJobId job);
    ExportEditResult removeJob(document::ExportJobId job);
    ExportEditResult removeFinished(const std::vector<document::ExportJobId>& finished);
    ExportEditResult moveJob(document::ExportJobId job, std::size_t index);
    ExportEditResult editJob(document::ExportJobId job, const ExportJobEdit& edit);
    ExportEditResult addOutput(document::ExportJobId job);
    ExportEditResult applyPreset(document::ExportOutputId output, const std::string& presetId);
    ExportEditResult editOutput(document::ExportOutputId output, const ExportOutputEdit& edit);
    ExportEditResult removeOutput(document::ExportOutputId output);

  private:
    CompositionSession& session_;
    DefaultPreset defaultPreset_;
    PresetById preset_;
    ProjectFolder projectFolder_;
};

} // namespace bloom::ui
