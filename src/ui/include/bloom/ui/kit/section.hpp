#pragma once

#include <bloom/ui/kit/tokens.hpp>

#include <QString>
#include <QWidget>

class QLabel;
class QVBoxLayout;

namespace bloom::ui::kit {

class KButton;

// The Kinetik collapsible section (task PROPS-1, deliverable 1).
//
// One header row -- [chevron][Title Case title][spring][Reset] -- above one collapsible
// body. Everything a panel wants to group goes into bodyLayout(); the section itself never knows
// what a row is, which is why this lives in the kit rather than in properties_editor.cpp: the
// Properties panel is its first consumer, not its owner.
//
// Title case, not the uppercase `editorSectionTitle` treatment the panel used before: a section
// header here carries a control of its own (Reset, plus the chevron), so it reads as a row of
// chrome rather than as a typographic divider, and uppercase micro-type next to a button reads
// as shouting.
//
// Collapsed state persists only when setPersistenceKey() names a QSettings key. The kit
// deliberately does not spell the `properties/sections/<id>/collapsed` shape itself -- that prefix
// belongs to the panel that owns the ids, and a kit widget that hard-coded "properties/" could not
// be reused by any other surface.
class KSection final : public QWidget {
    Q_OBJECT

  public:
    explicit KSection(const QString& title, QWidget* parent = nullptr);

    void addHeaderAction(QWidget* action);

    [[nodiscard]] QString title() const;
    void setTitle(const QString& title);

    // Rows are added here, never to the section itself.
    [[nodiscard]] QWidget* body() const noexcept;
    [[nodiscard]] QVBoxLayout* bodyLayout() const noexcept;

    [[nodiscard]] bool isCollapsed() const noexcept;
    void setCollapsed(bool collapsed);

    // The full QSettings key holding this section's collapsed flag. Setting it reads the stored
    // value immediately; every later collapse change writes it back.
    void setPersistenceKey(const QString& key);
    [[nodiscard]] QString persistenceKey() const;

    // A section with nothing to reset hides its Reset affordance rather than offering a control
    // that would do nothing.
    void setResetEnabled(bool enabled);
    [[nodiscard]] bool isResetEnabled() const noexcept;

    // Overrides the body's inner padding (default SectionPadding). Properties uses the
    // inter-panel Gutter so its content density matches the workspace around it.
    void setBodyPadding(int padding);

  Q_SIGNALS:
    void collapsedChanged(bool collapsed);
    void resetRequested();

  protected:
    void paintEvent(QPaintEvent* event) override;
    bool eventFilter(QObject* watched, QEvent* event) override;

  private:
    void applyCollapsedState();

    QWidget* header_ = nullptr;
    KButton* chevron_ = nullptr;
    QLabel* title_ = nullptr;
    KButton* reset_ = nullptr;
    QWidget* body_ = nullptr;
    QVBoxLayout* bodyLayout_ = nullptr;
    QString persistenceKey_;
    bool collapsed_ = false;
    bool resetEnabled_ = true;
};

} // namespace bloom::ui::kit
