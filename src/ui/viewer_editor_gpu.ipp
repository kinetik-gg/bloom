void paintViewerCompositionFrame(QPainter& painter, const QRectF& displayRect) {
    painter.setPen(QPen(kit::color(kit::Color::CompositionFrame), kit::kCompositionFrameWidth));
    painter.setBrush(Qt::NoBrush);
    painter.drawRect(
        displayRect.adjusted(0.0, 0.0, -kit::kCompositionFrameWidth, -kit::kCompositionFrameWidth));
}

void ViewerEditor::setGpuPresentationDependencies(
    std::shared_ptr<runtime::GpuPresentationClient> client, runtime::TaskScheduler* scheduler,
    std::string vulkanLoaderPath, const double devicePixelRatio) {
    if (packedHostShutdown_)
        return;
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
    const auto frame = preparedDisplayFrame();
    return frame != nullptr &&
           frame->provenance().provider == runtime::PreviewDisplayProvider::GpuResident;
}

PreparedPreviewFrameHandle ViewerEditor::paintableCpuFrame() {
    if (packedHostShutdown_)
        return nullptr;
    auto current = preparedDisplayFrame();
    if (!current)
        return frameInCurrentDomain(lastCpuFrame_) ? lastCpuFrame_ : nullptr;
    if (current != nullptr &&
        current->provenance().provider == runtime::PreviewDisplayProvider::GpuResident) {
        if (residentActive_) {
            return nullptr;
        }
        return frameInCurrentDomain(cpuFallbackFrame_) ? cpuFallbackFrame_
               : frameInCurrentDomain(lastCpuFrame_)   ? lastCpuFrame_
                                                       : nullptr;
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
    const auto frame = preparedDisplayFrame();
    const auto format =
        frame ? std::optional{frame->processIdentity().plan->format()} : std::nullopt;
    const auto extent = format ? render::ImageExtent::create(format->width(), format->height())
                               : render::ImageExtent::create(0, 0);
    const std::optional<DisplayGeometry> geometry =
        format && extent ? std::optional{DisplayGeometry{*extent.value(), format->pixelAspect()}}
                         : std::nullopt;
    if (geometry.has_value()) {
        request.destination = viewTransformedDisplayRect(canvasRect(), geometry->extent,
                                                         geometry->pixelAspect, transform_);
    }
    request.channel = static_cast<render::GpuPresentChannel>(channel_);
    // The same three colours the CPU paint uses (drawCanvasBackground / drawFrameBackground), all
    // display-referred: the surround outside the frame, the composition's own background inside
    // it, and the Viewer checker when the Transparency Grid replaces that background.
    const auto presentColor = [](const QColor& color) {
        return render::GpuPresentColor{static_cast<float>(color.redF()),
                                       static_cast<float>(color.greenF()),
                                       static_cast<float>(color.blueF()), 1.0F};
    };
    request.surroundColor = presentColor(viewerSurroundColor(background_));
    request.frameColor = presentColor(frameBackgroundColor());
    request.transparencyGrid = transparencyGrid_;
    // Base then alternate cell, exactly as kit::drawAlphaCheckerboard() paints them on the CPU.
    request.checkerColorA = presentColor(kit::color(kit::Color::ViewerCheckerLight));
    request.checkerColorB = presentColor(kit::color(kit::Color::ViewerCheckerDark));
    request.checkerTilePixels =
        static_cast<float>(kit::px(kit::Size::ViewerChecker) * request.devicePixelRatio);

    // Record the immutable overlays on the UI thread as a QPicture in container-local coordinates.
    // The worker only replays this picture; it reads no widget and no session state.
    const auto resident = frame != nullptr ? residentFrameGeometry(*frame) : std::nullopt;
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
            const ViewerMapping mapping{displayRect, *format, frame->desiredIdentity().resolution,
                                        resident->pixelAspect, *descriptor.value()};
            const auto bounds = previewController_.selectedLayerBounds(frame);
            paintViewerOverlays(painter, canvasRect(), mapping,
                                effectiveZoom(displayRect, *geometry), overlayOptions_,
                                textEdit_ ? std::span<const runtime::EvaluatedOperationBounds>{}
                                          : bounds,
                                pointTextLayers(session_, bounds));
        }
        paintRoi(painter);
        paintCreation(painter);
        paintPathTools(painter);
        paintAnchorEditing(painter);
        paintTextEditing(painter);
        painter.end();
        if (!picture.isNull()) {
            request.overlay.picture = picture;
            request.overlay.logicalSize = request.containerRect.size();
            request.overlay.token = ++gpuOverlayToken_;
            request.overlay.valid = true;
        }
    }
    // Diagnostic signature of recorded dependencies. The controller's actual reuse oracle is
    // bounded exact command equality; request generations alone never invalidate identical
    // geometry.
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
    mix(transparencyGrid_ ? 1ULL : 0ULL);
    mix(static_cast<std::uint64_t>(frameBackgroundColor().rgba()));
    mix(gpuOverlayRevision_);
    mix(static_cast<std::uint64_t>(overlayOptions_.safeAreas));
    mix(static_cast<std::uint64_t>(overlayOptions_.centreCross));
    mix(static_cast<std::uint64_t>(overlayOptions_.thirds));
    mix(static_cast<std::uint64_t>(overlayOptions_.rulers));
    mix(static_cast<std::uint64_t>(overlayOptions_.pixelGrid));
    mix(static_cast<std::uint64_t>(request.containerRect.width() * 1000.0));
    mix(static_cast<std::uint64_t>(request.containerRect.height() * 1000.0));
    request.overlaySignature = signature;
    request.overlayDomainRevision = gpuOverlayRevision_;
    return request;
}

QPixmap ViewerEditor::renderCpuCoverSnapshot() {
    const QRect content = contentRect().toRect();
    // Every returned pixmap, including an unavailable/empty capture, has its own metadata.
    cpuCoverDisplay_ = ViewerDisplayDescriptor{.frame = nullptr,
                                               .mapping = std::nullopt,
                                               .captureRect = contentRect(),
                                               .devicePixelRatio = devicePixelRatioF(),
                                               .channel = channel_,
                                               .surroundColor =
                                                   viewerSurroundColor(background_).rgba(),
                                               .frameColor = frameBackgroundColor().rgba(),
                                               .transparencyGrid = transparencyGrid_,
                                               .overlayRevision = gpuOverlayRevision_,
                                               .native = false,
                                               .cover = true};
    scheduleDisplayedStateRefresh();
    if (content.isEmpty()) {
        return {};
    }
    // Physical device extent is validated (finite positive DPR, ceil semantics, int/uint32 and
    // 64 MiB/4 device-pixel bounds) BEFORE any allocation or int cast.
    std::uint32_t deviceWidth = 0;
    std::uint32_t deviceHeight = 0;
    if (!checkedOverlayDeviceExtent(content.width(), content.height(), devicePixelRatioF(),
                                    cpuCoverByteBudget_, deviceWidth, deviceHeight)) {
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
    coverCaptureDisplay_ = ViewerDisplayDescriptor{.frame = nullptr,
                                                   .mapping = std::nullopt,
                                                   .captureRect = contentRect(),
                                                   .devicePixelRatio = devicePixelRatioF(),
                                                   .channel = channel_,
                                                   .surroundColor =
                                                       viewerSurroundColor(background_).rgba(),
                                                   .frameColor = frameBackgroundColor().rgba(),
                                                   .transparencyGrid = transparencyGrid_,
                                                   .overlayRevision = gpuOverlayRevision_,
                                                   .native = false,
                                                   .cover = true};
    paintViewerContent(painter);
    coverSnapshotInProgress_ = false;
    painter.end();
    // Install metadata from this exact completed capture together with the returned pixmap.
    cpuCoverDisplay_ = std::move(coverCaptureDisplay_);
    coverCaptureDisplay_.reset();
    scheduleDisplayedStateRefresh();
    return snapshot;
}

QPixmap ViewerEditor::renderCpuCoverSnapshotForTest() { return renderCpuCoverSnapshot(); }
QPixmap ViewerEditor::installedCpuCoverSnapshotForTest() const {
    return gpuResident_ ? gpuResident_->cpuCoverSnapshotForTest() : QPixmap{};
}

ResidentPresentRequest ViewerEditor::buildResidentPresentRequestForTest() {
    return buildResidentPresentRequest();
}

void ViewerEditor::updateGpuResidentPresentation() {
    if (gpuPresentationUpdating_) {
        // Re-entrant request from a controller callback during the outer update: drop it. The outer
        // call already owns the coherent surface/geometry transition. This is defensive only -- the
        // reported repaint/OOM depth was never established -- and never weakens a safety gate.
        return;
    }
    gpuPresentationUpdating_ = true;
    updateGpuResidentPresentationBody();
    gpuPresentationUpdating_ = false;
}

void ViewerEditor::updateGpuResidentPresentationBody() {
    if (gpuResident_ == nullptr) {
        return;
    }
    const GpuPublicationDomain domain{session_.snapshot().project().id(),
                                      session_.composition() ? session_.compositionId()
                                                             : document::CompositionId{}};
    if (!gpuPublicationDomain_ || *gpuPublicationDomain_ != domain) {
        // A selection clear and an earlier retirement cannot consume a later project/composition
        // clear. Refresh the cover for EVERY distinct domain before the retirement early return;
        // changing only its safe raster contents never unmaps or re-retires the live GPU target.
        gpuPublicationDomain_ = domain;
        gpuDomainCleared_ = true;
        gpuResident_->clearPublicationDomain();
        residentActive_ = false;
        nativeFailure_.clear();
        if (!frameInCurrentDomain(adjustedFrame_))
            adjustedFrame_.reset();
        if (!frameInCurrentDomain(lastCpuFrame_))
            lastCpuFrame_.reset();
        if (!frameInCurrentDomain(gpuRequestedFrame_))
            gpuRequestedFrame_.reset();
        cpuPaintedDisplay_.reset();
        cpuCoverDisplay_.reset();
        coverCaptureDisplay_.reset();
        cpuFallbackFrame_.reset();
        cpuFallbackFailedIdentity_.reset();
        if (cpuFallbackTask_) {
            cpuFallbackTask_->cancel();
        }
        if (gpuResident_->hasLiveTarget()) {
            gpuResident_->revealCpuCover(contentRect().toRect(), true);
            requestResidentNativeRetire();
            return;
        }
    }
    if (gpuNativeRetirePending_) {
        // A retire-before-unmap is in flight for a live target. Until it genuinely succeeds the
        // container must stay mapped and the cover must stay raised; never hide, never present.
        return;
    }
    const auto frame = preparedDisplayFrame();
    if (!frame && session_.composition()) {
        // A desired adjustment may have no new complete candidate. Keep the actual last-good
        // native pair; this is not the composition-less/domain-clear retirement signal.
        const auto& applied = gpuResident_->appliedDisplay();
        if (applied && applied->domainRevision != gpuOverlayRevision_ && !gpuDomainCleared_) {
            gpuDomainCleared_ = true;
            gpuResident_->clearPublicationDomain();
            gpuResident_->revealCpuCover(contentRect().toRect());
        }
        updatePreviewAccessibility();
        return;
    }
    if (frame == nullptr ||
        frame->provenance().provider != runtime::PreviewDisplayProvider::GpuResident) {
        residentActive_ = false;
        if (gpuResident_->hasLiveTarget()) {
            // A live target must be retired asynchronously BEFORE the container is hidden:
            // unmapping the window-container child can silently replace/destroy the VkSurfaceKHR
            // and latch the presenter into its terminal Retained state, refusing every later
            // present. The cover keeps the last native image hidden while the surface stays mapped.
            requestResidentNativeRetire();
            return;
        }
        // Nothing native is live: hiding the container mutates no surface. Expose the host's own
        // CPU paint, never a permanently visible cover.
        if (gpuContainer_ != nullptr) {
            gpuContainer_->hide();
        }
        gpuResident_->concealCpuCover();
        // This slot correlates an in-flight native presentation, which does not exist on the
        // packed route. Retaining its old CPU image here would survive a cache purge indefinitely.
        gpuRequestedFrame_.reset();
        gpuRequestedTransform_ = transform_;
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
    gpuDomainCleared_ = false;
    if (!intended) {
        nativeFailure_ = QString::fromStdString(gpuResident_->diagnostic());
        // Refusal / unsupported / stale lease: keep or restore the CPU paint. Never claim a blank
        // activation. Do NOT hide the container here: the target is still live, and unmapping the
        // window-container child can silently replace/destroy the surface. Raise the current CPU
        // cover so no stale native frame is exposed while the target stays mapped and safe to
        // retire later.
        residentActive_ = gpuResident_->canRetainAppliedDisplay();
        if (!residentActive_)
            gpuResident_->revealCpuCover(contentRect().toRect());
        updatePreviewAccessibility();
        return;
    }
    if (QWidget* container = gpuResident_->container(); container != nullptr) {
        if (gpuContainer_ != container) {
            // The presenter already parented this container to `this` BEFORE the window was
            // exposed/attached (setContainerParent). Never reparent a live surface here.
            gpuContainer_ = container;
            gpuContainer_->setFocusPolicy(Qt::StrongFocus);
            gpuContainer_->setMouseTracking(true);
            // The native window takes drags over the image itself; Assets drops still make
            // layers (#380).
            installLayerDropTarget(*gpuContainer_, session_);
            if (QWindow* window = gpuResident_->window(); window != nullptr)
                installLayerDropTarget(*window, session_);
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
    gpuRequestedFrame_ = frame;
    gpuRequestedTransform_ = transform_;
    updatePreviewAccessibility();
}

void ViewerEditor::requestResidentCpuFallback() {
    const auto frame = preparedDisplayFrame();
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
            if (gpuResident_->hasLiveTarget())
                gpuResident_->revealCpuCover(contentRect().toRect(), true);
            else
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

    const auto currentFrame = preparedDisplayFrame();
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
                    if (gpuResident_->hasLiveTarget())
                        gpuResident_->revealCpuCover(contentRect().toRect(), true);
                    else
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
    if (!residentFrameIsDisplayed() && !residentActive_) {
        return;
    }
    if (gpuRequestedFrame_ != preparedDisplayFrame() || !(gpuRequestedTransform_ == transform_)) {
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

bool ViewerEditor::gpuCpuCoverVisibleForTest() const noexcept {
    return gpuResident_ != nullptr && gpuResident_->cpuCoverVisibleForTest();
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
