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
#include <QDoubleSpinBox>
#include <QPushButton>
#include <QString>
#include <QTimer>
#include <QWidget>

#include <cmath>
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
[[nodiscard]] QDoubleSpinBox* durationControl(QDialog& dialog) {
    return dialog.findChild<QDoubleSpinBox*>(QStringLiteral("assetsDurationField"));
}
[[nodiscard]] QDoubleSpinBox* frameRateControl(QDialog& dialog) {
    return dialog.findChild<QDoubleSpinBox*>(QStringLiteral("assetsFrameRateField"));
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
    const auto id = runDialog(
        *fixture.session,
        [](QDialog& dialog, Expectations& expect) {
            auto* unit = unitControl(dialog);
            auto* duration = durationControl(dialog);
            auto* ok = okControl(dialog);
            if (unit == nullptr || duration == nullptr || ok == nullptr) {
                expect.expect(false, "one frame: controls exist");
                dialog.reject();
                return;
            }
            expect.expect(std::abs(duration->value() - 1.0) < 0.001,
                          "one frame: the inherited 1/24 s displays as one frame");
            unit->setCurrentIndex(1);
            expect.expect(std::abs(duration->value() - (1.0 / 24.0)) < 0.001,
                          "one frame: Seconds displays 1/24 s");
            unit->setCurrentIndex(0);
            unit->setCurrentIndex(1);
            unit->setCurrentIndex(0);
            expect.expect(std::abs(duration->value() - 1.0) < 0.001,
                          "one frame: repeated roundtrip returns to one frame");
            ok->click();
        },
        expectations);
    expectations.expect(id.has_value(), "one frame: the dialog adds a composition");
    if (!id.has_value()) {
        return;
    }
    const auto* composition = fixture.document.snapshot().project().findComposition(*id);
    expectations.expect(composition != nullptr &&
                            composition->duration() == *core::RationalTime::create(1, 24),
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
    const auto id = runDialog(
        *fixture.session,
        [](QDialog& dialog, Expectations& expect) {
            auto* unit = unitControl(dialog);
            auto* duration = durationControl(dialog);
            auto* ok = okControl(dialog);
            if (unit == nullptr || duration == nullptr || ok == nullptr) {
                expect.expect(false, "fractional seconds: controls exist");
                dialog.reject();
                return;
            }
            expect.expect(std::abs(duration->value() - 12.0) < 0.001,
                          "fractional seconds: 0.5 s at 24 fps displays as 12 frames");
            unit->setCurrentIndex(1);
            expect.expect(std::abs(duration->value() - 0.5) < 0.000001,
                          "fractional seconds: switching back shows 0.5 s");
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
    expectations.expect(composition != nullptr &&
                            composition->duration() == *core::RationalTime::create(1, 2),
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
        const auto id = runDialog(
            *fixture.session,
            [](QDialog& dialog, Expectations& expect) {
                auto* rate = frameRateControl(dialog);
                auto* ok = okControl(dialog);
                if (rate == nullptr || ok == nullptr) {
                    expect.expect(false, "fps frames: controls exist");
                    dialog.reject();
                    return;
                }
                expect.expect(std::abs(rate->value() - 24.0) < 0.001,
                              "fps frames: the inherited 24 fps is displayed");
                rate->setValue(30.0);
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
        const auto id = runDialog(
            *fixture.session,
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
                rate->setValue(30.0);
                ok->click();
            },
            expectations);
        const auto* composition =
            id.has_value() ? fixture.document.snapshot().project().findComposition(*id) : nullptr;
        expectations.expect(composition != nullptr &&
                                composition->duration() == *core::RationalTime::create(1, 24),
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
        const auto id = runDialog(
            *fixture.session,
            [](QDialog& dialog, Expectations& expect) {
                auto* rateControl = frameRateControl(dialog);
                auto* ok = okControl(dialog);
                if (rateControl == nullptr || ok == nullptr) {
                    expect.expect(false, "fractional rate: controls exist");
                    dialog.reject();
                    return;
                }
                expect.expect(std::abs(rateControl->value() - (30000.0 / 1001.0)) < 0.01,
                              "fractional rate: the field displays the inherited rate, not 29");
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
        const auto id = runDialog(
            *fixture.session,
            [](QDialog& dialog, Expectations& expect) {
                auto* rateControl = frameRateControl(dialog);
                auto* ok = okControl(dialog);
                if (rateControl == nullptr || ok == nullptr) {
                    expect.expect(false, "edited rate: controls exist");
                    dialog.reject();
                    return;
                }
                rateControl->setValue(30.0);
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

// An extreme but valid inherited duration must clamp the displayed frame count instead of invoking
// std::llround out of range.
void testExtremeInheritedDurationClampsSafely(Expectations& expectations) {
    const auto extreme = core::RationalTime::create(std::numeric_limits<std::int64_t>::max(), 1);
    const auto rate = document::FrameRate::create(24, 1);
    if (!extreme.has_value() || !rate.has_value()) {
        expectations.expect(false, "extreme-duration fixture is valid");
        return;
    }
    SeededSession fixture(*extreme, squareFormat(*rate));
    const auto id = runDialog(
        *fixture.session,
        [](QDialog& dialog, Expectations& expect) {
            auto* duration = durationControl(dialog);
            auto* ok = okControl(dialog);
            if (duration == nullptr || ok == nullptr) {
                expect.expect(false, "extreme duration: controls exist");
                dialog.reject();
                return;
            }
            expect.expect(duration->value() >= 1.0 && duration->value() <= 1'000'000.0,
                          "extreme duration: the frame display clamps into the field's range");
            dialog.reject();
        },
        expectations);
    expectations.expect(!id.has_value(), "extreme duration: cancel adds nothing");
    expectations.expect(fixture.document.snapshot().project().compositions().size() == 1,
                        "extreme duration: the seeded project is untouched");
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
    testExtremeInheritedDurationClampsSafely(expectations);

    return expectations.failures() == 0 ? 0 : 1;
}
