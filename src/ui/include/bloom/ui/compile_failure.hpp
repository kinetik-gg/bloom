#pragma once

// A composition that cannot compile, in words the artist can read (docs/ux/ui-grammar.md, "Compile
// failure details"). The status bar shows the one-sentence headline; a popup opens the plain
// detail and names the layer or node and the parameter at fault, with a Select button and a
// Copy details button.
//
// CompileFailure is the ONE adapter struct between the engine's compile diagnostic and the UI.
// Every consumer (status bar, popup, MCP Activity line) reads it and nothing else. Today it is
// filled from the preview state's first compile TaskDiagnostic: its plain detail and its subject
// ids, which the preview stages take from the engine's compile result (compileFailureSummary).
// Nothing is parsed out of text.

#include <QString>
#include <QWidget>

#include <cstdint>
#include <optional>

namespace bloom::ui {

class CompositionSession;
struct CompositionPreviewState;

struct CompileFailureSubject final {
    std::optional<std::uint64_t> composition;
    std::optional<std::uint64_t> node;
    std::optional<std::uint64_t> parameter;
    [[nodiscard]] bool empty() const noexcept { return !node && !parameter; }
};

struct CompileFailure final {
    // One plain sentence, no codes.
    QString headline;
    // Plain explanation, no ids.
    QString detail;
    CompileFailureSubject subject;
    // Everything, codes and ids included. Only "Copy details" puts this on the clipboard.
    QString raw;
};

// Nullopt unless `preview` is a failed preview whose diagnostics are compile
// diagnostics.
[[nodiscard]] std::optional<CompileFailure> compileFailureFromPreview(const CompositionPreviewState& preview);

// "Layer 'Title' · Blending Mode", "Node 'Merge'", or empty when the subject no longer resolves.
[[nodiscard]] QString compileFailureSubjectText(const CompileFailure& failure,
                                                const CompositionSession& session);

// Selects the failing layer (or node) through the session's selection commands, the same ones the
// timeline, Properties and Nodes use. False when the subject no longer exists.
bool selectCompileFailureSubject(CompositionSession& session, const CompileFailure& failure);

// The popup body: headline, detail, subject in words, and the Select and Copy details buttons.
// Object names: compileFailurePanel, compileFailureHeadline, compileFailureDetail,
// compileFailureSubject, compileFailureSelect, compileFailureCopy.
class CompileFailurePanel final : public QWidget {
    Q_OBJECT

  public:
    CompileFailurePanel(CompileFailure failure, CompositionSession& session,
                        QWidget* parent = nullptr);

    [[nodiscard]] const CompileFailure& failure() const noexcept { return failure_; }
    // The fixed token width and the height its wrapped text needs at that width.
    [[nodiscard]] QSize preferredSize() const;

  Q_SIGNALS:
    // Select or Copy details was pressed; the popup closes on either.
    void dismissed();

  private:
    CompileFailure failure_;
    CompositionSession& session_;
};

} // namespace bloom::ui
