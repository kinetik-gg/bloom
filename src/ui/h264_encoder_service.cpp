#include <bloom/ui/h264_encoder_service.hpp>

#include <bloom/ui/task_ui_bridge.hpp>

#include <utility>

namespace bloom::ui {
namespace {

[[nodiscard]] H264EncoderProbe defaultProbe(const output::H264EncoderSupportV1& support) {
    H264EncoderProbe probe;
    probe.vaapiDevice = support.hardwareVaapi && output::vaapiRenderNodeAvailableV1();
    if (support.software == output::H264EncoderV1::OpenH264) {
        const auto status = output::verifyH264RuntimeV1();
        probe.openh264Installed = status.installed;
        probe.detail = status.detail;
    }
    return probe;
}

[[nodiscard]] runtime::TaskRequest request(const char* name) {
    return runtime::TaskRequest(
        name, {.kind = runtime::TaskOwnerKind::Application, .id = runtime::TaskOwnerId::fromRaw(1)},
        runtime::TaskPriority::Foreground, runtime::TaskExecutor::BlockingIo);
}

} // namespace

H264EncoderService::H264EncoderService(runtime::TaskScheduler& scheduler, TaskUiBridge& bridge,
                                       output::H264EncoderSupportV1 support, Probe probe,
                                       Locate locate, QObject* parent)
    : QObject(parent), scheduler_(scheduler), bridge_(bridge),
      probe_(probe ? std::move(probe) : Probe(defaultProbe)),
      locate_(locate ? std::move(locate) : Locate(output::locateH264RuntimeV1)) {
    state_.support = support;
    state_.runtime = support.software == output::H264EncoderV1::OpenH264 ? Runtime::Checking
                                                                         : Runtime::NotNeeded;
    connect(&bridge_, &TaskUiBridge::snapshotsPolled, this, &H264EncoderService::poll);
    connect(&bridge_, &TaskUiBridge::resultsAvailable, this, &H264EncoderService::poll);
}

H264EncoderService::~H264EncoderService() {
    // The workers capture only their own inputs, never this service; the scheduler's quiescence
    // gate retires them at shutdown.
    if (probeTask_)
        probeTask_->cancel();
    if (locateTask_)
        locateTask_->cancel();
}

void H264EncoderService::refresh() {
    if (probeTask_)
        return;
    auto submission = scheduler_.submit<std::shared_ptr<H264EncoderProbe>>(
        request("Check the H.264 encoder"),
        [probe = probe_, support = state_.support](runtime::TaskContext& context) {
            if (context.isCancellationRequested())
                return runtime::TaskResult<std::shared_ptr<H264EncoderProbe>>::cancelled();
            return runtime::TaskResult<std::shared_ptr<H264EncoderProbe>>::succeeded(
                std::make_shared<H264EncoderProbe>(probe(support)));
        });
    if (!submission.accepted())
        return;
    probeTask_.emplace(std::move(submission.handle));
    if (state_.support.software == output::H264EncoderV1::OpenH264 &&
        state_.runtime != Runtime::Installed) {
        state_.runtime = Runtime::Checking;
        emit stateChanged();
    }
    bridge_.wake();
}

bool H264EncoderService::locate(const std::filesystem::path& file) {
    if (locateTask_)
        return false;
    auto submission = scheduler_.submit<std::shared_ptr<output::H264RuntimeAvailabilityV1>>(
        request("Locate OpenH264"), [locate = locate_, file](runtime::TaskContext& context) {
            context.reportProgress({"Verifying OpenH264", file.filename().string(), 0, {}});
            auto result = locate(file, [&context] { return context.isCancellationRequested(); });
            if (context.isCancellationRequested())
                return runtime::TaskResult<
                    std::shared_ptr<output::H264RuntimeAvailabilityV1>>::cancelled();
            return runtime::TaskResult<std::shared_ptr<output::H264RuntimeAvailabilityV1>>::
                succeeded(std::make_shared<output::H264RuntimeAvailabilityV1>(std::move(result)));
        });
    if (!submission.accepted())
        return false;
    locateTask_.emplace(std::move(submission.handle));
    emit stateChanged();
    bridge_.wake();
    return true;
}

bool H264EncoderService::install() {
    if (locateTask_)
        return false;
    auto submission = scheduler_.submit<std::shared_ptr<output::H264RuntimeAvailabilityV1>>(
        request("Install OpenH264"), [](runtime::TaskContext& context) {
            context.reportProgress({"Installing OpenH264", "Cisco binary", 0, {}});
            auto result = output::installH264RuntimeV1(true, [&context](std::uint64_t progress) {
                context.reportProgress({"Installing OpenH264", "Cisco binary", progress, 100});
            });
            if (context.isCancellationRequested())
                return runtime::TaskResult<
                    std::shared_ptr<output::H264RuntimeAvailabilityV1>>::cancelled();
            return runtime::TaskResult<std::shared_ptr<output::H264RuntimeAvailabilityV1>>::
                succeeded(std::make_shared<output::H264RuntimeAvailabilityV1>(std::move(result)));
        });
    if (!submission.accepted())
        return false;
    locateTask_.emplace(std::move(submission.handle));
    emit stateChanged();
    bridge_.wake();
    return true;
}

void H264EncoderService::cancelLocate() {
    if (locateTask_)
        locateTask_->cancel();
    bridge_.wake();
}

void H264EncoderService::poll() {
    if (probeTask_) {
        if (auto result = probeTask_->tryTakeResult()) {
            probeTask_.reset();
            if (result->state() == runtime::TaskState::Succeeded && result->value() &&
                *result->value()) {
                const auto& probe = **result->value();
                state_.vaapiDevice = probe.vaapiDevice;
                state_.detail = QString::fromStdString(probe.detail);
                if (state_.support.software == output::H264EncoderV1::OpenH264 &&
                    state_.runtime != Runtime::Installed)
                    state_.runtime =
                        probe.openh264Installed ? Runtime::Installed : Runtime::NotInstalled;
            } else if (state_.runtime == Runtime::Checking) {
                state_.runtime = Runtime::NotInstalled;
                state_.detail = tr("The H.264 encoder could not be checked.");
            }
            state_.probed = true;
            emit stateChanged();
        }
    }
    if (locateTask_) {
        if (auto result = locateTask_->tryTakeResult()) {
            locateTask_.reset();
            bool installed = false;
            QString detail = tr("Locating OpenH264 was cancelled.");
            if (result->state() == runtime::TaskState::Succeeded && result->value() &&
                *result->value()) {
                const auto& located = **result->value();
                installed = located.installed;
                detail = QString::fromStdString(located.detail);
            } else if (result->state() == runtime::TaskState::Failed) {
                detail = tr("OpenH264 could not be located.");
            }
            if (installed) {
                state_.runtime = Runtime::Installed;
                state_.detail = detail;
            }
            emit stateChanged();
            emit locateFinished(installed, detail);
        }
    }
}

} // namespace bloom::ui
