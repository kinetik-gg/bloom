// Deterministic off-UI raster delay with genuine native presentation. PHASE markers are
// capture synchronization only; owner counters do not prove compositor-visible pixels.
void runOutlineProof(ui::ViewerGpuResidentController& controller,
                     const runtime::PreparedPreviewFrame& frame, runtime::TaskScheduler& scheduler,
                     Expectations& expectations, const std::function<void()>& expireLeases,
                     const std::function<void(bool)>& gateOwner,
                     const std::function<runtime::GpuResidentFrameLease()>& produceLease) {
    const auto initialGeometry = ui::residentFrameGeometry(frame);
    expectations.expect(initialGeometry.has_value(), "outline proof starts with a live lease");
    if (!initialGeometry)
        return;
    const auto proofLease = initialGeometry->lease;
    struct Gate final {
        std::mutex mutex;
        std::condition_variable cv;
        bool started = false;
        bool release = false;
    };
    const auto outline = [](const double x, const std::uint64_t signature) {
        auto request = makeRequest(320.0, 240.0);
        request.destination = QRectF(x, 40.0, 180.0, 140.0);
        QPicture picture;
        QPainter painter(&picture);
        painter.setPen(QPen(QColor(255, 0, 0), 4.0));
        painter.setBrush(Qt::NoBrush);
        painter.drawRect(QRectF(x, 40.0, 180.0, 140.0));
        painter.end();
        request.overlay = {picture, QSizeF(320.0, 240.0), signature, true};
        request.overlaySignature = signature;
        return request;
    };
    const auto startupGate = [&](const std::uint64_t id) {
        auto held = std::make_shared<Gate>();
        auto submission = scheduler.submit<void>(
            runtime::TaskRequest("outline-startup-gate", {runtime::TaskOwnerKind::PanelRequest,
                                                          runtime::TaskOwnerId::fromRaw(id)}),
            [held](runtime::TaskContext& context) {
                std::unique_lock lock(held->mutex);
                held->started = true;
                while (!held->release && !context.isCancellationRequested())
                    held->cv.wait_for(lock, std::chrono::milliseconds(10));
                return runtime::TaskResult<void>::succeeded();
            });
        expectations.expect(submission.accepted(), "startup worker gate is admitted");
        return held;
    };
    auto firstGate = startupGate(490);
    expectations.expect(waitUntil(
                            [&] {
                                std::lock_guard lock(firstGate->mutex);
                                return firstGate->started;
                            },
                            10'000),
                        "startup CPU worker is held");
    expectations.expect(controller.present(frame, outline(20.0, 390)),
                        "startup active complete pair captured");
    for (int step = 0; step < 10; ++step)
        expectations.expect(controller.present(frame, outline(21.0 + step, 391)),
                            "startup producer continues");
    auto laterGate = startupGate(491);
    {
        std::lock_guard lock(firstGate->mutex);
        firstGate->release = true;
    }
    firstGate->cv.notify_all();
    expectations.expect(
        waitUntil(
            [&] {
                controller.poll();
                std::lock_guard lock(laterGate->mutex);
                return laterGate->started && controller.appliedDestination().x() == 20.0 &&
                       !controller.cpuCoverVisibleForTest();
            },
            20'000),
        "startup reveals a coherent intermediate pair despite newer pending geometry");
    expectations.expect(!controller.publicationCurrent(),
                        "startup intermediate publication remains explicitly stale");
    {
        std::lock_guard lock(laterGate->mutex);
        laterGate->release = true;
    }
    laterGate->cv.notify_all();
    expectations.expect(waitUntil(
                            [&] {
                                controller.poll();
                                return controller.presentationAcknowledged();
                            },
                            20'000),
                        "startup converges to latest complete pair");
    expectations.expect(controller.present(frame, outline(30.0, 401)),
                        "the first native outline request is admitted");
    expectations.expect(waitUntil(
                            [&] {
                                controller.poll();
                                return controller.overlayRasterCount() >= 1 &&
                                       controller.publicationCurrent() &&
                                       controller.presentationAcknowledged();
                            },
                            20'000),
                        "the complete first image/outline pair is acknowledged");
    for (int tick = 0; tick < 40; ++tick) {
        controller.poll();
        pumpQt(4);
    }
    std::cout << "PHASE_OUTLINE_BASE\n" << std::flush;
    pumpQt(2500);

    auto gate = std::make_shared<Gate>();
    auto blocker = scheduler.submit<void>(
        runtime::TaskRequest("native-outline-raster-delay", {runtime::TaskOwnerKind::PanelRequest,
                                                             runtime::TaskOwnerId::fromRaw(501)}),
        [gate](runtime::TaskContext& context) {
            std::unique_lock lock(gate->mutex);
            gate->started = true;
            gate->cv.notify_all();
            while (!gate->release && !context.isCancellationRequested())
                gate->cv.wait_for(lock, std::chrono::milliseconds(10));
            return context.isCancellationRequested() ? runtime::TaskResult<void>::cancelled()
                                                     : runtime::TaskResult<void>::succeeded();
        });
    expectations.expect(blocker.accepted(), "the worker delay task is admitted");
    expectations.expect(waitUntil(
                            [&] {
                                std::lock_guard lock(gate->mutex);
                                return gate->started;
                            },
                            10'000),
                        "the sole CPU raster worker is deterministically held");
    const auto before = controller.nativeLastEnqueuedSequence();
    expectations.expect(controller.present(frame, outline(90.0, 402)),
                        "changed outline is accepted while its raster is delayed");
    // Poll while the raster is blocked. This keeps the real UI and presentation owner running.
    for (int tick = 0; tick < 40; ++tick) {
        controller.poll();
        pumpQt(4);
    }
    std::cout << "PHASE_OUTLINE_PENDING sequence=" << controller.nativeLastEnqueuedSequence()
              << " previous=" << before << '\n'
              << std::flush;
    expectations.expect(controller.nativeLastEnqueuedSequence() == before,
                        "a pending required outline never publishes an outline-free replacement");
    pumpQt(2500);
    {
        std::lock_guard lock(gate->mutex);
        gate->release = true;
    }
    gate->cv.notify_all();
    expectations.expect(waitUntil(
                            [&] {
                                controller.poll();
                                return controller.nativeLastEnqueuedSequence() > before &&
                                       controller.publicationCurrent() &&
                                       controller.presentationAcknowledged();
                            },
                            20'000),
                        "the completed replacement image/outline pair is acknowledged");
    std::cout << "PHASE_OUTLINE_REPLACED\n" << std::flush;
    pumpQt(2500);
    auto removed = makeRequest(320.0, 240.0);
    removed.destination = QRectF(90.0, 40.0, 180.0, 140.0);
    expectations.expect(controller.present(frame, removed),
                        "deliberate outline removal is admitted");
    expectations.expect(waitUntil(
                            [&] {
                                controller.poll();
                                return controller.presentationAcknowledged();
                            },
                            20'000),
                        "deliberate removal is acknowledged without stale outline reuse");
    std::cout << "PHASE_OUTLINE_REMOVED\n" << std::flush;
    pumpQt(2500);
    // Exact commands, not request metadata or the caller's hash, govern semantic reuse.
    const auto equivalentFrame = makeResidentFrame(proofLease, 501, 7, 2);
    expectations.expect(controller.present(frame, outline(90.0, 1000)),
                        "semantic base is admitted");
    expectations.expect(waitUntil(
                            [&] {
                                controller.poll();
                                return controller.presentationAcknowledged();
                            },
                            20'000),
                        "semantic base settles");
    const auto rasterBeforeReuse = controller.overlayRasterCount();
    expectations.expect(equivalentFrame &&
                            controller.present(*equivalentFrame, outline(90.0, 2000)),
                        "equivalent commands across generation/revision/time are admitted");
    expectations.expect(waitUntil(
                            [&] {
                                controller.poll();
                                return controller.presentationAcknowledged();
                            },
                            20'000),
                        "equivalent publication settles");
    expectations.expect(
        controller.overlayRasterCount() == rasterBeforeReuse,
        "equivalent commands reuse raster despite changed request metadata/signature");
    expectations.expect(controller.present(frame, outline(100.0, 2000)),
                        "different commands with colliding signature are admitted");
    expectations.expect(waitUntil(
                            [&] {
                                controller.poll();
                                return controller.presentationAcknowledged();
                            },
                            20'000),
                        "changed colliding-signature commands settle");
    expectations.expect(controller.overlayRasterCount() > rasterBeforeReuse,
                        "a matching signature never authorizes different commands");

    const auto startGate = [&](const std::uint64_t id) {
        auto held = std::make_shared<Gate>();
        auto submission = scheduler.submit<void>(
            runtime::TaskRequest("outline-semantic-gate", {runtime::TaskOwnerKind::PanelRequest,
                                                           runtime::TaskOwnerId::fromRaw(id)}),
            [held](runtime::TaskContext& context) {
                std::unique_lock lock(held->mutex);
                held->started = true;
                while (!held->release && !context.isCancellationRequested())
                    held->cv.wait_for(lock, std::chrono::milliseconds(10));
                return runtime::TaskResult<void>::succeeded();
            });
        expectations.expect(submission.accepted(), "a semantic worker gate is admitted");
        return held;
    };
    const auto releaseGate = [](const std::shared_ptr<Gate>& held) {
        {
            std::lock_guard lock(held->mutex);
            held->release = true;
        }
        held->cv.notify_all();
    };
    const auto gateStarted = [](const std::shared_ptr<Gate>& held) {
        std::lock_guard lock(held->mutex);
        return held->started;
    };
    auto floodGate = startGate(510);
    expectations.expect(waitUntil([&] { return gateStarted(floodGate); }, 10'000),
                        "flood gate starts");
    expectations.expect(controller.present(frame, outline(110.0, 2100)),
                        "active changed geometry captured");
    for (int step = 0; step < 12; ++step)
        expectations.expect(controller.present(frame, outline(120.0 + step, 2200)),
                            "newest geometry coalesces");
    // This gate sits after the active raster and before the newest raster. It proves the active
    // older coherent pair makes progress while the newer producer continues and is still pending.
    auto newestGate = startGate(511);
    releaseGate(floodGate);
    expectations.expect(waitUntil(
                            [&] {
                                controller.poll();
                                return gateStarted(newestGate) &&
                                       controller.appliedDestination().x() == 110.0;
                            },
                            20'000),
                        "continuous geometry publishes a coherent intermediate pair before the "
                        "newest raster completes");
    expectations.expect(!controller.publicationCurrent(), "intermediate pair is explicitly stale");
    expectations.expect(controller.present(frame, outline(140.0, 2300)),
                        "producer continues after intermediate progress");
    releaseGate(newestGate);
    expectations.expect(waitUntil(
                            [&] {
                                controller.poll();
                                return controller.presentationAcknowledged();
                            },
                            20'000),
                        "continuous geometry converges to newest complete pair");
    expectations.expect(controller.appliedDestination().x() == 140.0,
                        "newest geometry is applied exactly");

    auto removalGate = startGate(520);
    expectations.expect(waitUntil([&] { return gateStarted(removalGate); }, 10'000),
                        "removal gate starts");
    expectations.expect(controller.present(frame, outline(150.0, 2400)),
                        "old selected outline captured");
    expectations.expect(controller.present(frame, removed),
                        "explicit removal invalidates old active outline");
    expectations.expect(controller.present(frame, outline(160.0, 2500)),
                        "new valid outline is authored in the same caller domain zero");
    auto afterRemovalGate = startGate(521);
    releaseGate(removalGate);
    expectations.expect(waitUntil(
                            [&] {
                                controller.poll();
                                return gateStarted(afterRemovalGate);
                            },
                            20'000),
                        "old raster completes before new valid raster");
    for (int tick = 0; tick < 40; ++tick) {
        controller.poll();
        pumpQt(4);
    }
    expectations.expect(controller.appliedDestination().x() != 150.0,
                        "removed old outline never resurrects before the new valid raster");
    releaseGate(afterRemovalGate);
    expectations.expect(waitUntil(
                            [&] {
                                controller.poll();
                                return controller.presentationAcknowledged();
                            },
                            20'000),
                        "new valid outline settles after removal");

    auto malformed = outline(170.0, 2600);
    malformed.overlay.logicalSize = QSizeF(0.0, 0.0);
    const auto lastCompleteSequence = controller.nativeLastEnqueuedSequence();
    expectations.expect(controller.present(frame, malformed),
                        "failed raster intent is accepted asynchronously");
    expectations.expect(waitUntil(
                            [&] {
                                controller.poll();
                                return controller.diagnostic().find("overlay failed") !=
                                       std::string::npos;
                            },
                            20'000),
                        "raster failure reports retained stale complete frame");
    expectations.expect(
        controller.nativeLastEnqueuedSequence() == lastCompleteSequence &&
            controller.appliedDestination().x() == 160.0 && !controller.publicationCurrent(),
        "failed required raster never replaces a complete pair with missing outlines");

    // More than two desired publications while the real owner is gated cannot overwrite the
    // sole admitted frame's correlation, and the complete newest update is retained boundedly.
    gateOwner(true);
    const auto admittedBefore = controller.acceptedPresentCount();
    for (std::uint64_t generation = 600; generation < 610; ++generation) {
        auto next = makeResidentFrame(proofLease, generation);
        expectations.expect(next && controller.present(*next, removed),
                            "gated owner flood is accepted");
    }
    expectations.expect(controller.acceptedPresentCount() == admittedBefore + 1,
                        "one in-flight admission bounds exact frame correlation under owner delay");
    gateOwner(false);
    expectations.expect(waitUntil(
                            [&] {
                                controller.poll();
                                return controller.presentationAcknowledged();
                            },
                            20'000),
                        "gated owner flood converges without losing frame identity");
    expectations.expect(controller.appliedFrame() &&
                            controller.appliedFrame()->desiredIdentity().requestGeneration == 609,
                        "owner-applied metadata names the newest actual admitted frame");

    // Repeat a delayed raster, then invalidate its genuine lease on the owner before completion.
    // The controller must revalidate at publication, never dereference vanished lease geometry.
    auto expiryGate = std::make_shared<Gate>();
    auto expiryBlocker = scheduler.submit<void>(
        runtime::TaskRequest("native-outline-expiry-delay", {runtime::TaskOwnerKind::PanelRequest,
                                                             runtime::TaskOwnerId::fromRaw(502)}),
        [expiryGate](runtime::TaskContext& context) {
            std::unique_lock lock(expiryGate->mutex);
            expiryGate->started = true;
            while (!expiryGate->release && !context.isCancellationRequested())
                expiryGate->cv.wait_for(lock, std::chrono::milliseconds(10));
            return runtime::TaskResult<void>::succeeded();
        });
    expectations.expect(expiryBlocker.accepted(), "the expiry worker gate is admitted");
    expectations.expect(waitUntil(
                            [&] {
                                std::lock_guard lock(expiryGate->mutex);
                                return expiryGate->started;
                            },
                            10'000),
                        "the expiry raster worker is held");
    const auto beforeExpiry = controller.nativeLastEnqueuedSequence();
    expectations.expect(controller.present(frame, outline(100.0, 403)),
                        "the pre-expiry required overlay is accepted");
    expireLeases();
    {
        std::lock_guard lock(expiryGate->mutex);
        expiryGate->release = true;
    }
    expiryGate->cv.notify_all();
    expectations.expect(waitUntil(
                            [&] {
                                controller.poll();
                                return controller.diagnostic().find("no longer live") !=
                                       std::string::npos;
                            },
                            20'000),
                        "a lease expired during raster is safely rejected at publication");
    expectations.expect(controller.nativeLastEnqueuedSequence() == beforeExpiry,
                        "late lease expiry never submits an invalid publication");
    auto raceLease = produceLease();
    auto raceFrame = makeResidentFrame(raceLease, 700);
    expectations.expect(raceFrame != nullptr, "owner-refusal race has a fresh live lease");
    gateOwner(true);
    expectations.expect(raceFrame && controller.present(*raceFrame, removed),
                        "valid race update is admitted");
    const auto appliedBeforeRefusal = controller.nativeAppliedSequence();
    expireLeases();
    gateOwner(false);
    expectations.expect(waitUntil(
                            [&] {
                                controller.poll();
                                return controller.diagnostic().find(
                                           "resident lease was rejected") != std::string::npos;
                            },
                            20'000),
                        "owner rejection carries its concrete diagnostic and releases admission");
    expectations.expect(controller.nativeAppliedSequence() == appliedBeforeRefusal &&
                            !controller.publicationCurrent(),
                        "owner refusal never fabricates applied progress or current intent");
    auto recovery = makeResidentFrame(produceLease(), 701);
    expectations.expect(recovery && controller.present(*recovery, removed),
                        "fresh recovery is admitted after owner refusal");
    expectations.expect(waitUntil(
                            [&] {
                                controller.poll();
                                return controller.presentationAcknowledged();
                            },
                            20'000),
                        "refused admission never wedges later coherent publication");

    // Safe refusal retains the exact owner-applied descriptor even after all its admission leases
    // expire. Unsafe early refusals must compare the latest requested safety domain, not the last
    // admitted publication. The private compositor captures these three stable surfaces.
    const auto known = controller.appliedDisplay();
    expectations.expect(known && known->geometry && known->containerRect == removed.containerRect,
                        "correlated applied geometry is immutable and captured with its container");
    std::cout << "PHASE_TRUTH_BASE\n" << std::flush;
    pumpQt(2500);
    auto rejected = makeResidentFrame(produceLease(), 702);
    gateOwner(true);
    expectations.expect(rejected && controller.present(*rejected, removed),
                        "same-domain refusal candidate is admitted before genuine lease expiry");
    expireLeases();
    gateOwner(false);
    expectations.expect(waitUntil(
                            [&] {
                                controller.poll();
                                return controller.diagnostic().find(
                                           "resident lease was rejected") != std::string::npos;
                            },
                            20'000),
                        "same-domain owner refusal is reported concretely");
    expectations.expect(
        known && controller.appliedDisplay() &&
            controller.appliedDisplay()->frame == known->frame &&
            controller.appliedDisplay()->geometry && known->geometry &&
            controller.appliedDisplay()->geometry->displayWindow ==
                known->geometry->displayWindow &&
            controller.appliedDisplay()->geometry->pixelAspect == known->geometry->pixelAspect &&
            controller.appliedDisplay()->geometry->lease.id() == known->geometry->lease.id() &&
            controller.canRetainAppliedDisplay() && !controller.cpuCoverVisibleForTest() &&
            !controller.publicationCurrent(),
        "safe refusal retains readable actual geometry and native pixels without claiming "
        "recovery");
    std::cout << "PHASE_TRUTH_RETAINED\n" << std::flush;
    pumpQt(2500);
    QPointer<QWindow> persistentCover = controller.cpuCoverPersistentWindowForTest();
    const auto paintsBeforeCover = controller.cpuCoverPaintCountForTest();
    const auto appliedBeforeCover = controller.nativeAppliedSequence();
    const auto windowsBeforeCover = QGuiApplication::allWindows().size();
    auto unsafe = removed;
    unsafe.overlayDomainRevision = 77;
    expectations.expect(
        rejected && !controller.present(*rejected, unsafe) &&
            !controller.canRetainAppliedDisplay() && controller.cpuCoverVisibleForTest(),
        "early expired admission with changed selection domain covers old-domain pixels");
    expectations.expect(waitUntil(
                            [&] {
                                controller.poll();
                                return controller.cpuCoverPaintCountForTest() > paintsBeforeCover;
                            },
                            5000) &&
                            persistentCover && persistentCover->isVisible() &&
                            controller.cpuCoverPersistentWindowForTest() == persistentCover &&
                            controller.nativeAppliedSequence() == appliedBeforeCover,
                        "the persistent child repaints an opaque cover after transparent idle "
                        "without another successful GPU parent present");
    std::cout << "PHASE_TRUTH_COVER\n" << std::flush;
    pumpQt(2500);
    const auto coverPaints = controller.cpuCoverPaintCountForTest();
    for (int repeat = 0; repeat < 20; ++repeat) {
        controller.revealCpuCover(unsafe.containerRect.toRect());
        controller.poll();
        QApplication::processEvents();
    }
    expectations.expect(controller.cpuCoverPaintCountForTest() == coverPaints &&
                            QGuiApplication::allWindows().size() == windowsBeforeCover,
                        "unchanged refusal reveal and poll do not paint or allocate new windows");
    const auto recover = [&](const ui::ResidentPresentRequest& request,
                             const std::uint64_t generation) {
        const auto next = makeResidentFrame(produceLease(), generation);
        expectations.expect(next && controller.present(*next, request),
                            "a real recovery candidate is admitted");
        expectations.expect(waitUntil(
                                [&] {
                                    controller.poll();
                                    return controller.presentationAcknowledged();
                                },
                                20'000),
                            "recovery applies its own complete descriptor");
        return next;
    };
    auto live = recover(removed, 703);
    expireLeases();
    auto moved = removed;
    moved.containerRect.translate(10.0, 0.0);
    expectations.expect(live && !controller.present(*live, moved) &&
                            !controller.canRetainAppliedDisplay() &&
                            controller.cpuCoverVisibleForTest(),
                        "equal-device-extent logical relocation is unsafe on early refusal");
    live = recover(removed, 704);
    expireLeases();
    auto ratio = removed;
    ratio.containerRect.setSize(QSizeF(160.0, 120.0));
    ratio.devicePixelRatio = 2.0;
    expectations.expect(live && !controller.present(*live, ratio) &&
                            !controller.canRetainAppliedDisplay() &&
                            controller.cpuCoverVisibleForTest(),
                        "equal-device-extent DPR change cannot relabel old painted geometry");
    live = recover(removed, 705);
    const auto foreign = makeResidentFrame(produceLease(), 706, 1, 0, 2);
    expireLeases();
    expectations.expect(
        foreign && !controller.present(*foreign, removed) &&
            !controller.canRetainAppliedDisplay() && controller.cpuCoverVisibleForTest(),
        "same composition id in a foreign project refuses native last-good retention");
    live = recover(removed, 707);
    const auto delayed = makeResidentFrame(produceLease(), 708);
    expectations.expect(delayed && controller.present(*delayed, removed),
                        "late-ack resize candidate is admitted");
    const auto delayedSequence = controller.nativeLastEnqueuedSequence();
    expectations.expect(
        waitUntil([&] { return controller.nativeAppliedSequence() >= delayedSequence; }, 20'000),
        "real owner applies the candidate before controller observes its acknowledgement");
    gateOwner(true);
    expectations.expect(
        controller.requestTargetResize(640, 480) && controller.cpuCoverVisibleForTest() &&
            !controller.publicationCurrent() && !controller.presentationAcknowledged(),
        "direct resize immediately covers and invalidates displayed currentness");
    controller.poll();
    expectations.expect(
        controller.appliedDisplay() &&
            controller.appliedDisplay()->frame->desiredIdentity().requestGeneration == 708 &&
            controller.cpuCoverVisibleForTest() && !controller.publicationCurrent(),
        "late actual old-extent acknowledgement cannot unlock a resized cover");
    gateOwner(false);
    expectations.expect(waitUntil(
                            [&] {
                                controller.poll();
                                return controller.presenterState() ==
                                       ui::ViewerGpuPresenter::State::Active;
                            },
                            20'000),
                        "the real resized native target becomes active");
    expectations.expect(
        controller.cpuCoverVisibleForTest() && !controller.publicationCurrent(),
        "resize without a replacement candidate never claims old pixels are current");
    auto resized = makeRequest(640.0, 480.0);
    live = recover(resized, 709);
    controller.clearPublicationDomain();
    expectations.expect(
        !controller.publicationCurrent() && !controller.presentationAcknowledged() &&
            !controller.canRetainAppliedDisplay(),
        "domain clear resets public readiness without inventing an acknowledgement");
    recover(resized, 710);
}
