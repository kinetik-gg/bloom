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

// The largest whole-frame count the dialog models; matches the Frames field's range.
constexpr std::int64_t kMaxFrames = 1'000'000;

// Rounds a double to a bounded int64 without invoking std::llround's out-of-range undefined
// behavior. Values at or outside [minimum, maximum] clamp to the nearer bound, so an inherited
// extreme (but valid) duration can be displayed safely.
[[nodiscard]] std::int64_t clampedRound(const double value, const std::int64_t minimum,
                                        const std::int64_t maximum) noexcept {
    const auto low = static_cast<double>(minimum);
    const auto high = static_cast<double>(maximum);
    if (!std::isfinite(value) || value <= low) {
        return minimum;
    }
    if (value >= high) {
        return maximum;
    }
    return std::llround(value);
}

// The exact composition duration a field value names in `unit` at `rate`, or nullopt when the value
// is not representable (nonpositive, or outside int64). Frames are integer by construction; seconds
// are converted at microsecond precision.
[[nodiscard]] std::optional<core::RationalTime>
durationForValue(const double value, const DurationUnit unit, const document::FrameRate rate) {
    if (unit == DurationUnit::Frames) {
        if (!std::isfinite(value) || value < 1.0) {
            return std::nullopt;
        }
        const auto frames = clampedRound(value, 1, kMaxFrames);
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
    const auto microseconds = value * static_cast<double>(kSecondsDenominator);
    if (!std::isfinite(microseconds) || microseconds < 1.0 ||
        microseconds > static_cast<double>(std::numeric_limits<std::int64_t>::max())) {
        return std::nullopt;
    }
    return core::RationalTime::create(std::llround(microseconds), kSecondsDenominator);
}

// The field value that displays `duration` in `unit` at `rate`. Frames round to the nearest whole
// frame and clamp to the field's range; seconds are returned exactly (the field's own decimal
// precision rounds only the display).
[[nodiscard]] double displayValueForDuration(const core::RationalTime duration,
                                             const DurationUnit unit,
                                             const document::FrameRate rate) {
    const double seconds = duration.toSeconds();
    if (unit == DurationUnit::Seconds) {
        return std::isfinite(seconds) ? seconds : 1.0 / static_cast<double>(kSecondsDenominator);
    }
    const auto rateNumerator = static_cast<double>(rate.numerator());
    const auto rateDenominator = static_cast<double>(rate.denominator());
    if (rateNumerator <= 0.0 || rateDenominator <= 0.0) {
        return 1.0;
    }
    const double frames = seconds * rateNumerator / rateDenominator;
    return static_cast<double>(clampedRound(frames, 1, kMaxFrames));
}

struct NewCompositionFields final {
    QLineEdit* name = nullptr;
    QSpinBox* width = nullptr;
    QSpinBox* height = nullptr;
    QDoubleSpinBox* frameRate = nullptr;
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
    const auto inheritedRate = currentFormat.frameRate();
    fields.frameRate = new QDoubleSpinBox(&dialog);
    fields.frameRate->setObjectName(QStringLiteral("assetsFrameRateField"));
    fields.frameRate->setDecimals(3);
    fields.frameRate->setRange(0.001, 1000.0);
    fields.frameRate->setSingleStep(1.0);
    fields.frameRate->setKeyboardTracking(false);
    {
        // Display the inherited rate precisely enough to read (29.970 for 30000/1001) instead of
        // truncating it to a whole number. The exact rational is kept until the artist edits it.
        const QSignalBlocker blocker(fields.frameRate);
        fields.frameRate->setValue(static_cast<double>(inheritedRate.numerator()) /
                                   static_cast<double>(inheritedRate.denominator()));
    }
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

    // The exact duration the dialog authors. It changes only on a numeric edit or a frame-rate
    // change -- never on a unit switch, which only re-displays the same canonical duration in the
    // other unit. That keeps one frame at 24 fps exactly 1/24 across Frames -> Seconds -> Frames
    // instead of degrading to a rounded microsecond count.
    std::optional<core::RationalTime> canonicalDuration = initialDuration;
    DurationUnit unit = DurationUnit::Frames;
    bool frameRateEdited = false;

    const auto effectiveRate = [&]() -> document::FrameRate {
        if (!frameRateEdited) {
            return inheritedRate;
        }
        // The artist's typed rate at millifps precision, reduced to lowest terms.
        const auto numerator =
            clampedRound(fields.frameRate->value() * 1000.0, 1, std::int64_t{1000} * 1000);
        return document::FrameRate::create(static_cast<std::uint32_t>(numerator), 1000)
            .value_or(inheritedRate);
    };
    const auto currentUnit = [&fields] {
        return fields.durationUnit->currentData().toString() == QStringLiteral("seconds")
                   ? DurationUnit::Seconds
                   : DurationUnit::Frames;
    };
    // Applies the numeric field's range/decimals/step to the active unit WITHOUT changing its
    // value.
    const auto configureValueField = [&fields](const DurationUnit next) {
        const QSignalBlocker blocker(fields.duration);
        if (next == DurationUnit::Frames) {
            fields.duration->setDecimals(0);
            fields.duration->setRange(1.0, static_cast<double>(kMaxFrames));
            fields.duration->setSingleStep(1.0);
        } else {
            fields.duration->setDecimals(6);
            fields.duration->setRange(1.0 / static_cast<double>(kSecondsDenominator), 1'000'000.0);
            fields.duration->setSingleStep(0.1);
        }
    };
    // Re-displays the canonical duration in the active unit; the field's own precision rounds only
    // the display, never the stored value.
    const auto displayCanonical = [&] {
        if (!canonicalDuration.has_value()) {
            return;
        }
        const QSignalBlocker blocker(fields.duration);
        fields.duration->setValue(
            displayValueForDuration(*canonicalDuration, unit, effectiveRate()));
    };
    // Enables OK only for a representable duration and reports the reason when not.
    const auto refreshValidity = [&] {
        const bool valid = canonicalDuration.has_value();
        if (!valid) {
            fields.error->setText(
                QObject::tr("Enter a duration greater than zero and within the supported range."));
            fields.error->show();
        } else {
            fields.error->hide();
        }
        fields.buttons->button(QDialogButtonBox::Ok)->setEnabled(valid);
    };

    configureValueField(unit);
    displayCanonical();
    refreshValidity();

    QObject::connect(fields.durationUnit, &kit::KDropdown::currentIndexChanged, &dialog, [&](int) {
        unit = currentUnit();
        configureValueField(unit);
        displayCanonical(); // same canonical duration, expressed in the new unit
        refreshValidity();
    });
    // A frame-rate change keeps the entered number: in Frames the exact frame count is re-divided
    // by the new rate, in Seconds the exact seconds are unchanged.
    QObject::connect(fields.frameRate, &QDoubleSpinBox::valueChanged, &dialog, [&](double) {
        frameRateEdited = true;
        if (unit == DurationUnit::Frames) {
            canonicalDuration = durationForValue(fields.duration->value(), unit, effectiveRate());
        }
        refreshValidity();
    });
    // A numeric edit replaces the canonical duration with the exact value the artist entered.
    QObject::connect(fields.duration, &QDoubleSpinBox::valueChanged, &dialog, [&](double) {
        canonicalDuration = durationForValue(fields.duration->value(), unit, effectiveRate());
        refreshValidity();
    });
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

    if (dialog.exec() != QDialog::Accepted) {
        return std::nullopt;
    }

    const auto rate = effectiveRate();
    const auto format = document::CompositionFormat::create(
        static_cast<std::uint32_t>(fields.width->value()),
        static_cast<std::uint32_t>(fields.height->value()), currentFormat.pixelAspect(), rate);
    if (!format.has_value() || !canonicalDuration.has_value()) {
        return std::nullopt;
    }
    const auto duration = *canonicalDuration;

    commands::Transaction transaction(QStringLiteral("Add Composition").toStdString(),
                                      session.snapshot().revision());
    transaction.emplace<commands::AddComposition>(
        fields.name->text().toStdString(), *format, rate, duration,
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
