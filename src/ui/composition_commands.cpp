#include <bloom/ui/composition_commands.hpp>
#include <bloom/ui/kit/color_chip.hpp>
#include <bloom/ui/kit/controls.hpp>
#include <bloom/ui/kit/dropdown.hpp>

#include <bloom/commands/operations.hpp>
#include <bloom/commands/transaction.hpp>
#include <bloom/core/frame_time_mapping.hpp>
#include <bloom/core/rational_time.hpp>
#include <bloom/core/safe_parse.hpp>
#include <bloom/document/composition_settings.hpp>
#include <bloom/document/persisted_text.hpp>
#include <bloom/document/project.hpp>
#include <bloom/ui/composition_session.hpp>

#include <QDialog>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QWidget>

#include <algorithm>
#include <cstdint>
#include <limits>
#include <numeric>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

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
// different units: switching re-displays the exact canonical duration and never silently changes
// its value.
enum class DurationUnit : std::uint8_t { Frames, Seconds };

[[nodiscard]] std::optional<std::int64_t> positiveInteger(const std::string_view text) {
    const auto value = core::parseInteger(text, 10);
    return value && *value > 0 ? value : std::nullopt;
}

[[nodiscard]] bool positiveDigitText(const std::string_view value) {
    const auto text = core::trimAsciiWhitespace(value);
    return !text.empty() && text.find_first_not_of("0123456789") == std::string_view::npos &&
           text.find_first_not_of('0') != std::string_view::npos;
}

[[nodiscard]] bool positiveSecondsSyntax(const QString& text) {
    const auto value = std::string(core::trimAsciiWhitespace(text.toUtf8().toStdString()));
    const auto slash = value.find('/');
    if (slash != std::string::npos)
        return positiveDigitText(std::string_view(value).substr(0, slash)) &&
               positiveDigitText(std::string_view(value).substr(slash + 1));
    const auto point = value.find('.');
    if (point == std::string::npos)
        return positiveDigitText(value);
    const auto whole = std::string_view(value).substr(0, point);
    const auto fraction = std::string_view(value).substr(point + 1);
    return !fraction.empty() && whole.find_first_not_of("0123456789") == std::string_view::npos &&
           fraction.find_first_not_of("0123456789") == std::string_view::npos &&
           (whole.find_first_not_of('0') != std::string_view::npos ||
            fraction.find_first_not_of('0') != std::string_view::npos);
}

[[nodiscard]] QString rateText(const document::FrameRate rate) {
    const auto numerator = QString::number(rate.numerator());
    return rate.denominator() == 1
               ? numerator
               : numerator + QLatin1Char('/') + QString::number(rate.denominator());
}

[[nodiscard]] std::optional<core::RationalTime> durationForFrames(const std::int64_t count,
                                                                  const document::FrameRate rate) {
    if (count <= 0)
        return std::nullopt;
    const auto divisor =
        std::gcd(static_cast<std::uint64_t>(count), static_cast<std::uint64_t>(rate.numerator()));
    const auto reduced = static_cast<std::uint64_t>(count) / divisor;
    if (reduced >
        static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()) / rate.denominator())
        return std::nullopt;
    return core::RationalTime::create(static_cast<std::int64_t>(reduced * rate.denominator()),
                                      static_cast<std::int64_t>(rate.numerator() / divisor));
}

// Divide a decimal integer by a small factor without constructing a wider machine integer.
void divideDecimal(std::string& digits, const unsigned factor) {
    unsigned remainder = 0;
    for (char& digit : digits) {
        const unsigned current = remainder * 10 + static_cast<unsigned>(digit - '0');
        digit = static_cast<char>('0' + current / factor);
        remainder = current % factor;
    }
    digits.erase(0, digits.find_first_not_of('0'));
}

[[nodiscard]] std::optional<core::RationalTime> parseSeconds(const QString& text) {
    const auto value = std::string(core::trimAsciiWhitespace(text.toUtf8().toStdString()));
    const auto slash = value.find('/');
    if (slash != std::string::npos) {
        const auto numerator = positiveInteger(std::string_view(value).substr(0, slash));
        const auto denominator = positiveInteger(std::string_view(value).substr(slash + 1));
        return numerator && denominator ? core::RationalTime::create(*numerator, *denominator)
                                        : std::nullopt;
    }
    const auto point = value.find('.');
    std::string whole = value.substr(0, point);
    std::string fraction = point == std::string::npos ? std::string{} : value.substr(point + 1);
    if (whole.empty())
        whole = "0";
    if (whole.find_first_not_of("0123456789") != std::string::npos ||
        fraction.find_first_not_of("0123456789") != std::string::npos ||
        (point != std::string::npos && fraction.empty()))
        return std::nullopt;
    while (!fraction.empty() && fraction.back() == '0')
        fraction.pop_back();
    // After trailing zeros are removed the final digit lacks either a factor of two or five.
    // More than 62 fractional places therefore leaves at least 2^63 or 5^63 in the reduced
    // denominator, which cannot fit RationalTime's positive signed denominator.
    if (fraction.size() > 62)
        return std::nullopt;
    std::string digits = whole + fraction;
    digits.erase(0, digits.find_first_not_of('0'));
    if (digits.empty())
        return std::nullopt;
    auto twos = fraction.size();
    auto fives = fraction.size();
    while (twos > 0 && ((digits.back() - '0') % 2) == 0) {
        divideDecimal(digits, 2);
        --twos;
    }
    while (fives > 0 && ((digits.back() - '0') % 5) == 0) {
        divideDecimal(digits, 5);
        --fives;
    }
    std::int64_t denominator = 1;
    constexpr auto maximum = std::numeric_limits<std::int64_t>::max();
    for (std::size_t index = 0; index < twos; ++index) {
        if (denominator > maximum / 2)
            return std::nullopt;
        denominator *= 2;
    }
    for (std::size_t index = 0; index < fives; ++index) {
        if (denominator > maximum / 5)
            return std::nullopt;
        denominator *= 5;
    }
    const auto numerator = positiveInteger(digits);
    return numerator ? core::RationalTime::create(*numerator, denominator) : std::nullopt;
}

[[nodiscard]] std::optional<document::FrameRate> parseRate(const QString& text) {
    // Decimal fps is an exact rational too: 29.97 means 2997/100, while the preset
    // 30000/1001 remains distinct. Reduce before checking the document's representation.
    const auto value = parseSeconds(text);
    constexpr auto maximum = std::numeric_limits<std::uint32_t>::max();
    if (!value || value->numerator() > maximum || value->denominator() > maximum)
        return std::nullopt;
    return document::FrameRate::create(static_cast<std::uint32_t>(value->numerator()),
                                       static_cast<std::uint32_t>(value->denominator()));
}

[[nodiscard]] QString secondsText(const core::RationalTime duration) {
    const auto numerator = QString::number(static_cast<qlonglong>(duration.numerator()));
    return duration.denominator() == 1
               ? numerator
               : numerator + QLatin1Char('/') +
                     QString::number(static_cast<qlonglong>(duration.denominator()));
}

[[nodiscard]] std::optional<std::int64_t> frameCountForDuration(const core::RationalTime duration,
                                                                const document::FrameRate rate) {
    const auto mapping =
        core::FrameTimeMapping::create(duration, rate.numerator(), rate.denominator());
    if (!mapping)
        return std::nullopt;
    const auto floor = mapping.value()->frameOffsetForTime(duration);
    if (!floor || *floor > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()))
        return std::nullopt;
    // Use the existing exact nearest-frame mapping when an enlarged interval is representable.
    // A huge inherited duration can still show its exact floor without converting via double.
    if (duration.numerator() <= std::numeric_limits<std::int64_t>::max() - duration.denominator()) {
        const auto enlarged = core::RationalTime::create(
            duration.numerator() + duration.denominator(), duration.denominator());
        if (enlarged) {
            const auto longer =
                core::FrameTimeMapping::create(*enlarged, rate.numerator(), rate.denominator());
            if (longer) {
                const auto nearest = longer.value()->nearestFrameIndex(duration);
                if (nearest <= static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()))
                    return std::max<std::int64_t>(1, static_cast<std::int64_t>(nearest));
            }
        }
    }
    return std::max<std::int64_t>(1, static_cast<std::int64_t>(*floor));
}

struct NewCompositionFields final {
    QLineEdit* name = nullptr;
    QSpinBox* width = nullptr;
    QSpinBox* height = nullptr;
    QLineEdit* frameRate = nullptr;
    kit::KDropdown* ratePresets = nullptr;
    QLineEdit* duration = nullptr;
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
    fields.frameRate = new kit::KLineEdit(&dialog);
    fields.frameRate->setObjectName(QStringLiteral("assetsFrameRateField"));
    fields.frameRate->setText(rateText(inheritedRate));
    fields.frameRate->setPlaceholderText(QObject::tr("24, 29.97 or 30000/1001"));
    fields.ratePresets = new kit::KDropdown(&dialog);
    fields.ratePresets->setObjectName(QStringLiteral("assetsFrameRatePresetDropdown"));
    fields.ratePresets->addItem(QObject::tr("Custom"), QString{});
    for (const auto* preset : {"24", "25", "30", "24000/1001", "30000/1001", "60000/1001"})
        fields.ratePresets->addItem(QString::fromLatin1(preset), QString::fromLatin1(preset));
    const auto inheritedPreset = fields.ratePresets->findData(rateText(inheritedRate));
    fields.ratePresets->setCurrentIndex(inheritedPreset < 0 ? 0 : inheritedPreset);
    auto* rateRow = new QWidget(&dialog);
    auto* rateLayout = new QHBoxLayout(rateRow);
    rateLayout->setContentsMargins(0, 0, 0, 0);
    rateLayout->addWidget(fields.frameRate, 1);
    rateLayout->addWidget(fields.ratePresets);
    form->addRow(QObject::tr("Frame rate (fps)"), rateRow);

    // Duration + unit selector on one row (label "Duration", numeric field, unit dropdown). The
    // default is the current composition's own duration when one exists, otherwise ten seconds --
    // which the Frames default shows as 10 * fps frames, never ten frames by accident.
    const auto initialDuration =
        current == nullptr ? core::RationalTime::fromInteger(10) : current->duration();
    fields.duration = new kit::KLineEdit(&dialog);
    fields.duration->setObjectName(QStringLiteral("assetsDurationField"));
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

    // The exact duration the dialog authors. Unit changes only re-display this rational value;
    // a rate change in Frames re-divides the exact integer count shown in the field.
    std::optional<core::RationalTime> canonicalDuration = initialDuration;
    DurationUnit unit = DurationUnit::Frames;
    const auto currentUnit = [&fields] {
        return fields.durationUnit->currentData().toString() == QStringLiteral("seconds")
                   ? DurationUnit::Seconds
                   : DurationUnit::Frames;
    };
    const auto configureValueField = [&fields](const DurationUnit next) {
        fields.duration->setPlaceholderText(next == DurationUnit::Frames
                                                ? QObject::tr("Whole frames")
                                                : QObject::tr("Seconds or an exact fraction"));
    };
    const auto displayCanonical = [&] {
        if (!canonicalDuration.has_value()) {
            return;
        }
        const QSignalBlocker blocker(fields.duration);
        if (unit == DurationUnit::Seconds) {
            fields.duration->setText(secondsText(*canonicalDuration));
            return;
        }
        const auto rate = parseRate(fields.frameRate->text());
        const auto frames = rate ? frameCountForDuration(*canonicalDuration, *rate) : std::nullopt;
        fields.duration->setText(frames ? QString::number(static_cast<qlonglong>(*frames))
                                        : QString{});
    };
    const auto refreshValidity = [&] {
        const auto rate = parseRate(fields.frameRate->text());
        const bool valid = rate.has_value() && canonicalDuration.has_value();
        if (!rate) {
            fields.error->setText(
                positiveSecondsSyntax(fields.frameRate->text())
                    ? QObject::tr("Frame-rate numerator and denominator must fit 32 bits.")
                    : QObject::tr("Enter a positive frame rate as a number or N/D."));
            fields.error->show();
        } else if (!canonicalDuration) {
            const bool overflow =
                unit == DurationUnit::Frames
                    ? positiveDigitText(fields.duration->text().toUtf8().toStdString())
                    : positiveSecondsSyntax(fields.duration->text());
            fields.error->setText(
                overflow
                    ? QObject::tr("Duration exceeds the signed rational-time range at this rate.")
                    : QObject::tr("Enter a positive duration in the selected unit."));
            fields.error->show();
        } else {
            fields.error->hide();
        }
        fields.buttons->button(QDialogButtonBox::Ok)->setEnabled(valid);
    };

    configureValueField(unit);
    displayCanonical();
    refreshValidity();

    const auto readDuration = [&] {
        if (unit == DurationUnit::Seconds) {
            canonicalDuration = parseSeconds(fields.duration->text());
        } else {
            const auto count = positiveInteger(fields.duration->text().toUtf8().toStdString());
            const auto rate = parseRate(fields.frameRate->text());
            canonicalDuration = count && rate ? durationForFrames(*count, *rate) : std::nullopt;
        }
    };

    QObject::connect(fields.durationUnit, &kit::KDropdown::currentIndexChanged, &dialog, [&](int) {
        unit = currentUnit();
        configureValueField(unit);
        if (canonicalDuration) {
            displayCanonical(); // same canonical duration, expressed in the new unit
        } else {
            // An invalid edit stays visible. Changing units must never silently restore an
            // older value; it may make the entered text meaningful (e.g. fractional seconds).
            readDuration();
        }
        refreshValidity();
    });
    QObject::connect(fields.frameRate, &QLineEdit::textChanged, &dialog, [&](const QString& text) {
        const QSignalBlocker blocker(fields.ratePresets);
        const auto selected = fields.ratePresets->findData(text);
        fields.ratePresets->setCurrentIndex(selected < 0 ? 0 : selected);
        if (unit == DurationUnit::Frames) {
            if (canonicalDuration && fields.duration->text().isEmpty()) {
                // A valid inherited duration may exceed int64 frame labels. Keep its exact
                // seconds available until the artist supplies a representable frame count.
                displayCanonical();
            } else {
                readDuration();
            }
        }
        refreshValidity();
    });
    QObject::connect(fields.ratePresets, &kit::KDropdown::currentIndexChanged, &dialog, [&](int) {
        const auto preset = fields.ratePresets->currentData().toString();
        if (!preset.isEmpty())
            fields.frameRate->setText(preset);
    });
    QObject::connect(fields.duration, &QLineEdit::textChanged, &dialog, [&](const QString&) {
        readDuration();
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

    const auto rate = parseRate(fields.frameRate->text());
    const auto format = document::CompositionFormat::create(
        static_cast<std::uint32_t>(fields.width->value()),
        static_cast<std::uint32_t>(fields.height->value()), currentFormat.pixelAspect(),
        rate.value_or(inheritedRate));
    if (!rate || !format.has_value() || !canonicalDuration.has_value()) {
        return std::nullopt;
    }
    const auto duration = *canonicalDuration;

    commands::Transaction transaction(QStringLiteral("Add Composition").toStdString(),
                                      session.snapshot().revision());
    transaction.emplace<commands::AddComposition>(
        fields.name->text().toStdString(), *format, *rate, duration,
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
