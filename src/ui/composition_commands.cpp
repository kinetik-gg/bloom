#include <bloom/ui/composition_commands.hpp>
#include <bloom/ui/kit/color_chip.hpp>
#include <bloom/ui/kit/controls.hpp>
#include <bloom/ui/kit/dropdown.hpp>
#include <memory>

#include <bloom/commands/operations.hpp>
#include <bloom/commands/transaction.hpp>
#include <bloom/core/rational_time.hpp>
#include <bloom/document/composition_settings.hpp>
#include <bloom/document/persisted_text.hpp>
#include <bloom/document/project.hpp>
#include <bloom/ui/composition_session.hpp>

#include <QDialog>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QWidget>

#include <cmath>
#include <cstdint>
#include <limits>
#include <optional>

namespace bloom::ui {
namespace {

[[nodiscard]] document::CompositionId lowestCompositionId(const document::Project& project) {
    const auto compositions = project.compositions();
    if (compositions.empty()) {
        return {};
    }
    auto lowest = compositions.front().id();
    for (const auto& composition : compositions) {
        if (composition.id().value() < lowest.value()) {
            lowest = composition.id();
        }
    }
    return lowest;
}

// The unit the Duration field is entered in. Frames is the default; Seconds is offered beside it so
// the artist can author a duration however they think about it. The two are the same value seen in
// different units: switching converts the field so the resulting composition duration is unchanged
// (up to frame precision), it never silently changes the length.
enum class DurationUnit : std::uint8_t { Frames, Seconds };

// Seconds are carried to microseconds so a fractional entry is exact and safe: 0.001 s is 1000/1e6,
// which reduces, and the value round-trips through toSeconds() without a float remainder.
constexpr std::int64_t kSecondsDenominator = 1'000'000;

constexpr std::int64_t kMaxFrames = 1'000'000;

// The exact composition duration a field value names in `unit` at `rate`, or nullopt when the value
// is not representable (nonpositive, or the frame product overflows a RationalTime numerator).
// Frames are integer by construction; seconds are converted at microsecond precision.
[[nodiscard]] std::optional<core::RationalTime>
durationForValue(const double value, const DurationUnit unit, const document::FrameRate rate) {
    if (unit == DurationUnit::Frames) {
        if (!std::isfinite(value) || value < 1.0 || value > static_cast<double>(kMaxFrames)) {
            return std::nullopt;
        }
        const auto frames = static_cast<std::int64_t>(std::llround(value));
        const auto rateDenominator = static_cast<std::int64_t>(rate.denominator());
        const auto rateNumerator = static_cast<std::int64_t>(rate.numerator());
        if (rateNumerator <= 0 || rateDenominator <= 0 ||
            frames > std::numeric_limits<std::int64_t>::max() / rateDenominator) {
            return std::nullopt;
        }
        return core::RationalTime::create(frames * rateDenominator, rateNumerator);
    }
    if (!std::isfinite(value) || value <= 0.0) {
        return std::nullopt;
    }
    const auto microseconds = std::llround(value * static_cast<double>(kSecondsDenominator));
    if (microseconds < 1) {
        return std::nullopt;
    }
    return core::RationalTime::create(microseconds, kSecondsDenominator);
}

// The field value that displays `duration` in `unit` at `rate`. Frames round to the nearest whole
// frame (documented frame precision) and clamp to at least one; seconds are returned exactly and
// the spin box's own decimal precision rounds the display.
[[nodiscard]] double displayValueForDuration(const core::RationalTime duration,
                                             const DurationUnit unit,
                                             const document::FrameRate rate) {
    const double seconds = duration.toSeconds();
    if (unit == DurationUnit::Seconds) {
        return seconds;
    }
    const auto rateNumerator = static_cast<double>(rate.numerator());
    const auto rateDenominator = static_cast<double>(rate.denominator());
    if (rateNumerator <= 0.0 || rateDenominator <= 0.0) {
        return 1.0;
    }
    const auto frames = std::llround(seconds * rateNumerator / rateDenominator);
    if (frames < 1) {
        return 1.0;
    }
    if (frames > kMaxFrames) {
        return static_cast<double>(kMaxFrames);
    }
    return static_cast<double>(frames);
}

struct NewCompositionFields final {
    QLineEdit* name = nullptr;
    QSpinBox* width = nullptr;
    QSpinBox* height = nullptr;
    QSpinBox* frameRate = nullptr;
    QDoubleSpinBox* duration = nullptr;
    kit::KDropdown* durationUnit = nullptr;
    kit::KColorChip* background = nullptr;
    QLabel* error = nullptr;
    QDialogButtonBox* buttons = nullptr;
};

} // namespace

std::optional<document::CompositionId> showNewCompositionDialog(CompositionSession& session,
                                                                QWidget* parent) {
    QDialog dialog(parent);
    dialog.setObjectName(QStringLiteral("newCompositionDialog"));
    dialog.setWindowTitle(QObject::tr("New Composition"));

    auto* form = new QFormLayout(&dialog);
    NewCompositionFields fields;
    fields.name = new kit::KLineEdit(&dialog);
    fields.name->setObjectName(QStringLiteral("assetsNameField"));
    fields.name->setText(
        QObject::tr("Composition %1").arg(session.snapshot().project().compositions().size() + 1));
    form->addRow(QObject::tr("Name"), fields.name);

    const auto* current = session.composition();
    const auto currentFormat =
        current == nullptr ? document::CompositionFormat{} : current->format();
    fields.width = new QSpinBox(&dialog);
    fields.width->setObjectName(QStringLiteral("assetsWidthField"));
    fields.width->setRange(1, static_cast<int>(document::CompositionFormat::kMaximumDimension));
    fields.width->setValue(static_cast<int>(currentFormat.width()));
    form->addRow(QObject::tr("Width"), fields.width);
    fields.height = new QSpinBox(&dialog);
    fields.height->setObjectName(QStringLiteral("assetsHeightField"));
    fields.height->setRange(1, static_cast<int>(document::CompositionFormat::kMaximumDimension));
    fields.height->setValue(static_cast<int>(currentFormat.height()));
    form->addRow(QObject::tr("Height"), fields.height);
    fields.frameRate = new QSpinBox(&dialog);
    fields.frameRate->setObjectName(QStringLiteral("assetsFrameRateField"));
    fields.frameRate->setRange(1, 1000);
    fields.frameRate->setValue(static_cast<int>(currentFormat.frameRate().numerator() /
                                                currentFormat.frameRate().denominator()));
    form->addRow(QObject::tr("Frame rate (fps)"), fields.frameRate);

    // Duration + unit selector on one row (label "Duration", numeric field, unit dropdown). The
    // default is the current composition's own duration when one exists, otherwise ten seconds --
    // which the Frames default shows as 10 * fps frames, never ten frames by accident.
    const auto initialDuration =
        current == nullptr ? core::RationalTime::fromInteger(10) : current->duration();
    fields.duration = new QDoubleSpinBox(&dialog);
    fields.duration->setObjectName(QStringLiteral("assetsDurationField"));
    fields.duration->setKeyboardTracking(false);
    fields.durationUnit = new kit::KDropdown(&dialog);
    fields.durationUnit->setObjectName(QStringLiteral("assetsDurationUnitDropdown"));
    fields.durationUnit->addItem(QObject::tr("Frames"), QStringLiteral("frames"));
    fields.durationUnit->addItem(QObject::tr("Seconds"), QStringLiteral("seconds"));
    fields.durationUnit->setCurrentIndex(0);
    auto* durationRow = new QWidget(&dialog);
    auto* durationLayout = new QHBoxLayout(durationRow);
    durationLayout->setContentsMargins(0, 0, 0, 0);
    durationLayout->addWidget(fields.duration, 1);
    durationLayout->addWidget(fields.durationUnit);
    form->addRow(QObject::tr("Duration"), durationRow);

    auto* background = new kit::KColorChip(&dialog);
    background->setObjectName(QStringLiteral("newCompositionBackgroundColor"));
    background->setAccessibleName(QObject::tr("Background Colour"));
    background->setColor({0.0F, 0.0F, 0.0F, 1.0F});
    form->addRow(QObject::tr("Background Colour"), background);
    fields.background = background;
    fields.error = new kit::KLabel(&dialog);
    fields.error->setObjectName(QStringLiteral("newCompositionErrorLabel"));
    fields.error->setWordWrap(true);
    fields.error->hide();
    form->addRow(fields.error);

    fields.buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    fields.buttons->setObjectName(QStringLiteral("assetsDialogButtons"));
    form->addRow(fields.buttons);

    fields.buttons->button(QDialogButtonBox::Ok)->setObjectName(QStringLiteral("assetsDialogOk"));

    const auto frameRateForField = [&fields] {
        return document::FrameRate::create(static_cast<std::uint32_t>(fields.frameRate->value()), 1)
            .value_or(document::FrameRate::framesPerSecond24());
    };
    const auto currentUnit = [&fields] {
        return fields.durationUnit->currentData().toString() == QStringLiteral("seconds")
                   ? DurationUnit::Seconds
                   : DurationUnit::Frames;
    };
    // Applies the numeric field's range/decimals/suffix to the active unit WITHOUT changing its
    // value. Called before a unit-conversion write so the spin box accepts the converted value.
    const auto configureValueField = [&fields](const DurationUnit unit) {
        const QSignalBlocker blocker(fields.duration);
        if (unit == DurationUnit::Frames) {
            fields.duration->setDecimals(0);
            fields.duration->setRange(1.0, static_cast<double>(kMaxFrames));
            fields.duration->setSingleStep(1.0);
        } else {
            fields.duration->setDecimals(6);
            fields.duration->setRange(1.0 / static_cast<double>(kSecondsDenominator), 1000000.0);
            fields.duration->setSingleStep(0.1);
        }
    };
    // Enables OK only for a representable duration and reports the reason when not.
    const auto refreshValidity = [&] {
        const auto duration =
            durationForValue(fields.duration->value(), currentUnit(), frameRateForField());
        if (!duration.has_value()) {
            fields.error->setText(
                QObject::tr("Enter a duration greater than zero and within the supported range."));
            fields.error->show();
        } else {
            fields.error->hide();
        }
        fields.buttons->button(QDialogButtonBox::Ok)->setEnabled(duration.has_value());
    };

    // Initial setup: Frames default, showing the initial duration as a frame count at the field's
    // frame rate. The value is written after the range/decimals so it is never clamped by a stale
    // configuration.
    configureValueField(DurationUnit::Frames);
    {
        const QSignalBlocker blocker(fields.duration);
        fields.duration->setValue(
            displayValueForDuration(initialDuration, DurationUnit::Frames, frameRateForField()));
    }

    QObject::connect(fields.durationUnit, &kit::KDropdown::currentIndexChanged, &dialog, [&](int) {
        const auto unit = currentUnit();
        const auto rate = frameRateForField();
        const auto duration = durationForValue(
            fields.duration->value(),
            unit == DurationUnit::Seconds ? DurationUnit::Frames : DurationUnit::Seconds, rate);
        configureValueField(unit);
        if (duration.has_value()) {
            const QSignalBlocker blocker(fields.duration);
            fields.duration->setValue(displayValueForDuration(*duration, unit, rate));
        }
        refreshValidity();
    });
    // A frame-rate change keeps the entered number: frames stay frames, seconds stay seconds, and
    // the duration follows naturally. Only validity is re-checked.
    QObject::connect(fields.frameRate, &QSpinBox::valueChanged, &dialog,
                     [&] { refreshValidity(); });
    QObject::connect(fields.duration, &QDoubleSpinBox::valueChanged, &dialog,
                     [&] { refreshValidity(); });
    QObject::connect(fields.buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    QObject::connect(fields.buttons, &QDialogButtonBox::accepted, &dialog, [&dialog, &fields] {
        if (!document::isValidHumanFacingName(fields.name->text().toUtf8().toStdString())) {
            fields.error->setText(QObject::tr("Enter a non-empty name up to 128 UTF-8 bytes."));
            fields.error->show();
            fields.name->setFocus();
            return;
        }
        fields.error->hide();
        dialog.accept();
    });

    refreshValidity();
    if (dialog.exec() != QDialog::Accepted) {
        return std::nullopt;
    }

    const auto rate =
        document::FrameRate::create(static_cast<std::uint32_t>(fields.frameRate->value()), 1);
    const auto format = rate.has_value() ? document::CompositionFormat::create(
                                               static_cast<std::uint32_t>(fields.width->value()),
                                               static_cast<std::uint32_t>(fields.height->value()),
                                               currentFormat.pixelAspect(), *rate)
                                         : std::nullopt;
    const auto duration = durationForValue(fields.duration->value(), currentUnit(), *rate);
    if (!rate.has_value() || !format.has_value() || !duration.has_value()) {
        return std::nullopt;
    }

    commands::Transaction transaction(QStringLiteral("Add Composition").toStdString(),
                                      session.snapshot().revision());
    transaction.emplace<commands::AddComposition>(
        fields.name->text().toStdString(), *format, *rate, *duration,
        core::Color4d{static_cast<double>(fields.background->color().red),
                      static_cast<double>(fields.background->color().green),
                      static_cast<double>(fields.background->color().blue),
                      static_cast<double>(fields.background->color().alpha)});
    const auto result = session.executeTransaction(std::move(transaction));
    return result.succeeded()
               ? result.outputId<document::CompositionId>(commands::kAddCompositionOutput)
               : std::nullopt;
}

std::optional<document::CompositionId> duplicateComposition(CompositionSession& session,
                                                            const document::CompositionId source) {
    if (!source.isValid()) {
        return std::nullopt;
    }
    commands::Transaction transaction(QStringLiteral("Duplicate Composition").toStdString(),
                                      session.snapshot().revision());
    transaction.emplace<commands::DuplicateComposition>(source);
    const auto result = session.executeTransaction(std::move(transaction));
    return result.succeeded()
               ? result.outputId<document::CompositionId>(commands::kDuplicateCompositionOutput)
               : std::nullopt;
}

bool deleteComposition(CompositionSession& session, const document::CompositionId id) {
    if (!id.isValid()) {
        return false;
    }
    const bool wasActive = session.compositionId() == id;
    commands::Transaction transaction(QStringLiteral("Delete Composition").toStdString(),
                                      session.snapshot().revision());
    transaction.emplace<commands::DeleteComposition>(id);
    const auto result = session.executeTransaction(std::move(transaction));
    if (!result.succeeded()) {
        return false;
    }
    if (wasActive && !session.snapshot().project().compositions().empty()) {
        (void)session.setComposition(lowestCompositionId(session.snapshot().project()));
    }
    return true;
}

bool renameComposition(CompositionSession& session, const document::CompositionId id,
                       QWidget* parent) {
    const auto* composition = session.snapshot().project().findComposition(id);
    if (composition == nullptr) {
        return false;
    }
    bool accepted = false;
    const QString name = QInputDialog::getText(
        parent, QObject::tr("Rename Composition"), QObject::tr("Name"), QLineEdit::Normal,
        QString::fromStdString(composition->name()), &accepted);
    if (!accepted || !document::isValidHumanFacingName(name.toUtf8().toStdString())) {
        return false;
    }
    commands::Transaction transaction(QStringLiteral("Rename Composition").toStdString(),
                                      session.snapshot().revision());
    transaction.emplace<commands::SetCompositionName>(id, name.toStdString());
    return session.executeTransaction(std::move(transaction)).succeeded();
}

} // namespace bloom::ui
