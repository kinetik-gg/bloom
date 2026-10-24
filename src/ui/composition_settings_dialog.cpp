#include "composition_settings_dialog.hpp"

#include <bloom/core/frame_time_mapping.hpp>
#include <bloom/core/safe_parse.hpp>
#include <bloom/document/persisted_text.hpp>
#include <bloom/ui/kit/color_chip.hpp>
#include <bloom/ui/kit/controls.hpp>
#include <bloom/ui/kit/dropdown.hpp>
#include <bloom/ui/kit/icons.hpp>
#include <bloom/ui/kit/section.hpp>
#include <bloom/ui/kit/switch_control.hpp>
#include <bloom/ui/kit/tokens.hpp>
#include <bloom/ui/kit/value_field.hpp>
#include <bloom/ui/timeline_ruler.hpp>

#include <QAbstractButton>
#include <QButtonGroup>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSettings>
#include <QSignalBlocker>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <numeric>
#include <string>
#include <string_view>
#include <utility>

namespace bloom::ui {
namespace {

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

// An exact positive time from an integer, a decimal or N/D seconds.
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

// Exactly `frames` frames of `rate`, as a non-negative time (zero allowed).
[[nodiscard]] std::optional<core::RationalTime> timeForFrames(const std::uint64_t frames,
                                                              const document::FrameRate rate) {
    if (frames == 0)
        return core::RationalTime{};
    if (frames > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()))
        return std::nullopt;
    return durationForFrames(static_cast<std::int64_t>(frames), rate);
}

struct PixelAspectChoice final {
    const char* label;
    std::uint32_t numerator;
    std::uint32_t denominator;
};
constexpr std::array<PixelAspectChoice, 8> kPixelAspects{{
    {"Square Pixels (1.0)", 1, 1},
    {"D1/DV NTSC (0.91)", 10, 11},
    {"D1/DV NTSC Widescreen (1.21)", 40, 33},
    {"D1/DV PAL (1.09)", 59, 54},
    {"D1/DV PAL Widescreen (1.46)", 118, 81},
    {"HDV 1080 / DVCPRO HD 720 (1.33)", 4, 3},
    {"DVCPRO HD 1080 (1.5)", 3, 2},
    {"Anamorphic 2:1 (2.0)", 2, 1},
}};

struct RateChoice final {
    const char* label;
    const char* rate;
};
constexpr std::array<RateChoice, 9> kRates{{{"23.976", "24000/1001"},
                                            {"24", "24"},
                                            {"25", "25"},
                                            {"29.97", "30000/1001"},
                                            {"30", "30"},
                                            {"48", "48"},
                                            {"50", "50"},
                                            {"59.94", "60000/1001"},
                                            {"60", "60"}}};

[[nodiscard]] QWidget* row(QWidget* parent, std::initializer_list<QWidget*> widgets,
                           QWidget* stretch = nullptr) {
    auto* holder = new QWidget(parent);
    auto* layout = new QHBoxLayout(holder);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(kit::px(kit::Spacing::XS));
    for (auto* widget : widgets)
        layout->addWidget(widget, widget == stretch ? 1 : 0);
    if (stretch == nullptr)
        layout->addStretch(1);
    return holder;
}

} // namespace

CompositionSettingsDialog::CompositionSettingsDialog(
    const Mode mode, commands::CompositionSettings initial,
    std::vector<WorkingColorSpaceChoice> workingSpaces, QWidget* parent)
    : QDialog(parent), mode_(mode), initial_(std::move(initial)),
      builtIns_(builtInCompositionPresets()), duration_(initial_.duration),
      start_(initial_.timecode.start) {
    setObjectName(mode == Mode::Create ? QStringLiteral("newCompositionDialog")
                                       : QStringLiteral("compositionSettingsDialog"));
    setWindowTitle(mode == Mode::Create ? tr("New Composition") : tr("Composition Settings"));
    setMinimumWidth(kit::px(kit::Size::DialogTextWidth));
    {
        QSettings store;
        userPresets_ = loadUserCompositionPresets(store);
    }

    auto* layout = new QVBoxLayout(this);
    layout->setSpacing(kit::px(kit::Spacing::S));
    auto* basic = new kit::KSection(tr("Basic"), this);
    basic->setObjectName(QStringLiteral("compositionSettingsBasic"));
    basic->setResetEnabled(false);
    buildBasic(*basic);
    layout->addWidget(basic);
    advanced_ = new kit::KSection(tr("Advanced"), this);
    advanced_->setObjectName(QStringLiteral("compositionSettingsAdvanced"));
    advanced_->setResetEnabled(false);
    buildAdvanced(*advanced_);
    advanced_->setCollapsed(true);
    layout->addWidget(advanced_);
    // Spare height sits below the sections, never inside a closed one.
    layout->addStretch(1);
    // Opening Advanced grows the dialog to fit it; closing it gives the height back.
    connect(advanced_, &kit::KSection::collapsedChanged, this, [this] {
        QTimer::singleShot(0, this, [this] {
            if (auto* own = this->layout())
                own->activate();
            resize(width(), sizeHint().height());
        });
    });

    if (workingSpace_ != nullptr) {
        for (const auto& choice : workingSpaces)
            workingSpace_->addItem(choice.label, choice.id);
        const auto current =
            initial_.workingColorSpaceId
                ? workingSpace_->findData(QString::fromStdString(*initial_.workingColorSpaceId))
                : 0;
        if (current < 0 && initial_.workingColorSpaceId) {
            // A space this configuration does not list stays selectable rather than vanishing.
            const auto text = QString::fromStdString(*initial_.workingColorSpaceId);
            workingSpace_->addItem(text, text);
            workingSpace_->setCurrentIndex(workingSpace_->count() - 1);
        } else {
            workingSpace_->setCurrentIndex(std::max(0, current));
        }
        connect(workingSpace_, &kit::KDropdown::currentIndexChanged, this,
                [this](int) { refresh(); });
    }

    error_ = new kit::KLabel(this);
    error_->setObjectName(QStringLiteral("newCompositionErrorLabel"));
    error_->setWordWrap(true);
    error_->hide();
    layout->addWidget(error_);
    buttons_ = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    buttons_->setObjectName(QStringLiteral("assetsDialogButtons"));
    buttons_->button(QDialogButtonBox::Ok)->setObjectName(QStringLiteral("assetsDialogOk"));
    if (mode == Mode::Create)
        buttons_->button(QDialogButtonBox::Ok)->setText(tr("Add"));
    layout->addWidget(buttons_);
    connect(buttons_, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(buttons_, &QDialogButtonBox::accepted, this, [this] {
        if (settings())
            accept();
    });

    applySettings(initial_);
}

void CompositionSettingsDialog::buildBasic(kit::KSection& section) {
    auto* body = new QWidget(section.body());
    auto* form = new QFormLayout(body);
    form->setContentsMargins(0, 0, 0, 0);
    section.bodyLayout()->addWidget(body);

    name_ = new kit::KLineEdit(body);
    name_->setObjectName(QStringLiteral("assetsNameField"));
    form->addRow(tr("Name"), name_);
    connect(name_, &QLineEdit::textChanged, this, [this](const QString&) { refresh(); });

    preset_ = new kit::KDropdown(body);
    preset_->setObjectName(QStringLiteral("compositionSettingsPreset"));
    preset_->setAccessibleName(tr("Preset"));
    savePreset_ = new kit::KIconButton(body);
    savePreset_->setObjectName(QStringLiteral("compositionSettingsSavePreset"));
    savePreset_->setIcon(kit::icon(kit::IconId::Add, kit::IconRole::Chrome));
    savePreset_->setToolTip(tr("Save these settings as a preset"));
    savePreset_->setAccessibleName(tr("Save Preset"));
    renamePreset_ = new kit::KIconButton(body);
    renamePreset_->setObjectName(QStringLiteral("compositionSettingsRenamePreset"));
    renamePreset_->setIcon(kit::icon(kit::IconId::Settings, kit::IconRole::Chrome));
    renamePreset_->setToolTip(tr("Rename the selected preset"));
    renamePreset_->setAccessibleName(tr("Rename Preset"));
    deletePreset_ = new kit::KIconButton(body);
    deletePreset_->setObjectName(QStringLiteral("compositionSettingsDeletePreset"));
    deletePreset_->setIcon(kit::icon(kit::IconId::Delete, kit::IconRole::Chrome));
    deletePreset_->setToolTip(tr("Delete the selected preset"));
    deletePreset_->setAccessibleName(tr("Delete Preset"));
    form->addRow(tr("Preset"),
                 row(body, {preset_, savePreset_, renamePreset_, deletePreset_}, preset_));
    connect(preset_, &kit::KDropdown::currentIndexChanged, this, [this](int) {
        if (!updating_)
            applySelectedPreset();
    });
    connect(savePreset_, &QAbstractButton::clicked, this, [this] {
        bool accepted = false;
        const auto name = QInputDialog::getText(this, tr("Save Preset"), tr("Preset name"),
                                                QLineEdit::Normal, name_->text(), &accepted);
        if (!accepted)
            return;
        if (const auto refused = savePreset(name)) {
            error_->setText(*refused);
            error_->show();
        }
    });
    connect(renamePreset_, &QAbstractButton::clicked, this, [this] {
        bool accepted = false;
        const auto name =
            QInputDialog::getText(this, tr("Rename Preset"), tr("Preset name"), QLineEdit::Normal,
                                  preset_->currentText(), &accepted);
        if (!accepted)
            return;
        if (const auto refused = renameSelectedPreset(name)) {
            error_->setText(*refused);
            error_->show();
        }
    });
    connect(deletePreset_, &QAbstractButton::clicked, this,
            [this] { (void)deleteSelectedPreset(); });

    const auto dimension = [body](const QString& name, const QString& accessible) {
        auto* field = new kit::KValueField(body);
        field->setObjectName(name);
        field->setAccessibleName(accessible);
        field->setDecimals(0);
        field->setSingleStep(1.0);
        field->setRange(1.0, static_cast<double>(document::CompositionFormat::kMaximumDimension));
        field->setUnit(tr("px"));
        return field;
    };
    width_ = dimension(QStringLiteral("assetsWidthField"), tr("Width"));
    height_ = dimension(QStringLiteral("assetsHeightField"), tr("Height"));
    lockAspect_ = new kit::KIconToggle(kit::IconId::Link, body);
    lockAspect_->setObjectName(QStringLiteral("compositionSettingsLockAspect"));
    lockAspect_->setCheckable(true);
    lockAspect_->setToolTip(tr("Lock the frame aspect ratio"));
    lockAspect_->setAccessibleName(tr("Lock Aspect Ratio"));
    auto* times = new kit::KLabel(QStringLiteral("×"), body);
    form->addRow(tr("Size"), row(body, {width_, times, height_, lockAspect_}));
    connect(lockAspect_, &QAbstractButton::toggled, this, [this](const bool locked) {
        if (locked && width_->value() > 0.0)
            aspectLockRatio_ = height_->value() / width_->value();
    });
    connect(width_, &kit::KValueField::valueChanged, this, [this](const double value) {
        if (!updating_ && lockAspect_->isChecked()) {
            const QSignalBlocker blocker(height_);
            height_->setValue(std::max(1.0, std::round(value * aspectLockRatio_)));
        }
        refresh();
    });
    connect(height_, &kit::KValueField::valueChanged, this, [this](const double value) {
        if (!updating_ && lockAspect_->isChecked() && aspectLockRatio_ > 0.0) {
            const QSignalBlocker blocker(width_);
            width_->setValue(std::max(1.0, std::round(value / aspectLockRatio_)));
        }
        refresh();
    });

    pixelAspect_ = new kit::KDropdown(body);
    pixelAspect_->setObjectName(QStringLiteral("compositionSettingsPixelAspect"));
    pixelAspect_->setAccessibleName(tr("Pixel Aspect Ratio"));
    for (const auto& choice : kPixelAspects)
        pixelAspect_->addItem(
            tr(choice.label),
            QStringLiteral("%1:%2").arg(choice.numerator).arg(choice.denominator));
    form->addRow(tr("Pixel Aspect Ratio"), pixelAspect_);
    connect(pixelAspect_, &kit::KDropdown::currentIndexChanged, this, [this](int) { refresh(); });

    frameRate_ = new kit::KLineEdit(body);
    frameRate_->setObjectName(QStringLiteral("assetsFrameRateField"));
    frameRate_->setPlaceholderText(tr("24, 29.97 or 30000/1001"));
    ratePresets_ = new kit::KDropdown(body);
    ratePresets_->setObjectName(QStringLiteral("assetsFrameRatePresetDropdown"));
    ratePresets_->addItem(tr("Custom"), QString{});
    for (const auto& choice : kRates)
        ratePresets_->addItem(QString::fromLatin1(choice.label), QString::fromLatin1(choice.rate));
    dropFrame_ = new kit::KCheckBox(body);
    dropFrame_->setObjectName(QStringLiteral("compositionSettingsDropFrame"));
    auto* dropFrameLabel = new kit::KLabel(tr("Drop-frame labels (29.97 and 59.94 fps)"), body);
    dropFrame_->setAccessibleName(dropFrameLabel->text());
    dropFrame_->setToolTip(tr("Label time with SMPTE drop-frame timecode; frames are unchanged"));
    dropFrameLabel->setToolTip(dropFrame_->toolTip());
    form->addRow(tr("Frame Rate"), row(body, {frameRate_, ratePresets_}, frameRate_));
    form->addRow(tr("Timecode"), row(body, {dropFrame_, dropFrameLabel}));
    connect(frameRate_, &QLineEdit::textChanged, this, [this](const QString& text) {
        {
            const QSignalBlocker blocker(ratePresets_);
            const auto selected = ratePresets_->findData(text);
            ratePresets_->setCurrentIndex(selected < 0 ? 0 : selected);
        }
        if (!updating_) {
            // Frames and timecode keep the frame count shown; seconds keep the exact time.
            if (unit() != TimeUnit::Seconds) {
                if (duration_ && durationField_->text().isEmpty())
                    displayDuration();
                else
                    readDuration();
                readStart();
            } else {
                displayStart();
            }
        }
        refresh();
    });
    connect(ratePresets_, &kit::KDropdown::currentIndexChanged, this, [this](int) {
        const auto preset = ratePresets_->currentData().toString();
        if (!preset.isEmpty())
            frameRate_->setText(preset);
    });
    connect(dropFrame_, &QAbstractButton::toggled, this, [this](bool) {
        if (!updating_ && unit() == TimeUnit::Timecode) {
            displayStart();
            displayDuration();
        }
        refresh();
    });

    unit_ = new kit::KDropdown(body);
    unit_->setObjectName(QStringLiteral("assetsDurationUnitDropdown"));
    unit_->setAccessibleName(tr("Time unit"));
    unit_->addItem(tr("Frames"), QStringLiteral("frames"));
    unit_->addItem(tr("Seconds"), QStringLiteral("seconds"));
    unit_->addItem(tr("Timecode"), QStringLiteral("timecode"));
    startField_ = new kit::KLineEdit(body);
    startField_->setObjectName(QStringLiteral("compositionSettingsStart"));
    startField_->setAccessibleName(tr("Start timecode"));
    form->addRow(tr("Start"), row(body, {startField_}, startField_));
    durationField_ = new kit::KLineEdit(body);
    durationField_->setObjectName(QStringLiteral("assetsDurationField"));
    form->addRow(tr("Duration"), row(body, {durationField_, unit_}, durationField_));
    connect(unit_, &kit::KDropdown::currentIndexChanged, this, [this](int) {
        const auto placeholder = unit() == TimeUnit::Frames    ? tr("Whole frames")
                                 : unit() == TimeUnit::Seconds ? tr("Seconds or an exact fraction")
                                                               : tr("HH:MM:SS:FF");
        durationField_->setPlaceholderText(placeholder);
        startField_->setPlaceholderText(placeholder);
        // The same exact values, re-displayed; an invalid entry stays visible.
        if (duration_)
            displayDuration();
        else
            readDuration();
        if (start_)
            displayStart();
        else
            readStart();
        refresh();
    });
    connect(durationField_, &QLineEdit::textChanged, this, [this](const QString&) {
        if (!updating_)
            readDuration();
        refresh();
    });
    connect(startField_, &QLineEdit::textChanged, this, [this](const QString&) {
        if (!updating_)
            readStart();
        refresh();
    });

    background_ = new kit::KColorChip(body);
    background_->setObjectName(QStringLiteral("newCompositionBackgroundColor"));
    background_->setAccessibleName(tr("Background Colour"));
    form->addRow(tr("Background Colour"), background_);
    connect(background_, &kit::KColorChip::colorChanged, this, [this](const kit::KColor& color) {
        if (updating_)
            return;
        initial_.background = {static_cast<double>(color.red), static_cast<double>(color.green),
                               static_cast<double>(color.blue), static_cast<double>(color.alpha)};
        refresh();
    });
}

void CompositionSettingsDialog::buildAdvanced(kit::KSection& section) {
    auto* body = new QWidget(section.body());
    auto* form = new QFormLayout(body);
    form->setContentsMargins(0, 0, 0, 0);
    section.bodyLayout()->addWidget(body);

    workingSpace_ = new kit::KDropdown(body);
    workingSpace_->setObjectName(QStringLiteral("compositionSettingsWorkingSpace"));
    workingSpace_->setAccessibleName(tr("Working colour space"));
    form->addRow(tr("Working Colour Space"), workingSpace_);

    // The 3 x 3 resize anchor: where existing layers stay when the size changes.
    auto* grid = new QWidget(body);
    auto* gridLayout = new QGridLayout(grid);
    gridLayout->setContentsMargins(0, 0, 0, 0);
    gridLayout->setSpacing(kit::px(kit::Spacing::XXS));
    anchorGroup_ = new QButtonGroup(this);
    anchorGroup_->setExclusive(true);
    const std::array<const char*, 9> names{"Top Left",    "Top",    "Top Right",
                                           "Left",        "Centre", "Right",
                                           "Bottom Left", "Bottom", "Bottom Right"};
    for (int cell = 0; cell < 9; ++cell) {
        auto* toggle = new kit::KIconToggle(kit::IconId::Handle, grid);
        toggle->setObjectName(QStringLiteral("compositionSettingsAnchor%1").arg(cell));
        toggle->setCheckable(true);
        toggle->setToolTip(
            tr("Keep existing layers anchored: %1").arg(tr(names[static_cast<std::size_t>(cell)])));
        toggle->setAccessibleName(tr(names[static_cast<std::size_t>(cell)]));
        anchorGroup_->addButton(toggle, cell);
        gridLayout->addWidget(toggle, cell / 3, cell % 3);
    }
    anchorGroup_->button(4)->setChecked(true);
    if (mode_ == Mode::Create) {
        grid->setEnabled(false);
        grid->setToolTip(tr("A new composition has no layers to anchor"));
    }
    form->addRow(tr("Resize Anchor"), row(body, {grid}));

    const auto percent = [body](const QString& name, const QString& accessible) {
        auto* field = new kit::KValueField(body);
        field->setObjectName(name);
        field->setAccessibleName(accessible);
        field->setDecimals(1);
        field->setSingleStep(1.0);
        field->setRange(0.1, 100.0);
        field->setUnit(QStringLiteral("%"));
        return field;
    };
    actionSafe_ = percent(QStringLiteral("compositionSettingsActionSafe"), tr("Action safe"));
    titleSafe_ = percent(QStringLiteral("compositionSettingsTitleSafe"), tr("Title safe"));
    form->addRow(tr("Action Safe"), row(body, {actionSafe_}));
    form->addRow(tr("Title Safe"), row(body, {titleSafe_}));
    connect(actionSafe_, &kit::KValueField::valueChanged, this, [this](double) { refresh(); });
    connect(titleSafe_, &kit::KValueField::valueChanged, this, [this](double) { refresh(); });
}

std::optional<document::FrameRate> CompositionSettingsDialog::rate() const {
    return parseRate(frameRate_->text());
}

CompositionSettingsDialog::TimeUnit CompositionSettingsDialog::unit() const {
    const auto data = unit_->currentData().toString();
    return data == QStringLiteral("seconds")    ? TimeUnit::Seconds
           : data == QStringLiteral("timecode") ? TimeUnit::Timecode
                                                : TimeUnit::Frames;
}

std::optional<core::PixelAspectRatio> CompositionSettingsDialog::pixelAspect() const {
    const auto parts = pixelAspect_->currentData().toString().split(QLatin1Char(':'));
    if (parts.size() != 2)
        return std::nullopt;
    return core::PixelAspectRatio::create(parts[0].toULongLong(), parts[1].toULongLong());
}

void CompositionSettingsDialog::displayDuration() {
    if (!duration_)
        return;
    const QSignalBlocker blocker(durationField_);
    if (unit() == TimeUnit::Seconds) {
        durationField_->setText(secondsText(*duration_));
        return;
    }
    const auto frameRate = rate();
    const auto frames = frameRate ? frameCountForDuration(*duration_, *frameRate) : std::nullopt;
    if (!frames) {
        durationField_->setText(QString{});
        return;
    }
    durationField_->setText(
        unit() == TimeUnit::Frames
            ? QString::number(static_cast<qlonglong>(*frames))
            : formatTimelineFrameLabel(static_cast<std::uint64_t>(*frames), *frameRate, true,
                                       {core::RationalTime{}, dropFrame_->isChecked()}));
}

void CompositionSettingsDialog::displayStart() {
    if (!start_)
        return;
    const QSignalBlocker blocker(startField_);
    if (unit() == TimeUnit::Seconds) {
        startField_->setText(*start_ == core::RationalTime{} ? QStringLiteral("0")
                                                             : secondsText(*start_));
        return;
    }
    const auto frameRate = rate();
    if (!frameRate) {
        startField_->setText(QString{});
        return;
    }
    const auto frame = static_cast<std::uint64_t>(timecodeStartFrame(*frameRate, {*start_, false}));
    startField_->setText(
        unit() == TimeUnit::Frames
            ? QString::number(static_cast<qulonglong>(frame))
            : formatTimelineFrameLabel(frame, *frameRate, true,
                                       {core::RationalTime{}, dropFrame_->isChecked()}));
}

void CompositionSettingsDialog::readDuration() {
    const auto frameRate = rate();
    switch (unit()) {
    case TimeUnit::Seconds:
        duration_ = parseSeconds(durationField_->text());
        break;
    case TimeUnit::Frames: {
        const auto count = positiveInteger(durationField_->text().toUtf8().toStdString());
        duration_ = count && frameRate ? durationForFrames(*count, *frameRate) : std::nullopt;
        break;
    }
    case TimeUnit::Timecode: {
        const auto frames =
            frameRate ? parseTimelineFrameLabel(durationField_->text(), *frameRate, true,
                                                {core::RationalTime{}, dropFrame_->isChecked()})
                      : std::nullopt;
        duration_ = frames && *frames > 0 ? timeForFrames(*frames, *frameRate) : std::nullopt;
        break;
    }
    }
}

void CompositionSettingsDialog::readStart() {
    const auto text = startField_->text().trimmed();
    const auto frameRate = rate();
    if (text.isEmpty() || text == QStringLiteral("0")) {
        start_ = core::RationalTime{};
        return;
    }
    switch (unit()) {
    case TimeUnit::Seconds:
        start_ = parseSeconds(text);
        break;
    case TimeUnit::Frames: {
        bool ok = false;
        const auto frames = text.toULongLong(&ok);
        start_ = ok && frameRate ? timeForFrames(frames, *frameRate) : std::nullopt;
        break;
    }
    case TimeUnit::Timecode: {
        const auto frames =
            frameRate ? parseTimelineFrameLabel(text, *frameRate, true,
                                                {core::RationalTime{}, dropFrame_->isChecked()})
                      : std::nullopt;
        start_ = frames ? timeForFrames(*frames, *frameRate) : std::nullopt;
        break;
    }
    }
}

std::optional<commands::CompositionSettings> CompositionSettingsDialog::settings() const {
    if (!issue().isEmpty())
        return std::nullopt;
    const auto frameRate = rate();
    const auto aspect = pixelAspect();
    const auto format = frameRate && aspect
                            ? document::CompositionFormat::create(
                                  static_cast<std::uint32_t>(width_->value()),
                                  static_cast<std::uint32_t>(height_->value()), *aspect, *frameRate)
                            : std::nullopt;
    if (!format || !duration_ || !start_)
        return std::nullopt;
    commands::CompositionSettings result;
    result.name = name_->text().toStdString();
    result.format = *format;
    result.duration = *duration_;
    result.timecode = {*start_,
                       dropFrame_->isChecked() && document::hasDropFrameTimecode(*frameRate)};
    result.background = initial_.background;
    const auto space = workingSpace_->currentData().toString();
    result.workingColorSpaceId =
        space.isEmpty() ? std::nullopt : std::optional(space.toStdString());
    result.safeAreas = {actionSafe_->value() / 100.0, titleSafe_->value() / 100.0};
    // Keep the exact stored fractions when the fields still show them.
    if (std::abs(result.safeAreas.action - initial_.safeAreas.action) < 0.0005)
        result.safeAreas.action = initial_.safeAreas.action;
    if (std::abs(result.safeAreas.title - initial_.safeAreas.title) < 0.0005)
        result.safeAreas.title = initial_.safeAreas.title;
    if (const auto refused = commands::compositionSettingsIssue(result))
        return std::nullopt;
    return result;
}

commands::ResizeAnchor CompositionSettingsDialog::anchor() const {
    const auto cell = std::max(0, anchorGroup_->checkedId());
    return {static_cast<std::uint8_t>(cell % 3), static_cast<std::uint8_t>(cell / 3)};
}

QString CompositionSettingsDialog::issue() const {
    if (!document::isValidHumanFacingName(name_->text().toUtf8().toStdString()))
        return tr("Enter a non-empty name up to 128 UTF-8 bytes.");
    const auto frameRate = rate();
    if (!frameRate)
        return positiveSecondsSyntax(frameRate_->text())
                   ? tr("Frame rate exceeds the representable rational range.")
                   : tr("Enter a positive frame rate as an integer, decimal, or N/D.");
    const auto aspect = pixelAspect();
    if (!aspect || !document::CompositionFormat::create(
                       static_cast<std::uint32_t>(width_->value()),
                       static_cast<std::uint32_t>(height_->value()), *aspect, *frameRate))
        return tr("Width × height must be at most 4294967296 pixels.");
    if (unit() != TimeUnit::Seconds && duration_ && durationField_->text().isEmpty())
        return tr("Duration cannot be shown as a whole frame count; switch to Seconds.");
    if (!duration_) {
        const bool overflow = unit() == TimeUnit::Frames
                                  ? positiveDigitText(durationField_->text().toUtf8().toStdString())
                                  : positiveSecondsSyntax(durationField_->text());
        return overflow && unit() != TimeUnit::Timecode
                   ? tr("Duration exceeds the signed rational-time range at this rate.")
               : unit() == TimeUnit::Timecode
                   ? tr("Enter the duration as HH:MM:SS:FF at this frame rate.")
                   : tr("Enter a positive duration in the selected unit.");
    }
    if (!start_)
        return unit() == TimeUnit::Timecode
                   ? tr("Enter the start as HH:MM:SS:FF at this frame rate.")
                   : tr("Enter a start of zero or more in the selected unit.");
    if (titleSafe_->value() > actionSafe_->value())
        return tr("Title safe must be inside action safe.");
    return {};
}

void CompositionSettingsDialog::refresh() {
    if (updating_)
        return;
    const auto rateValue = rate();
    dropFrame_->setEnabled(rateValue && document::hasDropFrameTimecode(*rateValue));
    const auto reason = issue();
    error_->setText(reason);
    error_->setVisible(!reason.isEmpty());
    buttons_->button(QDialogButtonBox::Ok)->setEnabled(reason.isEmpty());
    selectMatchingPreset();
}

void CompositionSettingsDialog::rebuildPresetItems() {
    const QSignalBlocker blocker(preset_);
    preset_->clearItems();
    preset_->addItem(tr("Custom"), QStringLiteral("custom"));
    for (std::size_t index = 0; index < userPresets_.size(); ++index)
        preset_->addItem(userPresets_[index].name, QStringLiteral("user:%1").arg(index));
    for (std::size_t index = 0; index < builtIns_.size(); ++index)
        preset_->addItem(builtIns_[index].name, QStringLiteral("builtin:%1").arg(index));
}

void CompositionSettingsDialog::selectMatchingPreset() {
    const auto current = settings();
    QString data = QStringLiteral("custom");
    if (current) {
        if (const auto user = matchingCompositionPreset(userPresets_, *current))
            data = QStringLiteral("user:%1").arg(*user);
        else if (const auto builtIn = matchingCompositionPreset(builtIns_, *current))
            data = QStringLiteral("builtin:%1").arg(*builtIn);
    }
    const QSignalBlocker blocker(preset_);
    preset_->setCurrentIndex(std::max(0, preset_->findData(data)));
    const bool user = data.startsWith(QStringLiteral("user:"));
    renamePreset_->setEnabled(user);
    deletePreset_->setEnabled(user);
    savePreset_->setEnabled(current.has_value());
}

void CompositionSettingsDialog::applySelectedPreset() {
    const auto data = preset_->currentData().toString();
    const auto colon = data.indexOf(QLatin1Char(':'));
    if (colon < 0)
        return;
    const auto index = data.mid(colon + 1).toULongLong();
    const auto& list = data.startsWith(QStringLiteral("user:")) ? userPresets_ : builtIns_;
    if (index >= list.size())
        return;
    auto base = settings().value_or(initial_);
    applySettings(applyCompositionPreset(list[index], std::move(base)));
}

void CompositionSettingsDialog::applySettings(const commands::CompositionSettings& settings) {
    updating_ = true;
    if (name_->text().toStdString() != settings.name)
        name_->setText(QString::fromStdString(settings.name));
    width_->setValue(static_cast<double>(settings.format.width()));
    height_->setValue(static_cast<double>(settings.format.height()));
    if (lockAspect_->isChecked())
        aspectLockRatio_ = height_->value() / width_->value();
    const auto aspect = settings.format.pixelAspect();
    const auto aspectData =
        QStringLiteral("%1:%2").arg(aspect.numerator()).arg(aspect.denominator());
    if (pixelAspect_->findData(aspectData) < 0)
        pixelAspect_->addItem(
            QStringLiteral("%1:%2").arg(aspect.numerator()).arg(aspect.denominator()), aspectData);
    pixelAspect_->setCurrentIndex(pixelAspect_->findData(aspectData));
    frameRate_->setText(rateText(settings.format.frameRate()));
    dropFrame_->setChecked(settings.timecode.dropFrame);
    duration_ = settings.duration;
    start_ = settings.timecode.start;
    initial_.background = settings.background;
    background_->setColor({static_cast<float>(settings.background.red),
                           static_cast<float>(settings.background.green),
                           static_cast<float>(settings.background.blue),
                           static_cast<float>(settings.background.alpha)});
    actionSafe_->setValue(settings.safeAreas.action * 100.0);
    titleSafe_->setValue(settings.safeAreas.title * 100.0);
    initial_.safeAreas = settings.safeAreas;
    updating_ = false;
    displayDuration();
    displayStart();
    rebuildPresetItems();
    refresh();
}

void CompositionSettingsDialog::persistPresets() {
    QSettings store;
    saveUserCompositionPresets(store, userPresets_);
    store.sync();
    rebuildPresetItems();
    refresh();
}

std::optional<QString> CompositionSettingsDialog::savePreset(const QString& name) {
    const auto current = settings();
    if (!current)
        return issue();
    if (const auto refused = userPresetNameIssue(name, userPresets_))
        return refused;
    userPresets_.push_back(userPresetFrom(name, *current));
    persistPresets();
    return std::nullopt;
}

std::optional<QString> CompositionSettingsDialog::renameSelectedPreset(const QString& name) {
    const auto data = preset_->currentData().toString();
    if (!data.startsWith(QStringLiteral("user:")))
        return tr("Only a saved preset can be renamed.");
    const auto index = static_cast<std::size_t>(data.mid(5).toULongLong());
    if (index >= userPresets_.size())
        return tr("Only a saved preset can be renamed.");
    if (const auto refused = userPresetNameIssue(name, userPresets_, index))
        return refused;
    userPresets_[index].name = name.trimmed();
    persistPresets();
    return std::nullopt;
}

std::optional<QString> CompositionSettingsDialog::deleteSelectedPreset() {
    const auto data = preset_->currentData().toString();
    if (!data.startsWith(QStringLiteral("user:")))
        return tr("Only a saved preset can be deleted.");
    const auto index = static_cast<std::size_t>(data.mid(5).toULongLong());
    if (index >= userPresets_.size())
        return tr("Only a saved preset can be deleted.");
    userPresets_.erase(userPresets_.begin() + static_cast<std::ptrdiff_t>(index));
    persistPresets();
    return std::nullopt;
}

} // namespace bloom::ui
