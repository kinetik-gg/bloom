#include <bloom/ui/composition_session.hpp>

#include <bloom/commands/command_stack.hpp>
#include <bloom/core/color.hpp>
#include <bloom/core/rational_time.hpp>
#include <bloom/document/document.hpp>
#include <bloom/document/graph.hpp>
#include <bloom/document/new_project.hpp>
#include <bloom/document/project.hpp>

#include <QApplication>
#include <QObject>

#include <cstdlib>
#include <iostream>
#include <optional>
#include <source_location>
#include <string>
#include <utility>
#include <variant>

// CompositionSession::rebind() unit test (task U1, issue #72, frozen design decision 2): bind ->
// select things -> rebind to a second document -> selection cleared, time zero, every changed
// signal emitted exactly once, and the OLD document is left completely untouched.
//
// Also carries the R1/issue #75 constructor-fallback pin: CompositionSession's own constructor
// falls back to a min-scan for the lowest CompositionId (mirroring
// ProjectHost::lowestCompositionId()) when constructed with an absent id, rather than
// compositions().front() (insertion order).

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

// ---------------------------------------------------------------------------------------------
// Issue #75: a hand-built minimal composition (mirrors makeNewProject()'s own layer-stack ->
// composition-output topology so Document's constructor validation passes) at a given
// CompositionId, node/edge ids offset per-composition so project-wide uniqueness holds when two
// of these share one Project.
// ---------------------------------------------------------------------------------------------

[[nodiscard]] bloom::document::Composition makeMinimalComposition(bloom::document::CompositionId id,
                                                                  std::string name,
                                                                  const std::uint64_t nodeIdBase) {
    using bloom::document::EdgeId;
    using bloom::document::NodeId;

    const auto layerStackNodeId = NodeId::fromRaw(nodeIdBase);
    const auto outputNodeId = NodeId::fromRaw(nodeIdBase + 1);
    const auto edgeId = EdgeId::fromRaw(nodeIdBase);

    bloom::document::CanonicalGraph graph(layerStackNodeId);
    const bool topologyCreated =
        graph.addNode({layerStackNodeId,
                       std::string(bloom::document::kLayerStackNodeType),
                       {},
                       bloom::document::kLayerStackNodeSchemaVersion}) &&
        graph.addNode({outputNodeId,
                       std::string(bloom::document::kCompositionOutputNodeType),
                       {},
                       bloom::document::kCompositionOutputNodeSchemaVersion}) &&
        graph.addEdge(
            {edgeId,
             {layerStackNodeId, std::string(bloom::document::kLayerStackOutputPort)},
             bloom::document::NodeInputRef{
                 outputNodeId, std::string(bloom::document::kCompositionOutputInputPort)}});
    if (!topologyCreated) {
        std::abort();
    }
    graph.setCompositionOutput(
        {outputNodeId, std::string(bloom::document::kCompositionOutputOutputPort)});

    return bloom::document::Composition(id, std::move(name), RationalTime::fromInteger(10),
                                        std::move(graph));
}

void testConstructorFallbackPicksLowestCompositionIdNotInsertionOrder(Expectations& expectations) {
    bloom::document::Project project(bloom::document::ProjectId::fromRaw(1),
                                     "Fallback Fixture Project");

    // Insertion order is [id 10, id 2] -- the OLD behavior (compositions().front()) would pick id
    // 10; the fixed min-scan must pick id 2, the numerically lowest valid CompositionId.
    const auto highId = bloom::document::CompositionId::fromRaw(10);
    const auto lowId = bloom::document::CompositionId::fromRaw(2);
    expectations.expect(
        project.addComposition(makeMinimalComposition(highId, "High Id Composition", 1)),
        "fallback fixture: the higher-id composition (inserted first) is added");
    expectations.expect(
        project.addComposition(makeMinimalComposition(lowId, "Low Id Composition", 10)),
        "fallback fixture: the lower-id composition (inserted second) is added");
    expectations.expect(project.compositions().size() == 2 &&
                            project.compositions().front().id() == highId,
                        "fallback fixture: insertion order really does differ from id order");

    bloom::document::Document document(std::move(project));
    bloom::commands::CommandStack commandStack(document);

    // A bogus/absent CompositionId: composition() finds nullptr for it, so the constructor's
    // fallback runs.
    const auto bogusId = bloom::document::CompositionId::fromRaw(999);
    CompositionSession session(document, commandStack, bogusId);

    expectations.expect(session.compositionId() == lowId,
                        "constructor fallback (#75): the lowest CompositionId wins over insertion "
                        "order, not compositions().front()");
}

} // namespace

int main(int argc, char** argv) {
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication application(argc, argv);
    Expectations expectations;
    testRebindClearsSelectionResetsTimeAndEmitsEverySignalOnce(expectations);
    testConstructorFallbackPicksLowestCompositionIdNotInsertionOrder(expectations);
    return expectations.failures() == 0 ? 0 : 1;
}
