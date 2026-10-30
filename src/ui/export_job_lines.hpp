#pragma once

// The cells and lines of the Export render list, as After Effects draws them under a job:
//
//   ▸ Render Settings:  ▾ Work area · 0–239
//   ▸ Output Module:    ▾ ProRes 422 HQ      + −   Output To: ▾ Exports/SC020.mov
//
// Each value is a kit Link button that opens its dialog (Render Settings, Output Module) or the
// file chooser (Output To); each ▾ is a short menu of quick picks. A line is one list row; on a
// narrow panel the Output Module line folds Output To onto a second row under its own label. The
// widgets hold no queue state: they show what the editor gives them and say what was asked for.

#include "export_panel_text.hpp"

#include <bloom/host/export_queue.hpp>
#include <bloom/ui/kit/tokens.hpp>

#include <QString>
#include <QStringList>
#include <QWidget>

#include <utility>
#include <vector>

class QHBoxLayout;
class QMenu;
class QVBoxLayout;

namespace bloom::ui::kit {
class KButton;
class KCheckBox;
class KIconButton;
class KLabel;
class KProgressBar;
} // namespace bloom::ui::kit

namespace bloom::ui::exportpanel {

class RenderList;

// The folders the artist saved exports to, newest first (a per-user setting, never project data),
// and recording one.
[[nodiscard]] QStringList recentExportFolders();
void rememberExportFolder(const QString& folder);

// The Render cell: the "Include in Start All" check box, centred.
class CheckCell final : public QWidget {
    Q_OBJECT
  public:
    explicit CheckCell(QWidget* parent);
    [[nodiscard]] kit::KCheckBox* box() const noexcept { return box_; }

  private:
    kit::KCheckBox* box_ = nullptr;
};

// The Status cell: the status words in their tone and, while the job runs, a thin bar after them.
class StatusCell final : public QWidget {
    Q_OBJECT
  public:
    explicit StatusCell(QWidget* parent);
    void show(const StatusText& status, const host::ExportJobView& job);
    [[nodiscard]] QString text() const { return text_; }
    [[nodiscard]] kit::KProgressBar* bar() const noexcept { return bar_; }
    [[nodiscard]] bool barShown() const;

  private:
    kit::KLabel* label_ = nullptr;
    kit::KProgressBar* bar_ = nullptr;
    QString text_;
};

// The row's ⋯ button, at the row's right end.
class ActionsCell final : public QWidget {
    Q_OBJECT
  public:
    explicit ActionsCell(QWidget* parent);
    [[nodiscard]] kit::KIconButton* button() const noexcept { return button_; }

  private:
    kit::KIconButton* button_ = nullptr;
};

// "Render Settings:  ▾ Work area · 0–239".
class RenderSettingsLine final : public QWidget {
    Q_OBJECT
  public:
    RenderSettingsLine(const RenderList& list, QWidget* parent);
    // `editable` is the job's EditJob availability; `reason` its words when it is not.
    void show(const QString& value, bool editable, const QString& reason, bool currentFrame,
              const QString& currentFrameReason);
    [[nodiscard]] kit::KButton* link() const noexcept { return link_; }
    [[nodiscard]] kit::KIconButton* picks() const noexcept { return picks_; }
    // The quick picks' menu, as the ▾ opens it (a test seam).
    [[nodiscard]] QMenu* buildPicks();

  Q_SIGNALS:
    void openRequested();
    // A quick pick: an int(document::ExportRangeKind) (Frame means the current frame).
    void rangePicked(int kind);

  private:
    kit::KLabel* label_ = nullptr;
    kit::KIconButton* picks_ = nullptr;
    kit::KButton* link_ = nullptr;
    bool currentFrame_ = false;
    QString currentFrameReason_;
};

// What an Output Module line shows.
struct OutputLineState final {
    QString module;
    QString moduleTip;
    QString destination;
    QString destinationTip;
    bool editable = true;
    QString editReason;
    bool canAdd = true;
    QString addReason;
    bool canRemove = true;
    QString removeReason;
    // (preset id, name) per format, video first; an empty id is a group title.
    std::vector<std::pair<QString, QString>> presets;
    // Folders the artist saved to before, newest first.
    QStringList recentFolders;
};

// "Output Module:  ▾ ProRes 422 HQ   + −   Output To: ▾ Exports/SC020.mov".
class OutputModuleLine final : public QWidget {
    Q_OBJECT
  public:
    OutputModuleLine(const RenderList& list, QWidget* parent);
    void show(const OutputLineState& state);
    // Output To on its own second row (a narrow panel), and the width one row needs.
    void setFolded(bool folded);
    [[nodiscard]] bool folded() const noexcept { return folded_; }
    [[nodiscard]] int oneRowWidth() const;
    void applyModuleWidth();

    [[nodiscard]] kit::KButton* moduleLink() const noexcept { return module_; }
    [[nodiscard]] kit::KButton* destinationLink() const noexcept { return destination_; }
    [[nodiscard]] kit::KIconButton* presetsButton() const noexcept { return presets_; }
    [[nodiscard]] kit::KIconButton* destinationButton() const noexcept { return destinationMenu_; }
    [[nodiscard]] kit::KIconButton* addButton() const noexcept { return add_; }
    [[nodiscard]] kit::KIconButton* removeButton() const noexcept { return remove_; }
    // The ▾ menus, as they open (test seams).
    [[nodiscard]] QMenu* buildPresets();
    [[nodiscard]] QMenu* buildDestinations();

  Q_SIGNALS:
    void openRequested();
    void presetPicked(const QString& presetId);
    void addRequested();
    void removeRequested();
    void chooseRequested();
    void defaultNameRequested();
    void folderPicked(const QString& folder);

  private:
    const RenderList& list_;
    QVBoxLayout* rows_ = nullptr;
    QHBoxLayout* first_ = nullptr;
    QHBoxLayout* second_ = nullptr;
    kit::KLabel* label_ = nullptr;
    kit::KIconButton* presets_ = nullptr;
    kit::KButton* module_ = nullptr;
    kit::KIconButton* add_ = nullptr;
    kit::KIconButton* remove_ = nullptr;
    kit::KLabel* toLabel_ = nullptr;
    kit::KIconButton* destinationMenu_ = nullptr;
    kit::KButton* destination_ = nullptr;
    QWidget* toGroup_ = nullptr;
    OutputLineState state_;
    bool folded_ = false;
};

// A line's disclosure: one Muted line of what the settings come to (the frames, the files, the
// size), starting under the line's values.
class SummaryLine final : public QWidget {
    Q_OBJECT
  public:
    SummaryLine(const RenderList& list, QWidget* parent);
    void show(const QString& text, kit::Color tone = kit::Color::Muted);
    [[nodiscard]] QString text() const { return text_; }

  private:
    kit::KLabel* label_ = nullptr;
    QString text_;
};

} // namespace bloom::ui::exportpanel
