#include <bloom/ui/h264_encoder_flow.hpp>

#include <bloom/media/provider/openh264_runtime.hpp>
#include <bloom/ui/h264_encoder_service.hpp>
#include <bloom/ui/kit/button.hpp>
#include <bloom/ui/kit/controls.hpp>
#include <bloom/ui/kit/dialog.hpp>
#include <bloom/ui/kit/dialog_prompts.hpp>
#include <bloom/ui/kit/file_chooser.hpp>
#include <bloom/ui/kit/progress_bar.hpp>

#include <QVBoxLayout>

#include <filesystem>

namespace bloom::ui {

bool confirmOpenH264Install(QWidget& owner) {
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
    spec.document = QString::fromStdString(media::provider::OpenH264Runtime::binaryLicenseText()) +
                    QObject::tr("\nLicence: %1")
                        .arg(QString::fromStdString(
                            media::provider::OpenH264Runtime::binaryLicenseUrl()));
    spec.acceptText = QObject::tr("Accept and Install");
    spec.rejectText = QObject::tr("Cancel");
    spec.defaultRole = kit::KDialog::Role::Reject;
    return kit::confirm(owner, spec) == kit::KDialog::Role::Accept;
}

std::optional<QString> chooseOpenH264File(QWidget& owner) {
    return kit::chooseFile(
        owner, {.mode = kit::KFileRequest::Mode::Open,
                .title = QObject::tr("Locate OpenH264"),
                .kind = QStringLiteral("openh264"),
                .nameFilters = QObject::tr("OpenH264 library (libopenh264*);;All files (*)"),
                .defaultSuffix = {},
                .startPath = {}});
}

kit::KDialog* buildH264EncoderDialog(QWidget& owner, H264EncoderService& service) {
    auto* dialog = new kit::KDialog(QObject::tr("H.264 Encoder"),
                                    QStringLiteral("h264EncoderDialog"), owner);
    auto* status = new kit::KLabel(dialog);
    status->setObjectName(QStringLiteral("h264EncoderStatus"));
    status->setWordWrap(true);
    auto* message = new kit::KLabel(QString{}, dialog, kit::TypeRole::UiSmall);
    message->setObjectName(QStringLiteral("h264EncoderMessage"));
    message->setWordWrap(true);
    auto* bar = new kit::KProgressBar(dialog);
    bar->setObjectName(QStringLiteral("h264EncoderProgress"));
    bar->setAccessibleName(QObject::tr("Working on the H.264 encoder"));
    bar->setBusy();
    auto* install = new kit::KButton(QObject::tr("Install OpenH264…"), dialog);
    install->setObjectName(QStringLiteral("installOpenH264"));
    auto* locate = new kit::KButton(QObject::tr("Locate Downloaded File…"), dialog);
    locate->setObjectName(QStringLiteral("locateOpenH264"));
    auto* cancel = new kit::KButton(QObject::tr("Cancel"), dialog);
    cancel->setObjectName(QStringLiteral("cancelLocateOpenH264"));
    auto* buttons = new QWidget(dialog);
    auto* buttonLayout = new QHBoxLayout(buttons);
    buttonLayout->setContentsMargins(0, 0, 0, 0);
    buttonLayout->addWidget(install);
    buttonLayout->addWidget(locate);
    buttonLayout->addWidget(cancel);
    buttonLayout->addStretch(1);
    auto* layout = dialog->contentLayout();
    layout->addWidget(status);
    layout->addWidget(bar);
    layout->addWidget(buttons);
    layout->addWidget(message);
    layout->addStretch(1);
    dialog->addButton(kit::KDialog::Role::Accept, QObject::tr("Done"));

    const auto update = [&service, status, bar, install, locate, cancel, buttons] {
        const auto& state = service.state();
        const bool busy = service.locating();
        const bool installed = state.runtime == H264EncoderService::Runtime::Installed ||
                               state.runtime == H264EncoderService::Runtime::NotNeeded;
        status->setText(installed ? QObject::tr("The H.264 encoder is installed.")
                        : state.runtime == H264EncoderService::Runtime::Checking
                            ? QObject::tr("Checking the H.264 encoder…")
                            : QObject::tr("The H.264 encoder (Cisco OpenH264) is not installed. "
                                          "Bloom fetches Cisco's binary only after you accept "
                                          "its licence, or you can locate a copy you downloaded."));
        bar->setVisible(busy);
        cancel->setVisible(busy);
        buttons->setVisible(true);
        install->setVisible(!installed);
        locate->setVisible(!installed);
        install->setEnabled(!busy);
        locate->setEnabled(!busy);
    };
    QObject::connect(install, &kit::KButton::clicked, dialog, [dialog, &service, message] {
        if (!confirmOpenH264Install(*dialog))
            return;
        message->clear();
        (void)service.install();
    });
    QObject::connect(locate, &kit::KButton::clicked, dialog, [dialog, &service, message] {
        const auto path = chooseOpenH264File(*dialog);
        if (!path)
            return;
        message->clear();
        (void)service.locate(std::filesystem::path(path->toStdString()));
    });
    QObject::connect(cancel, &kit::KButton::clicked, dialog, [&service] { service.cancelLocate(); });
    QObject::connect(&service, &H264EncoderService::stateChanged, dialog, update);
    QObject::connect(&service, &H264EncoderService::locateFinished, dialog,
                     [message](const bool installed, const QString& detail) {
                         message->setTone(installed ? kit::Color::Muted : kit::Color::Error);
                         message->setText(installed ? QObject::tr("OpenH264 2.6.0 verified.")
                                                    : QObject::tr("OpenH264 not accepted: %1")
                                                          .arg(detail));
                     });
    service.refresh();
    update();
    return dialog;
}

void showH264EncoderFlow(QWidget& owner, H264EncoderService& service) {
    buildH264EncoderDialog(owner, service)->open();
}

} // namespace bloom::ui
