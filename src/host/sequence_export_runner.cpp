#include <algorithm>
#include <atomic>
#include <bloom/host/export_capacity_wait.hpp>
#include <bloom/host/sequence_export_runner.hpp>
#include <bloom/output/composition_output_stream.hpp>
#include <bloom/runtime/cpu_composition_evaluator.hpp>
#include <condition_variable>
#include <exception>
#include <iostream>
#include <mutex>
#include <thread>

namespace bloom::host {
using namespace media::provider;
namespace {
using Stage = SequenceExportStageV1;
using Work = detail::SequenceExportWorkV1;
// Every stage uses the request's owner (the export queue run's TaskOwner).
void require(bool condition, const char* detail, Error code = Error::InvalidValue) {
    if (!condition)
        throw Unavailable{code, detail};
}
void checked(const std::optional<Unavailable>& error) {
    if (error)
        throw Unavailable{error->reason, error->detail};
}
template <typename T> T checked(Result<T> result) {
    if (const auto* error = std::get_if<Unavailable>(&result))
        throw Unavailable{error->reason, error->detail};
    return std::get<T>(std::move(result));
}
template <typename T> T required(std::optional<T> value, const char* message) {
    if (!value)
        throw Unavailable{Error::InvalidValue, message};
    return std::move(*value);
}
} // namespace

std::shared_ptr<output::ExportResourceReservationV1> detail::reserveSequenceExportQueueV1(
    output::ExportResourceLedgerV1& ledger, const std::uint64_t bytes,
    const std::optional<std::uint64_t> explicitJobLimit,
    std::shared_ptr<GpuExportProvider> provider, const runtime::CancellationToken& cancellation,
    output::ExportResourceAdmissionStatusV1& status) noexcept {
    if (explicitJobLimit && bytes > *explicitJobLimit) {
        status = output::ExportResourceAdmissionStatusV1::JobAllowanceExceeded;
        return {};
    }
    if (!provider) {
        status = output::ExportResourceAdmissionStatusV1::CapacityRefreshFailed;
        return {};
    }
    try {
        provider->refreshProcessingCapacity(cancellation);
        if (cancellation.isCancellationRequested()) {
            status = output::ExportResourceAdmissionStatusV1::CapacityRefreshFailed;
            return {};
        }
        auto policy = provider->processingPhasePolicy();
        if (!policy) {
            status = output::ExportResourceAdmissionStatusV1::ServiceAllowanceExceeded;
            return {};
        }
        output::ExportCapacityRefreshV1 refresh =
            [provider = std::move(provider)](const runtime::CancellationToken& token) {
                provider->refreshProcessingCapacity(token);
            };
        return ledger.reserve(bytes, status, std::nullopt, std::move(policy), 0,
                              std::move(refresh));
    } catch (...) {
        status = output::ExportResourceAdmissionStatusV1::CapacityRefreshFailed;
        return {};
    }
}

struct detail::SequenceExportWorkV1 {
    SequenceExportRequestV1 request;
    // Only the provider synthesized for a CPU-only sequence belongs to this runner.
    bool ownsDefaultGpuProvider = false;
    // UI controllers can retire while an admitted task still runs. Its ledger facade must remain
    // alive until that task or the independent cleanup lane releases the last reservation.
    std::shared_ptr<output::ExportResourceLedgerV1> ledgerLifetime;
    std::shared_ptr<const runtime::CompiledCompositionPlan> plan;
    std::unique_ptr<output::MediaOutputAnalysisV1> analysis;
    std::shared_ptr<output::ExportResourceReservationV1> queueReservation;
    // ADR 0033 section 9: the frame queue reservation was refused because the shared processing
    // root is held by other work; the owner waits and retries reserveQueue().
    bool queueAwaitingCapacity = false;
    std::uint64_t queueBytes = 0;
    std::unique_ptr<output::CompositionOutputStreamV1> encoder;
    std::unique_ptr<FrameExportRequestV1> approvedFrame;
    std::atomic<bool> cancelled{false};
    std::mutex ownership;
    std::condition_variable ownershipReady;
    bool active = false, abandoned = false;
    runtime::CancellationToken taskCancellation;
    std::optional<Unavailable> error;
    std::uint64_t encoded = 0;
    core::Sha256Hasher approvals;
    std::uint64_t gpuEvaluatedFrames = 0;
    std::uint64_t gpuNativeDispatches = 0;
    std::uint64_t gpuReadbacks = 0;
    std::uint64_t gpuReadbackSubmissions = 0;
    std::uint64_t gpuTransferredPayloads = 0;
    std::uint64_t gpuProcessPayloadBytes = 0;
    std::uint64_t gpuEncodedPayloadBytes = 0;
    std::uint64_t gpuDeviceOwnershipEpoch = 0;
    std::unique_ptr<output::OutputAnalysisAttemptTargetV1> target;
    std::optional<SequenceExportResultV1> result;
    // One final-render evaluator per range: its verified source revisions and video sessions are
    // reused by every frame's CPU fallback. Released with the work on the retirement owner.
    std::shared_ptr<const runtime::CpuCompositionEvaluator> fallbackEvaluator =
        std::make_shared<const runtime::CpuCompositionEvaluator>();
    explicit SequenceExportWorkV1(SequenceExportRequestV1 r,
                                  std::shared_ptr<output::ExportResourceLedgerV1> lifetime)
        : request(std::move(r)), ledgerLifetime(std::move(lifetime)) {}
    runtime::EvaluationColorIntent colorIntent() const {
        const auto* composition = request.composition.snapshot.project().findComposition(
            request.composition.compositionId);
        return {.workingColorSpaceId = composition && composition->workingColorSpaceId()
                                           ? std::string_view(*composition->workingColorSpaceId())
                                           : std::string_view(request.workingColorSpaceId),
                .ocioConfigRevision = request.ocioConfigRevision,
                .ocioConfigUri = request.ocioConfigUri};
    }
    bool isCancelled() const { return cancelled.load(); }
    void checkpoint() const {
        require(!isCancelled() && !taskCancellation.isCancellationRequested(),
                "Composition export cancelled", Error::Cancelled);
    }
    void clean() {
        encoder.reset();
        approvedFrame.reset();
        queueReservation.reset();
    }
    const EncodeSettingsV1& settings() const {
        require(analysis != nullptr, "Missing output analysis");
        return analysis->settings;
    }
    std::uint64_t total() const { return request.range.lastFrame - request.range.firstFrame + 1; }
    std::uint64_t samplesThrough(std::uint64_t frames) const {
        const auto& s = settings();
        return frames * s.sampleRate * static_cast<std::uint64_t>(s.rate.denominator) /
               static_cast<std::uint64_t>(s.rate.numerator);
    }
    void prepare(const runtime::SnapshotCompiler& compiler, output::ExportResourceLedgerV1& ledger,
                 runtime::TaskContext& context) {
        checkpoint();
        const auto& range = request.range;
        require(range.lastFrame >= range.firstFrame && range.lastFrame < Limits::indexEntries &&
                    total() <= Limits::indexEntries &&
                    FrameRangeRunnerV1::timeForFrame(range, range.firstFrame).has_value() &&
                    FrameRangeRunnerV1::timeForFrame(range, range.lastFrame).has_value(),
                "Invalid composition frame range");
        auto compiled = compiler.compile(request.composition, context.cancellation());
        checkpoint();
        require(compiled.plan != nullptr, "Composition could not be compiled");
        plan = std::move(compiled.plan);
        require(plan->duration() == range.duration && plan->format().frameRate() == range.frameRate,
                "Range timebase differs from the captured composition");
        EncodeSettingsV1 s;
        s.profile = request.profile;
        s.width = plan->format().width();
        s.height = plan->format().height();
        s.rate = {range.frameRate.numerator(), range.frameRate.denominator()};
        require(s.rate.numerator <= 1000000 && s.rate.denominator <= 1000000,
                "Composition cadence exceeds provider limits", Error::Oversized);
        s.frames = total();
        s.sampleRate = request.sampleRate;
        s.bwfDescription = request.bwfDescription;
#if defined(__APPLE__)
        // macOS exports through the AVAssetWriter/VideoToolbox provider: MOV/MP4 video (ProRes or
        // H.264) with PCM or AAC audio. WAV, MXF and TIFF need the FFmpeg worker and are refused
        // here; outputPresetAvailabilityV1() reports them unavailable in the UI too.
        if (request.preset == output::OutputPresetV1::ProResMovV1) {
            s.videoCodec = "prores";
            s.container = "mov";
            s.profile = request.profile.empty() ? "hq" : request.profile;
        } else if (request.preset == output::OutputPresetV1::H264MovV1 ||
                   request.preset == output::OutputPresetV1::H264Mp4V1) {
            s.videoCodec = "h264";
            s.container = request.preset == output::OutputPresetV1::H264Mp4V1 ? "mp4" : "mov";
            s.profile = "high";
        } else {
            require(false, "This export preset needs the FFmpeg worker, unavailable on macOS",
                    Error::Unavailable);
        }
#else
        if (request.preset == output::OutputPresetV1::HevcMovV1 ||
            request.preset == output::OutputPresetV1::HevcMp4V1) {
            // HEVC is hardware-only (ADR 0033): VA-API, never a software encoder.
            require(request.hardware, "HEVC needs a VA-API render node with HEVC encode",
                    Error::Unavailable);
            s.videoCodec = "hevc";
            s.container = request.preset == output::OutputPresetV1::HevcMp4V1 ? "mp4" : "mov";
            s.profile = request.profile == "main10" ? "main10" : "main";
            request.worker.vaapi = true;
        } else if (request.preset == output::OutputPresetV1::H264MovV1 ||
                   request.preset == output::OutputPresetV1::H264Mp4V1) {
            s.videoCodec = "h264";
            s.container = request.preset == output::OutputPresetV1::H264Mp4V1 ? "mp4" : "mov";
            s.profile = "high";
            request.worker.vaapi = request.hardware;
            if (!request.hardware) {
                const auto status = output::verifyH264RuntimeV1();
                if (!status.installed) {
                    require(request.openh264Consent, "H.264 encoder not installed",
                            Error::Unavailable);
                    const auto installed =
                        output::installH264RuntimeV1(true, [&](const std::uint64_t progress) {
                            context.reportProgress(
                                {"Installing OpenH264", "Cisco binary", progress, 100});
                            checkpoint();
                        });
                    require(installed.installed, installed.detail.c_str(), Error::Unavailable);
                    request.worker.openh264Directory = installed.directory.string();
                    request.worker.openh264Version = installed.version;
                    request.worker.openh264Digest = installed.digest;
                } else {
                    request.worker.openh264Directory = status.directory.string();
                    request.worker.openh264Version = status.version;
                    request.worker.openh264Digest = status.digest;
                }
            }
        } else if (request.preset == output::OutputPresetV1::DnxhrMxfV1) {
            s.videoCodec = "dnxhd";
            s.container = "mxf";
        } else if (request.preset == output::OutputPresetV1::PcmWavV1) {
            s.videoCodec.clear();
            s.profile = request.pcmCodec;
            s.container = "wav";
            s.frames = 0;
        }
#endif
        if (request.audio || request.preset == output::OutputPresetV1::PcmWavV1) {
            s.audioCodec = request.pcmCodec;
            require(s.sampleRate > 0 && s.sampleRate <= Limits::sampleRate,
                    "Invalid source audio sample rate");
            s.audioSamples = total() * s.sampleRate *
                             static_cast<std::uint64_t>(s.rate.denominator) /
                             static_cast<std::uint64_t>(s.rate.numerator);
        }
        // Issue #369: multithreaded encode (automatic thread count), the destination-derived
        // byte limit, the requested bit rate, the probed render node and colour tags derived from
        // the output colour intent.
        s.threads = 0;
        s.videoBitrateKbps = request.videoBitrateKbps.value_or(0);
        s.vaapiDevice = request.vaapiDevice;
        if (request.artifactByteLimit)
            s.byteLimit =
                std::min(s.byteLimit, std::max<std::uint64_t>(1, *request.artifactByteLimit));
        const auto display = output::PreparedOutputDisplayV1::prepare(
            colorIntent(), request.displayName, request.viewName);
        if (display) {
            const auto tags = output::outputColourTagsV1(display->displayName());
            s.colour = {tags.primaries, tags.transfer, tags.matrix, tags.range};
        }
        require(request.preset == output::OutputPresetV1::PcmWavV1 || display != nullptr,
                "Output display processor unavailable", Error::Unavailable);
        analysis =
            std::make_unique<output::MediaOutputAnalysisV1>(checked(output::analyzeMediaOutputV1(
                request.preset, std::move(s), display, output::outputLookEffectCountV1(*plan))));
#if defined(__APPLE__)
        if (request.preset == output::OutputPresetV1::H264MovV1 ||
            request.preset == output::OutputPresetV1::ProResMovV1)
            analysis->implementationNote += "; encoder=videotoolbox";
#else
        if (request.preset == output::OutputPresetV1::H264MovV1) {
            analysis->implementationNote +=
                request.hardware ? "; encoder=vaapi vaapi-runtime-unqualified"
                                 : "; encoder=openh264 " + request.worker.openh264Version +
                                       " sha256=" + request.worker.openh264Digest;
        }
#endif
        // Prepared frame, outbound protocol buffer and transport copy; one slot by construction.
        const auto bytes = static_cast<std::uint64_t>(settings().width) * settings().height * 24U +
                           std::uint64_t{8} * kEncodeChunkBytes;
        require(!request.queueByteLimit || bytes <= *request.queueByteLimit,
                "Frame queue byte budget exceeded", Error::Oversized);
        queueBytes = bytes;
        reserveQueue(ledger, context);
    }

    void reserveQueue(output::ExportResourceLedgerV1& ledger, runtime::TaskContext& context) {
        output::ExportResourceAdmissionStatusV1 status{};
        queueAwaitingCapacity = false;
        queueReservation = detail::reserveSequenceExportQueueV1(
            ledger, queueBytes, request.queueByteLimit, request.gpuProvider, context.cancellation(),
            status);
        if (!queueReservation &&
            status == output::ExportResourceAdmissionStatusV1::ServiceAllowanceExceeded &&
            request.gpuProvider) {
            // Wait only while other work holds the shared root; with nothing else reserved the
            // queue can never fit and the export fails below.
            if (otherProcessingPhasesAdmittedV1(request.gpuProvider->processingPhasePolicy().get(),
                                                0)) {
                queueAwaitingCapacity = true;
                return;
            }
        }
        require(queueReservation != nullptr,
                "This export needs more memory than this computer can give it, so it stopped. "
                "Try a smaller resolution.",
                Error::Oversized);
    }

    void encode(FrameExportRequestV1& approved, runtime::TaskContext& context) {
        checkpoint();
        const auto& attempt = *approved.attempt();
        if (!target)
            target = std::make_unique<output::OutputAnalysisAttemptTargetV1>(attempt.target());
        if (!encoder) {
            const auto origin =
                required(FrameRangeRunnerV1::timeForFrame(request.range, request.range.firstFrame),
                         "Missing exact audio origin");
            encoder = std::make_unique<output::CompositionOutputStreamV1>(
                output::CompositionOutputSourceV1{request.composition.snapshot, plan, origin,
                                                  request.assetBaseDirectory, analysis->display},
                settings(), request.worker, queueReservation,
                [this] { return isCancelled() || taskCancellation.isCancellationRequested(); });
        }
        const auto digest = required(attempt.digest(), "Frame approval digest missing");
        (void)approvals.update(std::as_bytes(std::span(digest.bytes())));
        checked(encoder->writeFrame(attempt.frame()->processImage(), encoded,
                                    settings().audioCodec.empty() ? 0 : samplesThrough(encoded + 1),
                                    context));
        ++encoded;
        context.reportProgress({"Encoding composition", "", encoded, total()});
    }
    void publish(FrameExportRequestV1& approved, platform::StagedArtifactCoordinator& artifacts,
                 runtime::TaskContext& context) {
        checkpoint();
        context.reportProgress({"Verifying composition", "Close and reopen", encoded, total()});
        const auto qc = checked(encoder->finish());
        require(qc.frames == settings().frames && qc.audioSamples == settings().audioSamples &&
                    qc.bytes <= settings().byteLimit,
                "Worker verification does not match approved stream layout",
                Error::IdentityMismatch);
        require(target != nullptr, "Missing publication target");
        auto preflight =
            artifacts.preflight({target->targetPath, target->overwritePolicy, target->observation});
        require(static_cast<bool>(preflight), "Export target changed or is unavailable", Error::Io);
        require(preflight.target()->targetKey() == target->targetKey,
                "Export target identity changed", Error::IdentityMismatch);
        auto staging = artifacts.stage(std::move(preflight).takeTarget());
        require(static_cast<bool>(staging), "Cannot stage composition export", Error::Io);
        auto lease = std::move(staging).takeLease();
        core::Sha256Hasher transferred;
        for (std::uint64_t offset = 0; offset < qc.bytes;) {
            checkpoint();
            const auto chunk = checked(encoder->read(offset));
            require(chunk.bytes.size() <= qc.bytes - offset && transferred.update(chunk.bytes) &&
                        static_cast<bool>(lease.write(chunk.bytes)),
                    "Encoded staging copy failed", Error::Io);
            offset += chunk.bytes.size();
        }
        checked(encoder->close());
        encoder.reset();
        require(transferred.finalize() == qc.artifact && static_cast<bool>(lease.finishWriting()),
                "Encoded artifact changed during transfer", Error::DigestMismatch);
        Bytes buffer(kEncodeChunkBytes);
        core::Sha256Hasher reopened;
        for (std::uint64_t offset = 0; offset < qc.bytes;) {
            checkpoint();
            const auto read = lease.readForVerification(offset, buffer);
            require(read && read.bytesRead > 0 && read.bytesRead <= qc.bytes - offset &&
                        reopened.update(std::span(buffer).first(read.bytesRead)),
                    "Staged artifact reopen failed", Error::Io);
            offset += read.bytesRead;
        }
        require(reopened.finalize() == qc.artifact && static_cast<bool>(lease.acceptVerification()),
                "Staged artifact digest differs", Error::DigestMismatch);
        auto evidence = checked(output::makeMediaQcEvidenceV1(*analysis, qc, approvals.finalize()));
        checkpoint();
        auto guardResult = FrameExportRequestAccessV1::claim(approved).tryEnterPublication();
        SequenceExportResultV1 outcome;
        outcome.evidence = std::move(evidence);
        outcome.expectedColourTags =
            settings().videoCodec.empty() ? ColourTags{} : settings().colour;
        const auto& codec =
            settings().videoCodec.empty() ? settings().audioCodec : settings().videoCodec;
        outcome.expectedContainer =
            settings().container + "/" + (codec == "prores_ks" ? std::string("prores") : codec);
        outcome.encodedFrames = encoded;
        outcome.gpuEvaluatedFrames = gpuEvaluatedFrames;
        outcome.gpuNativeDispatches = gpuNativeDispatches;
        outcome.gpuReadbacks = gpuReadbacks;
        outcome.gpuReadbackSubmissions = gpuReadbackSubmissions;
        outcome.gpuTransferredPayloads = gpuTransferredPayloads;
        outcome.gpuProcessPayloadBytes = gpuProcessPayloadBytes;
        outcome.gpuEncodedPayloadBytes = gpuEncodedPayloadBytes;
        outcome.gpuDeviceOwnershipEpoch = gpuDeviceOwnershipEpoch;
        if (guardResult.status() == PublicationGuardStatus::Entered) {
            auto guard = std::move(guardResult).takeGuard();
            outcome.publication = lease.publish(platform::PublicationDisposition::Proceed);
        } else if (guardResult.status() == PublicationGuardStatus::Superseded)
            outcome.publication = lease.publish(platform::PublicationDisposition::Superseded);
        else
            throw Unavailable{Error::Busy, "Publication guard refused composition export"};
        result = std::move(outcome);
        clean();
    }
};
struct SequenceExportRetirementOwnerV1::Ticket {
    std::shared_ptr<Work> work;
    std::unique_ptr<Ticket> next;
};

struct SequenceExportRetirementOwnerV1::State {
    static constexpr std::size_t kMaxLiveRunners = 4;
    mutable std::mutex mutex;
    std::condition_variable ready;
    std::unique_ptr<Ticket> first;
    Ticket* last = nullptr;
    std::thread worker;
    TestGate gate;
    std::size_t liveRunners = 0;
    bool closing = false;
    bool exited = false;
};

SequenceExportRetirementOwnerV1::SequenceExportRetirementOwnerV1(TestGate gate)
    : state_(std::make_unique<State>()) {
    auto* const state = state_.get();
    state->gate = std::move(gate);
    state->worker = std::thread([state] {
        for (;;) {
            std::unique_ptr<Ticket> ticket;
            {
                std::unique_lock lock(state->mutex);
                state->ready.wait(lock, [&] {
                    return state->first != nullptr || (state->closing && state->liveRunners == 0);
                });
                if (!state->first) {
                    state->exited = true;
                    return;
                }
                ticket = std::move(state->first);
                state->first = std::move(ticket->next);
                if (!state->first)
                    state->last = nullptr;
            }
            // This may close an encoder and reap its child process. It never runs on the authoring
            // thread, even when the ordinary scheduler has refused all further task admission.
            if (state->gate.beforeWorkCleanup)
                state->gate.beforeWorkCleanup();
            {
                std::unique_lock lock(ticket->work->ownership);
                ticket->work->ownershipReady.wait(lock, [&] { return !ticket->work->active; });
            }
            ticket->work->clean();
            if (ticket->work->ownsDefaultGpuProvider) {
                auto provider = std::move(ticket->work->request.gpuProvider);
                // This neutral retirement lane may wait; the authoring thread cannot. Keep the
                // provider and its host owner alive until both workers have really exited.
                while (provider && !provider->shutdownAndWait(std::chrono::seconds(1))) {
                }
            }
            ticket->work.reset();
            {
                const std::lock_guard lock(state->mutex);
                --state->liveRunners;
            }
            state->ready.notify_all();
        }
    });
}

std::shared_ptr<SequenceExportRetirementOwnerV1>
SequenceExportRetirementOwnerV1::create(TestGate gate) {
    return std::shared_ptr<SequenceExportRetirementOwnerV1>(
        new SequenceExportRetirementOwnerV1(std::move(gate)));
}

SequenceExportRetirementOwnerV1::~SequenceExportRetirementOwnerV1() {
    beginShutdown();
    // Application and headless roots collect after proving exit. This is a final safety net for
    // an exceptional teardown, never the GUI controller's destructor path.
    if (state_->worker.joinable())
        state_->worker.join();
}

std::unique_ptr<SequenceExportRetirementOwnerV1::Ticket>
SequenceExportRetirementOwnerV1::reserve() {
    auto ticket = std::make_unique<Ticket>();
    const std::lock_guard lock(state_->mutex);
    if (state_->closing || state_->liveRunners == State::kMaxLiveRunners)
        return {};
    ++state_->liveRunners;
    return ticket;
}

void SequenceExportRetirementOwnerV1::retire(std::unique_ptr<Ticket> ticket,
                                             std::shared_ptr<Work> work) noexcept {
    if (!ticket || !work)
        std::terminate();
    work->cancelled.store(true);
    ticket->work = std::move(work);
    {
        const std::lock_guard lock(state_->mutex);
        auto* const raw = ticket.get();
        if (state_->last)
            state_->last->next = std::move(ticket);
        else
            state_->first = std::move(ticket);
        state_->last = raw;
    }
    state_->ready.notify_one();
}

void SequenceExportRetirementOwnerV1::release(std::unique_ptr<Ticket> ticket) noexcept {
    if (!ticket)
        return;
    {
        const std::lock_guard lock(state_->mutex);
        --state_->liveRunners;
    }
    state_->ready.notify_all();
}

void SequenceExportRetirementOwnerV1::beginShutdown() noexcept {
    {
        const std::lock_guard lock(state_->mutex);
        state_->closing = true;
    }
    state_->ready.notify_all();
}

bool SequenceExportRetirementOwnerV1::retirementComplete() const noexcept {
    const std::lock_guard lock(state_->mutex);
    return state_->closing && state_->exited && state_->liveRunners == 0;
}

bool SequenceExportRetirementOwnerV1::collect() noexcept {
    if (!retirementComplete())
        return false;
    if (state_->worker.joinable())
        state_->worker.join();
    return true;
}

struct SequenceExportRunnerV1::State {
    runtime::TaskScheduler& scheduler;
    PublicationCoordinator& publications;
    platform::StagedArtifactCoordinator& artifacts;
    output::ExportResourceLedgerV1& ledger;
    SequenceExportRetirementOwnerV1& retirement;
    std::unique_ptr<SequenceExportRetirementOwnerV1::Ticket> ticket;
    std::shared_ptr<Work> work;
    Stage stage = Stage::Compiling;
    runtime::TaskHandle<void> task;
    std::optional<OutputAnalysisAttemptRunnerV1> attemptRunner;
    std::shared_ptr<const output::OutputAnalysisAttemptV1> pending;
    bool approved = false;
    bool pauseRequested = false;
    // Technical detail of the last failure, for logs only.
    std::string diagnostic;
    bool holding = false;
    // ADR 0033 section 9: a frame refused for capacity waits at the frame boundary.
    OutputAnalysisCapacityWaitV1 capacity;
    bool capacityLogged = false;
    [[nodiscard]] const runtime::GpuProcessingPhasePolicy* processingPolicy() const {
        if (!work || !work->request.gpuProvider)
            return nullptr;
        return work->request.gpuProvider->processingPhasePolicy().get();
    }
    // The movie frame queue's reservation is this export's own admitted phase.
    [[nodiscard]] std::size_t ownPhases() const { return work && work->queueReservation ? 1 : 0; }
    [[nodiscard]] std::uint64_t currentFrame() const {
        return work->request.range.firstFrame + visibleEncoded;
    }
    std::uint64_t visibleEncoded = 0;
    std::optional<SequenceExportResultV1> outcome;
    State(runtime::TaskScheduler& s, PublicationCoordinator& p,
          platform::StagedArtifactCoordinator& a, output::ExportResourceLedgerV1& l,
          SequenceExportRequestV1 r, SequenceExportRetirementOwnerV1& retirementOwner,
          std::shared_ptr<output::ExportResourceLedgerV1> ledgerOwner)
        : scheduler(s), publications(p), artifacts(a), ledger(l), retirement(retirementOwner),
          ticket(retirement.reserve()) {
        if (ticket) {
            try {
                work = std::make_shared<Work>(std::move(r), std::move(ledgerOwner));
            } catch (...) {
                retirement.release(std::move(ticket));
                throw;
            }
        } else {
            SequenceExportResultV1 refused;
            refused.failure =
                Unavailable{Error::Busy, "Composition export retirement capacity refused"};
            outcome = std::move(refused);
            stage = Stage::Complete;
        }
    }
    ~State() {
        if (!ticket)
            return;
        if (!work) {
            retirement.release(std::move(ticket));
            return;
        }
        work->cancelled.store(true);
        if (attemptRunner)
            attemptRunner->requestCancellation();
        task.cancel();
        auto retained = std::move(work);
        {
            const std::lock_guard lock(retained->ownership);
            retained->abandoned = true;
        }
        retirement.retire(std::move(ticket), std::move(retained));
    }
    void fail(Unavailable error) {
        SequenceExportResultV1 result;
        result.failure = std::move(error);
        result.diagnostic = std::exchange(diagnostic, {});
        result.encodedFrames = visibleEncoded;
        if (result.failure->reason == Error::Cancelled)
            result.publication.outcome =
                platform::StagedArtifactPublicationOutcome::CancelledBeforePublication;
        outcome = std::move(result);
        stage = Stage::Complete;
        pending.reset();
        attemptRunner.reset();
        // The pre-reserved ticket remains admissible even after scheduler shutdown. It retains
        // the encoder and ledger charge until the independent owner has closed/reaped them.
        retirement.retire(std::move(ticket), std::move(work));
    }
    template <typename Callback> void submit(Stage next, Callback callback) {
        stage = next;
        runtime::TaskRequest request("Export composition", work->request.owner,
                                     runtime::TaskPriority::Foreground,
                                     next == Stage::Compiling ? runtime::TaskExecutor::Cpu
                                                              : runtime::TaskExecutor::BlockingIo);
        auto submission = scheduler.submit<void>(
            std::move(request),
            [work = work, callback = std::move(callback)](runtime::TaskContext& context) mutable {
                {
                    const std::lock_guard lock(work->ownership);
                    if (work->abandoned)
                        return runtime::TaskResult<void>::cancelled();
                    work->active = true;
                }
                work->taskCancellation = context.cancellation();
                try {
                    work->checkpoint();
                    callback(*work, context);
                } catch (const Unavailable& e) {
                    work->error = e;
                    work->clean();
                } catch (const std::exception&) {
                    work->error = Unavailable{Error::Io, "Composition export failed"};
                    work->clean();
                }
                {
                    const std::lock_guard lock(work->ownership);
                    work->active = false;
                }
                work->ownershipReady.notify_all();
                return runtime::TaskResult<void>::succeeded();
            });
        if (!submission.accepted())
            fail({Error::Busy, "Composition export task admission refused"});
        else
            task = std::move(submission.handle);
    }
    void nextAttempt() {
        const auto index = work->request.range.firstFrame + visibleEncoded;
        const auto time = FrameRangeRunnerV1::timeForFrame(work->request.range, index);
        if (!time) {
            fail({Error::InvalidValue, "Frame has no exact composition time"});
            return;
        }
        // The per-frame preservation check compares the prepared float frame before encoding.
#if defined(__APPLE__)
        // The TIFF preset's adapter is unavailable on macOS (no FFmpeg worker), which made
        // every video export unapprovable; use the always-available flat-EXR analyzer instead.
        const auto preset = output::OutputPresetV1::FlatExrRgba32fLinRec709SceneV1;
#else
        const auto preset = output::OutputPresetV1::TiffRgba16SrgbV1;
#endif
        OutputAnalysisAttemptRequestV1 request{
            .plan = work->plan,
            .evaluation = {.time = *time,
                           .output = work->plan->output(),
                           .resolution = runtime::CompositionFormatResolution{},
                           .quality = runtime::EvaluationQuality::Reference,
                           .colorIntent = work->colorIntent(),
                           .pixelStorageByteLimit = 0,
                           .bypassLookNodes = work->request.bypassLookNodes},
            .assetBaseDirectory = work->request.assetBaseDirectory,
            .fallbackEvaluator = work->fallbackEvaluator,
            .targetPath = work->request.range.destination,
            .overwritePolicy = platform::ArtifactOverwritePolicy::CreateOrReplace,
            .owner = work->request.owner,
            .preset = preset,
            .gpuProvider = work->request.gpuProvider,
            .useRootPixelCapacity = true};
        auto begin = beginOutputAnalysisAttemptV1(scheduler, artifacts, ledger, std::move(request));
        if (!begin) {
            fail({Error::Busy, "Frame analysis admission refused"});
            return;
        }
        attemptRunner.emplace(std::move(begin).takeHandle());
        stage = Stage::Analyzing;
    }
    bool encodeApproved() {
        if (!pending)
            return false;
        const auto digest = pending->digest();
        if (!digest)
            return false;
        auto approval = approveFrameExportV1(publications, pending, *digest);
        if (!approval) {
            fail({Error::IdentityMismatch, "Frame approval refused"});
            return false;
        }
        work->approvedFrame = std::move(approval).takeRequest();
        pending.reset();
        auto* artifactService = &artifacts;
        submit(Stage::Encoding, [artifactService](Work& w, runtime::TaskContext& context) {
            struct Release {
                Work& work;
                ~Release() { work.approvedFrame.reset(); }
            } release{w};
            w.encode(*w.approvedFrame, context);
            if (w.encoded == w.total())
                w.publish(*w.approvedFrame, *artifactService, context);
        });
        return true;
    }
};
SequenceExportRunnerV1::SequenceExportRunnerV1(
    runtime::TaskScheduler& scheduler, const runtime::SnapshotCompiler& compiler,
    PublicationCoordinator& publications, platform::StagedArtifactCoordinator& artifacts,
    output::ExportResourceLedgerV1& ledger, SequenceExportRequestV1 request,
    SequenceExportRetirementOwnerV1& retirement,
    std::shared_ptr<output::ExportResourceLedgerV1> ledgerLifetime)
    : state_(std::make_unique<State>(scheduler, publications, artifacts, ledger, std::move(request),
                                     retirement, std::move(ledgerLifetime))) {
    if (!state_->work)
        return;
    if (!state_->work->request.gpuProvider) {
        runtime::GpuProcessFrameEvaluatorOptions options;
        options.enabled = false;
        state_->work->request.gpuProvider = GpuExportProvider::create(std::move(options));
        state_->work->ownsDefaultGpuProvider = true;
        state_->work->request.gpuProvider->prepare(scheduler);
    }
    state_->submit(Stage::Compiling,
                   [&compiler, &ledger](Work& work, runtime::TaskContext& context) {
                       work.prepare(compiler, ledger, context);
                   });
}
SequenceExportRunnerV1::~SequenceExportRunnerV1() = default;
void SequenceExportRunnerV1::cancel() {
    if (!state_->work || state_->stage == Stage::Complete)
        return;
    state_->work->cancelled.store(true);
    if (state_->attemptRunner)
        state_->attemptRunner->requestCancellation();
    state_->task.cancel();
}
void SequenceExportRunnerV1::setPaused(const bool paused) { state_->pauseRequested = paused; }
bool SequenceExportRunnerV1::pausedAtBoundary() const {
    const auto& s = *state_;
    const bool queueWait = s.stage == Stage::Compiling && !s.task.isValid() && s.capacity.waiting();
    return s.pauseRequested && (s.holding || queueWait);
}
bool SequenceExportRunnerV1::waitingForCapacity() const { return state_->capacity.waiting(); }
void SequenceExportRunnerV1::poll() {
    auto& s = *state_;
    if (s.stage == Stage::Complete)
        return;
    if (s.holding) {
        if (s.work->isCancelled()) {
            s.holding = false;
            s.fail({Error::Cancelled, "Composition export cancelled"});
        } else if (!s.pauseRequested) {
            s.holding = false;
            s.nextAttempt();
        }
        return;
    }
    if (s.stage == Stage::AwaitingApproval) {
        if (s.work->isCancelled())
            s.fail({Error::Cancelled, "Composition export cancelled"});
        return;
    }
    if (s.stage == Stage::Compiling && !s.task.isValid() && s.capacity.waiting()) {
        // Waiting for the frame queue reservation (nothing in flight).
        if (s.work->isCancelled()) {
            s.capacity.succeeded();
            s.fail({Error::Cancelled, "Composition export cancelled"});
            return;
        }
        const auto now = OutputAnalysisCapacityWaitV1::Clock::now();
        if (s.pauseRequested) {
            // Paused while waiting: nothing is in flight, so this is the boundary.
            s.capacity.paused(now);
            return;
        }
        if (s.capacity.stalled(now, s.processingPolicy())) {
            s.diagnostic = "frame queue capacity wait stalled; no processing phase was admitted "
                           "for " +
                           std::to_string(OutputAnalysisCapacityWaitV1::kStallAfter.count()) +
                           " s; " + describeProcessingReservationsV1(s.processingPolicy());
            std::cerr << "Bloom export: " << s.diagnostic << '\n';
            s.capacity.succeeded();
            s.fail({Error::Busy,
                    "Bloom waited for memory to start the export, but other work kept holding "
                    "it, so the export stopped. Close a RAM preview or other exports and try "
                    "again; the details are in the log."});
            return;
        }
        if (s.capacity.retryDue(now)) {
            auto& ledger = s.ledger;
            s.submit(Stage::Compiling, [&ledger](Work& work, runtime::TaskContext& context) {
                work.reserveQueue(ledger, context);
            });
        }
        return;
    }
    if (s.stage == Stage::Analyzing && !s.attemptRunner && s.capacity.waiting()) {
        if (s.work->isCancelled()) {
            s.capacity.succeeded();
            s.fail({Error::Cancelled, "Composition export cancelled"});
            return;
        }
        if (s.pauseRequested) {
            // Paused while waiting: nothing is in flight, so this is the frame boundary.
            s.capacity.succeeded();
            s.holding = true;
            return;
        }
        const auto now = OutputAnalysisCapacityWaitV1::Clock::now();
        if (s.capacity.stalled(now, s.processingPolicy(), s.ownPhases())) {
            const auto frame = s.currentFrame();
            s.diagnostic = "frame " + std::to_string(frame) +
                           " capacity wait stalled; no processing phase was admitted for " +
                           std::to_string(OutputAnalysisCapacityWaitV1::kStallAfter.count()) +
                           " s; " + describeProcessingReservationsV1(s.processingPolicy());
            std::cerr << "Bloom export: " << s.diagnostic << '\n';
            s.capacity.succeeded();
            s.fail({Error::Busy,
                    "Bloom waited for memory to export frame " + std::to_string(frame) +
                        ", but other work kept holding it, so the export stopped. Close a RAM "
                        "preview or other exports and try again; the details are in the log."});
            return;
        }
        if (s.capacity.retryDue(now))
            s.nextAttempt();
        return;
    }
    if (s.stage == Stage::Analyzing) {
        if (!s.attemptRunner) {
            s.fail({Error::InvalidValue, "Missing analysis runner"});
            return;
        }
        auto outcome = s.attemptRunner->tryComplete();
        if (!outcome)
            return;
        s.attemptRunner.reset();
        if (const auto& provenance = outcome->gpuProvenance();
            provenance.has_value() && provenance->gpuEvaluated()) {
            ++s.work->gpuEvaluatedFrames;
            s.work->gpuNativeDispatches += provenance->counters.nativeDispatches;
            s.work->gpuReadbacks += provenance->counters.readbacks;
            s.work->gpuReadbackSubmissions += provenance->readbackSubmissions;
            s.work->gpuTransferredPayloads += provenance->transferredPayloads;
            s.work->gpuProcessPayloadBytes += provenance->processPayloadBytes;
            s.work->gpuEncodedPayloadBytes += provenance->encodedPayloadBytes;
            if (provenance->deviceOwnershipEpoch != 0) {
                s.work->gpuDeviceOwnershipEpoch = provenance->deviceOwnershipEpoch;
            }
        }
        if (s.work->isCancelled()) {
            s.fail({Error::Cancelled, "Composition export cancelled"});
            return;
        }
        switch (outputAnalysisCapacityRefusalV1(*outcome, s.processingPolicy(), s.ownPhases())) {
        case OutputAnalysisCapacityRefusalV1::Wait:
            // Hold at this frame boundary and retry the same frame when capacity frees.
            s.capacity.refused(OutputAnalysisCapacityWaitV1::Clock::now(), s.processingPolicy(),
                               s.ownPhases());
            if (!std::exchange(s.capacityLogged, true))
                std::cerr << "Bloom export: frame " << s.currentFrame() << " waiting for capacity; "
                          << describeOutputAnalysisAttemptV1(*outcome) << "; "
                          << describeProcessingReservationsV1(s.processingPolicy()) << '\n';
            return;
        case OutputAnalysisCapacityRefusalV1::NeverFits: {
            const auto frame = s.currentFrame();
            s.diagnostic = "frame " + std::to_string(frame) + " never fits; " +
                           describeOutputAnalysisAttemptV1(*outcome) + "; " +
                           describeProcessingReservationsV1(s.processingPolicy());
            std::cerr << "Bloom export: " << s.diagnostic << '\n';
            s.capacity.succeeded();
            s.fail({Error::Oversized,
                    "Frame " + std::to_string(frame) +
                        " needs more memory than this computer can give the export, so the "
                        "export stopped. Try a smaller resolution; the details are in the log."});
            return;
        }
        case OutputAnalysisCapacityRefusalV1::None:
            s.capacity.succeeded();
            s.capacityLogged = false;
            break;
        }
        if (!*outcome || !outcome->attempt() || !outcome->attempt()->approvable()) {
            // The artist reads a sentence; the stage, codes and facets go to the diagnostic log.
            const auto frame = s.work->request.range.firstFrame + s.visibleEncoded;
            std::string facets;
            if (outcome->attempt() && outcome->attempt()->report())
                for (const auto& facet : outcome->attempt()->report()->view().facets)
                    if (facet.state == output::OutputPreservationStateV1::Missing ||
                        facet.state == output::OutputPreservationStateV1::Unsupported)
                        facets += (facets.empty() ? "" : ", ") +
                                  std::string(output::outputFacetDisplayNameV1(facet.facet));
            std::string message =
                facets.empty()
                    ? "Bloom couldn't check frame " + std::to_string(frame) +
                          " before encoding, so the export stopped. This is a bug in Bloom; the "
                          "details are in the log."
                    : "Frame " + std::to_string(frame) + " can't be exported in this format (" +
                          facets + "), so the export stopped.";
            std::string detail = "frame " + std::to_string(frame) + " not approvable; " +
                                 describeOutputAnalysisAttemptV1(*outcome);
            std::cerr << "Bloom export: " << detail << '\n';
            s.diagnostic = std::move(detail);
            s.fail({Error::Unavailable, std::move(message)});
            return;
        }
        s.pending = outcome->attempt();
        if (s.approved)
            (void)s.encodeApproved();
        else
            s.stage = Stage::AwaitingApproval;
        return;
    }
    auto completed = s.task.tryTakeResult();
    if (!completed)
        return;
    if (s.work->result) {
        s.outcome = std::move(s.work->result);
        s.visibleEncoded = s.work->encoded;
        s.stage = Stage::Complete;
        return;
    }
    if (s.work->error) {
        s.fail(*s.work->error);
        return;
    }
    if (s.work->isCancelled() || completed->state() != runtime::TaskState::Succeeded) {
        s.fail({Error::Cancelled, "Composition export cancelled"});
        return;
    }
    if (s.stage == Stage::Compiling && s.work->queueAwaitingCapacity) {
        // ADR 0033 section 9: the export has not started yet; it waits, never fails, while the
        // Viewer or other work holds the shared processing root.
        s.task = {};
        s.capacity.refused(OutputAnalysisCapacityWaitV1::Clock::now(), s.processingPolicy());
        if (!std::exchange(s.capacityLogged, true))
            std::cerr << "Bloom export: frame queue waiting for capacity; "
                      << describeProcessingReservationsV1(s.processingPolicy()) << '\n';
        return;
    }
    if (s.stage == Stage::Compiling)
        s.capacity.succeeded();
    s.visibleEncoded = s.work->encoded;
    if (s.pauseRequested) {
        s.holding = true;
        return;
    }
    s.nextAttempt();
}
SequenceExportStageV1 SequenceExportRunnerV1::stage() const { return state_->stage; }
const output::MediaOutputAnalysisV1* SequenceExportRunnerV1::analysis() const {
    if (!state_->work || state_->stage == Stage::Compiling || !state_->work->analysis)
        return nullptr;
    return state_->work->analysis.get();
}
std::optional<core::Sha256Digest> SequenceExportRunnerV1::frameApprovalDigest() const {
    return state_->pending ? state_->pending->digest() : std::nullopt;
}
bool SequenceExportRunnerV1::approve(core::Sha256Digest mediaDigest,
                                     core::Sha256Digest frameDigest) {
    if (state_->stage != Stage::AwaitingApproval || !analysis() ||
        analysis()->digest != mediaDigest || frameApprovalDigest() != frameDigest)
        return false;
    state_->approved = true;
    return state_->encodeApproved();
}
std::uint64_t SequenceExportRunnerV1::encodedFrames() const { return state_->visibleEncoded; }
std::uint64_t SequenceExportRunnerV1::totalFrames() const {
    return state_->work ? state_->work->total() : state_->visibleEncoded;
}
const std::optional<SequenceExportResultV1>& SequenceExportRunnerV1::result() const {
    return state_->outcome;
}
} // namespace bloom::host
