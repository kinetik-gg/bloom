#include "preferences_window.hpp"

#include "properties_sections.hpp"

#include <bloom/ui/acceleration_status.hpp>
#include <bloom/ui/kit/controls.hpp>
#include <bloom/ui/kit/section.hpp>
#include <bloom/ui/kit/tokens.hpp>

#include <QFrame>
#include <QHideEvent>
#include <QLabel>
#include <QScrollArea>
#include <QShowEvent>
#include <QTimer>
#include <QVBoxLayout>
#include <QWidget>

namespace bloom::ui {
namespace {

// A local copy of the tiny value-label helper. The performance page is its own owning translation
// unit so the dialog construction file stays under the size budget; it is presentation-only
// and duplicates no behavior.
[[nodiscard]] QLabel* makeValueLabel(const QString& text, QWidget* parent) {
    auto* label = properties::makeReadOnlyValueLabel(kit::TypeRole::Value, parent);
    label->setText(text);
    return label;
}

constexpr int kPerformanceRefreshIntervalMs = 250;

} // namespace

QWidget* PreferencesWindow::buildPerformancePage() {
    auto* content = new QWidget;
    auto* layout = new QVBoxLayout(content);
    setPageMargins(*layout);

    auto* section = properties::makeSection(
        layout, content, QStringLiteral("preferencesPerformanceSection"), tr("Acceleration"),
        QStringLiteral("preferences/sections/performance/collapsed"));
    section->setBodyPadding(kit::px(kit::Spacing::XS));
    // Read-only: nothing to reset.
    section->setResetEnabled(false);
    auto* rows = section->bodyLayout();

    // Each row is built once and updated in place; the Device/Driver rows exist from the start and
    // are shown/hidden as the cached status changes, so a device that appears after Initializing is
    // still reported without rebuilding the page. The value label and its row carry distinct,
    // explicit object names so a test can find either one.
    const auto addStatusRow = [&](const QString& valueObjectName, const QString& rowObjectName,
                                  const QString& label, QWidget** const rowOut) -> QLabel* {
        auto* valueLabel = makeValueLabel(QString{}, content);
        valueLabel->setObjectName(valueObjectName);
        QWidget* const row = addPreferencesRow(rows, content, label, {valueLabel});
        row->setObjectName(rowObjectName);
        if (rowOut != nullptr) {
            *rowOut = row;
        }
        return valueLabel;
    };

    performanceBackendValue_ =
        addStatusRow(QStringLiteral("preferencesPerformanceBackendValue"),
                     QStringLiteral("preferencesPerformanceBackendRow"), tr("Backend"), nullptr);
    performanceStateValue_ =
        addStatusRow(QStringLiteral("preferencesPerformanceStateValue"),
                     QStringLiteral("preferencesPerformanceStateRow"), tr("Device state"), nullptr);
    performancePresentationValue_ = addStatusRow(
        QStringLiteral("preferencesPerformancePresentationValue"),
        QStringLiteral("preferencesPerformancePresentationRow"), tr("Presentation"), nullptr);
    performanceDeviceValue_ = addStatusRow(QStringLiteral("preferencesPerformanceDeviceValue"),
                                           QStringLiteral("preferencesPerformanceDeviceRow"),
                                           tr("Device"), &performanceDeviceRow_);
    performanceDriverValue_ = addStatusRow(QStringLiteral("preferencesPerformanceDriverValue"),
                                           QStringLiteral("preferencesPerformanceDriverRow"),
                                           tr("Driver"), &performanceDriverRow_);

    layout->addStretch(1);

    performanceRefreshTimer_ = new QTimer(this);
    performanceRefreshTimer_->setObjectName(QStringLiteral("preferencesPerformanceRefreshTimer"));
    performanceRefreshTimer_->setInterval(kPerformanceRefreshIntervalMs);
    // The timer is parented to the dialog and only runs while the dialog is visible. The provider
    // read is a cheap cached getter, never a device query.
    connect(performanceRefreshTimer_, &QTimer::timeout, this, [this] { refreshPerformancePage(); });

    refreshPerformancePage();
    return wrapPage(content, QStringLiteral("preferencesPerformancePage"));
}

void PreferencesWindow::refreshPerformancePage() {
    if (performanceBackendValue_ == nullptr) {
        return;
    }
    const auto status = accelerationStatus_ != nullptr ? accelerationStatus_->accelerationStatus()
                                                       : cpuOnlyAccelerationStatus();

    performanceBackendValue_->setText(status.backend);
    performanceStateValue_->setText(status.deviceState);
    performancePresentationValue_->setText(status.presentationStatus);

    const bool hasDevice = !status.deviceName.isEmpty();
    performanceDeviceValue_->setText(status.deviceName);
    if (performanceDeviceRow_ != nullptr) {
        performanceDeviceRow_->setVisible(hasDevice);
    }
    const bool hasDriver = !status.driver.isEmpty();
    performanceDriverValue_->setText(status.driver);
    if (performanceDriverRow_ != nullptr) {
        performanceDriverRow_->setVisible(hasDriver);
    }
}

void PreferencesWindow::showEvent(QShowEvent* event) {
    fitPagesToContent();
    kit::KDialog::showEvent(event);
    if (performanceRefreshTimer_ == nullptr) {
        return;
    }
    refreshPerformancePage();
    performanceRefreshTimer_->start();
}

void PreferencesWindow::hideEvent(QHideEvent* event) {
    if (performanceRefreshTimer_ != nullptr) {
        performanceRefreshTimer_->stop();
    }
    kit::KDialog::hideEvent(event);
}

} // namespace bloom::ui
