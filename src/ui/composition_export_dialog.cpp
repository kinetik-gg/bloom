#include "composition_export_dialog.hpp"

#include "composition_export_capability.hpp"
#include <bloom/ui/h264_encoder_service.hpp>

#include <bloom/media/provider/ffmpeg_manifest.hpp>
#include <bloom/media/provider/openh264_runtime.hpp>
#include <bloom/ui/kit/button.hpp>
#include <bloom/ui/kit/controls.hpp>
#include <bloom/ui/kit/dialog.hpp>
#include <bloom/ui/kit/dialog_prompts.hpp>
#include <bloom/ui/kit/dropdown.hpp>
#include <bloom/ui/kit/file_chooser.hpp>
#include <bloom/ui/kit/progress_bar.hpp>
#include <bloom/ui/kit/switch_control.hpp>
#include <bloom/ui/kit/value_field.hpp>

#include <QFileInfo>
#include <QHBoxLayout>
#include <QSettings>
#include <QSignalBlocker>
#include <QVBoxLayout>
#include <QWidget>

#include <algorithm>
#include <array>
#include <filesystem>
#include <limits>
#include <vector>

namespace bloom::ui {
namespace {

constexpr int kVfxHandoff = 101;
constexpr int kReview = 102;

[[nodiscard]] QString text(const std::string_view view) {
    return QString::fromUtf8(view.data(), static_cast<qsizetype>(view.size()));
}

[[nodiscard]] QWidget* row(QWidget& parent, std::initializer_list<QWidget*> widgets,
                           QWidget* stretch = nullptr) {
    auto* holder = new QWidget(&parent);
    auto* layout = new QHBoxLayout(holder);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(kit::px(kit::Spacing::XS));
    for (auto* widget : widgets)
        layout->addWidget(widget, widget == stretch ? 1 : 0);
    if (stretch == nullptr)
        layout->addStretch(1);
    return holder;
}

[[nodiscard]] kit::KValueField* frameField(QWidget& parent, const QString& name,
                                           const QString& label, const double maximum) {
    auto* field = new kit::KValueField(&parent);
    field->setObjectName(name);
    field->setAccessibleName(label);
    field->setDecimals(0);
    field->setSingleStep(1.0);
    field->setRange(0.0, maximum);
    return field;
}

[[nodiscard]] QString nameFilter(const output::OutputPresetV1 preset) {
    switch (preset) {
    case output::OutputPresetV1::ProResMovV1:
    case output::OutputPresetV1::H264MovV1:
    case output::OutputPresetV1::HevcMovV1:
        return QObject::tr("QuickTime movie (*.mov)");
    case output::OutputPresetV1::H264Mp4V1:
    case output::OutputPresetV1::HevcMp4V1:
        return QObject::tr("MPEG-4 movie (*.mp4)");
    case output::OutputPresetV1::DnxhrMxfV1:
        return QObject::tr("MXF (*.mxf)");
    case output::OutputPresetV1::PcmWavV1:
        return QObject::tr("WAV audio (*.wav)");
    case output::OutputPresetV1::TiffRgba16SrgbV1:
        return QObject::tr("TIFF sequence (*.tiff *.tif)");
    case output::OutputPresetV1::PngRgba8SrgbV1:
        return QObject::tr("PNG sequence (*.png)");
    case output::OutputPresetV1::FlatExrRgba32fLinRec709SceneV1:
        return QObject::tr("OpenEXR sequence (*.exr)");
    }
    return {};
}

using ProfileList = std::vector<std::pair<QString, QString>>;

[[nodiscard]] ProfileList profilesFor(const output::OutputPresetV1 id) {
    switch (id) {
    case output::OutputPresetV1::ProResMovV1:
        return {{QObject::tr("422 Proxy"), QStringLiteral("proxy")},
                {QObject::tr("422 LT"), QStringLiteral("lt")},
                {QObject::tr("422"), QStringLiteral("422")},
                {QObject::tr("422 HQ"), QStringLiteral("hq")},
                {QObject::tr("4444 (alpha)"), QStringLiteral("4444")},
                {QObject::tr("4444 XQ (alpha)"), QStringLiteral("4444xq")}};
    case output::OutputPresetV1::DnxhrMxfV1:
        return {{QObject::tr("LB"), QStringLiteral("dnxhr_lb")},
                {QObject::tr("SQ"), QStringLiteral("dnxhr_sq")},
                {QObject::tr("HQ"), QStringLiteral("dnxhr_hq")},
                {QObject::tr("HQX"), QStringLiteral("dnxhr_hqx")},
                {QObject::tr("444"), QStringLiteral("dnxhr_444")}};
    case output::OutputPresetV1::PcmWavV1:
        return {{QObject::tr("16-bit PCM"), QStringLiteral("pcm_s16le")},
                {QObject::tr("24-bit PCM"), QStringLiteral("pcm_s24le")}};
    case output::OutputPresetV1::H264MovV1:
        return {{QObject::tr("H.264 High"), QStringLiteral("high")}};
    default:
        return {{QObject::tr("Preset default"), QString{}}};
    }
}

} // namespace

QString compositionExportExtension(const output::OutputPresetV1 preset) {
    switch (preset) {
    case output::OutputPresetV1::ProResMovV1:
    case output::OutputPresetV1::H264MovV1:
    case output::OutputPresetV1::HevcMovV1:
        return QStringLiteral(".mov");
    case output::OutputPresetV1::H264Mp4V1:
    case output::OutputPresetV1::HevcMp4V1:
        return QStringLiteral(".mp4");
    case output::OutputPresetV1::DnxhrMxfV1:
        return QStringLiteral(".mxf");
    case output::OutputPresetV1::PcmWavV1:
        return QStringLiteral(".wav");
    case output::OutputPresetV1::TiffRgba16SrgbV1:
        return QStringLiteral(".tiff");
    case output::OutputPresetV1::PngRgba8SrgbV1:
        return QStringLiteral(".png");
    case output::OutputPresetV1::FlatExrRgba32fLinRec709SceneV1:
        return QStringLiteral(".exr");
    }
    return QStringLiteral(".exr");
}

std::optional<CompositionExportRequest>
compositionExportDialog(QWidget& owner, const CompositionExportDialogOptions& options) {
    kit::KDialog dialog(QObject::tr("Export Composition"),
                        QStringLiteral("compositionExportDialog"), owner, Qt::WindowModal);
    auto* form = dialog.addForm();
    auto* preset = new kit::KDropdown(&dialog);
    preset->setObjectName("compositionExportPreset");
    preset->setAccessibleName(QObject::tr("Preset"));
    preset->addItem(QObject::tr("VFX handoff EXR (ACES2065-1, PIZ, 1001, ####)"), kVfxHandoff);
    preset->addItem(QObject::tr("Review H.264 (Rec.709, look on)"), kReview);
    const auto namedAvailability =
        options.colorSettings
            ? detail::namedDeliverableAvailability(*options.colorSettings,
                                                   DeliverablePreset::VfxHandoff)
            : detail::NamedDeliverableAvailability{false,
                                                   "Choose the built-in ACES CG configuration in "
                                                   "Project Settings to use ACES deliverables."};
    const auto reviewAvailability =
        options.colorSettings ? detail::namedDeliverableAvailability(*options.colorSettings,
                                                                     DeliverablePreset::Review)
                              : namedAvailability;
    // Why each preset is unavailable, by index (empty when it is available).
    std::vector<QString> unavailable(2);
    preset->setItemEnabled(0, namedAvailability.available);
    preset->setItemEnabled(1, reviewAvailability.available);
    if (!namedAvailability.available) {
        unavailable[0] = unavailable[1] = text(namedAvailability.reason);
    } else if (!reviewAvailability.available) {
        unavailable[1] = text(reviewAvailability.reason);
    }
    preset->setItemToolTip(0, unavailable[0]);
    preset->setItemToolTip(1, unavailable[1]);
    const auto selectedPreset = [preset] {
        const int value = preset->currentData().toInt();
        return value == kVfxHandoff ? output::OutputPresetV1::FlatExrRgba32fLinRec709SceneV1
               : value == kReview   ? output::OutputPresetV1::H264MovV1
                                    : static_cast<output::OutputPresetV1>(value);
    };
    const std::array<std::pair<QString, output::OutputPresetV1>, 7> presets{
        {{QObject::tr("ProRes MOV (preview)"), output::OutputPresetV1::ProResMovV1},
         {QObject::tr("DNxHR MXF"), output::OutputPresetV1::DnxhrMxfV1},
         {QObject::tr("PCM WAV / BWF"), output::OutputPresetV1::PcmWavV1},
         {QObject::tr("TIFF sequence"), output::OutputPresetV1::TiffRgba16SrgbV1},
         {QObject::tr("PNG sequence"), output::OutputPresetV1::PngRgba8SrgbV1},
         {QObject::tr("OpenEXR sequence"), output::OutputPresetV1::FlatExrRgba32fLinRec709SceneV1},
         {QObject::tr("H.264 MOV (review)"), output::OutputPresetV1::H264MovV1}}};
    for (const auto& [name, id] : presets) {
        preset->addItem(name, static_cast<int>(id));
        const auto capability = output::outputPresetAvailabilityV1(id);
        unavailable.push_back(capability.available ? QString{} : text(capability.reason));
        if (!capability.available) {
            preset->setItemEnabled(preset->count() - 1, false);
            preset->setItemToolTip(preset->count() - 1, unavailable.back());
        }
    }
    (void)form->addRow(QObject::tr("Preset"), preset);
    auto* colorNote = new kit::KLabel(&dialog);
    colorNote->setObjectName("compositionExportColorNote");
    colorNote->setWordWrap(true);
    colorNote->setTone(kit::Color::Muted);
    if (!namedAvailability.available)
        colorNote->setText(text(namedAvailability.reason));
    (void)form->addRow(colorNote);
    form->setRowVisible(colorNote, !namedAvailability.available);

    auto* profile = new kit::KDropdown(&dialog);
    profile->setObjectName("compositionExportProfile");
    profile->setAccessibleName(QObject::tr("Profile"));
    (void)form->addRow(QObject::tr("Profile"), profile);

    const auto maximum = static_cast<double>(std::min<std::uint64_t>(
        options.maximumFrame, static_cast<std::uint64_t>(std::numeric_limits<int>::max())));
    auto* first = frameField(dialog, QStringLiteral("compositionExportFirst"),
                             QObject::tr("First frame"), maximum);
    auto* last = frameField(dialog, QStringLiteral("compositionExportLast"),
                            QObject::tr("Last frame"), maximum);
    last->setValue(maximum);
    (void)form->addRow(QObject::tr("First Frame"), first);
    (void)form->addRow(QObject::tr("Last Frame"), last);

    auto* destination = new kit::KLineEdit(&dialog);
    destination->setObjectName("compositionExportDestination");
    destination->setAccessibleName(QObject::tr("Destination"));
    auto* browse = new kit::KButton(QObject::tr("Browse…"), &dialog);
    browse->setObjectName("compositionExportBrowse");
    auto* destinationRow = row(dialog, {destination, browse}, destination);
    (void)form->addRow(QObject::tr("Destination"), destinationRow);

    auto* startFrame = frameField(dialog, QStringLiteral("compositionExportStartFrame"),
                                  QObject::tr("Start frame"),
                                  static_cast<double>(std::numeric_limits<int>::max()));
    auto* padding = new kit::KValueField(&dialog);
    padding->setObjectName("compositionExportPadding");
    padding->setAccessibleName(QObject::tr("Frame number padding"));
    padding->setDecimals(0);
    padding->setSingleStep(1.0);
    padding->setRange(1.0, 20.0);
    padding->setValue(4.0);
    padding->setUnit(QObject::tr("digits"));
    auto* pattern = new kit::KLineEdit(&dialog);
    pattern->setObjectName("compositionExportPattern");
    pattern->setAccessibleName(QObject::tr("File name pattern"));
    pattern->setFont(kit::font(kit::TypeRole::Value));
    pattern->setText("<base>.####.<ext>");
    (void)form->addRow(QObject::tr("Start Frame"), startFrame);
    (void)form->addRow(QObject::tr("Padding"), padding);
    (void)form->addRow(QObject::tr("Pattern"), pattern);

    auto* outputSpace = new kit::KLineEdit(&dialog);
    outputSpace->setObjectName("compositionExportOutputSpace");
    outputSpace->setAccessibleName(QObject::tr("Output colour space"));
    outputSpace->setPlaceholderText(QObject::tr("Working colour space"));
    auto* compression = new kit::KDropdown(&dialog);
    compression->setObjectName("compositionExportCompression");
    compression->setAccessibleName(QObject::tr("Compression"));
    for (const auto value :
         {output::FlatExrCompressionV1::Zip, output::FlatExrCompressionV1::Piz,
          output::FlatExrCompressionV1::Zips, output::FlatExrCompressionV1::None})
        compression->addItem(text(output::flatExrCompressionNameV1(value)).toUpper(),
                             static_cast<int>(value));
    (void)form->addRow(QObject::tr("Output Colour Space"), outputSpace);
    (void)form->addRow(QObject::tr("Compression"), compression);

    auto* look = new kit::KCheckBox(&dialog);
    look->setObjectName("compositionExportLook");
    look->setChecked(true);
    (void)form->addCheckRow({}, *look, QObject::tr("Bake look-tagged effects"));
    auto* audio = new kit::KCheckBox(&dialog);
    audio->setObjectName("compositionExportAudio");
    audio->setChecked(true);
    (void)form->addCheckRow(
        {}, *audio,
        QObject::tr("Include audio — source sample rate %1 Hz").arg(options.sampleRate));

    auto* note = new kit::KLabel(&dialog);
    note->setWordWrap(true);
    note->setTone(kit::Color::Muted);
    note->setObjectName("compositionExportNote");
    (void)form->addRow(note);

    // H.264: which encoder this platform uses comes from the export provider, and only an
    // OpenH264 platform offers install, consent and Locate.
    const auto support = options.encoder != nullptr ? options.encoder->state().support
                                                    : output::h264EncoderSupportV1();
    auto* hardware = new kit::KCheckBox(&dialog);
    hardware->setObjectName("useVaapi");
    (void)form->addCheckRow({}, *hardware, QObject::tr("Use hardware (VA-API)"));
    auto* install = new kit::KButton(QObject::tr("Install OpenH264…"), &dialog);
    install->setObjectName("installOpenH264");
    auto* locate = new kit::KButton(QObject::tr("Locate Downloaded File…"), &dialog);
    locate->setObjectName("locateOpenH264");
    auto* openh264Row = row(dialog, {install, locate});
    (void)form->addRow(openh264Row);
    auto* locateBar = new kit::KProgressBar(&dialog);
    locateBar->setObjectName("locateOpenH264Progress");
    locateBar->setAccessibleName(QObject::tr("Verifying OpenH264"));
    locateBar->setBusy();
    auto* cancelLocate = new kit::KButton(QObject::tr("Cancel"), &dialog);
    cancelLocate->setObjectName("cancelLocateOpenH264");
    auto* locateRow = row(dialog, {locateBar, cancelLocate}, locateBar);
    (void)form->addRow(locateRow);
    auto* locateMessage = new kit::KLabel(QString{}, &dialog, kit::TypeRole::UiSmall);
    locateMessage->setObjectName("locateOpenH264Message");
    locateMessage->setWordWrap(true);
    (void)form->addRow(locateMessage);
    form->setRowVisible(locateMessage, false);
    dialog.contentLayout()->addStretch(1);

    bool openh264Consent = false;
    const auto runtime = [&options] {
        return options.encoder != nullptr ? options.encoder->state().runtime
                                          : H264EncoderService::Runtime::NotInstalled;
    };
    const auto openh264Installed = [&] {
        return runtime() == H264EncoderService::Runtime::Installed;
    };
    const auto vaapiDevice = [&options] {
        return options.encoder != nullptr && options.encoder->state().vaapiDevice;
    };
    const auto locating = [&options] {
        return options.encoder != nullptr && options.encoder->locating();
    };
    // VA-API is the only H.264 path where Cisco publishes no OpenH264 binary.
    const bool hardwareOnly =
        support.software == output::H264EncoderV1::None && support.hardwareVaapi;

    dialog.addButton(kit::KDialog::Role::Accept, QObject::tr("Continue"))
        ->setObjectName("compositionExportContinue");
    dialog.addButton(kit::KDialog::Role::Reject, QObject::tr("Cancel"));

    const auto update = [&] {
        const auto id = selectedPreset();
        {
            const QSignalBlocker blocked(profile);
            profile->clearItems();
            for (const auto& [name, value] : profilesFor(id))
                profile->addItem(name, value);
            profile->setCurrentIndex(id == output::OutputPresetV1::ProResMovV1  ? 3
                                     : id == output::OutputPresetV1::DnxhrMxfV1 ? 2
                                                                                : 0);
        }
        const bool h264 = id == output::OutputPresetV1::H264MovV1;
        const bool video = h264 || id == output::OutputPresetV1::ProResMovV1 ||
                           id == output::OutputPresetV1::DnxhrMxfV1;
        const bool exr = id == output::OutputPresetV1::FlatExrRgba32fLinRec709SceneV1;
        form->setRowVisible(outputSpace, exr);
        form->setRowVisible(compression, exr);
        look->setEnabled(exr);
        const bool sequence = !video && id != output::OutputPresetV1::PcmWavV1;
        form->setRowVisible(startFrame, sequence);
        form->setRowVisible(padding, sequence);
        form->setRowVisible(pattern, sequence);
        audio->setEnabled(video);
        form->setRowVisible(audio, video || id == output::OutputPresetV1::PcmWavV1);
        if (id == output::OutputPresetV1::PcmWavV1)
            audio->setChecked(true);

        // The hardware switch exists only where the provider can use VA-API and the worker found
        // a render node; where VA-API is the only H.264 path it is on and stays on.
        const bool offerHardware = h264 && support.hardwareVaapi && vaapiDevice();
        form->setRowVisible(hardware, offerHardware);
        if (hardwareOnly && offerHardware) {
            const QSignalBlocker blocker(hardware);
            hardware->setChecked(true);
        }
        hardware->setEnabled(offerHardware && !hardwareOnly);
        const bool needsOpenH264 = h264 && !hardware->isChecked() &&
                                   support.software == output::H264EncoderV1::OpenH264 &&
                                   !openh264Installed();
        form->setRowVisible(openh264Row, needsOpenH264 && !locating() &&
                                             runtime() != H264EncoderService::Runtime::Checking);
        install->setEnabled(!openh264Consent);
        locate->setEnabled(options.encoder != nullptr);
        form->setRowVisible(locateRow, h264 && locating());
        form->setRowVisible(locateMessage, h264 && !locateMessage->text().isEmpty());

        QString message;
        const auto review = QObject::tr("Review deliverable — not for archival.");
        if (id == output::OutputPresetV1::ProResMovV1)
            message = QString::fromUtf8(media::provider::kProResExportNote);
        else if (h264 && hardware->isChecked())
            message = QObject::tr("Encoded on VA-API hardware. %1").arg(review);
        else if (h264 && support.software == output::H264EncoderV1::VideoToolbox)
            message = QObject::tr("Encoded with Apple VideoToolbox. %1").arg(review);
        else if (h264 && support.software == output::H264EncoderV1::OpenH264 && openh264Installed())
            message = QObject::tr("Encoded with Cisco OpenH264 2.6.0. %1").arg(review);
        else if (needsOpenH264 && openh264Consent)
            message = QObject::tr("OpenH264 will be installed after you continue.");
        else if (needsOpenH264 && runtime() == H264EncoderService::Runtime::Checking)
            message = QObject::tr("Checking the H.264 encoder…");
        else if (needsOpenH264)
            message = QObject::tr("The H.264 encoder (Cisco OpenH264) is not installed. Bloom "
                                  "fetches Cisco's binary only after you accept its licence, or "
                                  "you can locate a copy you downloaded.");
        else if (h264)
            message = review;
        note->setText(message);
        form->setRowVisible(note, !message.isEmpty());
        dialog.revalidate();
    };

    // One validator: the first reason shows under its own field and holds Continue.
    dialog.setValidator([&]() -> std::optional<kit::KRefusal> {
        const auto id = selectedPreset();
        if (const auto index = static_cast<std::size_t>(std::max(0, preset->currentIndex()));
            !preset->isItemEnabled(preset->currentIndex()))
            return kit::KRefusal{preset,
                                 index < unavailable.size() ? unavailable[index] : QString{}};
        if (const auto capability = output::outputPresetAvailabilityV1(id); !capability.available)
            return kit::KRefusal{preset, text(capability.reason)};
        if (first->value() > last->value())
            return kit::KRefusal{last,
                                 QObject::tr("The last frame must not come before the first.")};
        const auto path = destination->text().trimmed();
        if (path.isEmpty())
            return kit::KRefusal{destinationRow, QObject::tr("Choose where the export goes.")};
        const auto suffix = QFileInfo(path).suffix().toLower();
        const auto expected = compositionExportExtension(id).mid(1);
        if (!suffix.isEmpty() && suffix != expected &&
            !(expected == QStringLiteral("tiff") && suffix == QStringLiteral("tif")))
            return kit::KRefusal{
                destinationRow,
                QObject::tr("This preset writes .%1 files: end the name in .%1.").arg(expected)};
        if (id != output::OutputPresetV1::H264MovV1 || hardware->isChecked())
            return std::nullopt;
        if (locating())
            return kit::KRefusal{
                locateRow, QObject::tr("Wait while OpenH264 is verified, or cancel it."), true};
        if (support.software == output::H264EncoderV1::None)
            return kit::KRefusal{note, hardwareOnly
                                           ? QObject::tr("No VA-API hardware was found, so H.264 "
                                                         "cannot be encoded on this machine.")
                                           : text(support.softwareReason)};
        if (support.software == output::H264EncoderV1::OpenH264 && !openh264Installed() &&
            !openh264Consent)
            return kit::KRefusal{openh264Row,
                                 runtime() == H264EncoderService::Runtime::Checking
                                     ? QObject::tr("Checking the H.264 encoder…")
                                     : QObject::tr("Install OpenH264 (accepting Cisco's licence), "
                                                   "locate a downloaded copy, or use VA-API "
                                                   "hardware."),
                                 runtime() == H264EncoderService::Runtime::Checking};
        return std::nullopt;
    });

    const auto applyPreset = [&] {
        const bool handoff = preset->currentData().toInt() == kVfxHandoff;
        outputSpace->setText(handoff ? "ACES2065-1" : "");
        compression->setCurrentIndex(handoff ? 1 : 0);
        startFrame->setValue(handoff ? 1001 : first->value());
        padding->setValue(4);
        pattern->setText("<base>.####.<ext>");
        look->setChecked(!handoff);
        update();
    };
    // A destination that still ends in the previous preset's extension follows the new preset,
    // visibly in the field; any other typed extension is the artist's (the validator says what it
    // should be).
    QString previousExtension;
    QObject::connect(preset, &kit::KDropdown::currentIndexChanged, &dialog, [&] {
        const auto next = compositionExportExtension(selectedPreset());
        auto path = destination->text().trimmed();
        if (!path.isEmpty() && path.endsWith(previousExtension, Qt::CaseInsensitive) &&
            next != previousExtension) {
            path.chop(previousExtension.size());
            destination->setText(path + next);
        }
        previousExtension = next;
        applyPreset();
    });
    QSettings settings;
    const auto settingsKey = "export/projects/" + options.projectKey + "/deliverable";
    const auto saved =
        options.projectKey.isEmpty() ? QString{} : settings.value(settingsKey).toString();
    int defaultIndex = saved == "review" && preset->isItemEnabled(1) ? 1
                       : saved == "handoff"                          ? 0
                                                                     : 2;
    // A saved deliverable may name a preset this build cannot offer (for example a video
    // deliverable on a platform without a codec provider), and the fallback default above assumes a
    // preset that may likewise be unavailable. Fall back to the first enabled preset so the dialog
    // never opens on an item the artist cannot use.
    if (!preset->isItemEnabled(defaultIndex)) {
        for (int index = 0; index < preset->count(); ++index) {
            if (preset->isItemEnabled(index)) {
                defaultIndex = index;
                break;
            }
        }
    }
    {
        const QSignalBlocker blocked(preset);
        preset->setCurrentIndex(defaultIndex);
    }
    previousExtension = compositionExportExtension(selectedPreset());
    applyPreset();

    double previousFirst = first->value();
    QObject::connect(first, &kit::KValueField::valueChanged, &dialog, [&](const double value) {
        if (startFrame->value() == previousFirst)
            startFrame->setValue(value);
        previousFirst = value;
    });
    QObject::connect(install, &kit::KButton::clicked, &dialog, [&] {
        // Synchronous: nested inside this modal, and it only records consent.
        kit::KConfirmSpec spec;
        spec.title = QObject::tr("Install OpenH264");
        spec.objectName = QStringLiteral("openh264Consent");
        spec.tone = kit::KPromptTone::Question;
        spec.heading = QObject::tr("Install Cisco OpenH264 2.6.0 for H.264 review exports?");
        spec.detail =
            QObject::tr("After you continue, Bloom downloads the binary from %1, verifies it by "
                        "SHA-256 and keeps it in your user data folder; it is not bundled with "
                        "Bloom. OpenH264 Video Codec provided by Cisco Systems, Inc. Cisco's "
                        "terms follow.")
                .arg(QString::fromStdString(media::provider::OpenH264Runtime::downloadUrl()));
        spec.document =
            QString::fromStdString(media::provider::OpenH264Runtime::binaryLicenseText()) +
            QObject::tr("\nLicence: %1")
                .arg(QString::fromStdString(media::provider::OpenH264Runtime::binaryLicenseUrl()));
        spec.acceptText = QObject::tr("Accept and Install");
        spec.rejectText = QObject::tr("Cancel");
        spec.defaultRole = kit::KDialog::Role::Reject;
        if (kit::confirm(dialog, spec) == kit::KDialog::Role::Accept)
            openh264Consent = true;
        update();
    });
    QObject::connect(locate, &kit::KButton::clicked, &dialog, [&] {
        if (options.encoder == nullptr)
            return;
        const auto path = kit::chooseFile(
            dialog, {.mode = kit::KFileRequest::Mode::Open,
                     .title = QObject::tr("Locate OpenH264"),
                     .kind = QStringLiteral("openh264"),
                     .nameFilters = QObject::tr("OpenH264 library (libopenh264*);;All files (*)"),
                     .defaultSuffix = {},
                     .startPath = {}});
        if (!path)
            return;
        locateMessage->clear();
        // Hashing and copying run as a task; the dialog shows a busy bar and Cancel meanwhile.
        (void)options.encoder->locate(std::filesystem::path(path->toStdString()));
        update();
    });
    QObject::connect(cancelLocate, &kit::KButton::clicked, &dialog, [&] {
        if (options.encoder != nullptr)
            options.encoder->cancelLocate();
    });
    if (options.encoder != nullptr) {
        QObject::connect(options.encoder, &H264EncoderService::stateChanged, &dialog, update);
        QObject::connect(
            options.encoder, &H264EncoderService::locateFinished, &dialog,
            [&](const bool installed, const QString& detail) {
                locateMessage->setTone(installed ? kit::Color::Muted : kit::Color::Error);
                locateMessage->setText(installed
                                           ? QObject::tr("OpenH264 2.6.0 verified.")
                                           : QObject::tr("OpenH264 not accepted: %1").arg(detail));
                update();
            });
        // The probe (render node, installed binary digest) runs on a worker; the dialog updates
        // when it answers.
        options.encoder->refresh();
    }
    QObject::connect(hardware, &kit::KCheckBox::toggled, &dialog, [&] { update(); });
    QObject::connect(browse, &kit::KButton::clicked, &dialog, [&] {
        const auto id = selectedPreset();
        const auto name =
            kit::chooseFile(dialog, {.mode = kit::KFileRequest::Mode::Save,
                                     .title = QObject::tr("Export Composition"),
                                     .kind = QStringLiteral("export-composition"),
                                     .nameFilters = nameFilter(id),
                                     .defaultSuffix = compositionExportExtension(id).mid(1),
                                     .startPath = destination->text()});
        if (name)
            destination->setText(*name);
    });
    // A name typed without an extension gets the preset's, visibly, when the field is left.
    QObject::connect(destination, &QLineEdit::editingFinished, &dialog, [&] {
        const auto path = destination->text().trimmed();
        if (!path.isEmpty())
            destination->setText(
                kit::withDefaultSuffix(path, compositionExportExtension(selectedPreset()).mid(1)));
    });
    dialog.watch(destination);
    dialog.watch(first);
    dialog.watch(last);
    dialog.setInitialFocus(destination);
    update();

    if (dialog.exec() != QDialog::Accepted)
        return std::nullopt;
    if (!preset->isItemEnabled(preset->currentIndex()))
        return std::nullopt;
    CompositionExportRequest request;
    const auto id = selectedPreset();
    const auto path = kit::withDefaultSuffix(destination->text().trimmed(),
                                             compositionExportExtension(id).mid(1));
    request.range = {std::filesystem::path(path.toStdString()),
                     static_cast<std::uint64_t>(first->value()),
                     static_cast<std::uint64_t>(last->value())};
    request.range.naming = {.startFrame = static_cast<std::uint64_t>(startFrame->value()),
                            .framePadding = static_cast<std::uint32_t>(padding->value()),
                            .namePattern = pattern->text().toStdString()};
    request.preset = id;
    const int deliverable = preset->currentData().toInt();
    request.deliverable = deliverable == kVfxHandoff ? DeliverablePreset::VfxHandoff
                          : deliverable == kReview   ? DeliverablePreset::Review
                                                     : DeliverablePreset::Custom;
    request.range.exr = {.outputColorSpaceId = outputSpace->text().trimmed().toStdString(),
                         .compression = static_cast<output::FlatExrCompressionV1>(
                             compression->currentData().toInt())};
    request.range.bypassLookNodes = !look->isChecked();
    if (!options.projectKey.isEmpty() && request.deliverable != DeliverablePreset::Custom) {
        settings.setValue(settingsKey,
                          request.deliverable == DeliverablePreset::Review ? "review" : "handoff");
        // Flush explicitly: on macOS QSettings writes are cached by cfprefsd, so a dialog reopened
        // immediately (as the deliverable-isolation test does) could otherwise read the old value.
        settings.sync();
    }
    request.profile = profile->currentData().toString().toStdString();
    request.audio = audio->isChecked();
    request.hardware = hardware->isChecked();
    request.openh264Consent = openh264Consent;
    request.sampleRate = options.sampleRate;
    return request;
}

} // namespace bloom::ui
