#include <bloom/commands/command_stack.hpp>
#include <bloom/document/new_project.hpp>
#include <bloom/document/project.hpp>
#include <bloom/document/shape.hpp>
#include <bloom/project/canonical_document.hpp>
#include <bloom/project/document_decode.hpp>
#include <bloom/project/document_migration.hpp>
#include <bloom/project/document_reconstruct.hpp>
#include <bloom/runtime/cpu_composition_evaluator.hpp>
#include <bloom/runtime/node_definition_registry.hpp>
#include <bloom/runtime/qualified_display_processor_provider.hpp>
#include <bloom/runtime/reference_display_preparation.hpp>
#include <bloom/runtime/snapshot_compiler.hpp>
#include <bloom/ui/composition_preview_controller.hpp>
#include <bloom/ui/composition_preview_pipeline.hpp>
#include <bloom/ui/composition_session.hpp>
#include <bloom/ui/kit/button.hpp>
#include <bloom/ui/kit/color_chip.hpp>
#include <bloom/ui/kit/color_picker.hpp>
#include <bloom/ui/kit/theme.hpp>
#include <bloom/ui/kit/value_field.hpp>
#include <bloom/ui/node_editor.hpp>
#include <bloom/ui/properties_editor.hpp>
#include <bloom/ui/task_ui_bridge.hpp>

#include <QApplication>
#include <QElapsedTimer>
#include <QGraphicsProxyWidget>
#include <QGraphicsView>
#include <QLineEdit>
#include <QSettings>
#include <QThread>
#include <QVBoxLayout>

#include <cmath>
#include <iostream>
#include <regex>
#include <string>

namespace {
using namespace bloom;
int failures = 0;
void expect(const bool condition, const char* message) {
    if (!condition) {
        ++failures;
        std::cerr << message << '\n';
    }
}
template <typename Predicate> bool waitUntil(Predicate predicate) {
    QElapsedTimer timer;
    timer.start();
    while (!predicate() && timer.elapsed() < 5000) {
        QCoreApplication::processEvents();
        QThread::msleep(1);
    }
    QCoreApplication::processEvents();
    return predicate();
}
void typeHex(ui::kit::KColorChip& chip, const QString& text) {
    auto* picker = chip.picker();
    picker->hexField()->setText(text);
    Q_EMIT picker->hexField()->editingFinished();
}
QColor centerPixel(QWidget& widget) {
    const auto image = widget.grab().toImage();
    return image.pixelColor(image.width() / 2, image.height() / 2);
}
ui::kit::KColorChip* nodeChip(ui::NodeGraphEditor& editor) {
    const auto* view = editor.findChild<QGraphicsView*>();
    if (!view)
        return nullptr;
    for (auto* item : view->scene()->items()) {
        if (const auto* proxy = qgraphicsitem_cast<QGraphicsProxyWidget*>(item)) {
            if (auto* chip = proxy->widget()->findChild<ui::kit::KColorChip*>("nodeColorChip"))
                return chip;
            if (auto* chip = qobject_cast<ui::kit::KColorChip*>(proxy->widget());
                chip && chip->objectName() == "nodeColorChip")
                return chip;
        }
    }
    return nullptr;
}
void boundaryPins() {
    const auto format = document::CompositionFormat::create(4, 4);
    if (!format)
        std::abort();
    auto project =
        document::makeNewProject("Colour", "Main", core::RationalTime::fromInteger(10), *format);
    const auto id = project.initialCompositionId;
    document::Document document(std::move(project.project));
    commands::CommandStack stack(document);
    ui::CompositionSession session(document, stack, id);
    expect(session.addSolidLayer("Solid", {0.2, 0.3, 0.4, 1}), "solid fixture created");
    expect(waitUntil([&] { return static_cast<bool>(session.colorConverter()); }),
           "session converter prepares off-thread");
    expect(!session.colorConverter("unknown.encoding"), "unknown parameter schema fails closed");
    const auto* parameter = session.parameterForSelection(document::kSolidColorParameterRole);
    if (!parameter)
        return;
    const auto parameterId = parameter->id;
    const auto before = session.snapshot().revision();
    QWidget window;
    auto* layout = new QVBoxLayout(&window);
    auto* properties = new ui::PropertiesEditor(session, &window);
    auto* nodes = new ui::NodeGraphEditor(session, &window);
    layout->addWidget(properties);
    layout->addWidget(nodes);
    window.resize(900, 900);
    window.show();
    QCoreApplication::processEvents();
    expect(session.snapshot().revision() == before &&
               session.constantColorValue(parameterId) == core::Color4d{0.2, 0.3, 0.4, 1},
           "presenting existing reference values authors no revision or data change");
    auto* chip = properties->findChild<ui::kit::KColorChip*>("propertiesSolidColorChip");
    expect(chip && chip->isEnabled(), "qualified properties chip is enabled");
    if (!chip)
        return;
    typeHex(*chip, "#F03B2E");
    const auto stored = session.constantColorValue(parameterId);
    expect(stored && std::abs(stored->red - 0.871367) < 4e-5 &&
               std::abs(stored->green - 0.043735) < 4e-5 &&
               std::abs(stored->blue - 0.027321) < 4e-5 && stored->alpha == 1,
           "display F03B2E commits reference linear sRGB through the session command");
    expect(chip->color().space == ui::kit::ColorSpace::Reference &&
               centerPixel(*chip) == QColor("#F03B2E"),
           "properties chip paints the picked display pixel");
    auto* card = nodeChip(*nodes);
    expect(card && centerPixel(*card) == QColor("#F03B2E"),
           "node card paints the same display pixel");
    for (int i = 0; i < 20; ++i) {
        chip->openPicker();
        typeHex(*chip, "#F03B2E");
        chip->closePicker();
    }
    expect(session.constantColorValue(parameterId) == stored,
           "repeated hex round trips have stable stored values");
    const auto stable = session.snapshot().revision();
    chip->openPicker();
    chip->closePicker();
    expect(session.snapshot().revision() == stable,
           "opening and closing a picker never authors a value");

    runtime::TaskScheduler scheduler;
    ui::TaskUiBridge bridge(scheduler);
    runtime::NodeDefinitionRegistry definitions;
    expect(runtime::registerBuiltInNodeDefinitions(definitions), "preview definitions registered");
    definitions.freeze();
    runtime::SnapshotCompiler compiler(definitions);
    const runtime::CpuCompositionEvaluator evaluator;
    const runtime::CpuReferenceDisplayPreparer displayPreparer;
    runtime::QualifiedDisplayProcessorProvider provider;
    provider.publish(runtime::buildBloomNeutralQualifiedDisplayProcessor());
    const auto pipeline =
        ui::makeCompositionPreviewPipeline(compiler, evaluator, displayPreparer, provider);
    ui::CompositionPreviewController preview(session, scheduler, bridge, pipeline);
    expect(waitUntil([&] { return preview.state().activity == ui::PreviewActivity::Ready; }),
           "qualified viewer frame completes");
    const auto frame = preview.state().frame;
    const auto display = frame ? frame->displayBufferView() : std::nullopt;
    expect(frame && frame->isOcioQualified() && display && !display->pixels.empty() &&
               display->pixels.front() == render::Rgba8{240, 59, 46, 255},
           "the viewer pipeline frame pixel equals the picked F03B2E after display processing");

    auto* red = properties->findChild<ui::kit::KValueField*>("solidColorRedEditor");
    expect(red && std::abs(red->value() - 240.0 / 255.0) < 3e-5 && red->unit().isEmpty(),
           "SDR RGBA fields show display numbers");
    if (red) {
        red->setValue(0.5);
        const auto edited = session.constantColorValue(parameterId);
        expect(edited && std::abs(edited->red - 0.214041) < 3e-5,
               "RGBA display edit commits reference numbers");
    }
    typeHex(*chip, "#FFFFFF");
    const auto white = session.constantColorValue(parameterId);
    expect(white && white->red <= 1.0 && white->green <= 1.0 && white->blue <= 1.0 && red &&
               red->unit().isEmpty(),
           "display white stays in range without a reference suffix");
    constexpr core::Color4d hdr{4.123456789, -0.25, 0.125, 0.75};
    expect(session.setSelectedSolidColor(hdr), "HDR values are authored through the session");
    expect(red && red->value() == hdr.red && red->unit() == "reference",
           "HDR RGBA field retains exact reference value and suffix");
    const auto hdrRevision = session.snapshot().revision();
    chip->openPicker();
    chip->closePicker();
    expect(session.constantColorValue(parameterId) == hdr &&
               session.snapshot().revision() == hdrRevision,
           "opening an HDR swatch preserves its unclipped stored RGB and alpha");
    if (red) {
        red->setValue(8.25);
        const auto edited = session.constantColorValue(parameterId);
        expect(edited && edited->red == 8.25 && edited->green == hdr.green &&
                   edited->blue == hdr.blue,
               "editing an HDR field preserves the other reference channels");
    }
    preview.beginShutdown();
    bridge.beginShutdown();
    expect(waitUntil([&] { return scheduler.isQuiescent(); }), "preview shuts down safely");
}

// SHAPEFIX-1, deliverable 1: the registry-generated Color4d row (the shape source's fillColor and
// strokeColor) has to behave exactly like the handcrafted Solid colour row above -- same chip
// enablement so the picker actually opens, same "no reference suffix" for an in-range colour, and
// the same channel-field geometry -- because both now go through the ONE shared
// properties::addColorRow()/refreshColor() implementation and the one CompositionSession::
// colorConverter() gate that decides which schemas may convert at all.
void shapeColorPin() {
    const auto format = document::CompositionFormat::create(4, 4);
    if (!format)
        std::abort();
    auto project = document::makeNewProject("Shape Colour", "Main",
                                            core::RationalTime::fromInteger(10), *format);
    const auto id = project.initialCompositionId;
    document::Document document(std::move(project.project));
    commands::CommandStack stack(document);
    ui::CompositionSession session(document, stack, id);

    expect(session.addSolidLayer("Solid", {0.2, 0.3, 0.4, 1}), "solid baseline fixture created");
    const auto* solidLayerId = std::get_if<document::LayerId>(&session.selection().primary);
    expect(solidLayerId != nullptr, "solid layer is selected after creation");
    const auto solidLayer = solidLayerId ? *solidLayerId : document::LayerId{};

    expect(session.addShapeLayer(document::ShapeKind::Rectangle), "shape fixture created");
    const auto* shapeLayerId = std::get_if<document::LayerId>(&session.selection().primary);
    expect(shapeLayerId != nullptr, "shape layer is selected after creation");
    const auto shapeLayer = shapeLayerId ? *shapeLayerId : document::LayerId{};

    expect(waitUntil([&] { return static_cast<bool>(session.colorConverter()); }),
           "session converter prepares off-thread");
    expect(static_cast<bool>(session.colorConverter("bloom.shape.fill-color")),
           "the shape source's fill-color schema is a recognized colour schema");
    expect(static_cast<bool>(session.colorConverter("bloom.shape.stroke-color")),
           "the shape source's stroke-color schema is a recognized colour schema");

    QWidget window;
    auto* layout = new QVBoxLayout(&window);
    auto* properties = new ui::PropertiesEditor(session, &window);
    layout->addWidget(properties);
    window.resize(900, 900);
    window.show();
    QCoreApplication::processEvents();

    session.selectLayer(solidLayer);
    QCoreApplication::processEvents();
    auto* solidRed = properties->findChild<ui::kit::KValueField*>("solidColorRedEditor");
    expect(solidRed != nullptr, "Solid row's red channel field is found");
    // The channel row is a disclosure (hidden until the chevron expands it, per ui-grammar.md's
    // "Expanded Properties RGBA fields..."), so its geometry is only meaningful once expanded --
    // an unexpanded QWidget keeps whatever stale geometry it had before its first real layout pass.
    auto* solidExpand = properties->findChild<ui::kit::KButton*>("propertiesSolidColorExpand");
    expect(solidExpand != nullptr, "the Solid row's RGBA disclosure chevron is found");
    if (solidExpand != nullptr)
        solidExpand->setChecked(true);
    QCoreApplication::processEvents();
    const QPoint solidRedPos = solidRed ? solidRed->mapTo(properties, QPoint(0, 0)) : QPoint();

    session.selectLayer(shapeLayer);
    QCoreApplication::processEvents();
    QWidget* fillRow = nullptr;
    for (auto* row : properties->findChildren<QWidget*>(QStringLiteral("propertiesRegistryRow"))) {
        if (row->property("role").toString() == QStringLiteral("fillColor")) {
            fillRow = row;
            break;
        }
    }
    expect(fillRow != nullptr, "the shape's fillColor registry row is found");
    auto* shapeChip =
        fillRow ? fillRow->findChild<ui::kit::KColorChip*>("propertiesRegistryColor") : nullptr;
    auto* shapeRed =
        fillRow ? fillRow->findChild<ui::kit::KValueField*>("propertiesRegistryValue") : nullptr;
    expect(shapeChip != nullptr && shapeRed != nullptr,
           "the registry row's chip and red channel field are found");

    expect(shapeChip != nullptr && shapeChip->isEnabled(),
           "the registry colour row's chip is enabled, exactly like the Solid row's");
    if (shapeChip != nullptr) {
        shapeChip->openPicker();
        expect(shapeChip->isPickerOpen(), "the registry colour row's chip click opens the picker");
        shapeChip->closePicker();
    }
    expect(shapeRed != nullptr && shapeRed->unit().isEmpty(),
           "the default in-range fillColor shows no 'reference' label overlapping the channels");

    auto* shapeExpand =
        fillRow ? fillRow->findChild<ui::kit::KButton*>("propertiesRegistryColorExpand") : nullptr;
    expect(shapeExpand != nullptr, "the registry row's RGBA disclosure chevron is found");
    if (shapeExpand != nullptr)
        shapeExpand->setChecked(true);
    QCoreApplication::processEvents();

    if (solidRed != nullptr && shapeRed != nullptr) {
        const QPoint shapeRedPos = shapeRed->mapTo(properties, QPoint(0, 0));
        expect(solidRedPos.x() == shapeRedPos.x(),
               "the registry row's channel field x position matches the Solid row's");
    }
}

void migrationPin() {
    auto project = document::makeNewProject("Legacy", "Main", core::RationalTime::fromInteger(10));
    const auto id = project.initialCompositionId;
    document::Document original(std::move(project.project));
    commands::CommandStack stack(original);
    ui::CompositionSession session(original, stack, id);
    const core::Color4d oldNumbers{0.86, 0.04, 2.5, 0.75};
    expect(session.addSolidLayer("Legacy solid", oldNumbers), "legacy colour fixture authored");
    auto snapshot = original.snapshot();
    const auto settings =
        document::makeBloomNeutralColorSettingsV1(color::kBloomNeutralV1ConfigDigest);
    std::vector<char> payload(4096);
    std::vector<std::size_t> sort(4096);
    const project::CanonicalDocumentV1 input{.snapshot = &snapshot,
                                             .colorSettings = &settings,
                                             .payloadScratch = payload,
                                             .sortScratch = sort};
    const auto size = project::canonicalDocumentSize(input);
    expect(static_cast<bool>(size), "legacy fixture serializes");
    if (!size)
        return;
    std::string json(*size.value(), '\0');
    expect(static_cast<bool>(project::encodeCanonicalDocument(input, json)),
           "fixture canonical encoding succeeds");
    const auto replace = [&json](const std::string& pattern, const std::string& to) {
        const std::regex expression(pattern);
        expect(std::regex_search(json, expression), "legacy fixture field found");
        json = std::regex_replace(json, expression, to);
    };
    // Whatever the current minor is, the fixture is a 1.9 document: the production chain from 1.9
    // to the current canonical version is what runs, and later minors add only optional fields.
    // Only the FIRST schemaVersion is the document's; colorSettings carries its own further down.
    json = std::regex_replace(json, std::regex(R"("minor"\s*:\s*\d+)"), "\"minor\": 9",
                              std::regex_constants::format_first_only);
    replace(R"(,\s*"assets"\s*:\s*\[\s*\])", "");
    replace(R"(,\s*"dataBlocks"\s*:\s*\[\s*\])", "");
    replace(R"(,\s*"backgroundColor"\s*:\s*\[[^\]]*\])", "");
    replace(R"(,\s*"asset"\s*:\s*"0")", "");
    replace(R"(,\s*"dataBlock"\s*:\s*"0")", "");
    auto coordinator = project::ProjectIoMemoryCoordinator::create(64ULL << 20U);
    if (!coordinator)
        std::abort();
    auto operation = coordinator->createOperation(64ULL << 20U, 64ULL << 20U);
    if (!operation)
        std::abort();
    const auto parsed = project::parseStrictJsonDom(
        {reinterpret_cast<const std::byte*>(json.data()), json.size()}, {}, *operation);
    expect(static_cast<bool>(parsed), "legacy document parses");
    if (!parsed)
        return;
    const auto migrated = project::migrateDocumentDom(
        parsed.document()->root(), {1, 9}, project::kCanonicalDocumentSchemaVersionV1,
        project::kProductionDocumentMigrationSteps, {}, *operation);
    expect(migrated.outcome() == project::MigrationOutcome::Migrated,
           "real production migration runs");
    if (!migrated.migratedRoot())
        return;
    const auto decoded = project::decodeDocumentEnvelope(*migrated.migratedRoot());
    expect(static_cast<bool>(decoded), "migrated document decodes");
    if (!decoded)
        return;
    auto rebuilt = project::reconstructDocument(*decoded.value());
    expect(static_cast<bool>(rebuilt), "migrated document reconstructs");
    if (!rebuilt)
        return;
    auto& document = *rebuilt.value()->document;
    commands::CommandStack reopenedStack(document);
    ui::CompositionSession reopened(document, reopenedStack, id);
    expect(waitUntil([&] { return static_cast<bool>(reopened.colorConverter()); }),
           "migrated session converter prepares");
    const auto* composition = reopened.composition();
    for (const auto& layer : composition->graph().layerStack().entries())
        reopened.selectLayer(layer.layerId);
    const auto* parameter = reopened.parameterForSelection(document::kSolidColorParameterRole);
    const auto revision = reopened.snapshot().revision();
    ui::PropertiesEditor panel(reopened);
    expect(parameter && reopened.constantColorValue(parameter->id) == oldNumbers &&
               reopened.snapshot().revision() == revision && reopenedStack.size() == 0,
           "migrated old document keeps every stored colour number; UI adds no command");
}
} // namespace
int main(int argc, char** argv) try {
    QApplication application(argc, argv);
    QCoreApplication::setOrganizationName("BloomColourBoundaryTests");
    QCoreApplication::setApplicationName("ColourBoundary");
    ui::kit::installKinetikTheme(application);
    boundaryPins();
    shapeColorPin();
    migrationPin();
    return failures == 0 ? 0 : 1;
} catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
}
