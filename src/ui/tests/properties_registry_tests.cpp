#include <QApplication>
#include <QSettings>
#include <bloom/commands/command_stack.hpp>
#include <bloom/document/document.hpp>
#include <bloom/document/new_project.hpp>
#include <bloom/document/project.hpp>
#include <bloom/ui/composition_session.hpp>
#include <bloom/ui/kit/dropdown.hpp>
#include <bloom/ui/kit/section.hpp>
#include <bloom/ui/properties_editor.hpp>
#include <iostream>
using namespace bloom;
namespace {
int failures = 0;
void expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
        ++failures;
    }
}
QWidget* row(QWidget& panel, const char* role) {
    for (auto* candidate : panel.findChildren<QWidget*>("propertiesRegistryRow"))
        if (candidate->property("role").toString() == role)
            return candidate;
    return nullptr;
}
void registryRows() {
    auto project =
        document::makeNewProject("Registry", "Main", core::RationalTime::fromInteger(10));
    auto id = project.initialCompositionId;
    document::Document document(std::move(project.project));
    commands::CommandStack stack(document);
    ui::CompositionSession session(document, stack, id);
    expect(session.addSolidLayer("Solid", {1, 0, 0, 1}), "add solid");
    ui::PropertiesEditor panel(session);
    auto* width = row(panel, "width");
    auto* height = row(panel, "height");
    expect(width && height, "registry adds both solid dimensions");
    if (width) {
        auto* field = width->findChild<ui::kit::KValueField*>();
        const auto parameter =
            document::ParameterId::fromRaw(width->property("parameterId").toULongLong());
        const auto selection = session.selection().primary;
        const auto before = stack.size();
        Q_EMIT field->scrubStarted();
        field->setValue(731);
        expect(stack.size() == before, "scrubbing publishes no intermediate commands");
        Q_EMIT field->scrubFinished();
        expect(stack.size() == before + 1 && session.effectiveScalarValue(parameter) == 731,
               "scrub commits one exact parameter edit");
        expect(session.selection().primary == selection, "registry edit preserves selection");
        expect(session.undo(), "registry edit is undoable");
        auto* section = panel.findChild<ui::kit::KSection*>("propertiesSection_solid");
        field->setValue(872);
        Q_EMIT section->resetRequested();
        expect(session.effectiveScalarValue(parameter) == 1920,
               "source reset includes registry defaults");
    }
    expect(session.addTextLayer("Text", "Hello", 48, {1, 1, 1, 1}), "add text");
    auto* alignment = row(panel, "alignment");
    expect(alignment && row(panel, "line-height") && row(panel, "letter-spacing"),
           "registry adds every text layout parameter");
    if (alignment) {
        auto* dropdown = alignment->findChild<ui::kit::KDropdown*>();
        expect(dropdown && dropdown->count() == 3, "alignment is a closed dropdown");
        if (dropdown)
            dropdown->setCurrentIndex(2);
        expect(session.canUndo(), "enum edit uses command history");
    }
}
} // namespace
int main(int argc, char** argv) {
    QApplication application(argc, argv);
    QCoreApplication::setOrganizationName("BloomPropertiesRegistryTest");
    QSettings().clear();
    registryRows();
    QSettings().clear();
    return failures ? 1 : 0;
}
