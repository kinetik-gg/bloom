#include "composition_export_dialog.hpp"
#include <QDialog>
#include <QFileDialog>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QSpinBox>
#include <algorithm>
#include <bloom/media/provider/ffmpeg_manifest.hpp>
#include <bloom/ui/kit/button.hpp>
#include <bloom/ui/kit/controls.hpp>
#include <bloom/ui/kit/dropdown.hpp>
#include <bloom/ui/kit/switch_control.hpp>
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
    const std::array<std::pair<const char*, output::OutputPresetV1>, 6> presets{
        {{"ProRes MOV (preview)", output::OutputPresetV1::ProResMovV1},
         {"DNxHR MXF", output::OutputPresetV1::DnxhrMxfV1},
         {"PCM WAV / BWF", output::OutputPresetV1::PcmWavV1},
         {"TIFF sequence", output::OutputPresetV1::TiffRgba16SrgbV1},
         {"PNG sequence", output::OutputPresetV1::PngRgba8SrgbV1},
         {"OpenEXR sequence", output::OutputPresetV1::FlatExrRgba32fLinRec709SceneV1}}};
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
    layout->addRow(audioRow);
    layout->addRow(note);
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
        } else
            profile->addItem(QObject::tr("Preset default"), "");
        const bool video =
            id == output::OutputPresetV1::ProResMovV1 || id == output::OutputPresetV1::DnxhrMxfV1;
        audio->setEnabled(video);
        audio->setVisible(video || id == output::OutputPresetV1::PcmWavV1);
        audioLabel->setVisible(video || id == output::OutputPresetV1::PcmWavV1);
        if (id == output::OutputPresetV1::PcmWavV1)
            audio->setChecked(true);
        note->setText(id == output::OutputPresetV1::ProResMovV1
                          ? QString::fromUtf8(media::provider::kProResExportNote)
                          : QString{});
        note->setVisible(!note->text().isEmpty());
    };
    QObject::connect(preset, &kit::KDropdown::currentIndexChanged, &dialog, update);
    update();
    QObject::connect(browse, &kit::KButton::clicked, &dialog, [&] {
        const auto name = QFileDialog::getSaveFileName(&dialog, QObject::tr("Export Composition"),
                                                       destination->text());
        if (!name.isEmpty())
            destination->setText(name);
    });
    const auto validate = [&] {
        proceed->setEnabled(!destination->text().trimmed().isEmpty() &&
                            first->value() <= last->value() &&
                            output::outputPresetAvailabilityV1(
                                static_cast<output::OutputPresetV1>(preset->currentData().toInt()))
                                .available);
    };
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
    request.preset = static_cast<output::OutputPresetV1>(preset->currentData().toInt());
    request.profile = profile->currentData().toString().toStdString();
    request.audio = audio->isChecked();
    request.sampleRate = sampleRate;
    const char* extension = request.preset == output::OutputPresetV1::ProResMovV1        ? ".mov"
                            : request.preset == output::OutputPresetV1::DnxhrMxfV1       ? ".mxf"
                            : request.preset == output::OutputPresetV1::PcmWavV1         ? ".wav"
                            : request.preset == output::OutputPresetV1::TiffRgba16SrgbV1 ? ".tiff"
                            : request.preset == output::OutputPresetV1::PngRgba8SrgbV1   ? ".png"
                                                                                         : ".exr";
    request.range.destination.replace_extension(extension);
    return request;
}
} // namespace bloom::ui
