#pragma once

#include <QByteArray>
#include <QObject>
#include <QString>

#include <optional>

class QWidget;
#include <vector>

namespace bloom::ui {

class WorkspaceHost;

// One named panel arrangement: the WorkspaceHost layout-state JSON (docs/architecture/
// workspace-layout.md), nothing else. Presets never carry project data or an editor's own state.
struct WorkspacePreset final {
    QString name;
    bool builtIn = false;
    QByteArray layout;
};

// The workspace preset list and the active preset (docs/architecture/workspace-layout.md,
// "Workspace presets"). Built-in presets are authored here; the artist's own are stored with the
// application preferences (QSettings, `workspace/presets/*`), never in a project. Choosing a preset
// rearranges the host's panels through WorkspaceHost::applyArrangement(), which keeps every panel
// the arrangement keeps -- and with it the editor's session state.
class WorkspacePresets final : public QObject {
    Q_OBJECT

  public:
    explicit WorkspacePresets(WorkspaceHost& host, QObject* parent = nullptr);

    // Default, Node, Export, Animation.
    [[nodiscard]] const std::vector<WorkspacePreset>& builtInPresets() const noexcept {
        return builtIn_;
    }
    [[nodiscard]] const std::vector<WorkspacePreset>& userPresets() const noexcept { return user_; }
    [[nodiscard]] const WorkspacePreset* find(const QString& name) const;

    // The name of the preset the layout was last set from; empty when none (Custom).
    [[nodiscard]] const QString& activeName() const noexcept { return active_; }
    // True when the active preset's arrangement no longer matches the live layout.
    [[nodiscard]] bool isEdited() const noexcept { return edited_; }
    // What the Workspace field shows: "Default", "Node (edited)" or "Custom".
    [[nodiscard]] QString label() const;
    [[nodiscard]] bool activeIsUser() const;

    // Rearranges the host into the named preset. False when it is unknown or the host refused.
    // `remember` false keeps the choice in memory only (the startup and migration paths, which then
    // re-read the stored one with load()).
    bool apply(const QString& name, bool remember = true);
    // Stores the live layout under `name` as a user preset and makes it active. A name must be
    // non-empty, not a built-in's and not another user preset's (validateName).
    bool saveCurrentAs(const QString& name);
    // Replaces the active user preset's arrangement with the live layout.
    bool updateActive();
    bool removeUser(const QString& name);
    // The layout was set some other way (Reset Workspace): no preset is active any more.
    void clearActive();
    // Why `name` cannot be used for a new preset, or nothing.
    [[nodiscard]] std::optional<QString> validateName(const QString& name) const;
    // Re-reads the user presets and active name from application preferences.
    void load();
    // The prompts the picker and the Window menu share. Both are asynchronous and window-modal.
    void promptSaveAs(QWidget& owner);
    void promptDeleteActive(QWidget& owner);

  signals:
    void changed();

  private:
    void persist() const;
    void refreshEdited();

    WorkspaceHost& host_;
    std::vector<WorkspacePreset> builtIn_;
    std::vector<WorkspacePreset> user_;
    QString active_;
    bool edited_ = false;
    bool applying_ = false;
};

} // namespace bloom::ui
