#pragma once

#include <QWidget>

class QLabel;
class QLineEdit;

namespace bloom::ui {

class CompositionSession;

class KeyframeDiamond;

namespace kit {
class KColorChip;
class KValueField;
} // namespace kit

class PropertiesEditor final : public QWidget {
    Q_OBJECT

  public:
    explicit PropertiesEditor(CompositionSession& session, QWidget* parent = nullptr);

  private:
    void rebuild();
    void configurePosition();
    void configureAnchor();
    void configureScale();
    void configureRotation();
    void configureOpacity();
    void configureSolidColor();
    // Task S3: the Text Source group (content, size, color). Shown exactly when the selection
    // resolves a bloom.text-source, the same isKnownSource + schema-key test configureSolidColor()
    // applies to a solid, so the two groups can never both claim a selection.
    void configureTextSource();
    // Issue #120, decision 3: composition/document properties shown in place of an empty panel
    // when nothing is selected. Toggles documentSection_/selectionSection_ visibility and fills
    // documentSection_'s rows from composition()'s own read-only format/duration -- never a new
    // CompositionSession API.
    void configureDocumentProperties();

    CompositionSession& session_;

    // The selection-driven groups (Transform/Appearance/source-specific), shown together and
    // hidden as one unit whenever configureDocumentProperties() shows documentSection_ instead
    // (issue #120, decision 3). Task P1 (owner review 2026-09-12) removed the selection title row
    // and its "Nothing selected" placeholder text entirely -- section headers are the only
    // grouping left, so there is no selectionLabel_ member any more.
    QWidget* selectionSection_ = nullptr;
    kit::KValueField* positionX_ = nullptr;
    kit::KValueField* positionY_ = nullptr;
    // Task S5, item 0: every indicator below is now a clickable KeyframeDiamond rather than the
    // QLabel that only reported a source. The member names are unchanged -- the row they live in
    // and the objectName the tests read are the same -- so only the control kind moved.
    KeyframeDiamond* positionKeyframe_ = nullptr;
    kit::KValueField* anchorX_ = nullptr;
    kit::KValueField* anchorY_ = nullptr;
    KeyframeDiamond* anchorKeyframe_ = nullptr;
    kit::KValueField* scaleX_ = nullptr;
    kit::KValueField* scaleY_ = nullptr;
    KeyframeDiamond* scaleKeyframe_ = nullptr;
    kit::KValueField* rotation_ = nullptr;
    KeyframeDiamond* rotationKeyframe_ = nullptr;
    kit::KValueField* opacity_ = nullptr;
    KeyframeDiamond* opacityKeyframe_ = nullptr;
    QWidget* solidColorPanel_ = nullptr;
    KeyframeDiamond* solidColorKeyframe_ = nullptr;
    // Task P3: the RGBA cells replacing the former read-only solidColorValue_ label.
    kit::KValueField* solidColorRed_ = nullptr;
    kit::KValueField* solidColorGreen_ = nullptr;
    kit::KValueField* solidColorBlue_ = nullptr;
    kit::KValueField* solidColorAlpha_ = nullptr;
    QLabel* solidAlphaAssociation_ = nullptr;
    QLabel* solidColorEncoding_ = nullptr;

    // Task S3's Text Source group. Content is a QLineEdit rather than a kit control because the kit
    // has no string field; it commits on editingFinished/returnPressed, not per keystroke, so
    // typing a word is one undo step instead of one per letter.
    QWidget* textSourcePanel_ = nullptr;
    QLineEdit* textContent_ = nullptr;
    kit::KValueField* textSize_ = nullptr;
    kit::KColorChip* textColor_ = nullptr;
    KeyframeDiamond* textColorKeyframe_ = nullptr;
    // Task S5, item 1: text size is animatable now, so its row gets a diamond like every other
    // animatable row. It had none before because no command could key it.
    KeyframeDiamond* textSizeKeyframe_ = nullptr;
    QLabel* textFontName_ = nullptr;

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
