#include <algorithm>
#include <atomic>
#include <bloom/host/sequence_export_runner.hpp>
#include <bloom/output/composition_output_stream.hpp>
#include <bloom/runtime/cpu_composition_evaluator.hpp>
#include <mutex>

namespace bloom::host {
namespace {
using namespace media::provider;
using Stage = SequenceExportStageV1;
const runtime::TaskOwner owner{runtime::TaskOwnerKind::Export, runtime::TaskOwnerId::fromRaw(404)};
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
struct Work {
    SequenceExportRequestV1 request;
    std::shared_ptr<const runtime::CompiledCompositionPlan> plan;
    std::unique_ptr<output::MediaOutputAnalysisV1> analysis;
    std::shared_ptr<output::ExportResourceReservationV1> queueReservation;
    std::unique_ptr<output::CompositionOutputStreamV1> encoder;
    std::unique_ptr<FrameExportRequestV1> approvedFrame;
    std::atomic<bool> cancelled{false};
    std::mutex ownership;
    bool active = false, abandoned = false;
    runtime::CancellationToken taskCancellation;
    std::optional<Unavailable> error;
    std::uint64_t encoded = 0;
    core::Sha256Hasher approvals;
    std::unique_ptr<output::OutputAnalysisAttemptTargetV1> target;
    std::optional<SequenceExportResultV1> result;
    explicit Work(SequenceExportRequestV1 r) : request(std::move(r)) {}
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
        } else if (request.preset == output::OutputPresetV1::H264MovV1) {
            s.videoCodec = "h264";
            s.container = "mov";
            s.profile = "high";
        } else {
            require(false, "This export preset needs the FFmpeg worker, unavailable on macOS",
                    Error::Unavailable);
        }
#else
        if (request.preset == output::OutputPresetV1::H264MovV1) {
            s.videoCodec = "h264";
            s.container = "mov";
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
        const auto display = output::PreparedOutputDisplayV1::prepare(
            colorIntent(), request.displayName, request.viewName);
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
        require(bytes <= request.queueByteLimit, "Frame queue byte budget exceeded",
                Error::Oversized);
        output::ExportResourceAdmissionStatusV1 status{};
        queueReservation = ledger.reserve(bytes, status);
        require(queueReservation != nullptr, "Frame queue ledger reservation refused",
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
        outcome.encodedFrames = encoded;
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
} // namespace
struct SequenceExportRunnerV1::State {
    runtime::TaskScheduler& scheduler;
    PublicationCoordinator& publications;
    platform::StagedArtifactCoordinator& artifacts;
    output::ExportResourceLedgerV1& ledger;
    std::shared_ptr<Work> work;
    Stage stage = Stage::Compiling;
    runtime::TaskHandle<void> task;
    std::optional<OutputAnalysisAttemptRunnerV1> attemptRunner;
    std::shared_ptr<const output::OutputAnalysisAttemptV1> pending;
    bool approved = false;
    std::uint64_t visibleEncoded = 0;
    std::optional<SequenceExportResultV1> outcome;
    State(runtime::TaskScheduler& s, PublicationCoordinator& p,
          platform::StagedArtifactCoordinator& a, output::ExportResourceLedgerV1& l,
          SequenceExportRequestV1 r)
        : scheduler(s), publications(p), artifacts(a), ledger(l),
          work(std::make_shared<Work>(std::move(r))) {}
    void fail(Unavailable error) {
        SequenceExportResultV1 result;
        result.failure = std::move(error);
        result.encodedFrames = visibleEncoded;
        if (result.failure->reason == Error::Cancelled)
            result.publication.outcome =
                platform::StagedArtifactPublicationOutcome::CancelledBeforePublication;
        outcome = std::move(result);
        stage = Stage::Complete;
        pending.reset();
        attemptRunner.reset();
        auto retained = std::move(work);
        runtime::TaskRequest cleanup("Close composition export", owner,
                                     runtime::TaskPriority::Foreground,
                                     runtime::TaskExecutor::BlockingIo);
        auto submission =
            scheduler.submit<void>(std::move(cleanup), [retained](runtime::TaskContext&) {
                retained->clean();
                return runtime::TaskResult<void>::succeeded();
            });
        // Scheduler shutdown retains and destroys work after quiescence; normal cleanup is
        // worker-side.
        if (!submission.accepted())
            retained->cancelled.store(true);
    }
    template <typename Callback> void submit(Stage next, Callback callback) {
        stage = next;
        runtime::TaskRequest request("Export composition", owner, runtime::TaskPriority::Foreground,
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
                bool dispose = false;
                {
                    const std::lock_guard lock(work->ownership);
                    dispose = work->abandoned;
                    work->active = dispose;
                }
                if (dispose)
                    work->clean();
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
        OutputAnalysisAttemptRequestV1 request{
            .plan = work->plan,
            .evaluation = {.time = *time,
                           .output = work->plan->output(),
                           .resolution = runtime::CompositionFormatResolution{},
                           .quality = runtime::EvaluationQuality::Reference,
                           .colorIntent = work->colorIntent(),
                           .pixelStorageByteLimit = 1024ULL * 1024U * 1024U,
                           .bypassLookNodes = false},
            .targetPath = work->request.range.destination,
            .overwritePolicy = platform::ArtifactOverwritePolicy::CreateOrReplace,
            .owner = owner,
        // The per-frame preservation check compares the prepared float frame before encoding.
#if defined(__APPLE__)
            // The TIFF preset's adapter is unavailable on macOS (no FFmpeg worker), which made
            // every video export unapprovable; use the always-available flat-EXR analyzer instead.
            .preset = output::OutputPresetV1::FlatExrRgba32fLinRec709SceneV1};
#else
            .preset = output::OutputPresetV1::TiffRgba16SrgbV1};
#endif
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
SequenceExportRunnerV1::SequenceExportRunnerV1(runtime::TaskScheduler& scheduler,
                                               const runtime::SnapshotCompiler& compiler,
                                               PublicationCoordinator& publications,
                                               platform::StagedArtifactCoordinator& artifacts,
                                               output::ExportResourceLedgerV1& ledger,
                                               SequenceExportRequestV1 request)
    : state_(
          std::make_unique<State>(scheduler, publications, artifacts, ledger, std::move(request))) {
    state_->submit(Stage::Compiling,
                   [&compiler, &ledger](Work& work, runtime::TaskContext& context) {
                       work.prepare(compiler, ledger, context);
                   });
}
SequenceExportRunnerV1::~SequenceExportRunnerV1() {
    cancel();
    if (!state_->work)
        return;
    auto work = std::move(state_->work);
    bool dispose = false;
    {
        const std::lock_guard lock(work->ownership);
        work->abandoned = true;
        dispose = !work->active;
    }
    if (dispose) {
        runtime::TaskRequest cleanup("Close abandoned composition export", owner,
                                     runtime::TaskPriority::Foreground,
                                     runtime::TaskExecutor::BlockingIo);
        (void)state_->scheduler.submit<void>(std::move(cleanup), [work](runtime::TaskContext&) {
            work->clean();
            return runtime::TaskResult<void>::succeeded();
        });
    }
}
void SequenceExportRunnerV1::cancel() {
    if (!state_->work || state_->stage == Stage::Complete)
        return;
    state_->work->cancelled.store(true);
    if (state_->attemptRunner)
        state_->attemptRunner->requestCancellation();
    state_->task.cancel();
}
void SequenceExportRunnerV1::poll() {
    auto& s = *state_;
    if (s.stage == Stage::Complete)
        return;
    if (s.stage == Stage::AwaitingApproval) {
        if (s.work->isCancelled())
            s.fail({Error::Cancelled, "Composition export cancelled"});
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
        if (s.work->isCancelled()) {
            s.fail({Error::Cancelled, "Composition export cancelled"});
            return;
        }
        if (!*outcome || !outcome->attempt() || !outcome->attempt()->approvable()) {
            std::string detail = "Frame preservation report is not approvable";
            if (const auto* failure = outcome->failure()) {
                detail += "; stage=" + std::to_string(static_cast<unsigned>(failure->stage()));
                std::visit(
                    [&](const auto& value) {
                        if constexpr (!std::is_same_v<std::decay_t<decltype(value)>,
                                                      std::monostate>)
                            detail += "; code=" + std::to_string(static_cast<unsigned>(value));
                    },
                    failure->payload());
            }
            if (outcome->attempt())
                for (const auto& facet : outcome->attempt()->report()->view().facets)
                    detail += "; facet=" + std::to_string(static_cast<unsigned>(facet.facet)) +
                              ":" + std::to_string(static_cast<unsigned>(facet.stableCode));
            s.fail({Error::Unavailable, std::move(detail)});
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
    s.visibleEncoded = s.work->encoded;
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
