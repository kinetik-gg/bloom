#pragma once

// Complete, not forward-declared: bindCell() below binds a cell's own gesture signals, which needs
// the type rather than just its name.
#include <bloom/ui/kit/value_field.hpp>

#include <QWidget>
#include <bloom/ui/editor_area.hpp>

#include <array>
#include <string_view>
#include <vector>

class QLabel;
class QLineEdit;
class QVBoxLayout;

namespace bloom::ui {

class CompositionSession;
class PropertiesRegistryRow;
class PropertiesAnchorGrid;

class KeyframeDiamond;

namespace kit {
class KButton;
class KColorChip;
class KDropdown;
class KSection;
class KSlider;
class KSwitch;
} // namespace kit

class PropertiesEditor final : public QWidget, public EditorChromeProvider {
    Q_OBJECT

  public:
    [[nodiscard]] EditorChromeSpec& editorChrome() override { return chrome_; }
    explicit PropertiesEditor(CompositionSession& session, QWidget* parent = nullptr);

  protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

  private:
    EditorChromeSpec chrome_;
    void rebuild();
    void configureRegistryRows();
    void configureUpstream();
    void configureDrivenRows();
    // Task DRIVE-1: this panel reads the session's resolved driven values rather than
    // running an evaluator of its own; the connection is made once.
    bool drivenValuesConnected_ = false;
    QWidget* upstreamPanel_ = nullptr;
    QString upstreamSignature_;
    std::vector<PropertiesRegistryRow*> upstreamRows_;
    void filterRows();
    QLineEdit* search_ = nullptr;
    std::vector<PropertiesRegistryRow*> registryRows_;
    QWidget* registryPanel_ = nullptr;
    QString registrySignature_;
    void configureMergeInputs();
    QWidget* mergeInputsPanel_ = nullptr;
    kit::KSection* mergeSection_ = nullptr;
    // Binds one numeric row's commit to the CELL's own gesture boundary rather than to every value
    // it passes through. ADR 0017: a drag does not mutate the document on pointer motion, and one
    // completed gesture is one undo step -- so a scrub publishes nothing until it is released, and
    // an abandoned one publishes nothing at all. `rebuilding_` still guards the projection's own
    // writes, exactly as it did.
    template <typename Commit> void bindCell(kit::KValueField* field, Commit commit) {
        connect(field, &kit::KValueField::valueChanged, this, [this, commit] {
            if (!rebuilding_ && !scrubbing_) {
                commit();
            }
        });
        connect(field, &kit::KValueField::scrubStarted, this, [this] { scrubbing_ = true; });
        connect(field, &kit::KValueField::scrubCancelled, this, [this] { scrubbing_ = false; });
        connect(field, &kit::KValueField::scrubFinished, this, [this, commit] {
            scrubbing_ = false;
            if (!rebuilding_) {
                commit();
            }
        });
    }

    // Task PROPS-1, deliverable 1: the section construction, split out of one constructor that had
    // grown past four hundred lines. Each appends exactly one kit::KSection to `layout`.
    void buildObjectSection(QVBoxLayout* layout);
    void buildTransformSection(QVBoxLayout* layout);
    void buildSolidSection(QVBoxLayout* layout);
    void buildTextSection(QVBoxLayout* layout);
    void buildDocumentSection(QVBoxLayout* layout);
    void bindCommits();

    // Task PROPS-1, deliverable 1: a slider shares its row's commit with the paired value cell.
    // KSlider carries no scrub gesture signals of its own, so the ADR 0017 boundary is the pointer
    // release, caught here: while the handle is dragged the paired cell mirrors the slider and
    // NOTHING is written; the release -- or a keyboard step, which is not a drag at all -- is the
    // single commit.
    void commitOpacityFromControls();
    void commitRotationFromControls();

    // Registers a section with the panel's own Collapse all / Expand all and per-section Reset.
    void adoptSection(kit::KSection* section, std::vector<std::string_view> resetRoles);
    void setAllSectionsCollapsed(bool collapsed);
    void resetRoles(const std::vector<std::string_view>& roles);

    void configurePosition();
    void configureAnchor();
    void configureScale();
    void configureRotation();
    void configureOpacity();
    void configureBlendMode();
    // Task PROPS-1, deliverable 1: the Object section's Visible/Solo/Locked switches, which author
    // the layer boundary's own flags through the same SetLayerEnabled/Solo/Locked commands the
    // timeline's toggle strip already uses -- one write path, two surfaces.
    void configureObjectToggles();
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

    // The selection-driven groups (Object/Transform/source-specific), shown together and
    // hidden as one unit whenever configureDocumentProperties() shows documentSection_ instead
    // (issue #120, decision 3). Task P1 (owner review 2026-09-12) removed the selection title row
    // and its "Nothing selected" placeholder text entirely -- section headers are the only
    // grouping left, so there is no selectionLabel_ member any more.
    QWidget* selectionSection_ = nullptr;
    // Every kit::KSection this panel owns, in the order it shows them. The panel, not the section,
    // answers Collapse all / Expand all: a section knows only itself.
    std::vector<kit::KSection*> sections_;

    kit::KSwitch* layerVisible_ = nullptr;
    kit::KSwitch* layerSolo_ = nullptr;
    kit::KSwitch* layerLocked_ = nullptr;

    kit::KValueField* positionX_ = nullptr;
    kit::KValueField* positionY_ = nullptr;
    kit::KButton* positionLink_ = nullptr;
    // Task S5, item 0: every indicator below is now a clickable KeyframeDiamond rather than the
    // QLabel that only reported a source. The member names are unchanged -- the row they live in
    // and the objectName the tests read are the same -- so only the control kind moved.
    KeyframeDiamond* positionKeyframe_ = nullptr;
    PropertiesAnchorGrid* anchorGrid_ = nullptr;
    kit::KValueField* anchorX_ = nullptr;
    kit::KValueField* anchorY_ = nullptr;
    KeyframeDiamond* anchorKeyframe_ = nullptr;
    kit::KValueField* scaleX_ = nullptr;
    kit::KValueField* scaleY_ = nullptr;
    kit::KButton* scaleLink_ = nullptr;
    KeyframeDiamond* scaleKeyframe_ = nullptr;
    kit::KValueField* rotation_ = nullptr;
    kit::KSlider* rotationSlider_ = nullptr;
    KeyframeDiamond* rotationKeyframe_ = nullptr;
    kit::KValueField* opacity_ = nullptr;
    kit::KSlider* opacitySlider_ = nullptr;
    KeyframeDiamond* opacityKeyframe_ = nullptr;
    // The Object group's blending row. A KDropdown rather than a KValueField because the value is
    // a closed vocabulary, not a number, and it carries no keyframe indicator because the schema
    // declares the blend mode non-animatable -- an indicator column that can never light up would
    // promise a capability that does not exist.
    kit::KDropdown* blendMode_ = nullptr;
    kit::KColorChip* solidColorChip_ = nullptr;
    QWidget* solidColorPanel_ = nullptr;
    KeyframeDiamond* solidColorKeyframe_ = nullptr;
    // Task P3: the RGBA cells replacing the former read-only solidColorValue_ label.
    kit::KValueField* solidColorRed_ = nullptr;
    kit::KValueField* solidColorGreen_ = nullptr;
    kit::KValueField* solidColorBlue_ = nullptr;
    kit::KValueField* solidColorAlpha_ = nullptr;
    // Task S3's Text Source group. Content is a QLineEdit rather than a kit control because the kit
    // has no string field; it commits on editingFinished/returnPressed, not per keystroke, so
    // typing a word is one undo step instead of one per letter.
    QWidget* textSourcePanel_ = nullptr;
    QLineEdit* textContent_ = nullptr;
    kit::KValueField* textSize_ = nullptr;
    std::array<kit::KValueField*, 4> textColorFields_{};
    kit::KColorChip* textColor_ = nullptr;
    KeyframeDiamond* textColorKeyframe_ = nullptr;
    // Task S5, item 1: text size is animatable now, so its row gets a diamond like every other
    // animatable row. It had none before because no command could key it.
    KeyframeDiamond* textSizeKeyframe_ = nullptr;
    kit::KDropdown* textFontName_ = nullptr;

    // The no-selection document/composition view (issue #120, decision 3).
    QWidget* documentSection_ = nullptr;
    kit::KColorChip* documentBackground_ = nullptr;
    QLabel* documentName_ = nullptr;
    QLabel* documentFormat_ = nullptr;
    QLabel* documentFrameRate_ = nullptr;
    QLabel* documentDuration_ = nullptr;
    QLabel* documentPixelAspect_ = nullptr;

    bool rebuilding_ = false;
    // True between a cell's scrubStarted() and its scrubFinished()/scrubCancelled(). One flag for
    // the panel, because the panel has one pointer on it.
    bool scrubbing_ = false;
};

} // namespace bloom::ui
