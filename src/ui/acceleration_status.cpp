#include <bloom/ui/acceleration_status.hpp>

#include <bloom/render/gpu_presentation_types.hpp>
#include <bloom/runtime/gpu_neutral_display_qualification.hpp>
#include <bloom/runtime/gpu_resident_preview_qualification.hpp>

#include <QString>

namespace bloom::ui {
namespace {

namespace rt = bloom::runtime;

[[nodiscard]] QString serviceStateText(const rt::GpuPreviewDisplayServiceState state) {
    switch (state) {
    case rt::GpuPreviewDisplayServiceState::Initializing:
        return QStringLiteral("Initializing");
    case rt::GpuPreviewDisplayServiceState::Ready:
        return QStringLiteral("Ready");
    case rt::GpuPreviewDisplayServiceState::Unavailable:
        return QStringLiteral("Unavailable");
    case rt::GpuPreviewDisplayServiceState::Stopping:
        return QStringLiteral("Stopping");
    case rt::GpuPreviewDisplayServiceState::Stopped:
        return QStringLiteral("Stopped");
    }
    return QStringLiteral("Unavailable");
}

[[nodiscard]] QString presentationText(const rt::GpuPreviewDisplayServiceStatus& status) {
    using render::GpuPresentationAvailability;
    switch (status.presentationAvailability) {
    case GpuPresentationAvailability::NotRequested:
        return QStringLiteral("Not requested");
    case GpuPresentationAvailability::Ready:
        return status.presentationClient != nullptr ? QStringLiteral("Ready")
                                                    : QStringLiteral("Ready (no service client)");
    case GpuPresentationAvailability::Unavailable:
        break;
    }
    return QStringLiteral("Unavailable");
}

} // namespace

AccelerationStatus cpuOnlyAccelerationStatus() {
    AccelerationStatus status;
    status.backend = QStringLiteral("CPU reference");
    status.deviceState = QStringLiteral("Unavailable");
    status.previewRoute = QStringLiteral("CPU fallback");
    status.presentationStatus = QStringLiteral("Not requested");
    status.summary = QStringLiteral(
        "GPU acceleration is unavailable. The CPU reference evaluator is active and deterministic "
        "output is unaffected.");
    return status;
}

AccelerationStatus
accelerationStatusFromServiceStatus(const rt::GpuPreviewDisplayServiceStatus& status) {
    AccelerationStatus out;
    out.deviceState = serviceStateText(status.state);

    const auto& resident = status.residentQualification;
    const auto& packed = status.qualification;
    const bool residentEligible = resident != nullptr && resident->eligible();

    if (status.state == rt::GpuPreviewDisplayServiceState::Ready) {
        // Ready is a capability: the device/pipeline bootstrap passed. It is never a claim that any
        // Viewer is currently presenting.
        out.backend = QStringLiteral("Vulkan");
    } else if (status.state == rt::GpuPreviewDisplayServiceState::Initializing) {
        out.backend = QStringLiteral("Vulkan (initializing)");
    } else {
        out.backend = QStringLiteral("CPU reference");
    }

    // Device identity comes only from a genuine immutable qualification report; a missing report
    // leaves it empty rather than inventing a device.
    if (resident != nullptr) {
        out.deviceName = QString::fromStdString(resident->deviceIdentity().device_name);
        out.driver = QString::fromStdString(resident->deviceIdentity().driver);
    } else if (packed != nullptr) {
        out.deviceName = QString::fromStdString(packed->deviceIdentity().device_name);
        out.driver = QString::fromStdString(packed->deviceIdentity().driver);
    }

    out.presentationStatus = presentationText(status);

    if (residentEligible) {
        if (const auto& interval = resident->eligibleInterval(); interval.has_value()) {
            out.operationStatus << QStringLiteral(
                                       "Bloom Neutral v1 (PreviewOnly): eligible for previews "
                                       "of %1-%2 pixels")
                                       .arg(interval->min_pixels)
                                       .arg(interval->max_pixels);
        }
    }

    const bool presentationUsable =
        status.presentationAvailability == render::GpuPresentationAvailability::Ready &&
        status.presentationClient != nullptr;
    const bool routeAvailable = status.state == rt::GpuPreviewDisplayServiceState::Ready &&
                                residentEligible && presentationUsable;
    out.previewRoute = routeAvailable ? QStringLiteral("Available for eligible previews")
                                      : QStringLiteral("CPU fallback");

    switch (status.state) {
    case rt::GpuPreviewDisplayServiceState::Initializing:
        out.summary = QStringLiteral(
            "Checking GPU acceleration. Previews use the CPU reference evaluator until it is "
            "ready.");
        break;
    case rt::GpuPreviewDisplayServiceState::Ready:
        if (!residentEligible) {
            out.summary = QStringLiteral(
                "GPU acceleration is available, but no eligible resident preview route is "
                "qualified on this device. Previews use the CPU reference evaluator.");
        } else if (!presentationUsable) {
            out.summary = QStringLiteral(
                "GPU acceleration is available, but presentation is not. Previews use the CPU "
                "reference evaluator.");
        } else {
            out.summary = QStringLiteral(
                "GPU acceleration is available for eligible previews; other operations fall back "
                "to the CPU reference evaluator.");
        }
        break;
    case rt::GpuPreviewDisplayServiceState::Stopping:
    case rt::GpuPreviewDisplayServiceState::Stopped:
        out.summary = QStringLiteral(
            "GPU acceleration is shutting down. Previews use the CPU reference evaluator.");
        break;
    case rt::GpuPreviewDisplayServiceState::Unavailable:
        if (status.diagnostic.code == rt::GpuPreviewDisplayServiceDiagnosticCode::Disabled) {
            out.summary = QStringLiteral(
                "GPU acceleration is disabled. The CPU reference evaluator is active and "
                "deterministic output is unaffected.");
        } else if (status.diagnostic.code ==
                   rt::GpuPreviewDisplayServiceDiagnosticCode::LoaderUnavailable) {
            out.summary = QStringLiteral(
                "The GPU acceleration loader is unavailable. The CPU reference evaluator is active "
                "and deterministic output is unaffected.");
        } else {
            out.summary = QStringLiteral(
                "GPU acceleration is unavailable. The CPU reference evaluator is active and "
                "deterministic output is unaffected.");
            if (!status.diagnostic.message.empty()) {
                out.summary +=
                    QStringLiteral(" (%1)").arg(QString::fromStdString(status.diagnostic.message));
            }
        }
        break;
    }
    return out;
}

AccelerationStatus CpuOnlyAccelerationStatus::accelerationStatus() const {
    return cpuOnlyAccelerationStatus();
}

void CachedAccelerationStatusProvider::setServiceStatus(
    const rt::GpuPreviewDisplayServiceStatus& status) {
    cached_ = accelerationStatusFromServiceStatus(status);
}

AccelerationStatus CachedAccelerationStatusProvider::accelerationStatus() const { return cached_; }

} // namespace bloom::ui
