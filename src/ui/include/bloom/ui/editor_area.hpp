#pragma once

#include <QFrame>
#include <QString>
#include <QStringView>

#include <string>
#include <string_view>

class QComboBox;
class QEvent;
class QObject;
class QToolButton;
class QVBoxLayout;

namespace bloom::ui {

class EditorRegistry;

class EditorArea final : public QFrame {
    Q_OBJECT

  public:
    explicit EditorArea(const EditorRegistry& registry, std::string_view initialEditorId = {},
                        QString areaId = {}, QWidget* parent = nullptr);

    [[nodiscard]] const QString& areaId() const noexcept;
    [[nodiscard]] static bool isValidAreaId(QStringView areaId);
    [[nodiscard]] std::string editorId() const;
    [[nodiscard]] bool setEditorId(std::string_view editorId);

    void setAreaActive(bool active);
    [[nodiscard]] bool isAreaActive() const noexcept;
    void setSplitEnabled(bool enabled);
    void setCloseEnabled(bool enabled);
    void setMaximizedAppearance(bool maximized);

  signals:
    void activationRequested(EditorArea* area);
    void splitRequested(EditorArea* area, Qt::Orientation orientation);
    void closeRequested(EditorArea* area);
    void maximizeRequested(EditorArea* area);

  protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

  private:
    void rebuildEditor(int editorIndex);
    int addUnavailableEditor(std::string_view editorId);
    void watchForActivation(QWidget* widget);

    const EditorRegistry& editorRegistry_;
    QString areaId_;
    QComboBox* editorPicker_ = nullptr;
    QWidget* editorWidget_ = nullptr;
    QVBoxLayout* contentLayout_ = nullptr;
    QToolButton* splitLeftRightButton_ = nullptr;
    QToolButton* splitTopBottomButton_ = nullptr;
    QToolButton* closeButton_ = nullptr;
    QToolButton* maximizeButton_ = nullptr;
    bool active_ = false;
};

} // namespace bloom::ui
