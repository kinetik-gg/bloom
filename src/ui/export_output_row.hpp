#pragma once

// One output of a job in the Export panel: a collapsible row whose header reads the format, where
// it saves and its state, and whose body holds only what belongs to that output: Format, Save to,
// "If a file is there", Quality (the settings that apply to the chosen format) and "After render".
// The inline "A file is already there" prompt sits at the top of the body.
//
// The row holds no queue state. It is filled from a snapshot view and issues every edit through
// the application's ExportQueueAdapter (one undoable command per gesture); the host's reasons
// stay visible as labels under the control they concern.

#include "export_colour_choices.hpp"

#include <bloom/document/ids.hpp>
#include <bloom/host/export_queue.hpp>

#include <QString>
#include <QWidget>

#include <functional>
#include <memory>
#include <optional>

class QMenu;
class QVBoxLayout;

namespace bloom::ui {
struct ExportEditResult;
class CompositionSession;
class ExportQueueAdapter;
namespace kit {
class KButton;
class KDropdown;
class KIconButton;
class KLabel;
class KPathField;
class KRow;
class KSection;
class KSwitch;
class KValueField;
} // namespace kit

namespace exportpanel {

class OutputRow final : public QWidget {
    Q_OBJECT

  public:
    OutputRow(CompositionSession& session, ExportQueueAdapter& adapter,
              document::ExportJobId job, document::ExportOutputId output, QWidget* parent);
    ~OutputRow() override;

    // Shows `output` of `job`.
    void update(const host::ExportJobView& job, const host::ExportOutputView& output,
                const host::ExportPresetCatalogueSnapshot* catalogue,
                const ExportColourChoices& colours);

    [[nodiscard]] document::ExportOutputId output() const noexcept { return output_; }
    [[nodiscard]] bool expanded() const noexcept { return expanded_; }
    void setExpanded(bool expanded);

    // The "Remove output" and similar items of the row's overflow menu, with their availability.
    [[nodiscard]] std::unique_ptr<QMenu> buildMenu();

    // Test seams.
    [[nodiscard]] QString destinationSummary() const;
    [[nodiscard]] QString statusText() const;
    [[nodiscard]] QString formatText() const;
    [[nodiscard]] QString resolvedText() const;
    [[nodiscard]] QString replacePromptText() const;
    [[nodiscard]] QString reasonsText() const;
    [[nodiscard]] kit::KSection* qualitySection() const noexcept { return quality_; }

  Q_SIGNALS:
    void expandedChanged(bool expanded);
    // The host refused an edit; the text is its reason.
    void refused(const QString& reason);
    void accepted();

  private:
    struct Fields;
    void build();
    void buildFormat(QVBoxLayout& rows);
    void buildDestination(QVBoxLayout& rows);
    void buildQuality(QVBoxLayout& rows);
    void buildAfterRender(QVBoxLayout& rows);
    void fillFormats(const host::ExportPresetCatalogueSnapshot* catalogue);
    void fillColour(const ExportColourChoices& colours, const document::ExportOutputSettings& s);
    [[nodiscard]] const host::ExportJobView* findJob() const;
    [[nodiscard]] const host::ExportOutputView* findOutput() const;
    void report(const ExportEditResult& result);
    void commitSettings(const std::function<void(document::ExportOutputSettings&)>& change);
    void commitDestination(const std::function<void(document::ExportDestination&)>& change);
    void commitChosenPath(const QString& path);
    void chooseDestination();
    void showTokenHelp();
    void applyFormat();

    CompositionSession& session_;
    ExportQueueAdapter& adapter_;
    document::ExportJobId job_;
    document::ExportOutputId output_;
    bool expanded_ = false;
    bool refreshing_ = false;
    std::uint64_t catalogueRevision_ = 0;
    QString suffix_;
    ExportColourChoices colours_;
    kit::KSection* quality_ = nullptr;
    std::unique_ptr<Fields> f_;
};

} // namespace exportpanel
} // namespace bloom::ui
