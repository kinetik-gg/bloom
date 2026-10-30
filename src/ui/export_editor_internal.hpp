#pragma once

// The Export panel's two composite widgets, internal to bloom_ui: the current-render strip and the
// detail pane. ExportEditor (export_editor.hpp) assembles them with the job list. Neither owns
// queue state: they are filled from an ExportQueueSnapshot and issue commands through the
// application's ExportQueueAdapter.

#include <bloom/document/ids.hpp>
#include <bloom/host/export_queue.hpp>

#include <QHash>
#include <QString>
#include <QWidget>

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>

class QVBoxLayout;

namespace bloom::ui {
class CompositionSession;
class ExportQueueAdapter;
namespace kit {
class KButton;
class KCheckBox;
class KColorChip;
class KDropdown;
class KLabel;
class KLineEdit;
class KPathField;
class KProgressBar;
class KPropertyRow;
class KSection;
class KSwitch;
class KTextArea;
class KValueField;
} // namespace kit

namespace exportpanel {

// Shown while something runs: the job name, a progress bar, "Frame 120 of 240", elapsed and, when
// the host has one, the time left. A job waiting for the Viewer is a calm neutral line with no
// estimate. Hidden when nothing runs.
class ExportRunStrip final : public QWidget {
    Q_OBJECT

  public:
    explicit ExportRunStrip(QWidget* parent);
    // Shows `job` (the active job) or hides the strip for nullptr.
    void setJob(const host::ExportJobView* job);
    [[nodiscard]] QString nameText() const;
    [[nodiscard]] QString stateText() const;
    [[nodiscard]] QString frameText() const;
    [[nodiscard]] QString timeText() const;

  private:
    kit::KLabel* name_ = nullptr;
    kit::KLabel* state_ = nullptr;
    kit::KProgressBar* bar_ = nullptr;
    kit::KLabel* frames_ = nullptr;
    kit::KLabel* time_ = nullptr;
};

// The selected job and output as editable sections: Render, Outputs, Post-render and Log and
// verification. Rebuilt from the snapshot on every change with signals blocked, so a refresh never
// writes back; only the artist's gestures issue commands, one undo entry each.
class ExportDetailPane final : public QWidget {
    Q_OBJECT

  public:
    ExportDetailPane(CompositionSession& session, ExportQueueAdapter& adapter, QWidget* parent);
    ~ExportDetailPane() override;

    // `output` absent selects the job's first output for the Outputs section.
    void setSelection(std::optional<document::ExportJobId> job,
                      std::optional<document::ExportOutputId> output);
    // The snapshot moved: refresh the shown values, then the log.
    void refresh();
    [[nodiscard]] std::optional<document::ExportJobId> job() const noexcept { return job_; }
    [[nodiscard]] std::optional<document::ExportOutputId> output() const noexcept {
        return output_;
    }

    // Test seams: the text a control shows or the reason it gives.
    [[nodiscard]] QString logText() const;

  private:
    struct Fields;
    void build();
    void buildRender(QVBoxLayout& layout);
    void buildOutputs(QVBoxLayout& layout);
    void buildPostRender(QVBoxLayout& layout);
    void buildResults(QVBoxLayout& layout);
    kit::KPropertyRow* addRow(QVBoxLayout& layout, const QString& label,
                                            std::initializer_list<QWidget*> controls,
                                            const QString& objectName = {});
    [[nodiscard]] const host::ExportJobView* currentJob() const;
    [[nodiscard]] const host::ExportOutputView* currentOutput() const;
    void refreshJobActions(const host::ExportJobView& job);
    void refreshRender(const host::ExportJobView& job);
    void refreshOutput(const host::ExportJobView& job, const host::ExportOutputView* output);
    void refreshResults(const host::ExportJobView& job, const host::ExportOutputView* output);
    void refreshLog(const host::ExportJobView& job);
    void commitSettings(const std::function<void(document::ExportOutputSettings&)>& change);
    void commitDestination(const std::function<void(document::ExportDestination&)>& change);
    void commitRange();
    void chooseDestination();
    void commitChosenPath(const QString& path);
    void applyPreset();
    void fillPresets();

    CompositionSession& session_;
    ExportQueueAdapter& adapter_;
    std::shared_ptr<const host::ExportQueueSnapshot> snapshot_;
    std::shared_ptr<const host::ExportPresetCatalogueSnapshot> catalogue_;
    std::optional<document::ExportJobId> job_;
    std::optional<document::ExportOutputId> output_;
    // True while the panel writes values into controls; their signals must not reach commands.
    bool refreshing_ = false;
    std::uint64_t catalogueRevision_ = 0;
    std::optional<document::ExportJobId> loggedJob_;
    std::uint64_t logSequence_ = 0;
    std::uint64_t loggedRun_ = 0;
    std::unique_ptr<Fields> f_;
};

} // namespace exportpanel
} // namespace bloom::ui
