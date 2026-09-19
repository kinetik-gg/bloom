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

// Local copies of the two tiny page helpers. The performance page is its own owning translation
// unit so the dialog construction file stays under the size budget; these are presentation-only
// and duplicate no behavior.
[[nodiscard]] QLabel* makeValueLabel(const QString& text, QWidget* parent) {
    auto* label = properties::makeReadOnlyValueLabel(kit::TypeRole::Value, parent);
    label->setText(text);
    return label;
}

[[nodiscard]] QScrollArea* wrapInScroll(QWidget* content, const QString& objectName) {
    auto* scroll = new QScrollArea;
    scroll->setObjectName(objectName);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setWidget(content);
    return scroll;
}

constexpr int kPerformanceRefreshIntervalMs = 250;

} // namespace

QWidget* PreferencesWindow::buildPerformancePage() {
    auto* content = new QWidget;
    auto* layout = new QVBoxLayout(content);
    layout->setContentsMargins(kit::px(kit::Spacing::M), kit::px(kit::Spacing::M),
                               kit::px(kit::Spacing::M), kit::px(kit::Spacing::M));
    layout->setSpacing(kit::px(kit::Spacing::S));

    auto* section = properties::makeSection(
        layout, content, QStringLiteral("preferencesPerformanceSection"), tr("Acceleration"),
        QStringLiteral("preferences/sections/performance/collapsed"));
    auto* rows = section->bodyLayout();

    // Each row is built once and updated in place; the Device/Driver rows exist from the start and
    // are shown/hidden as the cached status changes, so a device that appears after Initializing is
    // still reported without rebuilding the page. The value label and its row carry distinct,
    // explicit object names so a test can find either one.
    const auto addStatusRow = [&](const QString& valueObjectName, const QString& rowObjectName,
                                  const QString& label, QWidget** const rowOut) -> QLabel* {
        auto* valueLabel = makeValueLabel(QString{}, content);
        valueLabel->setObjectName(valueObjectName);
        QWidget* const row = properties::addRow(
            rows, content, properties::makeRowLabel(label, content), nullptr, valueLabel);
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
    performanceRouteValue_ = addStatusRow(QStringLiteral("preferencesPerformanceRouteValue"),
                                          QStringLiteral("preferencesPerformanceRouteRow"),
                                          tr("Available preview route"), nullptr);
    performancePresentationValue_ = addStatusRow(
        QStringLiteral("preferencesPerformancePresentationValue"),
        QStringLiteral("preferencesPerformancePresentationRow"), tr("Presentation"), nullptr);
    performanceDeviceValue_ = addStatusRow(QStringLiteral("preferencesPerformanceDeviceValue"),
                                           QStringLiteral("preferencesPerformanceDeviceRow"),
                                           tr("Device"), &performanceDeviceRow_);
    performanceDriverValue_ = addStatusRow(QStringLiteral("preferencesPerformanceDriverValue"),
                                           QStringLiteral("preferencesPerformanceDriverRow"),
                                           tr("Driver"), &performanceDriverRow_);

    performanceOperations_ = new kit::KLabel(content);
    performanceOperations_->setObjectName(QStringLiteral("preferencesPerformanceOperationValue"));
    performanceOperations_->setWordWrap(true);
    rows->addWidget(performanceOperations_);

    performanceSummary_ = new kit::KLabel(content);
    performanceSummary_->setObjectName(QStringLiteral("preferencesPerformanceSummary"));
    performanceSummary_->setWordWrap(true);
    rows->addWidget(performanceSummary_);

    auto* note = new kit::KLabel(
        tr("GPU execution is an acceleration over the same evaluation; the CPU reference remains "
           "the correctness oracle."),
        content);
    note->setObjectName(QStringLiteral("preferencesPerformanceNote"));
    note->setWordWrap(true);
    layout->addWidget(note);

    layout->addStretch(1);

    performanceRefreshTimer_ = new QTimer(this);
    performanceRefreshTimer_->setObjectName(QStringLiteral("preferencesPerformanceRefreshTimer"));
    performanceRefreshTimer_->setInterval(kPerformanceRefreshIntervalMs);
    // The timer is parented to the dialog and only runs while the dialog is visible. The provider
    // read is a cheap cached getter, never a device query.
    connect(performanceRefreshTimer_, &QTimer::timeout, this, [this] { refreshPerformancePage(); });

    refreshPerformancePage();
    return wrapInScroll(content, QStringLiteral("preferencesPerformancePage"));
}

void PreferencesWindow::refreshPerformancePage() {
    if (performanceBackendValue_ == nullptr) {
        return;
    }
    const auto status = accelerationStatus_ != nullptr ? accelerationStatus_->accelerationStatus()
                                                       : cpuOnlyAccelerationStatus();

    performanceBackendValue_->setText(status.backend);
    performanceStateValue_->setText(status.deviceState);
    performanceRouteValue_->setText(status.previewRoute);
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

    if (performanceOperations_ != nullptr) {
        performanceOperations_->setText(status.operationStatus.join(QStringLiteral("\n")));
        performanceOperations_->setVisible(!status.operationStatus.isEmpty());
    }
    performanceSummary_->setText(status.summary);
}

void PreferencesWindow::showEvent(QShowEvent* event) {
    QDialog::showEvent(event);
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
    QDialog::hideEvent(event);
}

} // namespace bloom::ui
