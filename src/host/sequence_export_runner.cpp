#include <algorithm>
#include <atomic>
#include <bloom/host/export_capacity_wait.hpp>
#include <bloom/host/sequence_export_runner.hpp>
#include <bloom/output/composition_output_stream.hpp>
#include <bloom/runtime/cpu_composition_evaluator.hpp>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <exception>
#include <iostream>
#include <mutex>
#include <numeric>
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
    bool streamed = false;
    std::unique_ptr<output::CompositionOutputStreamV1> encoder;
    // ADR 0033 section 16, staged streaming publication (Linux): the target is staged before the
    // first frame and the worker writes the container straight into it through an inherited
    // descriptor. Absent where the platform cannot hand out the descriptor: the encoded bytes are
    // then copied into a lease staged at publication, as before.
    std::optional<platform::StagedArtifactLease> streamedLease;
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
    // The export's display/view as one GPU command shared by every frame attempt (video only).
    std::shared_ptr<OutputMediaDisplayCommandV1> mediaDisplay;
    std::uint64_t gpuDisplayFrames = 0;
    std::uint64_t cpuDisplayFrames = 0;
    std::string cpuDisplayReason;
    std::unique_ptr<output::OutputAnalysisAttemptTargetV1> target;
    std::optional<SequenceExportResultV1> result;
    // One final-render evaluator per range: its verified source revisions and video sessions are
    // reused by every frame's CPU fallback. Released with the work on the retirement owner.
    std::shared_ptr<const runtime::CpuCompositionEvaluator> fallbackEvaluator =
        std::make_shared<const runtime::CpuCompositionEvaluator>();
    // The range is one linear pass: shared caches retain only what recurs between its frames.
    std::shared_ptr<runtime::LinearPassRetention> linearPass =
        std::make_shared<runtime::LinearPassRetention>();
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
        // Abandoning the lease removes the staged file; the destination is untouched.
        streamedLease.reset();
        approvedFrame.reset();
        queueReservation.reset();
    }
    // Stages the target and returns the descriptor the worker inherits, or null to keep the copy
    // path (a platform without inheritable descriptors).
    std::shared_ptr<const platform::StagedArtifactInheritableWriter>
    stageForStreaming(platform::StagedArtifactCoordinator& artifacts) {
        auto preflight =
            artifacts.preflight({target->targetPath, target->overwritePolicy, target->observation});
        require(static_cast<bool>(preflight), "Export target changed or is unavailable", Error::Io);
        require(preflight.target()->targetKey() == target->targetKey,
                "Export target identity changed", Error::IdentityMismatch);
        auto staging = artifacts.stage(std::move(preflight).takeTarget());
        require(static_cast<bool>(staging), "Cannot stage composition export", Error::Io);
        auto lease = std::move(staging).takeLease();
        auto writer = lease.inheritableWriter();
        if (!writer) {
            require(writer.error == platform::StagedArtifactError::UnsupportedPlatform,
                    "Cannot open the staged export for the encoder", Error::Io);
            return nullptr; // The unused lease is abandoned here; publication stages again.
        }
        streamedLease.emplace(std::move(lease));
        return std::move(writer.writer);
    }
    const EncodeSettingsV1& settings() const {
        require(analysis != nullptr, "Missing output analysis");
        return analysis->settings;
    }
    std::uint64_t total() const { return request.range.lastFrame - request.range.firstFrame + 1; }
    media::provider::Rational framePts(const std::uint64_t frame) const {
        const auto& s = settings();
        const auto numerator = frame * static_cast<std::uint64_t>(s.rate.denominator);
        const auto denominator = static_cast<std::uint64_t>(s.rate.numerator);
        const auto divisor = std::gcd(numerator, denominator);
        return {static_cast<std::int64_t>(numerator / divisor),
                static_cast<std::int64_t>(denominator / divisor)};
    }
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
        // The encode provider is selected when the encoder begins (ADR 0035): the platform media
        // framework first, then the bundled FFmpeg worker. Settings use the FFmpeg spellings; the
        // session translates them for the provider that serves the attempt.
        if (request.preset == output::OutputPresetV1::HevcMovV1 ||
            request.preset == output::OutputPresetV1::HevcMp4V1) {
            s.videoCodec = "hevc";
            s.container = request.preset == output::OutputPresetV1::HevcMp4V1 ? "mp4" : "mov";
            s.profile = request.profile == "main10" ? "main10" : "main";
            // NVENC, then VA-API when requested; the encode chain falls back to the bundled
            // Kvazaar silently (ADR 0036).
            request.worker.nvenc = request.nvenc;
            request.worker.vaapi = request.hardware;
        } else if (request.preset == output::OutputPresetV1::H264MovV1 ||
                   request.preset == output::OutputPresetV1::H264Mp4V1) {
            s.videoCodec = "h264";
            s.container = request.preset == output::OutputPresetV1::H264Mp4V1 ? "mp4" : "mov";
            s.profile = "high";
            // NVENC, then VA-API when requested; the encode chain falls back to the bundled
            // OpenH264 silently when no device encodes (ADR 0035, ADR 0036).
            request.worker.nvenc = request.nvenc;
            request.worker.vaapi = request.hardware;
        } else if (request.preset == output::OutputPresetV1::DnxhrMxfV1) {
            s.videoCodec = "dnxhd";
            s.container = "mxf";
        } else if (request.preset == output::OutputPresetV1::PcmWavV1) {
            s.videoCodec.clear();
            s.profile = request.pcmCodec;
            s.container = "wav";
            s.frames = 0;
        }
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
        if (!settings().videoCodec.empty() && analysis->display != nullptr)
            mediaDisplay =
                std::make_shared<OutputMediaDisplayCommandV1>(analysis->display, colorIntent());
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

    void encode(FrameExportRequestV1& approved, platform::StagedArtifactCoordinator& artifacts,
                runtime::TaskContext& context) {
        checkpoint();
        const auto& attempt = *approved.attempt();
        if (!target)
            target = std::make_unique<output::OutputAnalysisAttemptTargetV1>(attempt.target());
        if (!encoder) {
            auto worker = request.worker;
            worker.stagedOutput = stageForStreaming(artifacts);
            const auto origin =
                required(FrameRangeRunnerV1::timeForFrame(request.range, request.range.firstFrame),
                         "Missing exact audio origin");
            encoder = std::make_unique<output::CompositionOutputStreamV1>(
                output::CompositionOutputSourceV1{request.composition.snapshot, plan, origin,
                                                  request.assetBaseDirectory, analysis->display},
                settings(), std::move(worker), queueReservation,
                [this] { return isCancelled() || taskCancellation.isCancellationRequested(); });
        }
        const auto digest = required(attempt.digest(), "Frame approval digest missing");
        (void)approvals.update(std::as_bytes(std::span(digest.bytes())));
        const auto samples = settings().audioCodec.empty() ? 0 : samplesThrough(encoded + 1);
        const auto& media = attempt.gpuMediaDisplay();
        if (!settings().videoCodec.empty() && mediaDisplay != nullptr && media.isPresent() &&
            media.commandIdentity == mediaDisplay->commandIdentity() &&
            media.displayDigest == analysis->display->digest()) {
            // The GPU already ran the display/view transform; only un-association, clamping and
            // quantization happen at the codec boundary.
            auto prepared = checked(output::prepareMediaRgba16FromDisplayV1(
                media.pixels.pixels(), media.width, media.height, framePts(encoded),
                [this] { return isCancelled() || taskCancellation.isCancellationRequested(); }));
            checked(encoder->writePreparedFrame(std::move(prepared), samples, context));
            ++gpuDisplayFrames;
        } else {
            if (!settings().videoCodec.empty()) {
                // Explicit provenance: the CPU reference display transform ran for this frame.
                if (cpuDisplayFrames++ == 0) {
                    cpuDisplayReason = mediaDisplay == nullptr ? "no display command"
                                       : media.isPresent()
                                           ? "the GPU display payload's command differs"
                                       : mediaDisplay->unavailableReason().empty()
                                           ? "the frame was not evaluated on the GPU"
                                           : mediaDisplay->unavailableReason();
                    std::cerr << "Bloom export: CPU reference display transform for frame "
                              << request.range.firstFrame + encoded << " (" << cpuDisplayReason
                              << ")\n";
                }
            }
            checked(
                encoder->writeFrame(attempt.frame()->processImage(), encoded, samples, context));
        }
        ++encoded;
        context.reportProgress({"Encoding composition", "", encoded, total()});
    }
    void publish(FrameExportRequestV1& approved, platform::StagedArtifactCoordinator& artifacts,
                 runtime::TaskContext& context) {
        checkpoint();
        context.reportProgress({"Verifying composition", "Close and reopen", encoded, total()});
        const auto qc = checked(encoder->finish());
        // Captured before the encoder is released below: the serving provider and why the
        // providers ahead of it in the platform chain did not serve (ADR 0035).
        const auto served = encoder->handshake();
        auto provenance = encoder->provenance();
        // ADR 0036: a delivery that differs from the request (an 8-bit HEVC Main for a Main 10
        // request) is recorded in provenance, logged and stated in the evidence; never silent.
        provenance.adjustment = qc.adjustment;
        if (provenance.fellBack() || !provenance.adjustment.empty())
            std::cerr << "Bloom export: media provider " << provenance.diagnostic() << '\n';
        require(qc.frames == settings().frames && qc.audioSamples == settings().audioSamples &&
                    qc.bytes <= settings().byteLimit,
                "Worker verification does not match approved stream layout",
                Error::IdentityMismatch);
        require(target != nullptr, "Missing publication target");
        platform::StagedArtifactLease lease = [&] {
            if (streamedLease) {
                // The worker wrote and verified the staged file itself; once it has exited the
                // lease adopts exactly the bytes its reopen check hashed.
                checked(encoder->close());
                encoder.reset();
                auto adopted = std::move(*streamedLease);
                streamedLease.reset();
                require(static_cast<bool>(adopted.adoptExternalWrite(qc.bytes)) &&
                            static_cast<bool>(adopted.finishWriting()),
                        "The streamed export differs from the encoder's result", Error::Io);
                streamed = true;
                return adopted;
            }
            auto preflight = artifacts.preflight(
                {target->targetPath, target->overwritePolicy, target->observation});
            require(static_cast<bool>(preflight), "Export target changed or is unavailable",
                    Error::Io);
            require(preflight.target()->targetKey() == target->targetKey,
                    "Export target identity changed", Error::IdentityMismatch);
            auto staging = artifacts.stage(std::move(preflight).takeTarget());
            require(static_cast<bool>(staging), "Cannot stage composition export", Error::Io);
            auto copied = std::move(staging).takeLease();
            core::Sha256Hasher transferred;
            for (std::uint64_t offset = 0; offset < qc.bytes;) {
                checkpoint();
                const auto chunk = checked(encoder->read(offset));
                require(chunk.bytes.size() <= qc.bytes - offset &&
                            transferred.update(chunk.bytes) &&
                            static_cast<bool>(copied.write(chunk.bytes)),
                        "Encoded staging copy failed", Error::Io);
                offset += chunk.bytes.size();
            }
            checked(encoder->close());
            encoder.reset();
            require(transferred.finalize() == qc.artifact &&
                        static_cast<bool>(copied.finishWriting()),
                    "Encoded artifact changed during transfer", Error::DigestMismatch);
            return copied;
        }();
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
        // The encoder that actually served the attempt, after any silent VA-API fallback, and
        // the profile and bit depth the reopened stream carries.
        if (request.preset == output::OutputPresetV1::H264MovV1)
            analysis->implementationNote +=
                served.execution.device == "nvidia-nvenc"
                    ? "; encoder=nvenc nvenc-runtime-unqualified"
                : served.execution.implementation == media::provider::Implementation::Hardware
                    ? "; encoder=vaapi vaapi-runtime-unqualified"
                    : "; encoder=openh264 2.6.0 bundled";
        if (!qc.profile.empty() &&
            (settings().videoCodec == "h264" || settings().videoCodec == "hevc"))
            analysis->implementationNote += "; delivered=" + settings().videoCodec + " " +
                                            qc.profile + " " + std::to_string(qc.bitDepth) + "-bit";
        if (!qc.adjustment.empty())
            analysis->implementationNote += "; " + qc.adjustment;
        auto evidence =
            checked(output::makeMediaQcEvidenceV1(*analysis, qc, approvals.finalize(), &served));
        checkpoint();
        auto guardResult = FrameExportRequestAccessV1::claim(approved).tryEnterPublication();
        SequenceExportResultV1 outcome;
        outcome.evidence = std::move(evidence);
        outcome.expectedColourTags =
            settings().videoCodec.empty() ? ColourTags{} : settings().colour;
        // FFmpeg's MXF writes the matrix but neither the primaries nor the transfer, so a DNxHR
        // MXF reopens with both unspecified; expect only what the container can declare.
        if (settings().container == "mxf") {
            outcome.expectedColourTags.primaries = 2;
            outcome.expectedColourTags.transfer = 2;
        }
        const auto& codec =
            settings().videoCodec.empty() ? settings().audioCodec : settings().videoCodec;
        outcome.expectedContainer =
            settings().container + "/" + (codec == "prores_ks" ? std::string("prores") : codec);
        outcome.encodedFrames = encoded;
        outcome.streamedPublication = streamed;
        outcome.mediaProvider = provenance;
        outcome.gpuEvaluatedFrames = gpuEvaluatedFrames;
        outcome.gpuNativeDispatches = gpuNativeDispatches;
        outcome.gpuReadbacks = gpuReadbacks;
        outcome.gpuReadbackSubmissions = gpuReadbackSubmissions;
        outcome.gpuTransferredPayloads = gpuTransferredPayloads;
        outcome.gpuProcessPayloadBytes = gpuProcessPayloadBytes;
        outcome.gpuEncodedPayloadBytes = gpuEncodedPayloadBytes;
        outcome.gpuDeviceOwnershipEpoch = gpuDeviceOwnershipEpoch;
        outcome.gpuDisplayFrames = gpuDisplayFrames;
        outcome.cpuDisplayFrames = cpuDisplayFrames;
        outcome.cpuDisplayReason = cpuDisplayReason;
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

std::size_t detail::sequenceExportFramesInFlightV1(const GpuExportProvider* provider,
                                                   const std::optional<std::size_t> cap) noexcept {
    // Frames analysed ahead of the ordered encoder. The measured-capacity model
    // (GpuProcessingPhasePolicy::framesThatFit, measured free memory minus the headroom
    // preference) replaces this bounded default when it is available; the runner also shrinks
    // its window whenever admission reports the processing root out of capacity.
    (void)provider;
    constexpr std::size_t kDefaultFramesInFlight = 4;
    return std::max<std::size_t>(1, std::min(kDefaultFramesInFlight, cap.value_or(SIZE_MAX)));
}

struct SequenceExportRunnerV1::State {
    // One frame between its analysis attempt and the ordered encoder.
    struct InFlightFrame final {
        std::uint64_t offset = 0;
        std::optional<OutputAnalysisAttemptRunnerV1> runner;
        std::shared_ptr<const output::OutputAnalysisAttemptV1> attempt;
        // Admission found the processing root full while other frames held memory; the frame
        // starts again once an encode has released a retained frame (`releaseEpoch` advanced).
        bool awaitingCapacity = false;
        std::uint64_t refusedAtEpoch = 0;
        // Refused while nothing of this export held memory: other work holds the shared root, so
        // the retry follows the capacity wait's backoff instead of this export's completions.
        bool externalWait = false;
    };
    runtime::TaskScheduler& scheduler;
    PublicationCoordinator& publications;
    platform::StagedArtifactCoordinator& artifacts;
    output::ExportResourceLedgerV1& ledger;
    SequenceExportRetirementOwnerV1& retirement;
    std::unique_ptr<SequenceExportRetirementOwnerV1::Ticket> ticket;
    std::shared_ptr<Work> work;
    Stage stage = Stage::Compiling;
    // The compile task, then each ordered encode task; at most one at a time.
    runtime::TaskHandle<void> task;
    bool compiled = false;
    bool encoding = false;
    // Frames in flight, ordered by frame; the head is the next frame the encoder takes.
    std::deque<InFlightFrame> window;
    std::uint64_t nextOffset = 0;
    std::size_t depthLimit = 1;
    std::uint64_t releaseEpoch = 0;
    std::shared_ptr<const output::OutputAnalysisAttemptV1> pending;
    bool approved = false;
    bool pauseRequested = false;
    // Technical detail of the last failure, for logs only.
    std::string diagnostic;
    // ADR 0033 section 9: the frame queue reservation, or a frame refused while no frame of this
    // export holds memory, waits at the frame boundary with backoff and a stall rule.
    OutputAnalysisCapacityWaitV1 capacity;
    bool capacityLogged = false;
    [[nodiscard]] const runtime::GpuProcessingPhasePolicy* processingPolicy() const {
        if (!work || !work->request.gpuProvider)
            return nullptr;
        return work->request.gpuProvider->processingPhasePolicy().get();
    }
    // The movie frame queue's reservation is this export's own admitted phase.
    [[nodiscard]] std::size_t ownPhases() const { return work && work->queueReservation ? 1 : 0; }
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
        cancelAnalyses();
        task.cancel();
        auto retained = std::move(work);
        {
            const std::lock_guard lock(retained->ownership);
            retained->abandoned = true;
        }
        retirement.retire(std::move(ticket), std::move(retained));
    }
    void cancelAnalyses() noexcept {
        for (auto& frame : window)
            if (frame.runner)
                frame.runner->requestCancellation();
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
        cancelAnalyses();
        window.clear();
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
    [[nodiscard]] std::size_t runningAnalyses() const noexcept {
        return static_cast<std::size_t>(std::ranges::count_if(
            window, [](const auto& frame) { return frame.runner.has_value(); }));
    }
    // Starts the analysis attempt of one frame. False after a terminal failure.
    bool startAttempt(InFlightFrame& frame) {
        const auto index = work->request.range.firstFrame + frame.offset;
        const auto time = FrameRangeRunnerV1::timeForFrame(work->request.range, index);
        if (!time) {
            fail({Error::InvalidValue, "Frame has no exact composition time"});
            return false;
        }
        // The per-frame preservation check compares the prepared float frame before encoding.
        const auto preset = output::OutputPresetV1::TiffRgba16SrgbV1;
        OutputAnalysisAttemptRequestV1 request{
            .plan = work->plan,
            .evaluation = {.time = *time,
                           .output = work->plan->output(),
                           .resolution = runtime::CompositionFormatResolution{},
                           .quality = runtime::EvaluationQuality::Reference,
                           .colorIntent = work->colorIntent(),
                           .pixelStorageByteLimit = 0,
                           .bypassLookNodes = work->request.bypassLookNodes,
                           .linearPass = work->linearPass},
            .assetBaseDirectory = work->request.assetBaseDirectory,
            .fallbackEvaluator = work->fallbackEvaluator,
            .targetPath = work->request.range.destination,
            .overwritePolicy = platform::ArtifactOverwritePolicy::CreateOrReplace,
            .owner = work->request.owner,
            .preset = preset,
            .gpuProvider = work->request.gpuProvider,
            .mediaDisplay = work->mediaDisplay,
            .useRootPixelCapacity = true};
        auto begin = beginOutputAnalysisAttemptV1(scheduler, artifacts, ledger, std::move(request));
        if (!begin) {
            fail({Error::Busy, "Frame analysis admission refused"});
            return false;
        }
        frame.runner.emplace(std::move(begin).takeHandle());
        frame.awaitingCapacity = false;
        frame.externalWait = false;
        return true;
    }
    // Keeps up to `depthLimit` analyses running ahead of the encoder, in frame order. A frame
    // admission refused restarts once an encode released memory. Nothing new starts while paused
    // or cancelled. False after a terminal failure.
    bool fill() {
        if (!compiled || stage == Stage::Complete || work->isCancelled())
            return true;
        // Issue #396: no frame starts before the GPU provider's bootstrap is terminal (evaluator
        // and shared OCIO context published); an earlier frame would be evaluated and displayed
        // on the CPU reference path on a GPU-capable machine. A provider without a device still
        // reaches the terminal state, and its frames then take the reported CPU path.
        if (work->request.gpuProvider && work->request.gpuProvider->bootstrapPending())
            return true;
        // Retry the earliest refused frame once something of this export completed since its
        // refusal, or once the capacity wait's backoff elapsed when other work held the root.
        for (auto& frame : window) {
            if (!frame.awaitingCapacity)
                continue;
            const bool due = frame.externalWait
                                 ? capacity.retryDue(OutputAnalysisCapacityWaitV1::Clock::now())
                                 : frame.refusedAtEpoch != releaseEpoch;
            if (due) {
                frame.externalWait = false;
                if (!startAttempt(frame))
                    return false;
            }
            break;
        }
        // The frame being encoded counts: a depth of one is exactly one frame at a time.
        while (!pauseRequested && nextOffset < work->total() &&
               window.size() + (encoding ? 1U : 0U) < depthLimit) {
            window.emplace_back();
            window.back().offset = nextOffset++;
            if (!startAttempt(window.back()))
                return false;
        }
        return true;
    }
    [[nodiscard]] static bool capacityRefusal(const OutputAnalysisAttemptOutcomeV1& outcome) {
        const auto* failure = outcome.failure();
        if (!failure)
            return false;
        if (const auto* code = failure->payloadAs<runtime::GpuProcessFrameDiagnosticCode>())
            return *code == runtime::GpuProcessFrameDiagnosticCode::AwaitingCapacity ||
                   *code == runtime::GpuProcessFrameDiagnosticCode::BudgetRefused;
        if (const auto* code = failure->payloadAs<output::OutputAnalysisAttemptErrorCodeV1>())
            return *code == output::OutputAnalysisAttemptErrorCodeV1::ResourceReservationFailed;
        return false;
    }
    // Consumes the completed analysis of window[position]. False after a terminal failure.
    bool complete(const std::size_t position, OutputAnalysisAttemptOutcomeV1 result) {
        auto& frame = window[position];
        if (const auto& provenance = result.gpuProvenance();
            provenance.has_value() && provenance->gpuEvaluated()) {
            ++work->gpuEvaluatedFrames;
            work->gpuNativeDispatches += provenance->counters.nativeDispatches;
            work->gpuReadbacks += provenance->counters.readbacks;
            work->gpuReadbackSubmissions += provenance->readbackSubmissions;
            work->gpuTransferredPayloads += provenance->transferredPayloads;
            work->gpuProcessPayloadBytes += provenance->processPayloadBytes;
            work->gpuEncodedPayloadBytes += provenance->encodedPayloadBytes;
            if (provenance->deviceOwnershipEpoch != 0)
                work->gpuDeviceOwnershipEpoch = provenance->deviceOwnershipEpoch;
        }
        if (work->isCancelled()) {
            fail({Error::Cancelled, "Composition export cancelled"});
            return false;
        }
        const auto refusal =
            outputAnalysisCapacityRefusalV1(result, processingPolicy(), ownPhases());
        if (refusal != OutputAnalysisCapacityRefusalV1::None) {
            const auto index = work->request.range.firstFrame + frame.offset;
            // Another frame of this export (analysing, analysed or encoding) holds memory or the
            // device phase: keep everything else running and start this frame again once
            // something has completed (releaseEpoch advances on every completion).
            const bool ownFramesHold =
                encoding || std::ranges::any_of(window, [&frame](const auto& other) {
                    return &other != &frame &&
                           (other.runner.has_value() || other.attempt != nullptr);
                });
            if (ownFramesHold) {
                frame.awaitingCapacity = true;
                frame.externalWait = false;
                frame.refusedAtEpoch = releaseEpoch;
                return true;
            }
            if (refusal == OutputAnalysisCapacityRefusalV1::Wait) {
                // ADR 0033 section 9: other work holds the shared root; wait at this boundary.
                frame.awaitingCapacity = true;
                frame.externalWait = true;
                frame.refusedAtEpoch = releaseEpoch;
                capacity.refused(OutputAnalysisCapacityWaitV1::Clock::now(), processingPolicy(),
                                 ownPhases());
                if (!std::exchange(capacityLogged, true))
                    std::cerr << "Bloom export: frame " << index << " waiting for capacity; "
                              << describeOutputAnalysisAttemptV1(result) << "; "
                              << describeProcessingReservationsV1(processingPolicy()) << '\n';
                return true;
            }
            diagnostic = "frame " + std::to_string(index) + " never fits; " +
                         describeOutputAnalysisAttemptV1(result) + "; " +
                         describeProcessingReservationsV1(processingPolicy());
            std::cerr << "Bloom export: " << diagnostic << '\n';
            capacity.succeeded();
            fail({Error::Oversized,
                  "Frame " + std::to_string(index) +
                      " needs more memory than this computer can give the export, so the "
                      "export stopped. Try a smaller resolution; the details are in the log."});
            return false;
        }
        if (result && !frame.awaitingCapacity && capacity.waiting() &&
            std::ranges::none_of(window, [](const auto& other) { return other.externalWait; })) {
            capacity.succeeded();
            capacityLogged = false;
        }
        if (!result || !result.attempt() || !result.attempt()->approvable()) {
            // The artist reads a sentence; the stage, codes and facets go to the diagnostic log.
            const auto index = work->request.range.firstFrame + frame.offset;
            std::string facets;
            if (result.attempt() && result.attempt()->report())
                for (const auto& facet : result.attempt()->report()->view().facets)
                    if (facet.state == output::OutputPreservationStateV1::Missing ||
                        facet.state == output::OutputPreservationStateV1::Unsupported)
                        facets += (facets.empty() ? "" : ", ") +
                                  std::string(output::outputFacetDisplayNameV1(facet.facet));
            std::string message =
                facets.empty()
                    ? "Bloom couldn't check frame " + std::to_string(index) +
                          " before encoding, so the export stopped. This is a bug in Bloom; the "
                          "details are in the log."
                    : "Frame " + std::to_string(index) + " can't be exported in this format (" +
                          facets + "), so the export stopped.";
            std::string detail = "frame " + std::to_string(index) + " not approvable; " +
                                 describeOutputAnalysisAttemptV1(result);
            std::cerr << "Bloom export: " << detail << '\n';
            diagnostic = std::move(detail);
            fail({Error::Unavailable, std::move(message)});
            return false;
        }
        frame.attempt = result.attempt();
        return true;
    }
    // Hands the analysed head frame to the ordered encoder. False after a terminal failure.
    bool encodeHead() {
        auto attempt = std::move(window.front().attempt);
        window.pop_front();
        pending.reset();
        const auto digest = attempt->digest();
        if (!digest) {
            fail({Error::IdentityMismatch, "Frame approval digest missing"});
            return false;
        }
        auto approval = approveFrameExportV1(publications, attempt, *digest);
        if (!approval) {
            fail({Error::IdentityMismatch, "Frame approval refused"});
            return false;
        }
        work->approvedFrame = std::move(approval).takeRequest();
        encoding = true;
        auto* artifactService = &artifacts;
        submit(Stage::Encoding, [artifactService](Work& w, runtime::TaskContext& context) {
            struct Release {
                Work& work;
                ~Release() { work.approvedFrame.reset(); }
            } release{w};
            w.encode(*w.approvedFrame, *artifactService, context);
            if (w.encoded == w.total())
                w.publish(*w.approvedFrame, *artifactService, context);
        });
        return stage != Stage::Complete;
    }
    // The frame-boundary side of an external capacity wait: pausing drops the waiting frames (they
    // start again on resume) so the pause reaches its boundary, and a wait no admitted phase can
    // end stops the export with readable text. False after a terminal failure.
    bool waitForCapacity() {
        if (!compiled || stage == Stage::Complete)
            return true;
        const auto waiting =
            std::ranges::find_if(window, [](const auto& frame) { return frame.awaitingCapacity; });
        if (waiting == window.end())
            return true;
        if (pauseRequested) {
            nextOffset = waiting->offset;
            for (auto later = waiting; later != window.end(); ++later)
                if (later->runner)
                    later->runner->requestCancellation();
            window.erase(waiting, window.end());
            capacity.succeeded();
            return true;
        }
        const auto now = OutputAnalysisCapacityWaitV1::Clock::now();
        if (waiting->externalWait && capacity.stalled(now, processingPolicy(), ownPhases())) {
            const auto index = work->request.range.firstFrame + waiting->offset;
            diagnostic = "frame " + std::to_string(index) +
                         " capacity wait stalled; no processing phase was admitted for " +
                         std::to_string(OutputAnalysisCapacityWaitV1::kStallAfter.count()) +
                         " s; " + describeProcessingReservationsV1(processingPolicy());
            std::cerr << "Bloom export: " << diagnostic << '\n';
            capacity.succeeded();
            fail({Error::Busy,
                  "Bloom waited for memory to export frame " + std::to_string(index) +
                      ", but other work kept holding it, so the export stopped. Close a RAM "
                      "preview or other exports and try again; the details are in the log."});
            return false;
        }
        return true;
    }
    // Nothing runs, nothing encodes and the earliest unanalysed frame waits for capacity: only
    // analysed later frames still hold memory, and they can never reach the encoder first. Drop
    // them (they analyse again later) and retry; with nothing left to drop the refusal is real.
    // False after a terminal failure.
    bool resolveStall() {
        if (stage == Stage::Complete || encoding || runningAnalyses() != 0)
            return true;
        const auto waiting =
            std::ranges::find_if(window, [](const auto& frame) { return frame.awaitingCapacity; });
        if (waiting == window.end() || waiting->externalWait ||
            waiting->refusedAtEpoch != releaseEpoch)
            return true;
        bool released = false;
        for (auto later = std::next(waiting); later != window.end(); ++later) {
            if (later->attempt != nullptr) {
                later->attempt.reset();
                later->awaitingCapacity = true;
                later->refusedAtEpoch = releaseEpoch;
                released = true;
            }
        }
        if (released) {
            ++releaseEpoch;
            return fill();
        }
        const auto index = work->request.range.firstFrame + waiting->offset;
        diagnostic = "frame " + std::to_string(index) + " processing admission refused";
        std::cerr << "Bloom export: " << diagnostic << '\n';
        fail({Error::Unavailable, "Bloom couldn't reserve processing memory for frame " +
                                      std::to_string(index) + ", so the export stopped."});
        return false;
    }
    void updateStage() {
        if (stage == Stage::Complete || !compiled)
            return;
        if (encoding)
            stage = Stage::Encoding;
        else if (!window.empty() && window.front().attempt && !approved)
            stage = Stage::AwaitingApproval;
        else
            stage = Stage::Analyzing;
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
    state_->depthLimit = detail::sequenceExportFramesInFlightV1(
        state_->work->request.gpuProvider.get(), state_->work->request.framesInFlight);
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
    state_->cancelAnalyses();
    state_->task.cancel();
}
void SequenceExportRunnerV1::setPaused(const bool paused) { state_->pauseRequested = paused; }
bool SequenceExportRunnerV1::pausedAtBoundary() const {
    const auto& s = *state_;
    const bool queueWait = s.stage == Stage::Compiling && !s.task.isValid() && s.capacity.waiting();
    const bool drained =
        s.compiled && s.stage != Stage::Complete && !s.encoding && s.window.empty();
    return s.pauseRequested && (queueWait || drained);
}
bool SequenceExportRunnerV1::waitingForCapacity() const {
    const auto& s = *state_;
    return s.capacity.waiting() ||
           std::ranges::any_of(s.window, [](const auto& frame) { return frame.awaitingCapacity; });
}
void SequenceExportRunnerV1::poll() {
    auto& s = *state_;
    if (s.stage == Stage::Complete)
        return;
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
    if (!s.compiled || s.encoding) {
        // The compile (or frame queue) task, or the ordered encode task of the head frame.
        if (auto completed = s.task.tryTakeResult()) {
            const bool wasEncoding = s.encoding;
            s.encoding = false;
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
            if (!wasEncoding && s.work->queueAwaitingCapacity) {
                // ADR 0033 section 9: the export has not started yet; it waits, never fails,
                // while the Viewer or other work holds the shared processing root.
                s.task = {};
                s.capacity.refused(OutputAnalysisCapacityWaitV1::Clock::now(),
                                   s.processingPolicy());
                if (!std::exchange(s.capacityLogged, true))
                    std::cerr << "Bloom export: frame queue waiting for capacity; "
                              << describeProcessingReservationsV1(s.processingPolicy()) << '\n';
                return;
            }
            if (!wasEncoding) {
                s.capacity.succeeded();
                s.capacityLogged = false;
            }
            s.visibleEncoded = s.work->encoded;
            if (wasEncoding)
                ++s.releaseEpoch;
            s.compiled = true;
        } else if (!s.compiled) {
            return;
        }
    }
    if (s.work->isCancelled() && !s.encoding) {
        s.capacity.succeeded();
        s.fail({Error::Cancelled, "Composition export cancelled"});
        return;
    }
    for (std::size_t position = 0; position < s.window.size(); ++position) {
        auto& frame = s.window[position];
        if (!frame.runner)
            continue;
        auto result = frame.runner->tryComplete();
        if (!result)
            continue;
        frame.runner.reset();
        ++s.releaseEpoch;
        if (!s.complete(position, std::move(*result)))
            return;
    }
    if (!s.encoding && !s.window.empty() && s.window.front().attempt) {
        if (s.approved) {
            if (!s.encodeHead())
                return;
        } else {
            s.pending = s.window.front().attempt;
        }
    }
    if (!s.waitForCapacity())
        return;
    if (!s.fill())
        return;
    if (!s.resolveStall())
        return;
    s.updateStage();
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
    auto& s = *state_;
    if (s.stage != Stage::AwaitingApproval || !analysis() || analysis()->digest != mediaDigest ||
        frameApprovalDigest() != frameDigest || s.window.empty() ||
        s.window.front().attempt != s.pending)
        return false;
    s.approved = true;
    if (!s.encodeHead())
        return false;
    s.updateStage();
    return true;
}
std::uint64_t SequenceExportRunnerV1::encodedFrames() const { return state_->visibleEncoded; }
std::uint64_t SequenceExportRunnerV1::totalFrames() const {
    return state_->work ? state_->work->total() : state_->visibleEncoded;
}
const std::optional<SequenceExportResultV1>& SequenceExportRunnerV1::result() const {
    return state_->outcome;
}
} // namespace bloom::host
