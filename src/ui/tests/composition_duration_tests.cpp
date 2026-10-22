// Exact New Composition duration and rational frame-rate coverage.
//
// The dialog must keep the *canonical* rational duration across a Frames/Seconds unit switch until
// the artist actually edits the number: one frame at 24 fps stays exactly 1/24 rather than
// degrading to a rounded microsecond count. It must also inherit a fractional composition frame
// rate exactly (30000/1001 rather than a truncated 29), and never overflow an integer conversion
// for an extreme but valid inherited duration. These tests assert the value stored in the document,
// not a re-implementation of the conversion.

#include <bloom/commands/command_stack.hpp>
#include <bloom/commands/operations.hpp>
#include <bloom/commands/transaction.hpp>
#include <bloom/core/rational_time.hpp>
#include <bloom/document/composition_settings.hpp>
#include <bloom/document/document.hpp>
#include <bloom/document/new_project.hpp>
#include <bloom/ui/composition_commands.hpp>
#include <bloom/ui/composition_session.hpp>
#include <bloom/ui/kit/dropdown.hpp>

#include <QApplication>
#include <QDialog>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QString>
#include <QTimer>
#include <QWidget>

#include <cstdint>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <limits>
#include <optional>
#include <source_location>
#include <string>
#include <string_view>
#include <utility>

namespace {

using namespace bloom;

class Expectations final {
  public:
    void expect(const bool condition, const std::string_view message,
                const std::source_location location = std::source_location::current()) {
        if (condition) {
            return;
        }
        ++failures_;
        std::cerr << location.file_name() << ':' << location.line() << ": " << message << '\n';
    }

    [[nodiscard]] int failures() const noexcept { return failures_; }

  private:
    int failures_ = 0;
};

// A standalone session seeded with exactly one composition of `duration` / `format`.
struct SeededSession final {
    document::Document document;
    commands::CommandStack stack;
    std::optional<ui::CompositionSession> session;

    SeededSession(const core::RationalTime duration, const document::CompositionFormat format)
        : document(document::makeNewProject("Duration Project", "Main", duration, format).project),
          stack(document), session(std::in_place, document, stack,
                                   document.snapshot().project().compositions().front().id()) {}
};

// The session is constructed in place by SeededSession. Check it explicitly so a future change
// cannot silently turn a missing fixture into a skipped test.
[[nodiscard]] ui::CompositionSession* seededSession(SeededSession& fixture,
                                                    Expectations& expectations) {
    if (!fixture.session) {
        expectations.expect(false, "the seeded composition session is constructed");
        return nullptr;
    }
    return &*fixture.session;
}

[[nodiscard]] document::CompositionFormat squareFormat(const document::FrameRate rate) {
    return document::CompositionFormat::create(1920, 1080, core::PixelAspectRatio::square(), rate)
        .value_or(document::CompositionFormat{});
}

[[nodiscard]] QDialog* activeNewCompositionDialog() {
    if (auto* modal = qobject_cast<QDialog*>(QApplication::activeModalWidget())) {
        return modal;
    }
    for (QWidget* widget : QApplication::topLevelWidgets()) {
        if (widget->objectName() == QStringLiteral("newCompositionDialog")) {
            return qobject_cast<QDialog*>(widget);
        }
    }
    return nullptr;
}

[[nodiscard]] std::optional<document::CompositionId>
runDialog(ui::CompositionSession& session,
          const std::function<void(QDialog&, Expectations&)>& interact,
          Expectations& expectations) {
    QTimer::singleShot(0, [&interact, &expectations] {
        QDialog* dialog = activeNewCompositionDialog();
        expectations.expect(dialog != nullptr, "the New Composition dialog is up");
        if (dialog == nullptr) {
            return;
        }
        interact(*dialog, expectations);
    });
    return ui::showNewCompositionDialog(session, nullptr);
}

[[nodiscard]] ui::kit::KDropdown* unitControl(QDialog& dialog) {
    return dialog.findChild<ui::kit::KDropdown*>(QStringLiteral("assetsDurationUnitDropdown"));
}
[[nodiscard]] QLineEdit* durationControl(QDialog& dialog) {
    return dialog.findChild<QLineEdit*>(QStringLiteral("assetsDurationField"));
}
[[nodiscard]] QLineEdit* frameRateControl(QDialog& dialog) {
    return dialog.findChild<QLineEdit*>(QStringLiteral("assetsFrameRateField"));
}
[[nodiscard]] ui::kit::KDropdown* ratePresets(QDialog& dialog) {
    return dialog.findChild<ui::kit::KDropdown*>(QStringLiteral("assetsFrameRatePresetDropdown"));
}
[[nodiscard]] QPushButton* okControl(QDialog& dialog) {
    return dialog.findChild<QPushButton*>(QStringLiteral("assetsDialogOk"));
}

// One frame at 24 fps stays exactly 1/24 across repeated Frames -> Seconds -> Frames switches.
void testOneFrameSurvivesUnitRoundtrip(Expectations& expectations) {
    const auto oneFrame = core::RationalTime::create(1, 24);
    const auto rate = document::FrameRate::create(24, 1);
    if (!oneFrame.has_value() || !rate.has_value()) {
        expectations.expect(false, "one-frame fixture is valid");
        return;
    }
    SeededSession fixture(*oneFrame, squareFormat(*rate));
    auto* session = seededSession(fixture, expectations);
    if (session == nullptr) {
        return;
    }
    const auto id = runDialog(
        *session,
        [](QDialog& dialog, Expectations& expect) {
            auto* unit = unitControl(dialog);
            auto* duration = durationControl(dialog);
            auto* ok = okControl(dialog);
            if (unit == nullptr || duration == nullptr || ok == nullptr) {
                expect.expect(false, "one frame: controls exist");
                dialog.reject();
                return;
            }
            expect.expect(duration->text() == QStringLiteral("1"),
                          "one frame: the inherited 1/24 s displays as one frame");
            unit->setCurrentIndex(1);
            expect.expect(duration->text() == QStringLiteral("1/24"),
                          "one frame: Seconds displays the exact 1/24 s");
            unit->setCurrentIndex(0);
            unit->setCurrentIndex(1);
            unit->setCurrentIndex(0);
            expect.expect(duration->text() == QStringLiteral("1"),
                          "one frame: repeated roundtrip returns to one frame");
            ok->click();
        },
        expectations);
    expectations.expect(id.has_value(), "one frame: the dialog adds a composition");
    if (!id.has_value()) {
        return;
    }
    const auto* composition = fixture.document.snapshot().project().findComposition(*id);
    expectations.expect(composition != nullptr && composition->duration() == *oneFrame,
                        "one frame: the stored duration is exactly 1/24, not a rounded microsecond "
                        "count");
}

void testFractionalSecondsSurviveUnitRoundtrip(Expectations& expectations) {
    const auto halfSecond = core::RationalTime::create(1, 2);
    const auto rate = document::FrameRate::create(24, 1);
    if (!halfSecond.has_value() || !rate.has_value()) {
        expectations.expect(false, "fractional-seconds fixture is valid");
        return;
    }
    SeededSession fixture(*halfSecond, squareFormat(*rate));
    auto* session = seededSession(fixture, expectations);
    if (session == nullptr) {
        return;
    }
    const auto id = runDialog(
        *session,
        [](QDialog& dialog, Expectations& expect) {
            auto* unit = unitControl(dialog);
            auto* duration = durationControl(dialog);
            auto* ok = okControl(dialog);
            if (unit == nullptr || duration == nullptr || ok == nullptr) {
                expect.expect(false, "fractional seconds: controls exist");
                dialog.reject();
                return;
            }
            expect.expect(duration->text() == QStringLiteral("12"),
                          "fractional seconds: 0.5 s at 24 fps displays as 12 frames");
            unit->setCurrentIndex(1);
            expect.expect(duration->text() == QStringLiteral("1/2"),
                          "fractional seconds: switching back shows exact 1/2 s");
            unit->setCurrentIndex(0);
            unit->setCurrentIndex(1);
            ok->click();
        },
        expectations);
    expectations.expect(id.has_value(), "fractional seconds: the dialog adds a composition");
    if (!id.has_value()) {
        return;
    }
    const auto* composition = fixture.document.snapshot().project().findComposition(*id);
    expectations.expect(composition != nullptr && composition->duration() == *halfSecond,
                        "fractional seconds: the stored duration remains exactly 1/2 s");
}

void testFpsChangeKeepsEnteredUnit(Expectations& expectations) {
    const auto oneFrame = core::RationalTime::create(1, 24);
    const auto rate24 = document::FrameRate::create(24, 1);
    if (!oneFrame.has_value() || !rate24.has_value()) {
        expectations.expect(false, "fps-change fixture is valid");
        return;
    }
    // Frames stay frames: one frame at 24 fps becomes one frame at 30 fps, i.e. 1/30 s.
    {
        SeededSession fixture(*oneFrame, squareFormat(*rate24));
        auto* session = seededSession(fixture, expectations);
        if (session == nullptr) {
            return;
        }
        const auto id = runDialog(
            *session,
            [](QDialog& dialog, Expectations& expect) {
                auto* rate = frameRateControl(dialog);
                auto* ok = okControl(dialog);
                if (rate == nullptr || ok == nullptr) {
                    expect.expect(false, "fps frames: controls exist");
                    dialog.reject();
                    return;
                }
                expect.expect(rate->text() == QStringLiteral("24"),
                              "fps frames: the inherited 24 fps is displayed");
                rate->setText(QStringLiteral("30"));
                ok->click();
            },
            expectations);
        const auto expected = core::RationalTime::create(1, 30);
        const auto* composition =
            id.has_value() ? fixture.document.snapshot().project().findComposition(*id) : nullptr;
        expectations.expect(composition != nullptr && expected.has_value() &&
                                composition->duration() == *expected,
                            "fps frames: frames stay frames (1 frame becomes 1/30 s)");
    }
    // Seconds stay seconds: 1/24 s at 30 fps is still 1/24 s.
    {
        SeededSession fixture(*oneFrame, squareFormat(*rate24));
        auto* session = seededSession(fixture, expectations);
        if (session == nullptr) {
            return;
        }
        const auto id = runDialog(
            *session,
            [](QDialog& dialog, Expectations& expect) {
                auto* unit = unitControl(dialog);
                auto* rate = frameRateControl(dialog);
                auto* ok = okControl(dialog);
                if (unit == nullptr || rate == nullptr || ok == nullptr) {
                    expect.expect(false, "fps seconds: controls exist");
                    dialog.reject();
                    return;
                }
                unit->setCurrentIndex(1);
                rate->setText(QStringLiteral("30"));
                ok->click();
            },
            expectations);
        const auto* composition =
            id.has_value() ? fixture.document.snapshot().project().findComposition(*id) : nullptr;
        expectations.expect(composition != nullptr && composition->duration() == *oneFrame,
                            "fps seconds: the exact seconds are unchanged by the frame-rate edit");
    }
}

void testInheritedFractionalRateIsPreserved(Expectations& expectations) {
    const auto rate = document::FrameRate::create(30000, 1001);
    const auto duration = core::RationalTime::fromInteger(10);
    if (!rate.has_value()) {
        expectations.expect(false, "fractional-rate fixture is valid");
        return;
    }
    // Untouched: the exact 30000/1001 is reused, not truncated to 29.
    {
        SeededSession fixture(duration, squareFormat(*rate));
        auto* session = seededSession(fixture, expectations);
        if (session == nullptr) {
            return;
        }
        const auto id = runDialog(
            *session,
            [](QDialog& dialog, Expectations& expect) {
                auto* rateControl = frameRateControl(dialog);
                auto* unit = unitControl(dialog);
                auto* ok = okControl(dialog);
                if (rateControl == nullptr || unit == nullptr || ok == nullptr) {
                    expect.expect(false, "fractional rate: controls exist");
                    dialog.reject();
                    return;
                }
                expect.expect(rateControl->text() == QStringLiteral("30000/1001"),
                              "fractional rate: the field displays the exact inherited rate");
                unit->setCurrentIndex(1);
                unit->setCurrentIndex(0);
                ok->click();
            },
            expectations);
        const auto* composition =
            id.has_value() ? fixture.document.snapshot().project().findComposition(*id) : nullptr;
        expectations.expect(composition != nullptr && composition->format().frameRate() == *rate &&
                                composition->duration() == duration,
                            "fractional rate: an untouched field preserves 30000/1001 and the "
                            "duration exactly");
    }
    // Edited: the artist's whole-number rate wins.
    {
        SeededSession fixture(duration, squareFormat(*rate));
        auto* session = seededSession(fixture, expectations);
        if (session == nullptr) {
            return;
        }
        const auto id = runDialog(
            *session,
            [](QDialog& dialog, Expectations& expect) {
                auto* rateControl = frameRateControl(dialog);
                auto* ok = okControl(dialog);
                if (rateControl == nullptr || ok == nullptr) {
                    expect.expect(false, "edited rate: controls exist");
                    dialog.reject();
                    return;
                }
                rateControl->setText(QStringLiteral("30"));
                ok->click();
            },
            expectations);
        const auto expected = document::FrameRate::create(30, 1);
        const auto* composition =
            id.has_value() ? fixture.document.snapshot().project().findComposition(*id) : nullptr;
        expectations.expect(composition != nullptr && expected.has_value() &&
                                composition->format().frameRate() == *expected,
                            "edited rate: a typed 30 fps is used exactly");
    }
}

// An extreme valid inherited duration cannot fit an int64 frame count at 24 fps. Its canonical
// value survives and remains accessible in Seconds without a fabricated clamped frame count.
void testExtremeInheritedDurationStaysExact(Expectations& expectations) {
    const auto extreme = core::RationalTime::create(std::numeric_limits<std::int64_t>::max(), 1);
    const auto rate = document::FrameRate::create(24, 1);
    if (!extreme.has_value() || !rate.has_value()) {
        expectations.expect(false, "extreme-duration fixture is valid");
        return;
    }
    SeededSession fixture(*extreme, squareFormat(*rate));
    auto* session = seededSession(fixture, expectations);
    if (session == nullptr) {
        return;
    }
    const auto id = runDialog(
        *session,
        [](QDialog& dialog, Expectations& expect) {
            auto* duration = durationControl(dialog);
            auto* unit = unitControl(dialog);
            auto* rate = frameRateControl(dialog);
            auto* ok = okControl(dialog);
            if (duration == nullptr || unit == nullptr || rate == nullptr || ok == nullptr) {
                expect.expect(false, "extreme duration: controls exist");
                dialog.reject();
                return;
            }
            expect.expect(duration->text().isEmpty(),
                          "extreme duration: unrepresentable frame count is left blank");
            rate->setText(QStringLiteral("25"));
            unit->setCurrentIndex(1);
            expect.expect(duration->text() == QString::number(static_cast<qlonglong>(
                                                  std::numeric_limits<std::int64_t>::max())) &&
                              ok->isEnabled(),
                          "extreme duration: exact inherited seconds remain available");
            dialog.reject();
        },
        expectations);
    expectations.expect(!id.has_value(), "extreme duration: cancel adds nothing");
    expectations.expect(fixture.document.snapshot().project().compositions().size() == 1,
                        "extreme duration: the seeded project is untouched");
}

void testLongFrameCountAndExactRate(Expectations& expectations) {
    const auto rate24 = document::FrameRate::framesPerSecond24();
    SeededSession fixture(core::RationalTime::fromInteger(50'000), squareFormat(rate24));
    auto* session = seededSession(fixture, expectations);
    if (!session)
        return;
    const auto id = runDialog(
        *session,
        [](QDialog& dialog, Expectations& expect) {
            auto* duration = durationControl(dialog);
            auto* rate = frameRateControl(dialog);
            auto* ok = okControl(dialog);
            if (!duration || !rate || !ok) {
                expect.expect(false, "long duration controls exist");
                dialog.reject();
                return;
            }
            expect.expect(duration->text() == QStringLiteral("1200000"),
                          "50,000 seconds at 24 fps displays all 1.2 million frames");
            rate->setText(QStringLiteral("25"));
            expect.expect(duration->text() == QStringLiteral("1200000"),
                          "changing rate in Frames retains the exact count");
            ok->click();
        },
        expectations);
    const auto* composition =
        id ? fixture.document.snapshot().project().findComposition(*id) : nullptr;
    expectations.expect(
        composition && composition->duration() == core::RationalTime::fromInteger(48'000) &&
            composition->format().frameRate() == *document::FrameRate::create(25, 1),
        "1.2 million frames at 25 fps stores exactly 48,000 seconds");
}

void testPresetAndCustomRationalRates(Expectations& expectations) {
    const auto oneFrame = core::RationalTime::create(1, 24);
    if (!oneFrame)
        return;
    for (const auto* spelling : {"24000/1001", "30000/1001", "60000/1001", "1234567/1001"}) {
        SeededSession fixture(*oneFrame, squareFormat(document::FrameRate::framesPerSecond24()));
        auto* session = seededSession(fixture, expectations);
        if (!session)
            return;
        const QString requested = QString::fromLatin1(spelling);
        const auto id = runDialog(
            *session,
            [requested](QDialog& dialog, Expectations& expect) {
                auto* presets = ratePresets(dialog);
                auto* rate = frameRateControl(dialog);
                auto* ok = okControl(dialog);
                if (!presets || !rate || !ok) {
                    expect.expect(false, "rational rate controls exist");
                    dialog.reject();
                    return;
                }
                const auto preset = presets->findData(requested);
                if (preset >= 0)
                    presets->setCurrentIndex(preset);
                else
                    rate->setText(requested);
                expect.expect(rate->text() == requested && ok->isEnabled(),
                              "preset or custom rational rate is accepted exactly");
                ok->click();
            },
            expectations);
        const auto slash = requested.split(QLatin1Char('/'));
        const auto expectedRate = document::FrameRate::create(slash[0].toUInt(), slash[1].toUInt());
        const auto expectedDuration =
            expectedRate
                ? core::RationalTime::create(expectedRate->denominator(), expectedRate->numerator())
                : std::nullopt;
        const auto* composition =
            id ? fixture.document.snapshot().project().findComposition(*id) : nullptr;
        expectations.expect(composition && expectedRate && expectedDuration &&
                                composition->format().frameRate() == *expectedRate &&
                                composition->duration() == *expectedDuration,
                            "one frame uses the exact selected rational rate");
    }
}

void testLongAndPreciseSeconds(Expectations& expectations) {
    SeededSession fixture(core::RationalTime::fromInteger(10),
                          squareFormat(document::FrameRate::framesPerSecond24()));
    auto* session = seededSession(fixture, expectations);
    if (!session)
        return;
    const auto id = runDialog(
        *session,
        [](QDialog& dialog, Expectations& expect) {
            auto* unit = unitControl(dialog);
            auto* duration = durationControl(dialog);
            auto* rate = frameRateControl(dialog);
            auto* ok = okControl(dialog);
            if (!unit || !duration || !rate || !ok) {
                expect.expect(false, "seconds controls exist");
                dialog.reject();
                return;
            }
            unit->setCurrentIndex(1);
            duration->setText(QStringLiteral("50000.000000000000000000"));
            rate->setText(QStringLiteral("30000/1001"));
            expect.expect(ok->isEnabled(), "long decimal seconds remain valid across a rate edit");
            ok->click();
        },
        expectations);
    const auto* composition =
        id ? fixture.document.snapshot().project().findComposition(*id) : nullptr;
    expectations.expect(composition &&
                            composition->duration() == core::RationalTime::fromInteger(50'000),
                        "Seconds mode retains the authored duration at a fractional rate");
    SeededSession precise(core::RationalTime::fromInteger(10),
                          squareFormat(document::FrameRate::framesPerSecond24()));
    session = seededSession(precise, expectations);
    if (!session)
        return;
    const auto preciseId = runDialog(
        *session,
        [](QDialog& dialog, Expectations& expect) {
            auto* unit = unitControl(dialog);
            auto* duration = durationControl(dialog);
            auto* ok = okControl(dialog);
            if (!unit || !duration || !ok) {
                expect.expect(false, "precise seconds controls exist");
                dialog.reject();
                return;
            }
            unit->setCurrentIndex(1);
            duration->setText(QStringLiteral("0.0000000000000000005"));
            expect.expect(ok->isEnabled(), "representable decimal beyond six places is accepted");
            ok->click();
        },
        expectations);
    const auto* preciseComposition =
        preciseId ? precise.document.snapshot().project().findComposition(*preciseId) : nullptr;
    const auto expectedPrecise = core::RationalTime::create(1, 2'000'000'000'000'000'000);
    expectations.expect(preciseComposition && expectedPrecise &&
                            preciseComposition->duration() == *expectedPrecise,
                        "long decimal seconds reduce to their exact representable rational value");
}

void testOverflowValidation(Expectations& expectations) {
    SeededSession fixture(core::RationalTime::fromInteger(10),
                          squareFormat(document::FrameRate::framesPerSecond24()));
    auto* session = seededSession(fixture, expectations);
    if (!session)
        return;
    const auto id = runDialog(
        *session,
        [](QDialog& dialog, Expectations& expect) {
            auto* duration = durationControl(dialog);
            auto* rate = frameRateControl(dialog);
            auto* unit = unitControl(dialog);
            auto* ok = okControl(dialog);
            auto* error = dialog.findChild<QLabel*>(QStringLiteral("newCompositionErrorLabel"));
            if (!duration || !rate || !unit || !ok || !error) {
                expect.expect(false, "overflow validation controls exist");
                dialog.reject();
                return;
            }
            rate->setText(QStringLiteral("4294967296/1"));
            expect.expect(!ok->isEnabled() && error->isVisible() &&
                              error->text().contains(QStringLiteral("Frame-rate")),
                          "out-of-range uint32 rate is diagnosed");
            rate->setText(QStringLiteral("1/4294967295"));
            duration->setText(QStringLiteral("9223372036854775807"));
            expect.expect(!ok->isEnabled() && error->isVisible() &&
                              error->text().contains(QStringLiteral("Duration")),
                          "unrepresentable frame duration is diagnosed");
            unit->setCurrentIndex(1);
            duration->setText(QString::fromStdString("0." + std::string(63, '0') + "1"));
            expect.expect(!ok->isEnabled() && error->isVisible() &&
                              error->text().contains(QStringLiteral("Duration")),
                          "unrepresentable decimal seconds are diagnosed");
            dialog.reject();
        },
        expectations);
    expectations.expect(!id, "invalid input adds no composition");
}

} // namespace

int main(int argc, char** argv) {
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication application(argc, argv);
    Expectations expectations;

    testOneFrameSurvivesUnitRoundtrip(expectations);
    testFractionalSecondsSurviveUnitRoundtrip(expectations);
    testFpsChangeKeepsEnteredUnit(expectations);
    testInheritedFractionalRateIsPreserved(expectations);
    testExtremeInheritedDurationStaysExact(expectations);
    testLongFrameCountAndExactRate(expectations);
    testPresetAndCustomRationalRates(expectations);
    testLongAndPreciseSeconds(expectations);
    testOverflowValidation(expectations);

    return expectations.failures() == 0 ? 0 : 1;
}
