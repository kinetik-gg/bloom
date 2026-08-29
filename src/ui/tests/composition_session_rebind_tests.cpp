#include <bloom/ui/composition_session.hpp>

#include <bloom/commands/command_stack.hpp>
#include <bloom/core/color.hpp>
#include <bloom/core/rational_time.hpp>
#include <bloom/document/document.hpp>
#include <bloom/document/new_project.hpp>
#include <bloom/document/project.hpp>

#include <QApplication>
#include <QObject>

#include <iostream>
#include <optional>
#include <source_location>
#include <string>
#include <utility>
#include <variant>

// CompositionSession::rebind() unit test (task U1, issue #72, frozen design decision 2): bind ->
// select things -> rebind to a second document -> selection cleared, time zero, every changed
// signal emitted exactly once, and the OLD document is left completely untouched.

namespace {

using bloom::core::Color4d;
using bloom::core::RationalTime;
using bloom::ui::CompositionSession;

class Expectations final {
  public:
    void expect(const bool condition, const std::string_view message,
                const std::source_location location = std::source_location::current()) {
        if (condition) {
            return;
        }
        ++failures_;
        std::cerr << location.file_name() << ':' << location.line() << ": " << message << '\n';
    }

    [[nodiscard]] int failures() const noexcept { return failures_; }

  private:
    int failures_ = 0;
};

void testRebindClearsSelectionResetsTimeAndEmitsEverySignalOnce(Expectations& expectations) {
    auto firstNewProject = bloom::document::makeNewProject("First Project", "Main Composition",
                                                           RationalTime::fromInteger(10));
    const auto firstCompositionId = firstNewProject.initialCompositionId;
    bloom::document::Document firstDocument(std::move(firstNewProject.project));
    bloom::commands::CommandStack firstCommandStack(firstDocument);

    auto secondNewProject = bloom::document::makeNewProject("Second Project", "Main Composition",
                                                            RationalTime::fromInteger(10));
    const auto secondCompositionId = secondNewProject.initialCompositionId;
    bloom::document::Document secondDocument(std::move(secondNewProject.project));
    bloom::commands::CommandStack secondCommandStack(secondDocument);

    CompositionSession session(firstDocument, firstCommandStack, firstCompositionId);

    expectations.expect(session.addSolidLayer("Layer One", Color4d{1.0, 0.0, 0.0, 1.0}),
                        "rebind fixture: adding a solid layer to the first document succeeds");
    expectations.expect(
        std::holds_alternative<bloom::document::LayerId>(session.selection().primary),
        "rebind fixture: adding the layer selects it");

    const auto nonzeroTime = RationalTime::create(3, 2);
    expectations.expect(nonzeroTime.has_value() && session.setCurrentTime(*nonzeroTime),
                        "rebind fixture: the session starts at a nonzero time");

    const auto firstDocumentRevisionBeforeRebind = firstDocument.snapshot().revision();

    int snapshotChangedCount = 0;
    int compositionChangedCount = 0;
    int currentTimeChangedCount = 0;
    int selectionChangedCount = 0;
    int historyChangedCount = 0;
    QObject::connect(&session, &CompositionSession::snapshotChanged,
                     [&] { ++snapshotChangedCount; });
    QObject::connect(&session, &CompositionSession::compositionChanged,
                     [&] { ++compositionChangedCount; });
    QObject::connect(&session, &CompositionSession::currentTimeChanged,
                     [&] { ++currentTimeChangedCount; });
    QObject::connect(&session, &CompositionSession::selectionChanged,
                     [&] { ++selectionChangedCount; });
    QObject::connect(&session, &CompositionSession::historyChanged, [&] { ++historyChangedCount; });

    session.rebind(secondDocument, secondCommandStack, secondCompositionId);

    expectations.expect(snapshotChangedCount == 1 && compositionChangedCount == 1 &&
                            currentTimeChangedCount == 1 && selectionChangedCount == 1 &&
                            historyChangedCount == 1,
                        "rebind: every existing changed signal fires exactly once");

    expectations.expect(session.currentTime() == RationalTime::fromInteger(0),
                        "rebind: current time resets to zero");
    expectations.expect(std::holds_alternative<std::monostate>(session.selection().primary),
                        "rebind: selection is cleared");
    expectations.expect(session.compositionId() == secondCompositionId,
                        "rebind: the session now targets the given composition id");
    expectations.expect(session.snapshot().project().name() == "Second Project",
                        "rebind: the session now projects the second document");

    // Post-rebind activity targets the SECOND document only.
    const auto secondNonzeroTime = RationalTime::create(1, 2);
    expectations.expect(secondNonzeroTime.has_value() && session.setCurrentTime(*secondNonzeroTime),
                        "rebind: the rebound session accepts further edits against the new "
                        "document");

    expectations.expect(firstDocument.snapshot().revision() == firstDocumentRevisionBeforeRebind,
                        "rebind: the OLD document's revision is untouched by post-rebind activity");
    expectations.expect(firstDocument.snapshot().project().name() == "First Project",
                        "rebind: the OLD document's content is untouched by post-rebind activity");
}

} // namespace

int main(int argc, char** argv) {
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication application(argc, argv);
    Expectations expectations;
    testRebindClearsSelectionResetsTimeAndEmitsEverySignalOnce(expectations);
    return expectations.failures() == 0 ? 0 : 1;
}
