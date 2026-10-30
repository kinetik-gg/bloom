#include "export_output_row.hpp"

#include "export_detail_support.hpp"
#include "export_panel_text.hpp"
#include "properties_sections.hpp"
#include "reveal_in_file_manager.hpp"

#include <bloom/ui/composition_session.hpp>
#include <bloom/ui/export_queue_adapter.hpp>
#include <bloom/ui/kit/button.hpp>
#include <bloom/ui/kit/controls.hpp>
#include <bloom/ui/kit/dropdown.hpp>
#include <bloom/ui/kit/file_chooser.hpp>
#include <bloom/ui/kit/icons.hpp>
#include <bloom/ui/kit/popup_window.hpp>
#include <bloom/ui/kit/row.hpp>
#include <bloom/ui/kit/section.hpp>
#include <bloom/ui/kit/switch_control.hpp>
#include <bloom/ui/kit/text_field.hpp>
#include <bloom/ui/kit/value_field.hpp>

#include <QAction>
#include <QDir>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QMenu>
#include <QSettings>
#include <QSignalBlocker>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>

namespace bloom::ui::exportpanel {

using detail::choicesOf;
using detail::displayPath;
using detail::fill;
using detail::isH26x;
using detail::isImage;
using detail::isMovie;
using detail::named;
using detail::reasonLabel;
using detail::select;
using document::ExportFormat;

namespace {

// A single-line label that gives up the middle of its text, so a path keeps its folder's start and
// its file's end.
class MiddleElidedLabel final : public kit::KLabel {
  public:
    using kit::KLabel::KLabel;
    void setFullText(const QString& text) {
        full_ = text;
        setMinimumWidth(0);
        setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
        setToolTip(text);
        refit();
    }
    [[nodiscard]] QString fullText() const { return full_; }

  protected:
    void resizeEvent(QResizeEvent* event) override {
        kit::KLabel::resizeEvent(event);
        refit();
    }

  private:
    void refit() { setText(fontMetrics().elidedText(full_, Qt::ElideMiddle, width())); }
    QString full_;
};

} // namespace

struct OutputRow::Fields final {
    kit::KRow* header = nullptr;
    kit::KIconButton* disclosure = nullptr;
    kit::KSwitch* enabled = nullptr;
    kit::KLabel* preset = nullptr;
    MiddleElidedLabel* destinationSummary = nullptr;
    kit::KLabel* status = nullptr;
    kit::KIconButton* menu = nullptr;
    QWidget* body = nullptr;
    kit::KLabel* refusal = nullptr;

    // The inline "a file is already there" prompt.
    QWidget* replaceBox = nullptr;
    kit::KLabel* replaceText = nullptr;
    kit::KButton* replace = nullptr;
    kit::KButton* chooseName = nullptr;

    // Format.
    kit::KDropdown* format = nullptr;
    kit::KLabel* formatReason = nullptr;
    kit::KButton* remedy = nullptr;
    kit::KPropertyRow* formatReasonRow = nullptr;
    kit::KLabel* otherReasons = nullptr;
    kit::KPropertyRow* otherReasonsRow = nullptr;

    // Save to.
    kit::KPathField* destination = nullptr;
    kit::KButton* tokenHelp = nullptr;
    kit::KLabel* resolved = nullptr;
    kit::KPropertyRow* resolvedRow = nullptr;
    kit::KLabel* destinationReason = nullptr;
    kit::KPropertyRow* destinationReasonRow = nullptr;
    kit::KLabel* facts = nullptr;
    kit::KPropertyRow* factsRow = nullptr;
    kit::KDropdown* overwrite = nullptr;

    // Quality.
    kit::KDropdown* profile = nullptr;
    kit::KValueField* bitrate = nullptr;
    kit::KDropdown* encoder = nullptr;
    kit::KDropdown* compression = nullptr;
    kit::KDropdown* colourSpace = nullptr;
    kit::KDropdown* display = nullptr;
    kit::KDropdown* view = nullptr;
    kit::KSwitch* bakeLook = nullptr;
    kit::KSwitch* useCachedFrames = nullptr;
    kit::KSwitch* includeAudio = nullptr;
    kit::KDropdown* sampleRate = nullptr;
    kit::KDropdown* numbering = nullptr;
    kit::KValueField* startNumber = nullptr;
    kit::KValueField* padding = nullptr;
    kit::KLineEdit* pattern = nullptr;
    kit::KPropertyRow* profileRow = nullptr;
    kit::KPropertyRow* bitrateRow = nullptr;
    kit::KPropertyRow* encoderRow = nullptr;
    kit::KPropertyRow* compressionRow = nullptr;
    kit::KPropertyRow* colourSpaceRow = nullptr;
    kit::KPropertyRow* displayRow = nullptr;
    kit::KPropertyRow* viewRow = nullptr;
    kit::KPropertyRow* bakeLookRow = nullptr;
    kit::KPropertyRow* cachedRow = nullptr;
    kit::KPropertyRow* audioRow = nullptr;
    kit::KPropertyRow* sampleRateRow = nullptr;
    kit::KPropertyRow* numberingRow = nullptr;
    kit::KPropertyRow* patternRow = nullptr;

    // After render.
    kit::KSwitch* importIntoAssets = nullptr;
    kit::KLabel* importLabel = nullptr;

    // Every control that edits project truth: disabled with the host's reason while the job runs.
    QList<QWidget*> editors;
    QPointer<kit::KPopupWindow> help;
};

OutputRow::OutputRow(CompositionSession& session, ExportQueueAdapter& adapter,
                     const document::ExportJobId job, const document::ExportOutputId output,
                     QWidget* const parent)
    : QWidget(parent), session_(session), adapter_(adapter), job_(job), output_(output),
      f_(std::make_unique<Fields>()) {
    setObjectName(QStringLiteral("exportOutputRow"));
    setProperty("outputId", static_cast<qulonglong>(output.value()));
    build();
    setExpanded(false);
}

OutputRow::~OutputRow() = default;

namespace {

kit::KPropertyRow* addRow(QVBoxLayout& layout, QWidget* parent, const QString& label,
                          std::initializer_list<QWidget*> controls) {
    auto* row = new kit::KPropertyRow(properties::makeRowLabel(label, parent), nullptr, controls,
                                      parent);
    layout.addWidget(row);
    return row;
}

[[nodiscard]] kit::KButton* compactButton(const QString& text, const QString& name, QWidget* parent) {
    auto* button = new kit::KButton(text, parent);
    button->setObjectName(name);
    button->setControlSize(kit::KButton::ControlSize::Compact);
    // A button keeps its own width; the row's flexible field takes the rest.
    button->setFixedWidth(button->sizeHint().width());
    return button;
}

} // namespace

void OutputRow::build() {
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    // Header: [caret][switch][format][where it saves ........][state][menu].
    f_->header = new kit::KRow(this);
    f_->header->setObjectName(QStringLiteral("exportOutputHeader"));
    f_->disclosure = new kit::KIconButton(f_->header);
    f_->disclosure->setObjectName(QStringLiteral("exportOutputDisclosure"));
    f_->disclosure->setProperty("disclosure", true);
    f_->disclosure->setAccessibleName(tr("Show output settings"));
    auto* cells = new QWidget(f_->header);
    auto* cellLayout = new QHBoxLayout(cells);
    cellLayout->setContentsMargins(0, 0, 0, 0);
    cellLayout->setSpacing(kit::px(kit::Spacing::S));
    f_->enabled = named(new kit::KSwitch(cells), QStringLiteral("exportOutputEnabled"),
                        tr("Render this output"));
    cellLayout->addWidget(f_->enabled, 0);
    f_->preset = new kit::KLabel(cells);
    f_->preset->setObjectName(QStringLiteral("exportOutputPreset"));

    f_->destinationSummary = new MiddleElidedLabel(cells);
    f_->destinationSummary->setObjectName(QStringLiteral("exportOutputDestination"));
    f_->destinationSummary->setTone(kit::Color::Muted);
    f_->status = new kit::KLabel(cells);
    f_->status->setObjectName(QStringLiteral("exportOutputStatus"));
    f_->status->setTone(kit::Color::Muted);
    f_->status->setMaximumWidth(kit::px(kit::Size::DropdownWidthWide) + kit::px(kit::Size::ToggleCell));
    f_->menu = new kit::KIconButton(f_->header);
    f_->menu->setObjectName(QStringLiteral("exportOutputMenuButton"));
    f_->menu->setIcon(kit::icon(kit::IconId::ContextMenu, kit::IconRole::Chrome));
    f_->menu->setAccessibleName(tr("Output actions"));
    // Every text gives way when the row is narrow: the format first keeps most of its room.
    cellLayout->addWidget(f_->preset, 3);
    cellLayout->addWidget(f_->destinationSummary, 4);
    cellLayout->addWidget(f_->status, 3);
    f_->header->setCells({f_->disclosure}, cells, {}, f_->menu);
    f_->header->setLeadingInset(kit::px(kit::Size::ToggleCell));
    layout->addWidget(f_->header);

    // Body.
    f_->body = new QWidget(this);
    f_->body->setObjectName(QStringLiteral("exportOutputBody"));
    auto* rows = new QVBoxLayout(f_->body);
    rows->setContentsMargins(kit::px(kit::Spacing::XS), kit::px(kit::Spacing::XS),
                             kit::px(kit::Spacing::XS), kit::px(kit::Spacing::S));
    rows->setSpacing(0);
    layout->addWidget(f_->body);

    f_->refusal = reasonLabel(f_->body, QStringLiteral("exportOutputRefusal"), kit::Color::Muted);
    f_->refusal->hide();
    rows->addWidget(f_->refusal);

    // The inline prompt: the artist's question and its two answers, at the top of this output.
    f_->replaceBox = new QWidget(f_->body);
    f_->replaceBox->setObjectName(QStringLiteral("exportReplaceBox"));
    auto* promptLayout = new QVBoxLayout(f_->replaceBox);
    promptLayout->setContentsMargins(0, 0, 0, 0);
    promptLayout->setSpacing(0);
    f_->replaceText = reasonLabel(f_->replaceBox, QStringLiteral("exportReplaceText"), kit::Color::Foreground);
    f_->replaceText->setTypeRole(kit::TypeRole::Ui);
    f_->replace = compactButton(tr("Replace it"), QStringLiteral("exportReplaceButton"), f_->replaceBox);
    f_->chooseName = compactButton(tr("Choose another name"), QStringLiteral("exportChooseNameButton"),
                                   f_->replaceBox);
    auto* promptText = new kit::KPropertyRow(properties::makeRowLabel(QString{}, f_->replaceBox), nullptr,
                                             {f_->replaceText}, f_->replaceBox);
    promptText->setObjectName(QStringLiteral("exportReplaceTextRow"));
    auto* promptButtons = new kit::KPropertyRow(properties::makeRowLabel(QString{}, f_->replaceBox), nullptr,
                                                {f_->replace, f_->chooseName}, f_->replaceBox);
    promptButtons->setObjectName(QStringLiteral("exportReplaceButtonRow"));
    promptLayout->addWidget(promptText);
    promptLayout->addWidget(promptButtons);
    rows->addWidget(f_->replaceBox);

    buildFormat(*rows);
    buildDestination(*rows);
    buildQuality(*rows);
    buildAfterRender(*rows);

    connect(f_->disclosure, &QAbstractButton::clicked, this,
            [this] { setExpanded(!expanded_); });
    connect(f_->enabled, &QAbstractButton::toggled, this, [this](const bool on) {
        if (!refreshing_)
            report(adapter_.editOutput(job_, output_, outputEdit([&](auto& edit) { edit.enabled = on; })));
    });
    connect(f_->menu, &QAbstractButton::clicked, this, [this] {
        const auto menu = buildMenu();
        if (menu)
            menu->exec(f_->menu->mapToGlobal(QPoint(0, f_->menu->height())));
    });
    f_->editors.push_back(f_->enabled);
}

void OutputRow::buildFormat(QVBoxLayout& rows) {
    f_->format = named(new kit::KDropdown(f_->body), QStringLiteral("exportPresetPicker"), tr("Format"));
    addRow(rows, f_->body, tr("Format"), {f_->format});
    f_->formatReason = reasonLabel(f_->body, QStringLiteral("exportPresetReason"), kit::Color::Muted);
    f_->remedy = compactButton({}, QStringLiteral("exportRemedyButton"), f_->body);
    f_->formatReasonRow = addRow(rows, f_->body, QString{}, {f_->formatReason, f_->remedy});
    f_->otherReasons = reasonLabel(f_->body, QStringLiteral("exportOtherReasons"), kit::Color::Muted);
    f_->otherReasonsRow = addRow(rows, f_->body, QString{}, {f_->otherReasons});
    f_->editors.push_back(f_->format);
    connect(f_->format, &kit::KDropdown::activated, this, [this] { applyFormat(); });
    connect(f_->remedy, &kit::KButton::clicked, this, [this] {
        if (const auto* output = findOutput())
            adapter_.requestRemedy(output->availability.remedy);
    });
}

void OutputRow::buildDestination(QVBoxLayout& rows) {
    f_->destination = named(new kit::KPathField(f_->body), QStringLiteral("exportDestination"),
                            tr("Save to"));
    // Browse lives inside the field: one control for one job.
    auto* browse = f_->destination->addAction(
        kit::icon(kit::IconId::Folder, kit::IconRole::Chrome), QLineEdit::TrailingPosition);
    browse->setObjectName(QStringLiteral("exportBrowseAction"));
    browse->setToolTip(tr("Choose where to save"));
    f_->tokenHelp = compactButton(tr("?"), QStringLiteral("exportTokenHelpButton"), f_->body);
    f_->tokenHelp->setFixedWidth(std::max(f_->tokenHelp->sizeHint().width(), kit::px(kit::Size::Control)));
    f_->tokenHelp->setToolTip(tr("Name tokens"));
    f_->tokenHelp->setAccessibleName(tr("Name tokens help"));
    addRow(rows, f_->body, tr("Save to"), {f_->destination, f_->tokenHelp});
    f_->resolved = reasonLabel(f_->body, QStringLiteral("exportResolved"), kit::Color::Muted);
    f_->resolved->setWordWrap(false);
    f_->resolved->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
    f_->resolved->setMinimumWidth(0);
    f_->resolvedRow = addRow(rows, f_->body, QString{}, {f_->resolved});
    f_->facts = reasonLabel(f_->body, QStringLiteral("exportEstimate"), kit::Color::Muted);
    f_->factsRow = addRow(rows, f_->body, QString{}, {f_->facts});
    f_->destinationReason =
        reasonLabel(f_->body, QStringLiteral("exportDestinationReason"), kit::Color::Muted);
    f_->destinationReasonRow = addRow(rows, f_->body, QString{}, {f_->destinationReason});
    f_->overwrite = named(new kit::KDropdown(f_->body), QStringLiteral("exportOverwrite"),
                          tr("If a file is already there"));
    f_->overwrite->addItem(tr("Ask me first"),
                           static_cast<int>(document::ExportOverwritePolicy::AskBeforeReplacing));
    f_->overwrite->addItem(tr("Replace it"), static_cast<int>(document::ExportOverwritePolicy::Replace));
    f_->overwrite->addItem(tr("Don't render"), static_cast<int>(document::ExportOverwritePolicy::Refuse));
    addRow(rows, f_->body, tr("If a file is there"), {f_->overwrite})
        ->setToolTip(tr("If a file is already there"));
    f_->editors.push_back(f_->destination);
    f_->editors.push_back(f_->overwrite);

    connect(f_->destination, &QLineEdit::editingFinished, this, [this] {
        const auto text = f_->destination->text();
        if (refreshing_)
            return;
        if (QDir::isAbsolutePath(text))
            commitChosenPath(text);
        else
            commitDestination([&text](document::ExportDestination& d) {
                d.portability = "project-relative";
                d.path = text.toStdString();
            });
    });
    connect(browse, &QAction::triggered, this, [this] { chooseDestination(); });
    connect(f_->tokenHelp, &kit::KButton::clicked, this, [this] { showTokenHelp(); });
    connect(f_->overwrite, &kit::KDropdown::activated, this, [this] {
        commitDestination([this](document::ExportDestination& d) {
            d.overwrite = static_cast<document::ExportOverwritePolicy>(
                f_->overwrite->currentData().toInt());
        });
    });
    connect(f_->replace, &kit::KButton::clicked, this, [this] {
        (void)adapter_.confirmReplace(job_, output_);
    });
    connect(f_->chooseName, &kit::KButton::clicked, this, [this] {
        f_->destination->setFocus(Qt::OtherFocusReason);
        f_->destination->selectAll();
    });
}

void OutputRow::buildQuality(QVBoxLayout& rows) {
    quality_ = properties::makeSection(&rows, f_->body, QStringLiteral("exportQualitySection"),
                                       tr("Quality"),
                                       QStringLiteral("export/output/quality/collapsed"));
    quality_->setBodyPadding(0);
    quality_->setResetEnabled(false);
    if (!QSettings().contains(quality_->persistenceKey()))
        quality_->setCollapsed(true);
    auto* q = quality_->bodyLayout();
    QWidget* parent = quality_->body();

    f_->includeAudio = named(new kit::KSwitch(parent), QStringLiteral("exportIncludeAudio"),
                             tr("Include audio"));
    f_->audioRow = addRow(*q, parent, tr("Include audio"), {f_->includeAudio});
    f_->sampleRate = named(new kit::KDropdown(parent), QStringLiteral("exportSampleRate"),
                           tr("Audio sample rate"));
    fill(*f_->sampleRate,
         {{tr("44.1 kHz"), "44100"}, {tr("48 kHz"), "48000"}, {tr("96 kHz"), "96000"}});
    f_->sampleRateRow = addRow(*q, parent, tr("Sample rate"), {f_->sampleRate});
    f_->profile = named(new kit::KDropdown(parent), QStringLiteral("exportProfile"), tr("Profile"));
    f_->profileRow = addRow(*q, parent, tr("Profile"), {f_->profile});
    f_->bitrate = named(new kit::KValueField(parent), QStringLiteral("exportBitrate"),
                        tr("Quality target"));
    f_->bitrate->setIntegerMode(true);
    f_->bitrate->setIntegerRange(0, 1'000'000);
    f_->bitrate->setUnit(tr("kbps"));
    f_->bitrate->setSpecialValueText(tr("Preset default"));
    f_->bitrate->setFixedWidth(kit::px(kit::Size::DropdownWidthWide));
    f_->bitrateRow = addRow(*q, parent, tr("Quality target"), {f_->bitrate});
    f_->encoder = named(new kit::KDropdown(parent), QStringLiteral("exportEncoder"), tr("Use"));
    f_->encoder->addItem(tr("Automatic"), static_cast<int>(document::ExportEncoderPreference::Automatic));
    f_->encoder->addItem(tr("Software"), static_cast<int>(document::ExportEncoderPreference::Software));
    f_->encoder->addItem(tr("GPU"), static_cast<int>(document::ExportEncoderPreference::Hardware));
    f_->encoderRow = addRow(*q, parent, tr("Use"), {f_->encoder});
    f_->compression = named(new kit::KDropdown(parent), QStringLiteral("exportCompression"),
                            tr("Compression"));
    f_->compressionRow = addRow(*q, parent, tr("Compression"), {f_->compression});
    f_->colourSpace = named(new kit::KDropdown(parent), QStringLiteral("exportColourSpace"),
                            tr("Colour space"));
    f_->colourSpaceRow = addRow(*q, parent, tr("Colour space"), {f_->colourSpace});
    f_->display = named(new kit::KDropdown(parent), QStringLiteral("exportDisplay"), tr("Display"));
    f_->displayRow = addRow(*q, parent, tr("Display"), {f_->display});
    f_->view = named(new kit::KDropdown(parent), QStringLiteral("exportView"), tr("View"));
    f_->viewRow = addRow(*q, parent, tr("View"), {f_->view});
    f_->bakeLook = named(new kit::KSwitch(parent), QStringLiteral("exportBakeLook"),
                         tr("Include the look"));
    f_->bakeLookRow = addRow(*q, parent, tr("Include the look"), {f_->bakeLook});
    f_->useCachedFrames = named(new kit::KSwitch(parent), QStringLiteral("exportUseCachedFrames"),
                                tr("Cached frames"));
    f_->cachedRow = addRow(*q, parent, tr("Cached frames"), {f_->useCachedFrames});

    const auto numberField = [parent](const QString& name, const QString& label) {
        auto* field = named(new kit::KValueField(parent), name, label);
        field->setIntegerMode(true);
        return field;
    };
    f_->numbering = named(new kit::KDropdown(parent), QStringLiteral("exportSequenceStartMode"),
                          tr("File numbering"));
    f_->numbering->addItem(tr("From first frame"), 0);
    f_->numbering->addItem(tr("From a number"), 1);
    f_->startNumber = numberField(QStringLiteral("exportSequenceStart"), tr("Start number"));
    f_->startNumber->setIntegerRange(0, 1'000'000'000);
    f_->padding = numberField(QStringLiteral("exportPadding"), tr("Digits in the number"));
    f_->padding->setIntegerRange(1, 20);
    f_->padding->setUnit(tr("digits"));
    f_->padding->setFixedWidth(kit::px(kit::Size::DropdownWidth));
    f_->startNumber->setFixedWidth(kit::px(kit::Size::DropdownWidth));
    f_->numberingRow = addRow(*q, parent, tr("File numbering"),
                              {f_->numbering, f_->startNumber, f_->padding});
    f_->pattern = named(new kit::KLineEdit(parent), QStringLiteral("exportPattern"), tr("File names"));
    f_->patternRow = addRow(*q, parent, tr("File names"), {f_->pattern});

    for (QWidget* widget : std::initializer_list<QWidget*>{
             f_->includeAudio, f_->sampleRate, f_->profile, f_->bitrate, f_->encoder, f_->compression,
             f_->colourSpace, f_->display, f_->view, f_->bakeLook, f_->useCachedFrames, f_->numbering,
             f_->startNumber, f_->padding, f_->pattern})
        f_->editors.push_back(widget);

    connect(f_->includeAudio, &QAbstractButton::toggled, this, [this](const bool on) {
        commitSettings([on](document::ExportOutputSettings& s) { s.includeAudio = on; });
    });
    connect(f_->sampleRate, &kit::KDropdown::activated, this, [this] {
        commitSettings([this](document::ExportOutputSettings& s) {
            s.audioSampleRate = f_->sampleRate->currentData().toUInt();
        });
    });
    connect(f_->profile, &kit::KDropdown::activated, this, [this] {
        commitSettings([this](document::ExportOutputSettings& s) {
            s.codecProfile = f_->profile->currentData().toString().toStdString();
        });
    });
    connect(f_->bitrate, &kit::KValueField::editFinished, this, [this] {
        commitSettings([this](document::ExportOutputSettings& s) {
            const auto kbps = f_->bitrate->integerValue();
            s.videoBitrateKbps = kbps > 0 ? std::optional<std::uint32_t>(static_cast<std::uint32_t>(kbps))
                                          : std::nullopt;
        });
    });
    connect(f_->encoder, &kit::KDropdown::activated, this, [this] {
        commitSettings([this](document::ExportOutputSettings& s) {
            s.encoder = static_cast<document::ExportEncoderPreference>(f_->encoder->currentData().toInt());
        });
    });
    connect(f_->compression, &kit::KDropdown::activated, this, [this] {
        commitSettings([this](document::ExportOutputSettings& s) {
            s.compression = f_->compression->currentData().toString().toStdString();
        });
    });
    connect(f_->colourSpace, &kit::KDropdown::activated, this, [this] {
        commitSettings([this](document::ExportOutputSettings& s) {
            s.outputColorSpaceId = f_->colourSpace->currentData().toString().toStdString();
        });
    });
    connect(f_->display, &kit::KDropdown::activated, this, [this] {
        commitSettings([this](document::ExportOutputSettings& s) {
            const auto display = f_->display->currentData().toString();
            s.displayName = display.toStdString();
            // A view belongs to its display: keep it only when the new display offers it.
            const auto views = colours_.views(display);
            if (!s.viewName.empty() &&
                std::ranges::find(views, QString::fromStdString(s.viewName)) == views.end())
                s.viewName.clear();
        });
    });
    connect(f_->view, &kit::KDropdown::activated, this, [this] {
        commitSettings([this](document::ExportOutputSettings& s) {
            s.viewName = f_->view->currentData().toString().toStdString();
        });
    });
    connect(f_->bakeLook, &QAbstractButton::toggled, this, [this](const bool on) {
        commitSettings([on](document::ExportOutputSettings& s) { s.bakeLook = on; });
    });
    connect(f_->useCachedFrames, &QAbstractButton::toggled, this, [this](const bool on) {
        commitSettings([on](document::ExportOutputSettings& s) { s.useCachedFrames = on; });
    });
    connect(f_->numbering, &kit::KDropdown::activated, this, [this] {
        commitSettings([this](document::ExportOutputSettings& s) {
            if (f_->numbering->currentData().toInt() == 0)
                s.naming.startFrame = std::nullopt;
            else if (!s.naming.startFrame.has_value())
                s.naming.startFrame = 1;
        });
    });
    connect(f_->startNumber, &kit::KValueField::editFinished, this, [this] {
        commitSettings([this](document::ExportOutputSettings& s) {
            s.naming.startFrame = static_cast<std::uint64_t>(f_->startNumber->integerValue());
        });
    });
    connect(f_->padding, &kit::KValueField::editFinished, this, [this] {
        commitSettings([this](document::ExportOutputSettings& s) {
            s.naming.framePadding = static_cast<std::uint32_t>(f_->padding->integerValue());
        });
    });
    connect(f_->pattern, &QLineEdit::editingFinished, this, [this] {
        commitSettings([this](document::ExportOutputSettings& s) {
            s.naming.pattern = f_->pattern->text().toStdString();
        });
    });
}

void OutputRow::buildAfterRender(QVBoxLayout& rows) {
    f_->importIntoAssets = named(new kit::KSwitch(f_->body), QStringLiteral("exportImportIntoAssets"),
                                 tr("Add to Assets when done"));
    f_->importIntoAssets->setToolTip(
        tr("After a checked render, add the result to this project's Assets"));
    f_->importLabel = reasonLabel(f_->body, QStringLiteral("exportImportLabel"), kit::Color::Muted);
    f_->importLabel->setWordWrap(false);
    f_->importLabel->setTypeRole(kit::TypeRole::Ui);
    f_->importLabel->setText(tr("Add to Assets when done"));
    addRow(rows, f_->body, tr("After render"), {f_->importIntoAssets, f_->importLabel});
    f_->editors.push_back(f_->importIntoAssets);
    connect(f_->importIntoAssets, &QAbstractButton::toggled, this, [this](const bool on) {
        if (!refreshing_)
            report(adapter_.editOutput(job_, output_,
                                       outputEdit([&](auto& edit) { edit.importIntoAssets = on; })));
    });
}

void OutputRow::setExpanded(const bool expanded) {
    const bool changed = expanded != expanded_;
    expanded_ = expanded;
    f_->body->setVisible(expanded);
    f_->disclosure->setIcon(kit::icon(expanded ? kit::IconId::CaretDown : kit::IconId::CaretRight,
                                      kit::IconRole::Chrome));
    f_->disclosure->setAccessibleName(expanded ? tr("Hide output settings") : tr("Show output settings"));
    if (changed)
        Q_EMIT expandedChanged(expanded);
}

const host::ExportJobView* OutputRow::findJob() const {
    const auto snapshot = adapter_.snapshot();
    if (!snapshot)
        return nullptr;
    // The snapshot outlives this call through the adapter's reference to the port's current one.
    for (const auto& job : snapshot->jobs)
        if (job.id == job_)
            return &job;
    return nullptr;
}

const host::ExportOutputView* OutputRow::findOutput() const {
    const auto* job = findJob();
    if (job == nullptr)
        return nullptr;
    for (const auto& output : job->outputs)
        if (output.id == output_)
            return &output;
    return nullptr;
}

void OutputRow::report(const ExportEditResult& result) {
    if (result.accepted) {
        f_->refusal->hide();
        Q_EMIT accepted();
        return;
    }
    f_->refusal->setText(result.reason);
    f_->refusal->setVisible(!result.reason.isEmpty());
    Q_EMIT refused(result.reason);
}

void OutputRow::commitSettings(const std::function<void(document::ExportOutputSettings&)>& change) {
    const auto snapshot = adapter_.snapshot();
    const auto* output = findOutput();
    if (refreshing_ || output == nullptr)
        return;
    auto settings = output->record.settings;
    change(settings);
    if (settings == output->record.settings)
        return;
    report(adapter_.editOutput(job_, output_, outputEdit([&](auto& edit) { edit.settings = settings; })));
}

void OutputRow::commitDestination(const std::function<void(document::ExportDestination&)>& change) {
    const auto snapshot = adapter_.snapshot();
    const auto* output = findOutput();
    if (refreshing_ || output == nullptr)
        return;
    auto destination = output->record.destination;
    change(destination);
    if (destination == output->record.destination)
        return;
    report(adapter_.editOutput(job_, output_,
                               outputEdit([&](auto& edit) { edit.destination = destination; })));
}

void OutputRow::commitChosenPath(const QString& path) {
    const auto snapshot = adapter_.snapshot();
    const auto* output = findOutput();
    if (refreshing_ || output == nullptr)
        return;
    report(adapter_.editOutput(
        job_, output_, outputEdit([&](auto& edit) {
            edit.chosenPath = std::pair{std::filesystem::path(path.toStdString()),
                                        output->record.destination.overwrite};
        })));
}

void OutputRow::chooseDestination() {
    const auto snapshot = adapter_.snapshot();
    const auto* output = findOutput();
    if (output == nullptr)
        return;
    kit::KFileRequest request;
    request.mode = kit::KFileRequest::Mode::Save;
    request.title = tr("Choose where to save");
    request.kind = QStringLiteral("export");
    request.defaultSuffix = suffix_;
    request.startPath = displayPath(output->record.destination);
    if (const auto chosen = kit::chooseFile(*this, request))
        commitChosenPath(*chosen);
}

void OutputRow::applyFormat() {
    const auto snapshot = adapter_.snapshot();
    const auto* output = findOutput();
    if (refreshing_ || output == nullptr)
        return;
    const auto id = f_->format->currentData().toString();
    if (id.isEmpty() || id.toStdString() == output->record.presetId)
        return;
    report(adapter_.applyPreset(job_, output_, id.toStdString()));
}

void OutputRow::showTokenHelp() {
    if (f_->help != nullptr)
        f_->help->close();
    auto* content = new QWidget;
    auto* contentLayout = new QVBoxLayout(content);
    contentLayout->setContentsMargins(kit::px(kit::Spacing::M), kit::px(kit::Spacing::S),
                                      kit::px(kit::Spacing::M), kit::px(kit::Spacing::S));
    contentLayout->setSpacing(kit::px(kit::Spacing::XXS));
    const std::pair<const char*, const char*> tokens[] = {
        {"{name}", "the composition's name"},
        {"{job}", "this job's name"},
        {"{format}", "the format, such as mp4"},
        {"{date}", "today's date"},
        {"<base> and <ext>", "the file name and its extension"},
        {"#### or {frame}", "the frame number, for image sequences"}};
    auto* heading = new kit::KLabel(tr("Name tokens"), content, kit::TypeRole::UiSmallStrong);
    contentLayout->addWidget(heading);
    for (const auto& [token, meaning] : tokens) {
        auto* line = new kit::KLabel(QStringLiteral("%1  %2").arg(
                                         QString::fromLatin1(token), tr(meaning)),
                                     content, kit::TypeRole::UiSmall);
        line->setTone(kit::Color::Muted);
        contentLayout->addWidget(line);
    }
    auto* popup = new kit::KPopupWindow(nullptr, kit::Elevation::Popup);
    popup->setObjectName(QStringLiteral("exportTokenHelpPopup"));
    popup->setAttribute(Qt::WA_DeleteOnClose);
    popup->setContent(content);
    f_->help = popup;
    const int border = std::max(1, static_cast<int>(std::lround(kit::kHairlineWidth)));
    popup->openFor(*f_->tokenHelp,
                   content->sizeHint().grownBy(QMargins(border, border, border, border)));
}

std::unique_ptr<QMenu> OutputRow::buildMenu() {
    std::unique_ptr<QMenu> menu(kit::makeMenu(this));
    menu->setObjectName(QStringLiteral("exportOutputMenu"));
    menu->setToolTipsVisible(true);
    const auto snapshot = adapter_.snapshot();
    const auto* output = findOutput();
    const auto reveal = output != nullptr ? output->commands[host::ExportCommand::RevealOutput]
                                          : host::ExportCommandAvailability{};
    auto* show = menu->addAction(tr("Show in folder"));
    show->setObjectName(QStringLiteral("exportRevealAction"));
    show->setEnabled(reveal.available);
    if (!reveal.available)
        show->setToolTip(qs(reveal.reason));
    connect(show, &QAction::triggered, this, [this] {
        const auto snapshot = adapter_.snapshot();
        const auto* current = findOutput();
        if (current == nullptr)
            return;
        const auto& artifacts = current->verification;
        revealInFileManager(artifacts && !artifacts->artifacts.firstPath.empty()
                                ? artifacts->artifacts.firstPath
                                : current->destination.firstPath,
                            this);
    });
    const auto remove = output != nullptr ? output->commands[host::ExportCommand::RemoveOutput]
                                          : host::ExportCommandAvailability{};
    auto* removeAction = menu->addAction(tr("Remove output"));
    removeAction->setObjectName(QStringLiteral("exportRemoveOutputAction"));
    removeAction->setEnabled(remove.available);
    // The reason is shown as the item's own text when it is the last output, not only as a tip.
    if (!remove.available) {
        removeAction->setToolTip(qs(remove.reason));
        removeAction->setText(tr("Remove output (a job keeps at least one)"));
    }
    connect(removeAction, &QAction::triggered, this,
            [this] { report(adapter_.removeOutput(job_, output_)); });
    return menu;
}

QString OutputRow::destinationSummary() const {
    return static_cast<MiddleElidedLabel*>(f_->destinationSummary)->fullText();
}
QString OutputRow::statusText() const { return f_->status->property("fullText").toString(); }
QString OutputRow::formatText() const { return f_->format->currentText(); }
QString OutputRow::resolvedText() const { return f_->resolved->text(); }
QString OutputRow::replacePromptText() const {
    return f_->replaceBox->isHidden() ? QString{} : f_->replaceText->text();
}
QString OutputRow::reasonsText() const {
    QStringList lines;
    for (const auto* label : {f_->refusal, f_->formatReason, f_->otherReasons, f_->destinationReason,
                              f_->facts})
        if (!label->isHidden() && !label->text().isEmpty() &&
            label->parentWidget() != nullptr && !label->parentWidget()->isHidden())
            lines.push_back(label->text());
    return lines.join(QLatin1Char('\n'));
}

void OutputRow::fillFormats(const host::ExportPresetCatalogueSnapshot* catalogue) {
    f_->format->clearItems();
    if (catalogue == nullptr)
        return;
    struct Group {
        host::ExportPresetCategory category;
        QString title;
    };
    // Video first, then image sequences, audio and the artist's own (ADR 0033, presets).
    const Group groups[] = {{host::ExportPresetCategory::Video, tr("Video")},
                            {host::ExportPresetCategory::VfxSequences, tr("Image sequences")},
                            {host::ExportPresetCategory::Audio, tr("Audio")},
                            {host::ExportPresetCategory::User, tr("My presets")}};
    for (const auto& group : groups) {
        bool header = false;
        for (const auto& preset : catalogue->presets) {
            if (preset.category != group.category)
                continue;
            if (!header) {
                // A group title: shown, never chosen.
                const int index = f_->format->addItem(group.title, QVariant{});
                f_->format->setItemEnabled(index, false);
                header = true;
            }
            auto text = qs(preset.displayName);
            // Unavailable formats stay listed and choosable: the reason shows under the dropdown.
            if (!preset.platformAvailability.available)
                text += tr(" (not available)");
            const int index = f_->format->addItem(text, qs(preset.id));
            f_->format->setItemToolTip(index, qs(preset.description));
        }
    }
    catalogueRevision_ = catalogue->revision;
}

void OutputRow::fillColour(const ExportColourChoices& colours,
                           const document::ExportOutputSettings& settings) {
    colours_ = colours;
    const auto fillChoices = [](kit::KDropdown& dropdown, const QString& first,
                                const std::vector<std::pair<QString, QString>>& choices,
                                const QString& current) {
        const QSignalBlocker block(dropdown);
        dropdown.clearItems();
        dropdown.addItem(first, QString{});
        for (const auto& [label, id] : choices)
            dropdown.addItem(label, id);
        int index = dropdown.findData(current);
        if (index < 0) {
            // A name the configuration no longer offers stays visible rather than turning blank.
            index = dropdown.addItem(current, current);
            dropdown.setItemToolTip(index, QCoreApplication::translate(
                                               "ExportOutputRow", "Not in this project's colour setup"));
        }
        dropdown.setCurrentIndex(index);
    };
    fillChoices(*f_->colourSpace, tr("Working colour space"), colours.colourSpaces,
                qs(settings.outputColorSpaceId));
    const auto display = qs(settings.displayName);
    std::vector<std::pair<QString, QString>> displays;
    for (const auto& name : colours.displays())
        displays.emplace_back(ExportColourChoices::readable(name), name);
    std::vector<std::pair<QString, QString>> views;
    for (const auto& name : colours.views(display))
        views.emplace_back(ExportColourChoices::readable(name), name);
    fillChoices(*f_->display, tr("Project default"), displays, display);
    fillChoices(*f_->view, tr("Project default"), views, qs(settings.viewName));
}

void OutputRow::update(const host::ExportJobView& job, const host::ExportOutputView& output,
                       const host::ExportPresetCatalogueSnapshot* catalogue,
                       const ExportColourChoices& colours) {
    const QSignalBlocker blockEnabled(f_->enabled);
    refreshing_ = true;
    const auto& record = output.record;
    const auto& settings = record.settings;
    const auto& edit = output.commands[host::ExportCommand::EditOutput];

    // Header.
    f_->enabled->setChecked(record.enabled);
    f_->preset->setElidedText(presetText(output, catalogue));
    f_->preset->setToolTip(destinationToolTip(output));
    static_cast<MiddleElidedLabel*>(f_->destinationSummary)->setFullText(displayPath(record.destination));
    const auto status = outputStatus(output, job);
    f_->status->setElidedText(status.text);
    f_->status->setProperty("fullText", status.text);
    f_->status->setToolTip(status.detail.isEmpty() ? status.text : status.detail);
    f_->status->setTone(toneColor(status.tone));

    for (auto* widget : f_->editors) {
        widget->setEnabled(edit.available);
        widget->setToolTip(edit.available ? QString{} : qs(edit.reason));
    }
    f_->importIntoAssets->setToolTip(edit.available
                                         ? tr("After a checked render, add the result to this project's Assets")
                                         : qs(edit.reason));
    f_->destination->setToolTip(edit.available ? destinationToolTip(output) : qs(edit.reason));

    // Format, and why it cannot run here.
    if (catalogue != nullptr && (f_->format->count() == 0 || catalogueRevision_ != catalogue->revision)) {
        const QSignalBlocker block(f_->format);
        fillFormats(catalogue);
    }
    {
        const QSignalBlocker block(f_->format);
        const int index = f_->format->findData(qs(record.presetId));
        f_->format->setCurrentIndex(index);
        f_->format->setMutedValue(index < 0);
    }
    suffix_.clear();
    if (catalogue != nullptr)
        for (const auto& preset : catalogue->presets)
            if (preset.id == record.presetId)
                suffix_ = qs(preset.capabilities.extension);
    const bool blocked = !output.availability.available;
    f_->formatReason->setText(blocked ? qs(output.availability.reason) : QString{});
    f_->remedy->setVisible(blocked && output.availability.remedy != host::ExportRemedy::None);
    struct FitAfter {
        kit::KButton* button;
        ~FitAfter() { button->setFixedWidth(button->sizeHint().width()); }
    } fitRemedy{f_->remedy};
    switch (output.availability.remedy) {
    case host::ExportRemedy::InstallOpenH264:
        f_->remedy->setText(tr("Install H.264 encoder\u2026"));
        break;
    case host::ExportRemedy::LocateOpenH264:
        f_->remedy->setText(tr("Locate encoder\u2026"));
        break;
    case host::ExportRemedy::OpenProjectSettings:
        f_->remedy->setText(tr("Project settings\u2026"));
        break;
    case host::ExportRemedy::ChooseSoftwareEncoder:
        f_->remedy->setText(tr("Use software encoder"));
        break;
    case host::ExportRemedy::None:
        break;
    }
    f_->formatReasonRow->setVisible(blocked);

    // Quality: only what applies to this format.
    const auto format = settings.format;
    const bool image = isImage(format);
    const bool movie = isMovie(format);
    const auto& description = host::exportFormatDescription(format);
    const auto profiles = choicesOf(description.codecProfiles);
    {
        const QSignalBlocker a(f_->profile);
        const QSignalBlocker b(f_->compression);
        const QSignalBlocker c(f_->sampleRate);
        fill(*f_->profile, profiles);
        select(*f_->profile, qs(settings.codecProfile.empty() ? std::string(description.defaultCodecProfile)
                                                              : settings.codecProfile));
        fill(*f_->compression, choicesOf(description.compressions));
        select(*f_->compression, qs(settings.compression.empty() ? std::string(description.defaultCompression)
                                                                 : settings.compression));
        select(*f_->sampleRate, QString::number(settings.audioSampleRate));
    }
    f_->profileRow->setVisible(profiles.size() > 1);
    f_->bitrate->setIntegerValue(settings.videoBitrateKbps.value_or(0));
    f_->bitrateRow->setVisible(isH26x(format));
    f_->encoder->setCurrentIndex(std::max(0, f_->encoder->findData(static_cast<int>(settings.encoder))));
    f_->encoderRow->setVisible(isH26x(format));
    f_->compressionRow->setVisible(!description.compressions.empty());
    const bool displayReferred = format == ExportFormat::Png || format == ExportFormat::Tiff || movie;
    f_->colourSpaceRow->setVisible(format == ExportFormat::OpenExr || format == ExportFormat::Dpx);
    f_->displayRow->setVisible(displayReferred);
    f_->viewRow->setVisible(displayReferred);
    fillColour(colours, settings);
    f_->bakeLook->setChecked(settings.bakeLook);
    f_->bakeLookRow->setVisible(format != ExportFormat::Wav);
    f_->useCachedFrames->setChecked(settings.useCachedFrames);
    f_->cachedRow->setVisible(format != ExportFormat::Wav);
    f_->includeAudio->setChecked(settings.includeAudio);
    f_->audioRow->setVisible(movie);
    f_->sampleRateRow->setVisible(movie || format == ExportFormat::Wav);
    f_->numbering->setCurrentIndex(settings.naming.startFrame.has_value() ? 1 : 0);
    f_->startNumber->setIntegerValue(static_cast<std::int64_t>(settings.naming.startFrame.value_or(0)));
    f_->startNumber->setVisible(settings.naming.startFrame.has_value());
    f_->padding->setIntegerValue(settings.naming.framePadding);
    if (!f_->pattern->hasFocus())
        f_->pattern->setText(qs(settings.naming.pattern));
    f_->numberingRow->setVisible(image);
    f_->patternRow->setVisible(image);

    // Save to, with what the host found about it.
    if (!f_->destination->hasFocus())
        f_->destination->setText(displayPath(record.destination));
    f_->overwrite->setCurrentIndex(
        std::max(0, f_->overwrite->findData(static_cast<int>(record.destination.overwrite))));
    QStringList destinationReasons;
    QStringList otherReasons;
    bool pending = false;
    for (const auto& issue : orderedIssues(output.preflight)) {
        if (issue.code == host::ExportPreflightCode::DestinationCheckPending) {
            pending = true;
        } else if (issue.code == host::ExportPreflightCode::DestinationExists &&
                   issue.severity == host::ExportPreflightSeverity::NeedsConfirmation) {
            // Asked inline, at the top of the output.
        } else if (issue.code == host::ExportPreflightCode::PresetUnavailable && blocked) {
            // Already shown under Format.
        } else if (issue.target == host::ExportPreflightTarget::Destination ||
                   issue.target == host::ExportPreflightTarget::Naming) {
            destinationReasons.push_back(qs(issue.reason));
        } else {
            otherReasons.push_back(qs(issue.reason));
        }
    }
    if (pending)
        destinationReasons.push_back(tr("Checking where it saves\u2026"));
    f_->destinationReason->setText(destinationReasons.join(QLatin1Char(' ')));
    f_->destinationReasonRow->setVisible(!destinationReasons.isEmpty());
    const bool blocking = std::ranges::any_of(output.preflight, [](const auto& issue) {
        return issue.severity == host::ExportPreflightSeverity::Blocking &&
               issue.code != host::ExportPreflightCode::DestinationCheckPending &&
               (issue.target == host::ExportPreflightTarget::Destination ||
                issue.target == host::ExportPreflightTarget::Naming);
    });
    f_->destination->setError(blocking && !destinationReasons.isEmpty() ? destinationReasons.front()
                                                                         : QString{});
    f_->otherReasons->setText(otherReasons.join(QLatin1Char(' ')));
    f_->otherReasonsRow->setVisible(!otherReasons.isEmpty());

    // The resolved result: what will exist, and what it takes.
    QString resolved;
    if (output.destination.fileCount > 1 && !output.destination.lastPath.empty())
        resolved = tr("%1 to %2 (%3 files)")
                       .arg(destinationText(output),
                            QString::fromStdString(output.destination.lastPath.filename().generic_string()))
                       .arg(output.destination.fileCount);
    else if (!output.destination.firstPath.empty())
        resolved = destinationText(output);
    f_->resolved->setText(resolved);
    f_->resolved->setToolTip(resolved);
    f_->resolvedRow->setVisible(!resolved.isEmpty());
    // Only what the host could estimate: an absent size shows nothing, never a guess.
    QStringList facts;
    if (output.estimatedBytes.has_value())
        facts.push_back(tr("About %1").arg(bytesText(*output.estimatedBytes)));
    if (output.freeBytesAtDestination.has_value())
        facts.push_back(tr("%1 free").arg(bytesText(*output.freeBytesAtDestination)));
    f_->facts->setText(facts.join(QStringLiteral(", ")));
    f_->factsRow->setVisible(!facts.isEmpty());

    // The inline question.
    const bool asking = awaitsReplaceConfirmation(output);
    f_->replaceBox->setVisible(asking);
    if (asking) {
        const QFileInfo file(destinationText(output));
        f_->replaceText->setText(
            tr("A file called %1 is already in %2.")
                .arg(file.fileName(), QFileInfo(file.absolutePath()).fileName()));
    }
    const auto& confirm = output.commands[host::ExportCommand::ConfirmReplace];
    f_->replace->setEnabled(confirm.available);
    f_->replace->setToolTip(confirm.available ? QString{} : qs(confirm.reason));

    f_->importIntoAssets->setChecked(record.importIntoAssets);
    QString importText;
    switch (output.import.status) {
    case host::ExportAssetImportStatus::NotRequested:
        break;
    case host::ExportAssetImportStatus::Pending:
        importText = tr("Adding to Assets\u2026");
        break;
    case host::ExportAssetImportStatus::Imported:
        importText = tr("Added to Assets");
        break;
    case host::ExportAssetImportStatus::Refreshed:
        importText = tr("Updated in Assets");
        break;
    case host::ExportAssetImportStatus::Skipped:
        importText = tr("Not added to Assets: %1").arg(qs(output.import.reason));
        break;
    case host::ExportAssetImportStatus::Failed:
        importText = tr("Couldn't add to Assets: %1").arg(qs(output.import.reason));
        break;
    }
    f_->importLabel->setText(importText.isEmpty() ? tr("Add to Assets when done") : importText);
    refreshing_ = false;
}

} // namespace bloom::ui::exportpanel
