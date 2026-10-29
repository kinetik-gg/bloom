#include <bloom/ui/frame_export_controller.hpp>

#include "composition_editor_support.hpp"

#include <bloom/ui/composition_session.hpp>
#include <bloom/ui/kit/button.hpp>
#include <bloom/ui/kit/controls.hpp>
#include <bloom/ui/kit/dialog_prompts.hpp>
#include <bloom/ui/kit/file_chooser.hpp>

#include <QAbstractButton>
#include <QDir>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QPointer>
#include <QWidget>

#include <algorithm>
#include <limits>
#include <memory>

// The export controller's default dialogs (lane C): the Export Frame destination, the one Export
// Frame Range dialog and the asynchronous approval prompt, all kit dialogs over the dialog owner.
// Tests replace them through the seam setters.

namespace bloom::ui {

void FrameExportController::installDefaultDialogs() {
    // All frame presets are listed; TIFF uses the supervised worker. The chosen extension -- not
    // the selected filter entry -- is what actually selects the preset (presetForDestination()
    // above), so a path typed by hand behaves identically to one picked through a filter. OpenEXR
    // stays first so the dialog's default selection, and therefore every existing artist habit, is
    // unchanged. kit::chooseFile() adds the mounted network shares and remembers the folder.
    destinationProvider_ = [this]() -> std::optional<std::filesystem::path> {
        if (!dialogOwner_)
            return std::nullopt;
        const auto chosen = kit::chooseFile(
            *dialogOwner_, {.mode = kit::KFileRequest::Mode::Save,
                            .title = tr("Export Frame"),
                            .kind = QStringLiteral("export-frame"),
                            .nameFilters = tr("OpenEXR (*.exr);;PNG (*.png);;TIFF (*.tif *.tiff)"),
                            .defaultSuffix = {},
                            .startPath = {}});
        if (!chosen)
            return std::nullopt;
        return std::filesystem::path(chosen->toStdString());
    };

    // The range seam: ONE dialog holding the sequence's destination and its inclusive first/last
    // frame. PNG-only, because a frame range is a PNG sequence (task S5, item 3a) -- and the
    // extension, not the filter, is still what selects the preset through the one closed mapping
    // presetForDestination() owns. Synchronous: the seam returns the request to requestRange-
    // Export().
    rangeProvider_ = [this]() -> std::optional<FrameExportRangeRequest> {
        if (!dialogOwner_)
            return std::nullopt;
        const auto context = frameContextFor(session_);
        if (!context.has_value())
            return std::nullopt;
        const auto maximum = static_cast<std::int64_t>(
            std::min<std::uint64_t>(context->maxFrameIndexValue,
                                    static_cast<std::uint64_t>(std::numeric_limits<int>::max())));
        auto destination = std::make_shared<QPointer<kit::KLineEdit>>();
        auto destinationRow = std::make_shared<QPointer<QWidget>>();
        kit::KRangePromptSpec spec;
        spec.title = tr("Export Frame Range");
        spec.objectName = QStringLiteral("exportFrameRangeDialog");
        spec.firstLabel = tr("First Frame");
        spec.lastLabel = tr("Last Frame");
        spec.acceptText = tr("Export");
        spec.minimum = 0;
        spec.maximum = maximum;
        spec.first = 0;
        spec.last = maximum;
        spec.orderReason = tr("The last frame must not come before the first.");
        spec.addRows = [destination, destinationRow](kit::KDialog& dialog, kit::KForm& form) {
            auto* field = new kit::KLineEdit(&dialog);
            field->setObjectName(QStringLiteral("exportFrameRangeDestination"));
            field->setAccessibleName(tr("Destination"));
            field->setPlaceholderText(tr("Where the PNG sequence goes, e.g. shot.png"));
            auto* browse = new kit::KButton(tr("Browse…"), &dialog);
            browse->setObjectName(QStringLiteral("exportFrameRangeBrowse"));
            auto* row = new QWidget(&dialog);
            auto* layout = new QHBoxLayout(row);
            layout->setContentsMargins(0, 0, 0, 0);
            layout->setSpacing(kit::px(kit::Spacing::XS));
            layout->addWidget(field, 1);
            layout->addWidget(browse);
            (void)form.addRow(tr("Destination"), row);
            *destination = field;
            *destinationRow = row;
            QObject::connect(browse, &QAbstractButton::clicked, &dialog, [&dialog, field] {
                if (const auto chosen =
                        kit::chooseFile(dialog, {.mode = kit::KFileRequest::Mode::Save,
                                                 .title = tr("Export Frame Range"),
                                                 .kind = QStringLiteral("export-frame-range"),
                                                 .nameFilters = tr("PNG sequence (*.png)"),
                                                 .defaultSuffix = QStringLiteral("png"),
                                                 .startPath = field->text()}))
                    field->setText(*chosen);
            });
            dialog.watch(field);
            dialog.setInitialFocus(field);
        };
        spec.validate = [destination, destinationRow]() -> std::optional<kit::KRefusal> {
            const auto text = *destination ? (*destination)->text().trimmed() : QString{};
            if (text.isEmpty())
                return kit::KRefusal{*destinationRow, tr("Choose where the PNG sequence goes.")};
            const auto suffix = QFileInfo(text).suffix();
            if (!suffix.isEmpty() &&
                suffix.compare(QStringLiteral("png"), Qt::CaseInsensitive) != 0)
                return kit::KRefusal{*destinationRow,
                                     tr("A frame range is a PNG sequence: end the name in .png.")};
            return std::nullopt;
        };
        const std::unique_ptr<kit::KDialog> dialog(kit::buildRangePrompt(*dialogOwner_, spec));
        if (dialog->exec() != QDialog::Accepted || !*destination)
            return std::nullopt;
        const auto range = kit::rangeOf(*dialog);
        // A name typed without a suffix gets .png; a typed suffix is never replaced.
        const auto path =
            kit::withDefaultSuffix((*destination)->text().trimmed(), QStringLiteral("png"));
        return FrameExportRangeRequest{.destination = std::filesystem::path(path.toStdString()),
                                       .firstFrame = static_cast<std::uint64_t>(range.first),
                                       .lastFrame = static_cast<std::uint64_t>(range.last)};
    };

    // Asynchronous and window-modal: the poll that reaches approval returns at once, and the
    // answer continues the export later. Human-readable text; the preset id and digest are in
    // the detail's tooltip.
    approvalRequester_ = [this](const FrameExportApprovalPrompt& prompt,
                                std::function<void(FrameExportApprovalDecision)> reply) {
        if (!dialogOwner_) {
            reply(FrameExportApprovalDecision::Cancel);
            return;
        }
        QStringList lines;
        QString format =
            tr("%1 × %2 · %3")
                .arg(prompt.width)
                .arg(prompt.height)
                .arg(prompt.presetLabel.isEmpty() ? prompt.presetName : prompt.presetLabel);
        if (!prompt.profile.isEmpty())
            format += QStringLiteral(" · ") + prompt.profile;
        lines << format;
        lines << tr("Kept exactly: %1 of %2 output properties")
                     .arg(prompt.facets.exactFacetCount)
                     .arg(prompt.facets.exactFacetCount + prompt.facets.nonExactFacetCount);
        if (!prompt.facets.nonExactFacetNames.isEmpty())
            lines << tr("Changed: %1").arg(prompt.facets.nonExactFacetNames.join(", "));
        if (!prompt.implementationNote.isEmpty())
            lines << prompt.implementationNote;
        lines << tr("In %1").arg(QDir::toNativeSeparators(
            QString::fromStdString(prompt.destination.parent_path().string())));
        kit::KConfirmSpec spec;
        spec.title = tr("Approve Export");
        spec.objectName = QStringLiteral("exportApproval");
        spec.tone = kit::KPromptTone::Question;
        spec.heading =
            tr("Export “%1”?").arg(QString::fromStdString(prompt.destination.filename().string()));
        spec.detail = lines.join(QLatin1Char('\n'));
        spec.detailToolTip =
            tr("Preset %1 · approval digest %2…").arg(prompt.presetName, prompt.digestShortForm);
        spec.acceptText = tr("Export");
        spec.rejectText = tr("Cancel");
        spec.defaultRole = kit::KDialog::Role::Accept;
        auto* dialog = kit::buildConfirm(*dialogOwner_, spec);
        approvalDialog_ = dialog;
        dialog->open([reply = std::move(reply)](const kit::KDialog::Role role) {
            reply(role == kit::KDialog::Role::Accept ? FrameExportApprovalDecision::Export
                                                     : FrameExportApprovalDecision::Cancel);
        });
    };
}

void FrameExportController::askApproval(
    const FrameExportApprovalPrompt& prompt,
    std::function<void(FrameExportApprovalDecision)> onDecision) {
    const auto ticket = ++approvalTicket_;
    if (!approvalRequester_) {
        ++approvalTicket_;
        onDecision(FrameExportApprovalDecision::Cancel);
        return;
    }
    QPointer<FrameExportController> self(this);
    approvalRequester_(prompt, [self, ticket, onDecision = std::move(onDecision)](
                                   const FrameExportApprovalDecision decision) {
        // A ticket answers once; a withdrawn or superseded question answers nothing.
        if (!self || self->approvalTicket_ != ticket)
            return;
        ++self->approvalTicket_;
        self->approvalDialog_.clear();
        onDecision(decision);
    });
}

void FrameExportController::withdrawApproval() {
    ++approvalTicket_;
    if (approvalDialog_) {
        auto* dialog = approvalDialog_.data();
        approvalDialog_.clear();
        dialog->reject();
    }
}

} // namespace bloom::ui
