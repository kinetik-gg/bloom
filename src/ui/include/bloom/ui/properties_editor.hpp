#pragma once

#include <QWidget>

class QLabel;

namespace bloom::ui {

class CompositionSession;

namespace kit {
class KValueField;
} // namespace kit

class PropertiesEditor final : public QWidget {
    Q_OBJECT

  public:
    explicit PropertiesEditor(CompositionSession& session, QWidget* parent = nullptr);

  private:
    void rebuild();
    void configurePosition();
    void configureOpacity();
    void configureSolidColor();
    // Issue #120, decision 3: composition/document properties shown in place of an empty panel
    // when nothing is selected. Toggles documentSection_/selectionSection_ visibility and fills
    // documentSection_'s rows from composition()'s own read-only format/duration -- never a new
    // CompositionSession API.
    void configureDocumentProperties();

    CompositionSession& session_;
    QLabel* selectionLabel_ = nullptr;

    // The selection-driven groups (Transform/Appearance/source-specific), shown together and
    // hidden as one unit whenever configureDocumentProperties() shows documentSection_ instead
    // (issue #120, decision 3).
    QWidget* selectionSection_ = nullptr;
    kit::KValueField* positionX_ = nullptr;
    kit::KValueField* positionY_ = nullptr;
    QLabel* positionKeyframe_ = nullptr;
    kit::KValueField* opacity_ = nullptr;
    QLabel* opacityKeyframe_ = nullptr;
    QWidget* solidColorPanel_ = nullptr;
    QLabel* solidColorKeyframe_ = nullptr;
    QLabel* solidColorValue_ = nullptr;
    QLabel* solidAlphaAssociation_ = nullptr;
    QLabel* solidColorEncoding_ = nullptr;

    // The no-selection document/composition view (issue #120, decision 3).
    QWidget* documentSection_ = nullptr;
    QLabel* documentName_ = nullptr;
    QLabel* documentFormat_ = nullptr;
    QLabel* documentFrameRate_ = nullptr;
    QLabel* documentDuration_ = nullptr;
    QLabel* documentPixelAspect_ = nullptr;

    bool rebuilding_ = false;
};

} // namespace bloom::ui
