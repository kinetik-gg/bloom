struct ViewerGpuResidentController::Impl final {
    std::shared_ptr<PreviewFrameTrace> trace;
    PreviewTraceIdentity pendingTraceIdentity;
    struct TraceSubmission final {
        std::uint64_t sequence = 0;
        PreviewTraceIdentity identity;
    };
    std::array<TraceSubmission, 32> traceSubmissions{};
    std::size_t nextTraceSubmission = 0;
    std::uint64_t tracePresenterId = 0, lastTracedAppliedSequence = 0;
    Dependencies dependencies;
    std::shared_ptr<ViewerGpuPort> port;
    std::unique_ptr<ViewerGpuPresenter> presenter;
    InputSink inputSink;
    PresentAck presentAck;
    CpuFallback cpuFallback;
    CpuCoverSnapshot coverSnapshot;
    // Alien host/wrapper owns only the independent raster cover windows. No native QWidget cover
    // participates in Qt's sibling promotion or decorated-parent backing-store crop.
    ViewerGpuCpuCoverHost* coverHost = nullptr;
    ViewerGpuCpuCover* cover = nullptr;
    // True only while a native transition (attach/resize/resume/refusal) needs the CPU cover.
    bool coverRequired = true;
    // True once the cover pixmap holds the current CPU paint. It is captured on the hidden->visible
    // transition only, so a retained/refused transition that re-raises the cover does not
    // re-rasterize the snapshot every poll tick.
    bool coverCaptured = false;
    std::function<void()> stateChanged;
    ViewerGpuPresenter::State lastReportedState = ViewerGpuPresenter::State::Uninitialized;

    // The last request/lease we intend to present. Cheap: an opaque lease plus
    // plain params.
    runtime::GpuResidentFrameLease pendingLease;
    render::GpuPresentImageParams pendingParams;
    std::uint32_t targetWidth = 0;
    std::uint32_t targetHeight = 0;
    std::uint64_t requestGeneration = 0;
    std::uint64_t enqueuedSequence = 0;
    bool presentAcked = false;
    // A present waiting for the target to become Active; enqueued once when presents are admitted.
    bool presentPending = false;
    std::shared_ptr<const runtime::GpuPresentationOverlay> pendingOverlayToPresent;

    struct Publication final {
        std::shared_ptr<const runtime::PreparedPreviewFrame> frame;
        render::GpuPresentImageParams params;
        ResidentOverlaySource source;
        std::optional<ResidentFrameGeometry> geometry;
        QRectF containerRect;
        double devicePixelRatio = 1.0;
        QRectF destination;
        document::ProjectId projectId;
        document::CompositionId compositionId;
        std::uint64_t domainRevision = 0, surfaceEpoch = 0, removalEpoch = 0, generation = 0;
    };
    std::optional<Publication> desiredPublication, activePublication, newestPublication;
    std::optional<Publication> rasterPublication, pendingPublication;
    std::shared_ptr<const runtime::PreparedPreviewFrame> appliedPublicationFrame;
    std::optional<ResidentAppliedDisplay> appliedDisplay;
    struct RequestedSafetyDomain final {
        document::ProjectId projectId;
        document::CompositionId compositionId;
        std::uint64_t domainRevision = 0, surfaceEpoch = 0, removalEpoch = 0;
        std::uint32_t width = 0, height = 0;
        QRectF containerRect;
        double devicePixelRatio = 1.0;
        bool overlayRequired = false;
    };
    std::optional<RequestedSafetyDomain> requestedSafetyDomain;
    QRectF appliedPublicationDestination;
    std::uint64_t appliedPublicationGeneration = 0, surfaceEpoch = 1, removalEpoch = 0;
    struct FrameSubmission final {
        std::uint64_t sequence = 0, generation = 0;
        std::shared_ptr<const runtime::PreparedPreviewFrame> frame;
        QRectF destination;
        std::uint64_t domainRevision = 0, surfaceEpoch = 0, removalEpoch = 0;
        std::uint32_t width = 0, height = 0;
        std::optional<ResidentFrameGeometry> geometry;
        render::GpuPresentImageParams params;
        QRectF containerRect;
        double devicePixelRatio = 1.0;
    };
    std::array<FrameSubmission, 2> frameSubmissions{};
    std::size_t nextFrameSubmission = 0;
    std::uint64_t lastObservedAppliedSequence = 0;
    std::optional<runtime::TaskHandle<std::shared_ptr<const runtime::GpuPresentationOverlay>>>
        activeOverlayTask;
    std::uint64_t nextOverlayToken = 0;
    std::shared_ptr<const runtime::GpuPresentationOverlay> presentedOverlay;

    [[nodiscard]] static bool compatible(const Publication& a, const Publication& b) {
        return a.projectId == b.projectId && a.compositionId == b.compositionId &&
               a.domainRevision == b.domainRevision && a.surfaceEpoch == b.surfaceEpoch &&
               a.removalEpoch == b.removalEpoch && a.params.targetWidth == b.params.targetWidth &&
               a.params.targetHeight == b.params.targetHeight;
    }
    [[nodiscard]] static bool equivalent(const Publication& a, const Publication& b) {
        // Signatures are hints, never the correctness oracle. The immutable recorded commands
        // capture selection geometry, style, text/paths/ROI and every other painted dependency.
        // Direct comparison is limited to a small command stream (not pixel storage). Larger
        // recordings conservatively schedule a worker raster rather than scanning on the UI.
        constexpr std::uint32_t kDirectComparisonLimit = 64U * 1024U;
        return a.source.picture.size() <= kDirectComparisonLimit && compatible(a, b) &&
               a.source.valid == b.source.valid && a.source.logicalSize == b.source.logicalSize &&
               a.source.picture.size() == b.source.picture.size() &&
               (a.source.picture.size() == 0 ||
                std::memcmp(a.source.picture.data(), b.source.picture.data(),
                            a.source.picture.size()) == 0);
    }
    void stagePublication(Publication publication,
                          std::shared_ptr<const runtime::GpuPresentationOverlay> overlay) {
        const auto geometry = residentFrameGeometry(*publication.frame);
        if (!geometry) {
            failCpu("the completed publication lease is no longer live");
            return;
        }
        pendingLease = geometry->lease;
        pendingParams = publication.params;
        pendingTraceIdentity = PreviewFrameTrace::identity(publication.frame->desiredIdentity());
        pendingPublication = std::move(publication);
        pendingOverlayToPresent = std::move(overlay);
        presentPending = true;
        pumpPendingPresent();
    }

    std::size_t presentAttempts = 0;
    std::size_t acceptedPresents = 0;
    std::size_t overlayRasters = 0;
    bool lastResident = false;
    std::string diagnostic;

    [[nodiscard]] bool usable() const noexcept {
        return dependencies.client != nullptr || port != nullptr;
    }

    [[nodiscard]] bool appliedMatchesRequestedSafetyDomain() const noexcept {
        return appliedDisplay && requestedSafetyDomain &&
               appliedDisplay->frame->desiredIdentity().projectId ==
                   requestedSafetyDomain->projectId &&
               appliedDisplay->frame->desiredIdentity().compositionId ==
                   requestedSafetyDomain->compositionId &&
               appliedDisplay->domainRevision == requestedSafetyDomain->domainRevision &&
               appliedDisplay->surfaceEpoch == requestedSafetyDomain->surfaceEpoch &&
               appliedDisplay->surfaceEpoch == surfaceEpoch &&
               appliedDisplay->removalEpoch == requestedSafetyDomain->removalEpoch &&
               appliedDisplay->removalEpoch == removalEpoch &&
               appliedDisplay->params.targetWidth == requestedSafetyDomain->width &&
               appliedDisplay->params.targetHeight == requestedSafetyDomain->height &&
               appliedDisplay->containerRect == requestedSafetyDomain->containerRect &&
               appliedDisplay->devicePixelRatio == requestedSafetyDomain->devicePixelRatio;
    }

    [[nodiscard]] bool nativeDisplayAvailable() const noexcept {
        const auto* window = presenter ? presenter->window() : nullptr;
        const auto* container = presenter ? presenter->container() : nullptr;
        const auto state =
            presenter ? presenter->state() : ViewerGpuPresenter::State::Uninitialized;
        const bool visiblePublicationState = state == ViewerGpuPresenter::State::Active ||
                                             state == ViewerGpuPresenter::State::Resizing ||
                                             state == ViewerGpuPresenter::State::Retiring;
        return presenter && window && window->isVisible() && window->isExposed() && container &&
               container->isVisible() && visiblePublicationState && !coverRequired;
    }

    [[nodiscard]] bool canRetainAppliedDisplay() const noexcept {
        return nativeDisplayAvailable() &&
               presenter->state() == ViewerGpuPresenter::State::Active &&
               appliedMatchesRequestedSafetyDomain();
    }

    void failCpu(const std::string& message) {
        diagnostic = message;
        if (cover != nullptr && !canRetainAppliedDisplay()) {
            coverRequired = true;
            showCover();
        }
        if (cpuFallback) {
            cpuFallback();
        }
    }

    // Returns true only on the conceal->reveal transition that actually showed and restacked the
    // cover. A cover that is already visible is left completely untouched.
    [[nodiscard]] bool ensureCover(const QRect& rect) {
        if (!coverSnapshot) {
            return false;
        }
        if (coverHost == nullptr) {
            coverHost = new ViewerGpuCpuCoverHost(dependencies.containerParent);
        }
        if (coverHost->geometry() != rect) {
            coverHost->setGeometry(rect);
            coverCaptured = false;
        }
        if (cover == nullptr) {
            cover = new ViewerGpuCpuCover(coverHost);
            cover->setObjectName(QStringLiteral("bloomViewerGpuCpuCover"));
            coverCaptured = false;
        }
        if (cover->bindNativeParent(presenter ? presenter->window() : nullptr))
            coverCaptured = false;
        const QRect local(QPoint(0, 0), rect.size());
        if (cover->geometry() != local) {
            cover->setGeometry(local);
        }
        // Snapshot once per conceal->reveal transition; a cover that is merely re-raised while it
        // already holds the current paint is not re-rasterized (and a hidden ancestor cannot be
        // mistaken for a transition, unlike a bare isVisible() test).
        if (!coverCaptured) {
            cover->setSnapshot(coverSnapshot());
            coverCaptured = true;
        }
        if (!cover->isVisible()) {
            if (!coverHost->isVisible()) {
                coverHost->show();
            }
            cover->show();
            cover->raise();
            return true;
        }
        return false;
    }

    void showCover() {
        if (cover == nullptr) {
            return;
        }
        if (!coverCaptured) {
            cover->setSnapshot(coverSnapshot());
            coverCaptured = true;
        }
        if (!cover->isVisible()) {
            if (coverHost != nullptr && !coverHost->isVisible()) {
                coverHost->show();
            }
            cover->show();
            cover->raise();
        }
    }

    void hideCover() {
        if (cover != nullptr) {
            cover->hide();
        }
        if (coverHost != nullptr) {
            coverHost->hide();
        }
        // The next reveal must capture fresh paint, whatever changed while it was concealed.
        coverCaptured = false;
    }

    [[nodiscard]] bool ensurePresenter(const std::uint32_t width, const std::uint32_t height) {
        if (presenter != nullptr) {
            if (presenter->initialized()) {
                return true;
            }
            return presenter->initialize();
        }
        if (!usable() || width == 0U || height == 0U) {
            diagnostic = "no presentation client/port or positive target extent was supplied";
            return false;
        }
        // CPU concealment may have cleared this after genuine retirement destroyed the old GPU
        // window and its raster child. A fresh target must establish its own cover before first
        // application, regardless of that former target's state.
        coverRequired = true;
        coverCaptured = false;
        ViewerGpuPresenter::Config config;
        config.vulkan_loader_path = dependencies.vulkanLoaderPath;
        config.target_width = width;
        config.target_height = height;
        config.device_pixel_ratio =
            dependencies.devicePixelRatio > 0.0 ? dependencies.devicePixelRatio : 1.0;
        if (trace) {
            tracePresenterId = trace->allocatePresenterId();
            traceSubmissions = {};
            lastTracedAppliedSequence = 0;
        }
        if (port != nullptr) {
            presenter = std::make_unique<ViewerGpuPresenter>(port, config);
        } else {
            presenter = std::make_unique<ViewerGpuPresenter>(dependencies.client, config);
        }
        // Parent the container BEFORE the window is exposed and the surface
        // attached.
        presenter->setContainerParent(dependencies.containerParent);
        presenter->setInputCallback([this](const ViewerGpuInputEvent& event) {
            if (inputSink) {
                inputSink(event);
            }
        });
        if (!presenter->initialize()) {
            diagnostic = presenter->diagnostic();
            return false;
        }
        return true;
    }

    // Enqueues one present; the returned bool is mailbox admission only, the result arrives via
    // poll().
    [[nodiscard]] bool
    enqueuePresent(std::shared_ptr<const runtime::GpuPresentationOverlay> overlay) {
        if (presenter == nullptr || !presenter->acceptingPresent()) {
            failCpu(presenter != nullptr ? presenter->diagnostic()
                                         : "no presentation target is active");
            if (presentAck) {
                presentAck(false, diagnostic);
            }
            return false;
        }
        ++presentAttempts;
        const bool admitted = presenter->present(pendingLease, pendingParams, std::move(overlay));
        if (!admitted) {
            if (trace)
                trace->record(
                    pendingTraceIdentity, PreviewTraceStage::PresentSubmitted,
                    PreviewTraceOutcome::Rejected,
                    static_cast<std::uint8_t>(runtime::PreviewDisplayProvider::GpuResident), 0, 0,
                    0, 0, tracePresenterId);
            failCpu(presenter->diagnostic());
            if (presentAck) {
                presentAck(false, diagnostic);
            }
            return false;
        }
        ++acceptedPresents;
        enqueuedSequence = presenter->lastEnqueuedSequence();
        if (pendingPublication) {
            frameSubmissions[nextFrameSubmission] = {enqueuedSequence,
                                                     pendingPublication->generation,
                                                     pendingPublication->frame,
                                                     pendingPublication->destination,
                                                     pendingPublication->domainRevision,
                                                     pendingPublication->surfaceEpoch,
                                                     pendingPublication->removalEpoch,
                                                     pendingPublication->params.targetWidth,
                                                     pendingPublication->params.targetHeight,
                                                     pendingPublication->geometry,
                                                     pendingPublication->params,
                                                     pendingPublication->containerRect,
                                                     pendingPublication->devicePixelRatio};
            nextFrameSubmission = (nextFrameSubmission + 1) % frameSubmissions.size();
        }
        if (trace) {
            traceSubmissions[nextTraceSubmission] = {enqueuedSequence, pendingTraceIdentity};
            nextTraceSubmission = (nextTraceSubmission + 1) % traceSubmissions.size();
            trace->record(pendingTraceIdentity, PreviewTraceStage::PresentSubmitted,
                          PreviewTraceOutcome::Accepted,
                          static_cast<std::uint8_t>(runtime::PreviewDisplayProvider::GpuResident),
                          0, enqueuedSequence, 0, 0, tracePresenterId);
        }
        pendingPublication.reset();
        presentAcked = false;
        return true;
    }

    // Enqueues the pending present once the target admits presents (first attach
    // / after resize).
    void pumpPendingPresent() {
        if (!presentPending || presenter == nullptr || !presenter->acceptingPresent() ||
            (enqueuedSequence != 0 && presenter->appliedSequence() < enqueuedSequence)) {
            return;
        }
        presentPending = false;
        static_cast<void>(enqueuePresent(pendingOverlayToPresent));
    }

    void startOverlayRaster(Publication publication) {
        if (dependencies.scheduler == nullptr) {
            diagnostic = "the required viewer overlay has no raster worker";
            failCpu(diagnostic);
            return;
        }
        OverlayRasterRequest request;
        request.picture = publication.source.picture;
        request.logicalSize = publication.source.logicalSize;
        request.width = publication.params.targetWidth;
        request.height = publication.params.targetHeight;
        request.token = ++nextOverlayToken;
        request.byteBudget = kResidentOverlayByteBudget;
        ++overlayRasters;
        auto submission =
            dependencies.scheduler->submit<std::shared_ptr<const runtime::GpuPresentationOverlay>>(
                runtime::TaskRequest(
                    "viewer-gpu-overlay-raster",
                    runtime::TaskOwner{.kind = runtime::TaskOwnerKind::PanelRequest,
                                       .id = runtime::TaskOwnerId::fromRaw(
                                           (std::uint64_t{1} << 63U) | 0x5A17ULL)}),
                [request](runtime::TaskContext& context) {
                    auto overlay = rasterizeResidentOverlay(
                        request, [&context] { return context.isCancellationRequested(); });
                    if (overlay == nullptr)
                        return runtime::TaskResult<std::shared_ptr<
                            const runtime::GpuPresentationOverlay>>::failed(runtime::TaskDiagnostic{
                            .code = "bloom.ui.overlay-raster",
                            .severity = runtime::DiagnosticSeverity::Error,
                            .summary = "the viewer overlay could not be rasterized",
                            .detail = {},
                            .suggestedAction = {}});
                    return runtime::TaskResult<std::shared_ptr<
                        const runtime::GpuPresentationOverlay>>::succeeded(std::move(overlay));
                });
        if (!submission.accepted()) {
            diagnostic = "the overlay raster task was not admitted";
            failCpu(diagnostic);
            return;
        }
        activePublication = std::move(publication);
        activeOverlayTask = submission.handle;
    }

    void pumpOverlayStart() {
        if (activeOverlayTask || !newestPublication)
            return;
        auto publication = std::move(*newestPublication);
        newestPublication.reset();
        startOverlayRaster(std::move(publication));
    }

    void pumpOverlayCompletion() {
        if (!activeOverlayTask || !activePublication)
            return;
        auto result = activeOverlayTask->tryTakeResult();
        if (!result)
            return;
        activeOverlayTask.reset();
        auto completed = std::move(*activePublication);
        activePublication.reset();
        // Make progress during continuous geometry edits, but never cross a surface/project or
        // selection/style domain. A deliberate removal invalidates the old active pair as well.
        if (desiredPublication && desiredPublication->source.valid &&
            compatible(completed, *desiredPublication)) {
            if (result->state() == runtime::TaskState::Succeeded && result->value() &&
                *result->value()) {
                presentedOverlay = *result->value();
                rasterPublication = completed;
                // Reuse retains commands, not an old GPU frame pin.
                rasterPublication->frame.reset();
                if (equivalent(completed, *desiredPublication)) {
                    completed = *desiredPublication;
                    newestPublication.reset();
                }
                stagePublication(std::move(completed), presentedOverlay);
            } else {
                diagnostic = "the viewer overlay failed to rasterize";
                failCpu(diagnostic);
            }
        }
        pumpOverlayStart();
    }

    void pumpPresentAck() {
        if (presenter && enqueuedSequence != 0 &&
            presenter->rejectedSequence() == enqueuedSequence) {
            const auto rejected = enqueuedSequence;
            if (trace) {
                PreviewTraceIdentity identity;
                for (const auto& submission : traceSubmissions)
                    if (submission.sequence == rejected) {
                        identity = submission.identity;
                        break;
                    }
                trace->record(
                    identity, PreviewTraceStage::PresentAcknowledged, PreviewTraceOutcome::Rejected,
                    static_cast<std::uint8_t>(runtime::PreviewDisplayProvider::GpuResident), 0,
                    rejected, presenter->appliedSequence(), presenter->presentCount(),
                    tracePresenterId);
            }
            enqueuedSequence = 0;
            presentAcked = false;
            for (auto& submission : frameSubmissions)
                if (submission.sequence == rejected)
                    submission = {};
            // Refusal is distinct from application. The next complete pending pair may proceed;
            // the refused update never receives current identity or a successful acknowledgement.
            failCpu(presenter->diagnostic());
            if (presentAck)
                presentAck(false, diagnostic);
        }
        if (presenter && presenter->appliedSequence() != 0 &&
            presenter->appliedSequence() != lastObservedAppliedSequence) {
            lastObservedAppliedSequence = presenter->appliedSequence();
            // Unknown correlation is explicitly unavailable, never the previous known frame.
            appliedPublicationFrame.reset();
            appliedDisplay.reset();
            appliedPublicationGeneration = 0;
            appliedPublicationDestination = {};
            for (auto& submission : frameSubmissions) {
                if (submission.sequence == lastObservedAppliedSequence) {
                    appliedPublicationFrame = submission.frame;
                    appliedPublicationGeneration = submission.generation;
                    appliedPublicationDestination = submission.destination;
                    appliedDisplay =
                        ResidentAppliedDisplay{submission.frame,           submission.geometry,
                                               submission.destination,     submission.params,
                                               submission.domainRevision,  submission.surfaceEpoch,
                                               submission.removalEpoch,    submission.containerRect,
                                               submission.devicePixelRatio};
                    const bool sameDomain =
                        desiredPublication &&
                        submission.frame->desiredIdentity().projectId ==
                            desiredPublication->projectId &&
                        submission.frame->desiredIdentity().compositionId ==
                            desiredPublication->compositionId &&
                        submission.domainRevision == desiredPublication->domainRevision &&
                        submission.surfaceEpoch == desiredPublication->surfaceEpoch &&
                        submission.removalEpoch == desiredPublication->removalEpoch &&
                        submission.width == desiredPublication->params.targetWidth &&
                        submission.height == desiredPublication->params.targetHeight;
                    submission = {};
                    if (sameDomain && appliedMatchesRequestedSafetyDomain()) {
                        diagnostic =
                            appliedPublicationGeneration == requestGeneration
                                ? presenter->diagnostic()
                                : "previous complete image and overlays are displayed (stale)";
                        coverRequired = false;
                        hideCover();
                        if (presentAck)
                            presentAck(true, diagnostic);
                    }
                    break;
                }
            }
            if (!appliedDisplay)
                failCpu(
                    "the native publication identity is unavailable; awaiting a complete display");
        }
        // Trace the owner's actual applied sequence, even while a newer request is pending.
        // Never stamp the current pending identity onto an older acknowledgement. If correlation
        // has fallen out of this bounded map, identity remains explicitly unavailable (all zero).
        if (trace && presenter && presenter->appliedSequence() != 0 &&
            presenter->appliedSequence() != lastTracedAppliedSequence) {
            const auto applied = presenter->appliedSequence();
            PreviewTraceIdentity acknowledged;
            for (const auto& submission : traceSubmissions) {
                if (submission.sequence == applied) {
                    acknowledged = submission.identity;
                    break;
                }
            }
            trace->record(acknowledged, PreviewTraceStage::PresentAcknowledged,
                          PreviewTraceOutcome::Accepted,
                          static_cast<std::uint8_t>(runtime::PreviewDisplayProvider::GpuResident),
                          0, applied, applied, presenter->presentCount(), tracePresenterId);
            lastTracedAppliedSequence = applied;
        }
        if (presentAcked || presenter == nullptr || enqueuedSequence == 0) {
            return;
        }
        const auto state = presenter->state();
        if (state == ViewerGpuPresenter::State::Retained ||
            state == ViewerGpuPresenter::State::Unsupported ||
            state == ViewerGpuPresenter::State::Uninitialized) {
            presentAcked = true;
            failCpu(presenter->diagnostic());
            if (presentAck) {
                presentAck(false, diagnostic);
            }
            return;
        }
        if (presentPending) {
            // A newer present is authored but not yet enqueued (the target is still attaching or
            // resizing). The previous present's sequence must not acknowledge the newer request, or
            // the cover would hide over a stale frame.
            return;
        }
        if (!presenter->acceptingPresent()) {
            return; // still attaching/resizing; wait
        }
        if (presentSequenceAcknowledged(presenter->appliedSequence(), enqueuedSequence)) {
            presentAcked = true;
            diagnostic = appliedPublicationGeneration == requestGeneration
                             ? presenter->diagnostic()
                             : "previous complete image and overlays are displayed (stale)";
        }
    }
};
