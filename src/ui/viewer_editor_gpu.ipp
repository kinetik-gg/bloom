void paintViewerCompositionFrame(QPainter& painter, const QRectF& displayRect) {
    painter.setPen(QPen(kit::color(kit::Color::CompositionFrame), kit::kCompositionFrameWidth));
    painter.setBrush(Qt::NoBrush);
    painter.drawRect(
        displayRect.adjusted(0.0, 0.0, -kit::kCompositionFrameWidth, -kit::kCompositionFrameWidth));
}

void paintViewerEmptyInvitation(QPainter& painter, const QRectF& canvasRect,
                                const QString& invitation) {
    if (invitation.isEmpty()) {
        return;
    }
    painter.setFont(kit::font(kit::TypeRole::Ui));
    painter.setPen(kit::color(kit::Color::Muted));
    painter.drawText(canvasRect, Qt::AlignCenter, invitation);
}

void ViewerEditor::setGpuPresentationDependencies(
    std::shared_ptr<runtime::GpuPresentationClient> client, runtime::TaskScheduler* scheduler,
    std::string vulkanLoaderPath, const double devicePixelRatio) {
    if (gpuResidentTimer_ != nullptr) {
        gpuResidentTimer_->stop();
    }
    ViewerGpuResidentController::Dependencies dependencies;
    dependencies.client = std::move(client);
    dependencies.scheduler = scheduler;
    dependencies.containerParent = this;
    dependencies.vulkanLoaderPath = std::move(vulkanLoaderPath);
    dependencies.devicePixelRatio = devicePixelRatio > 0.0 ? devicePixelRatio : 1.0;
    gpuResident_->setDependencies(std::move(dependencies));
    if (gpuResident_->configured() && gpuResidentTimer_ != nullptr) {
        gpuResidentTimer_->start();
    }
    updateGpuResidentPresentation();
    update();
}

bool ViewerEditor::residentFrameIsDisplayed() const {
    const auto frame = displayedFrame();
    return frame != nullptr &&
           frame->provenance().provider == runtime::PreviewDisplayProvider::GpuResident;
}

PreparedPreviewFrameHandle ViewerEditor::paintableCpuFrame() {
    auto current = displayedFrame();
    if (current != nullptr &&
        current->provenance().provider == runtime::PreviewDisplayProvider::GpuResident) {
        if (residentActive_) {
            return nullptr;
        }
        return cpuFallbackFrame_ != nullptr ? cpuFallbackFrame_ : lastCpuFrame_;
    }
    lastCpuFrame_ = current;
    cpuFallbackFrame_.reset();
    cpuFallbackFailedIdentity_.reset();
    return current;
}

ResidentPresentRequest ViewerEditor::buildResidentPresentRequest() {
    ResidentPresentRequest request;
    request.containerRect = contentRect();
    request.devicePixelRatio = devicePixelRatioF() > 0.0 ? devicePixelRatioF() : 1.0;
    const auto geometry = currentDisplayGeometry();
    if (geometry.has_value()) {
        request.destination = viewTransformedDisplayRect(canvasRect(), geometry->extent,
                                                         geometry->pixelAspect, transform_);
    }
    request.channel = static_cast<render::GpuPresentChannel>(channel_);
    request.background = static_cast<render::GpuPresentBackground>(background_);
    // The surround must match drawCanvasBackground()'s CPU paint for every mode. Solid uses the
    // panel Canvas token -- the composition's authored background lives in the display image
    // itself, never in the surround -- Black and White are literal, and Checkerboard comes from the
    // checker colours below. The present shader overrides Black/White/Checkerboard from the mode,
    // so this value is authoritative for Solid and consistent with the CPU path for the others.
    QColor surround = kit::color(kit::Color::Canvas);
    if (background_ == ViewerBackground::Black) {
        surround = QColor(Qt::black);
    } else if (background_ == ViewerBackground::White) {
        surround = QColor(Qt::white);
    }
    request.backgroundColor = render::GpuPresentColor{static_cast<float>(surround.redF()),
                                                      static_cast<float>(surround.greenF()),
                                                      static_cast<float>(surround.blueF()), 1.0F};
    const QColor checkerA = kit::color(kit::Color::Surface);
    const QColor checkerB = kit::color(kit::Color::SurfaceRaised);
    request.checkerColorA = render::GpuPresentColor{static_cast<float>(checkerA.redF()),
                                                    static_cast<float>(checkerA.greenF()),
                                                    static_cast<float>(checkerA.blueF()), 1.0F};
    request.checkerColorB = render::GpuPresentColor{static_cast<float>(checkerB.redF()),
                                                    static_cast<float>(checkerB.greenF()),
                                                    static_cast<float>(checkerB.blueF()), 1.0F};
    request.checkerTilePixels =
        static_cast<float>(kit::px(kit::Size::ViewerChecker) * request.devicePixelRatio);

    // Record the immutable overlays on the UI thread as a QPicture in container-local coordinates.
    // The worker only replays this picture; it reads no widget and no session state.
    const auto frame = displayedFrame();
    const auto resident = frame != nullptr ? residentFrameGeometry(*frame) : std::nullopt;
    const auto* composition = session_.composition();
    if (frame != nullptr && resident.has_value() && geometry.has_value()) {
        const QRectF displayRect = request.destination;
        QPicture picture;
        QPainter painter(&picture);
        painter.translate(-request.containerRect.topLeft());
        const auto descriptor = render::ReferenceDisplayBufferDescriptor::create(
            resident->displayWindow, resident->pixelAspect);
        if (descriptor) {
            // The composition frame is part of the CPU paint (paintViewerContent); the native
            // overlay must draw it too or a resident composition has no visible bounds at all.
            paintViewerCompositionFrame(painter, displayRect);
            const ViewerMapping mapping{displayRect, composition->format(),
                                        frame->desiredIdentity().resolution, resident->pixelAspect,
                                        *descriptor.value()};
            const auto bounds = previewController_.selectedLayerBounds();
            paintViewerOverlays(painter, canvasRect(), mapping,
                                effectiveZoom(displayRect, *geometry), overlayOptions_,
                                textEdit_ ? std::span<const runtime::EvaluatedOperationBounds>{}
                                          : bounds,
                                pointTextLayers(session_, bounds));
        }
        paintRoi(painter);
        paintCreation(painter);
        paintPathTools(painter);
        paintTextEditing(painter);
        // The empty-state invitation is drawn by paintEvent() on the CPU path, which resident
        // presentation skips entirely; record it here so an active composition with no layers still
        // invites a layer instead of reading as a bare checkerboard.
        paintViewerEmptyInvitation(painter, canvasRect(), emptyStateInvitation());
        painter.end();
        if (!picture.isNull()) {
            request.overlay.picture = picture;
            request.overlay.logicalSize = request.containerRect.size();
            request.overlay.token = ++gpuOverlayToken_;
            request.overlay.valid = true;
        }
    }
    // Stable signature of everything the overlay recording depends on. The controller reuses the
    // retained overlay and its token while this is unchanged, so a steady view never re-rasterizes.
    std::uint64_t signature = 1469598103934665603ULL;
    const auto mix = [&signature](const std::uint64_t value) {
        signature ^= value;
        signature *= 1099511628211ULL;
    };
    if (frame != nullptr) {
        const auto& identity = frame->desiredIdentity();
        mix(identity.requestGeneration);
        mix(static_cast<std::uint64_t>(identity.sourceRevision.value()));
        mix(static_cast<std::uint64_t>(identity.time.numerator()));
        mix(static_cast<std::uint64_t>(identity.time.denominator()));
    }
    mix(transform_.fitToWindow ? 1ULL : 0ULL);
    mix(static_cast<std::uint64_t>(std::llround(transform_.zoom * 1000000.0)));
    mix(static_cast<std::uint64_t>(std::llround(transform_.pan.x() * 1000.0)));
    mix(static_cast<std::uint64_t>(std::llround(transform_.pan.y() * 1000.0)));
    mix(static_cast<std::uint64_t>(channel_));
    mix(static_cast<std::uint64_t>(background_));
    mix(gpuOverlayRevision_);
    mix(static_cast<std::uint64_t>(overlayOptions_.safeAreas));
    mix(static_cast<std::uint64_t>(overlayOptions_.centreCross));
    mix(static_cast<std::uint64_t>(overlayOptions_.thirds));
    mix(static_cast<std::uint64_t>(overlayOptions_.rulers));
    mix(static_cast<std::uint64_t>(overlayOptions_.pixelGrid));
    mix(static_cast<std::uint64_t>(request.containerRect.width() * 1000.0));
    mix(static_cast<std::uint64_t>(request.containerRect.height() * 1000.0));
    request.overlaySignature = signature;
    return request;
}

QPixmap ViewerEditor::renderCpuCoverSnapshot() {
    const QRect content = contentRect().toRect();
    if (content.isEmpty()) {
        return {};
    }
    // Physical device extent is validated (finite positive DPR, ceil semantics, int/uint32 and
    // 64 MiB/4 device-pixel bounds) BEFORE any allocation or int cast.
    std::uint32_t deviceWidth = 0;
    std::uint32_t deviceHeight = 0;
    if (!checkedOverlayDeviceExtent(content.width(), content.height(), devicePixelRatioF(),
                                    kResidentOverlayByteBudget, deviceWidth, deviceHeight)) {
        return {};
    }
    const double ratio = devicePixelRatioF() > 0.0 ? devicePixelRatioF() : 1.0;
    QPixmap snapshot(static_cast<int>(deviceWidth), static_cast<int>(deviceHeight));
    snapshot.setDevicePixelRatio(ratio);
    snapshot.fill(Qt::transparent);
    QPainter painter(&snapshot);
    if (!painter.isActive()) {
        return {};
    }
    painter.translate(-content.topLeft());
    // The cover must reproduce the CPU paint, including the canvas background (the display buffer
    // may be transparent where no layer covers).
    drawCanvasBackground(painter, content, background_);
    coverSnapshotInProgress_ = true;
    paintViewerContent(painter);
    coverSnapshotInProgress_ = false;
    painter.end();
    return snapshot;
}

QPixmap ViewerEditor::renderCpuCoverSnapshotForTest() { return renderCpuCoverSnapshot(); }

ResidentPresentRequest ViewerEditor::buildResidentPresentRequestForTest() {
    return buildResidentPresentRequest();
}

void ViewerEditor::updateGpuResidentPresentation() {
    if (gpuResident_ == nullptr) {
        return;
    }
    const auto frame = displayedFrame();
    if (frame == nullptr ||
        frame->provenance().provider != runtime::PreviewDisplayProvider::GpuResident) {
        residentActive_ = false;
        if (gpuContainer_ != nullptr) {
            gpuContainer_->hide();
        }
        // No resident frame: expose the host's own CPU paint, never a permanently visible cover.
        if (gpuResident_ != nullptr) {
            gpuResident_->concealCpuCover();
        }
        gpuPresentedFrame_ = frame;
        gpuPresentedTransform_ = transform_;
        return;
    }
    if (!currentDisplayGeometry().has_value()) {
        residentActive_ = false;
        return;
    }
    const ResidentPresentRequest request = buildResidentPresentRequest();
    if (request.destination.isEmpty()) {
        return;
    }
    const bool intended = gpuResident_->present(*frame, request);
    if (!intended) {
        // Refusal / unsupported / stale lease: keep or restore the CPU paint and hide the native
        // container. Never claim a blank activation.
        residentActive_ = false;
        if (gpuContainer_ != nullptr) {
            gpuContainer_->hide();
        }
        return;
    }
    if (QWidget* container = gpuResident_->container(); container != nullptr) {
        if (gpuContainer_ != container) {
            // The presenter already parented this container to `this` BEFORE the window was
            // exposed/attached (setContainerParent). Never reparent a live surface here.
            gpuContainer_ = container;
            gpuContainer_->setFocusPolicy(Qt::StrongFocus);
            gpuContainer_->setMouseTracking(true);
        }
        const QRect target = contentRect().toRect();
        if (gpuContainer_->geometry() != target) {
            gpuContainer_->setGeometry(target);
        }
        // The window must be exposed for the surface to exist and attach. The native CPU cover
        // (owned by the controller) stays above it until a genuine owner present acknowledgement,
        // so the last valid CPU image remains visible; `residentActive_` is not claimed until then.
        if (!gpuContainer_->isVisible()) {
            gpuContainer_->show();
        }
    }
    gpuPresentedFrame_ = frame;
    gpuPresentedTransform_ = transform_;
}

void ViewerEditor::requestResidentCpuFallback() {
    const auto frame = displayedFrame();
    if (frame == nullptr ||
        frame->provenance().provider != runtime::PreviewDisplayProvider::GpuResident) {
        return;
    }
    if (cpuFallbackTask_.has_value()) {
        // One fallback at a time; pollResidentCpuFallback() requeues the latest after a stale or
        // failed completion.
        return;
    }
    const auto identity = frame->desiredIdentity();
    if (cpuFallbackFrame_ != nullptr && cpuFallbackFrame_->desiredIdentity() == identity) {
        // A valid cached CPU fallback for this exact request already answers it. A repeated present
        // refusal re-shows the native cover, so expose the cached CPU paint again instead of
        // returning and leaving the cover occluding it forever. This branch is only reached after a
        // genuine resident present failure (failCpu), so no live native attach still needs the
        // cover.
        if (gpuResident_ != nullptr) {
            gpuResident_->concealCpuCover();
        }
        update();
        return;
    }
    if (cpuFallbackFailedIdentity_.has_value() && *cpuFallbackFailedIdentity_ == identity) {
        return; // an actual failure for this exact request is not retried forever
    }
    // The existing async viewer-analysis CPU path answers the SAME request identity with real CPU
    // pixels. It is not a GPU readback and never runs on the UI thread.
    const auto submission = previewController_.submitViewerAnalysis(identity);
    if (submission.accepted()) {
        cpuFallbackTask_ = submission.handle;
        cpuFallbackIdentity_ = identity;
        if (gpuResidentTimer_ != nullptr && !gpuResidentTimer_->isActive()) {
            gpuResidentTimer_->start();
        }
    }
}

void ViewerEditor::pollResidentCpuFallback() {
    if (!cpuFallbackTask_.has_value()) {
        return;
    }
    auto result = cpuFallbackTask_->tryTakeResult();
    if (!result.has_value()) {
        return;
    }
    const auto requested = cpuFallbackIdentity_;
    cpuFallbackTask_.reset();
    cpuFallbackIdentity_.reset();

    const auto currentFrame = displayedFrame();
    const bool stillResident =
        currentFrame != nullptr &&
        currentFrame->provenance().provider == runtime::PreviewDisplayProvider::GpuResident;
    const std::optional<runtime::PreviewRequestIdentity> current =
        stillResident ? std::optional{currentFrame->desiredIdentity()} : std::nullopt;

    bool succeeded = false;
    if (result->state() == runtime::TaskState::Succeeded && result->value().has_value()) {
        const auto& value = *result->value();
        if (value != nullptr && value->frame() != nullptr) {
            succeeded = true;
            const auto completed = value->frame();
            if (current.has_value() &&
                cpuFallbackCompletionIsCurrent(completed->desiredIdentity(), *current)) {
                cpuFallbackFrame_ = completed;
                cpuFallbackFailedIdentity_.reset();
                // A real CPU fallback frame is now painted directly; the native cover must not
                // keep occluding it.
                if (gpuResident_ != nullptr) {
                    gpuResident_->concealCpuCover();
                }
                update();
                return;
            }
            // Stale completion for a superseded identity: drop it and requeue the latest below.
        }
    }
    if (!succeeded && requested.has_value()) {
        cpuFallbackFailedIdentity_ = requested;
    }

    const bool satisfied = current.has_value() && cpuFallbackFrame_ != nullptr &&
                           cpuFallbackFrame_->desiredIdentity() == *current;
    const bool failed = current.has_value() && cpuFallbackFailedIdentity_.has_value() &&
                        *cpuFallbackFailedIdentity_ == *current;
    if (stillResident && !satisfied && !failed) {
        requestResidentCpuFallback();
    }
}

void ViewerEditor::pollGpuResident() {
    if (gpuResident_ == nullptr) {
        return;
    }
    gpuResident_->poll();
    pollResidentCpuFallback();
    if (!residentFrameIsDisplayed() && !residentActive_) {
        return;
    }
    if (gpuPresentedFrame_ != displayedFrame() || !(gpuPresentedTransform_ == transform_)) {
        updateGpuResidentPresentation();
    }
}

void ViewerEditor::forwardGpuInput(const ViewerGpuInputEvent& event) {
    const QPointF origin = gpuContainer_ != nullptr ? QPointF(gpuContainer_->pos()) : QPointF{};
    const QPointF local = event.local + origin;
    const QPointF global = event.global;
    // Native present routes input through the presenter's QWindow, which is NOT a descendant the
    // workspace's own widget event filter watches. Calling the handlers directly would therefore
    // bypass both that filter (panel activation) and Qt's normal delivery. Re-dispatch through
    // QCoreApplication::sendEvent() so application and receiver event filters see the event exactly
    // as they would for a click on the widget itself; the viewer is the correct receiver because
    // `local` was translated into its coordinates above.
    const auto dispatch = [this](QEvent* forwarded) {
        static_cast<void>(QCoreApplication::sendEvent(this, forwarded));
        updateGpuResidentPresentation();
    };
    switch (event.kind) {
    case ViewerGpuInputKind::MousePress:
    case ViewerGpuInputKind::MouseRelease:
    case ViewerGpuInputKind::MouseDoubleClick: {
        const QEvent::Type type =
            event.kind == ViewerGpuInputKind::MousePress     ? QEvent::MouseButtonPress
            : event.kind == ViewerGpuInputKind::MouseRelease ? QEvent::MouseButtonRelease
                                                             : QEvent::MouseButtonDblClick;
        QMouseEvent forwarded(type, local, local, global, event.button, event.buttons,
                              event.modifiers);
        dispatch(&forwarded);
        break;
    }
    case ViewerGpuInputKind::MouseMove: {
        QMouseEvent forwarded(QEvent::MouseMove, local, local, global, Qt::NoButton, event.buttons,
                              event.modifiers);
        dispatch(&forwarded);
        break;
    }
    case ViewerGpuInputKind::Wheel: {
        QWheelEvent forwarded(local, global, event.pixelDelta, event.angleDelta, event.buttons,
                              event.modifiers, event.scrollPhase, event.inverted);
        dispatch(&forwarded);
        break;
    }
    case ViewerGpuInputKind::KeyPress:
    case ViewerGpuInputKind::KeyRelease: {
        QKeyEvent forwarded(event.kind == ViewerGpuInputKind::KeyPress ? QEvent::KeyPress
                                                                       : QEvent::KeyRelease,
                            event.key, event.modifiers, event.text, event.autoRepeat);
        dispatch(&forwarded);
        break;
    }
    case ViewerGpuInputKind::InputMethod: {
        QInputMethodEvent forwarded;
        forwarded.setCommitString(event.text);
        dispatch(&forwarded);
        break;
    }
    case ViewerGpuInputKind::FocusIn:
        setFocus(Qt::OtherFocusReason);
        break;
    case ViewerGpuInputKind::FocusOut:
        if (textEdit_) {
            finishTextEditing(true);
        }
        break;
    case ViewerGpuInputKind::Enter:
        break;
    case ViewerGpuInputKind::Leave: {
        QEvent forwarded(QEvent::Leave);
        dispatch(&forwarded);
        break;
    }
    case ViewerGpuInputKind::GrabMouse:
        grabMouse();
        break;
    case ViewerGpuInputKind::UngrabMouse:
        releaseMouse();
        break;
    case ViewerGpuInputKind::Cancel:
        cancelCreation();
        if (dragActive_) {
            endDrag(false);
        }
        updateGpuResidentPresentation();
        break;
    }
}

bool ViewerEditor::hasLiveNativeTarget() const {
    return gpuResident_ != nullptr && gpuResident_->hasLiveTarget();
}

EditorNativeSurface::PrepareOutcome
ViewerEditor::prepareNativeSurfaceMutation(const std::uint64_t generation,
                                           PrepareCallback completion) {
    if (gpuResident_ == nullptr) {
        if (completion) {
            completion(generation, PrepareResult{true, "no native surface implementation"});
        }
        return EditorNativeSurface::PrepareOutcome::NoLiveTarget;
    }
    return gpuResident_->prepareForMutation(generation, completion);
}

void ViewerEditor::resumeNativeSurfaceAfterMutation() {
    if (gpuResident_ != nullptr) {
        gpuResident_->resumeAfterMutation();
        residentActive_ = false;
        gpuPresentedFrame_.reset();
        cpuFallbackFrame_.reset();
        cpuFallbackIdentity_.reset();
        cpuFallbackFailedIdentity_.reset();
        if (cpuFallbackTask_.has_value()) {
            cpuFallbackTask_->cancel();
            cpuFallbackTask_.reset();
        }
        if (gpuContainer_ != nullptr) {
            gpuContainer_->hide();
            gpuContainer_ = nullptr;
        }
    }
}

std::string ViewerEditor::nativeSurfaceDiagnostic() const {
    return gpuResident_ != nullptr ? gpuResident_->mutationDiagnostic()
                                   : std::string{"no native surface implementation"};
}

bool ViewerEditor::residentPresentationActiveForTest() const noexcept { return residentActive_; }

bool ViewerEditor::gpuResidentConfiguredForTest() const noexcept {
    return gpuResident_ != nullptr && gpuResident_->configured();
}

std::size_t ViewerEditor::gpuPresentAttemptCountForTest() const noexcept {
    return gpuResident_ != nullptr ? gpuResident_->presentAttemptCount() : 0U;
}

std::size_t ViewerEditor::gpuPresentAcceptedCountForTest() const noexcept {
    return gpuResident_ != nullptr ? gpuResident_->acceptedPresentCount() : 0U;
}

std::uint64_t ViewerEditor::gpuNativeAppliedSequenceForTest() const noexcept {
    return gpuResident_ != nullptr ? gpuResident_->nativeAppliedSequence() : 0U;
}

std::uint64_t ViewerEditor::gpuNativePresentCountForTest() const noexcept {
    return gpuResident_ != nullptr ? gpuResident_->nativePresentCount() : 0U;
}

std::uint64_t ViewerEditor::gpuNativeLastEnqueuedSequenceForTest() const noexcept {
    return gpuResident_ != nullptr ? gpuResident_->nativeLastEnqueuedSequence() : 0U;
}

std::string ViewerEditor::gpuPresentationDiagnosticForTest() const {
    return gpuResident_ != nullptr ? gpuResident_->diagnostic() : std::string{};
}

void ViewerEditor::pollGpuResidentForTest() { pollGpuResident(); }

void ViewerEditor::setGpuPresentationPortForTest(std::shared_ptr<ViewerGpuPort> port) {
    if (gpuResident_ == nullptr) {
        return;
    }
    gpuResident_->setPresentationPortForTest(std::move(port));
    ViewerGpuResidentController::Dependencies dependencies;
    dependencies.containerParent = this;
    dependencies.devicePixelRatio = devicePixelRatioF() > 0.0 ? devicePixelRatioF() : 1.0;
    gpuResident_->setDependencies(std::move(dependencies));
    if (gpuResidentTimer_ != nullptr) {
        gpuResidentTimer_->start();
    }
    updateGpuResidentPresentation();
    update();
}
