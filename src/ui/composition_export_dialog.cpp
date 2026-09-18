#include "composition_export_dialog.hpp"
#include <QDialog>
#include <QFileDialog>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QMessageBox>
#include <QSpinBox>
#include <algorithm>
#include <bloom/media/provider/ffmpeg_manifest.hpp>
#include <bloom/media/provider/openh264_runtime.hpp>
#include <bloom/ui/kit/button.hpp>
#include <bloom/ui/kit/controls.hpp>
#include <bloom/ui/kit/dropdown.hpp>
#include <bloom/ui/kit/switch_control.hpp>
#include <filesystem>
#include <limits>

namespace bloom::ui {
std::optional<CompositionExportRequest> compositionExportDialog(std::uint64_t maximumFrame,
                                                                std::uint32_t sampleRate) {
    QDialog dialog;
    dialog.setWindowTitle(QObject::tr("Export Composition"));
    dialog.setMinimumWidth(kit::px(kit::Size::DialogTextWidth));
    auto* layout = new QFormLayout(&dialog);
    auto* preset = new kit::KDropdown(&dialog);
    preset->setObjectName("compositionExportPreset");
    const std::array<std::pair<const char*, output::OutputPresetV1>, 7> presets{
        {{"ProRes MOV (preview)", output::OutputPresetV1::ProResMovV1},
         {"DNxHR MXF", output::OutputPresetV1::DnxhrMxfV1},
         {"PCM WAV / BWF", output::OutputPresetV1::PcmWavV1},
         {"TIFF sequence", output::OutputPresetV1::TiffRgba16SrgbV1},
         {"PNG sequence", output::OutputPresetV1::PngRgba8SrgbV1},
         {"OpenEXR sequence", output::OutputPresetV1::FlatExrRgba32fLinRec709SceneV1},
         {"H.264 MOV (review)", output::OutputPresetV1::H264MovV1}}};
    for (const auto& [name, id] : presets) {
        preset->addItem(QString::fromUtf8(name), static_cast<int>(id));
        const auto capability = output::outputPresetAvailabilityV1(id);
        if (!capability.available) {
            preset->setItemEnabled(preset->count() - 1, false);
            preset->setItemToolTip(
                preset->count() - 1,
                QString::fromUtf8(capability.reason.data(),
                                  static_cast<qsizetype>(capability.reason.size())));
        }
    }
    auto* profile = new kit::KDropdown(&dialog);
    profile->setObjectName("compositionExportProfile");
    auto* first = new QSpinBox(&dialog);
    auto* last = new QSpinBox(&dialog);
    const auto maximum =
        static_cast<int>(std::min<std::uint64_t>(maximumFrame, std::numeric_limits<int>::max()));
    first->setRange(0, maximum);
    last->setRange(0, maximum);
    last->setValue(maximum);
    auto* startFrame = new QSpinBox(&dialog);
    startFrame->setObjectName("compositionExportStartFrame");
    startFrame->setRange(0, std::numeric_limits<int>::max());
    auto* padding = new QSpinBox(&dialog);
    padding->setObjectName("compositionExportPadding");
    padding->setRange(1, 20);
    padding->setValue(4);
    auto* pattern = new kit::KLineEdit(&dialog);
    pattern->setObjectName("compositionExportPattern");
    pattern->setText("<base>.####.<ext>");
    QObject::connect(first, &QSpinBox::valueChanged, startFrame, &QSpinBox::setValue);
    auto* destination = new kit::KLineEdit(&dialog);
    destination->setObjectName("compositionExportDestination");
    auto* browse = new kit::KButton(QObject::tr("Browse…"), &dialog);
    auto* destinationRow = new QHBoxLayout;
    destinationRow->addWidget(destination);
    destinationRow->addWidget(browse);
    auto* audio = new kit::KCheckBox(&dialog);
    auto* audioLabel = new kit::KLabel(
        QObject::tr("Include audio — source sample rate %1 Hz").arg(sampleRate), &dialog);
    audio->setAccessibleName(audioLabel->text());
    auto* audioRow = new QHBoxLayout;
    audioRow->addWidget(audio);
    audioRow->addWidget(audioLabel);
    audioRow->addStretch();
    audio->setChecked(true);
    auto* note = new kit::KLabel(&dialog);
    note->setWordWrap(true);
    note->setObjectName("compositionExportNote");
    auto* install = new kit::KButton(QObject::tr("Install OpenH264…"), &dialog);
    install->setObjectName("installOpenH264");
    auto* locate = new kit::KButton(QObject::tr("Locate downloaded file…"), &dialog);
    locate->setObjectName("locateOpenH264");
    auto* hardware = new kit::KCheckBox(&dialog);
    hardware->setObjectName("useVaapi");
    hardware->setText(QObject::tr("Use hardware (VA-API)"));
    const auto hasVaapiDevice = [] {
        std::error_code error;
        for (const auto& entry : std::filesystem::directory_iterator("/dev/dri", error))
            if (entry.path().filename().string().starts_with("renderD"))
                return true;
        return false;
    }();
    hardware->setVisible(hasVaapiDevice);
    bool openh264Consent = false;
    bool openh264Installed = media::provider::OpenH264Runtime().verify().installed;
    auto* proceed = new kit::KButton(QObject::tr("Continue"), &dialog);
    proceed->setVariant(kit::KButton::Variant::Primary);
    auto* cancel = new kit::KButton(QObject::tr("Cancel"), &dialog);
    auto* buttons = new QHBoxLayout;
    buttons->addStretch();
    buttons->addWidget(cancel);
    buttons->addWidget(proceed);
    layout->addRow(QObject::tr("Preset"), preset);
    layout->addRow(QObject::tr("Profile"), profile);
    layout->addRow(QObject::tr("First frame"), first);
    layout->addRow(QObject::tr("Last frame"), last);
    layout->addRow(QObject::tr("Destination"), destinationRow);
    layout->addRow(QObject::tr("Start frame"), startFrame);
    layout->addRow(QObject::tr("Padding"), padding);
    layout->addRow(QObject::tr("Pattern"), pattern);
    layout->addRow(audioRow);
    layout->addRow(note);
    layout->addRow(install);
    layout->addRow(locate);
    layout->addRow(hardware);
    layout->addRow(buttons);
    const auto update = [&] {
        const auto id = static_cast<output::OutputPresetV1>(preset->currentData().toInt());
        profile->clearItems();
        if (id == output::OutputPresetV1::ProResMovV1) {
            for (const auto& item : std::array<std::pair<const char*, const char*>, 6>{
                     {{"422 Proxy", "proxy"},
                      {"422 LT", "lt"},
                      {"422", "422"},
                      {"422 HQ", "hq"},
                      {"4444 (alpha)", "4444"},
                      {"4444 XQ (alpha)", "4444xq"}}})
                profile->addItem(QString::fromUtf8(item.first), QString::fromUtf8(item.second));
            profile->setCurrentIndex(3);
        } else if (id == output::OutputPresetV1::DnxhrMxfV1) {
            for (const auto& item :
                 std::array<std::pair<const char*, const char*>, 5>{{{"LB", "dnxhr_lb"},
                                                                     {"SQ", "dnxhr_sq"},
                                                                     {"HQ", "dnxhr_hq"},
                                                                     {"HQX", "dnxhr_hqx"},
                                                                     {"444", "dnxhr_444"}}})
                profile->addItem(QString::fromUtf8(item.first), QString::fromUtf8(item.second));
            profile->setCurrentIndex(2);
        } else if (id == output::OutputPresetV1::PcmWavV1) {
            profile->addItem("16-bit PCM", "pcm_s16le");
            profile->addItem("24-bit PCM", "pcm_s24le");
        } else if (id == output::OutputPresetV1::H264MovV1) {
            profile->addItem("H.264 High", "high");
        } else
            profile->addItem(QObject::tr("Preset default"), "");
        const bool h264 = id == output::OutputPresetV1::H264MovV1;
        const bool video = h264 || id == output::OutputPresetV1::ProResMovV1 ||
                           id == output::OutputPresetV1::DnxhrMxfV1;
        const bool sequence = !video && id != output::OutputPresetV1::PcmWavV1;
        layout->setRowVisible(startFrame, sequence);
        layout->setRowVisible(padding, sequence);
        layout->setRowVisible(pattern, sequence);
        audio->setEnabled(video);
        audio->setVisible(video || id == output::OutputPresetV1::PcmWavV1);
        audioLabel->setVisible(video || id == output::OutputPresetV1::PcmWavV1);
        if (id == output::OutputPresetV1::PcmWavV1)
            audio->setChecked(true);
        note->setText(id == output::OutputPresetV1::ProResMovV1
                          ? QString::fromUtf8(media::provider::kProResExportNote)
                      : h264 && !openh264Installed && !hardware->isChecked()
                          ? QObject::tr("H.264 encoder not installed. Cisco's binary is fetched "
                                        "only after consent. Hardware VA-API is an alternative.")
                      : h264 ? QObject::tr("Review deliverable — not for archival")
                             : QString{});
        note->setVisible(!note->text().isEmpty());
        install->setVisible(h264 && !openh264Installed && !hardware->isChecked());
        locate->setVisible(h264 && !openh264Installed && !hardware->isChecked());
        hardware->setEnabled(h264 && hasVaapiDevice);
    };
    QObject::connect(preset, &kit::KDropdown::currentIndexChanged, &dialog, update);
    update();
    const auto validate = [&] {
        const auto id = static_cast<output::OutputPresetV1>(preset->currentData().toInt());
        const bool h264 = id == output::OutputPresetV1::H264MovV1;
        proceed->setEnabled(
            !destination->text().trimmed().isEmpty() && first->value() <= last->value() &&
            output::outputPresetAvailabilityV1(id).available &&
            (!h264 || hardware->isChecked() || openh264Installed || openh264Consent));
    };
    QObject::connect(install, &kit::KButton::clicked, &dialog, [&] {
        QMessageBox licenseDialog(&dialog);
        licenseDialog.setWindowTitle(QObject::tr("Install OpenH264"));
        licenseDialog.setText(QObject::tr("Cisco OpenH264 2.6.0 is a review encoder."));
        licenseDialog.setInformativeText(
            QObject::tr("Bloom downloads the binary from:\n%1\n\nThe binary is fetched to "
                        "your user data directory, verified by SHA-256, and is not bundled "
                        "with Bloom. The Cisco terms are shown below.")
                .arg(QString::fromStdString(media::provider::OpenH264Runtime::downloadUrl())));
        licenseDialog.setDetailedText(
            QString::fromStdString(media::provider::OpenH264Runtime::binaryLicenseText()) +
            QObject::tr("\nLicense URL: %1")
                .arg(QString::fromStdString(media::provider::OpenH264Runtime::binaryLicenseUrl())));
        licenseDialog.setStandardButtons(QMessageBox::Yes | QMessageBox::No);
        licenseDialog.setDefaultButton(QMessageBox::No);
        const auto consent = licenseDialog.exec();
        if (consent == QMessageBox::Yes) {
            openh264Consent = true;
            note->setText(QObject::tr("OpenH264 will be installed after you continue."));
            note->setVisible(true);
        }
        validate();
    });
    QObject::connect(locate, &kit::KButton::clicked, &dialog, [&] {
        const auto path = QFileDialog::getOpenFileName(&dialog, QObject::tr("Locate OpenH264"));
        if (path.isEmpty())
            return;
        const auto result = media::provider::OpenH264Runtime().locate(path.toStdString());
        if (result.installed) {
            openh264Installed = true;
            note->setText(QObject::tr("OpenH264 2.6.0 verified."));
            update();
        } else {
            QMessageBox::warning(&dialog, QObject::tr("OpenH264 not accepted"),
                                 QString::fromStdString(result.detail));
        }
        validate();
    });
    QObject::connect(hardware, &kit::KCheckBox::toggled, &dialog, [&] {
        update();
        validate();
    });
    QObject::connect(browse, &kit::KButton::clicked, &dialog, [&] {
        const auto name = QFileDialog::getSaveFileName(&dialog, QObject::tr("Export Composition"),
                                                       destination->text());
        if (!name.isEmpty())
            destination->setText(name);
    });
    QObject::connect(destination, &kit::KLineEdit::textChanged, &dialog, validate);
    QObject::connect(first, &QSpinBox::valueChanged, &dialog, validate);
    QObject::connect(last, &QSpinBox::valueChanged, &dialog, validate);
    QObject::connect(preset, &kit::KDropdown::currentIndexChanged, &dialog, validate);
    validate();
    QObject::connect(proceed, &kit::KButton::clicked, &dialog, &QDialog::accept);
    QObject::connect(cancel, &kit::KButton::clicked, &dialog, &QDialog::reject);
    if (dialog.exec() != QDialog::Accepted)
        return std::nullopt;
    CompositionExportRequest request;
    request.range = {std::filesystem::path(destination->text().toStdString()),
                     static_cast<std::uint64_t>(first->value()),
                     static_cast<std::uint64_t>(last->value())};
    request.range.naming = {.startFrame = static_cast<std::uint64_t>(startFrame->value()),
                            .framePadding = static_cast<std::uint32_t>(padding->value()),
                            .namePattern = pattern->text().toStdString()};
    request.preset = static_cast<output::OutputPresetV1>(preset->currentData().toInt());
    request.profile = profile->currentData().toString().toStdString();
    request.audio = audio->isChecked();
    request.hardware = hardware->isChecked();
    request.openh264Consent = openh264Consent;
    request.sampleRate = sampleRate;
    const char* extension = request.preset == output::OutputPresetV1::ProResMovV1        ? ".mov"
                            : request.preset == output::OutputPresetV1::H264MovV1        ? ".mov"
                            : request.preset == output::OutputPresetV1::DnxhrMxfV1       ? ".mxf"
                            : request.preset == output::OutputPresetV1::PcmWavV1         ? ".wav"
                            : request.preset == output::OutputPresetV1::TiffRgba16SrgbV1 ? ".tiff"
                            : request.preset == output::OutputPresetV1::PngRgba8SrgbV1   ? ".png"
                                                                                         : ".exr";
    request.range.destination.replace_extension(extension);
    return request;
}
} // namespace bloom::ui
