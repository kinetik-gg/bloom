// Actual production packed frame, live Viewer, and active Qt shutdown loop. The private gate
// withholds the real backing free; it does not replace GPU evaluation or retirement evidence.
namespace {
class PackedHostTestTeardown final {
  public:
    explicit PackedHostTestTeardown(std::shared_ptr<render::PackedHostRetirementOwner> owner)
        : owner_(std::move(owner)) {}
    ~PackedHostTestTeardown() {
        owner_->beginShutdown();
        while (!owner_->exitAcknowledged())
            std::this_thread::sleep_for(1ms);
        while (!owner_->collect())
            std::this_thread::sleep_for(1ms);
    }

  private:
    std::shared_ptr<render::PackedHostRetirementOwner> owner_;
};
struct PackedHostFreeGate final {
    std::mutex mutex;
    std::condition_variable changed;
    bool entered = false, released = false;
    ~PackedHostFreeGate() { release(); }
    static void hold(void* context) noexcept {
        auto& gate = *static_cast<PackedHostFreeGate*>(context);
        std::unique_lock lock(gate.mutex);
        gate.entered = true;
        gate.changed.notify_all();
        gate.changed.wait(lock, [&] { return gate.released; });
    }
    bool hasEntered() {
        std::lock_guard lock(mutex);
        return entered;
    }
    void release() {
        std::lock_guard lock(mutex);
        released = true;
        changed.notify_all();
    }
};
int exercisePackedHostRetirement(Expectations& checks, ui::ViewerEditor& viewer,
                                 ui::EditorArea& area, ui::EditorRegistry& registry,
                                 QSplitter& splitter, ui::CompositionPreviewController& controller,
                                 ui::TaskUiBridge& bridge,
                                 runtime::GpuPreviewDisplayService& service,
                                 const std::shared_ptr<runtime::GpuProcessingPhasePolicy>& policy,
                                 const std::shared_ptr<render::PackedHostRetirementOwner>& owner) {
    auto external = viewer.displayedFrameForTest();
    const auto before = policy->snapshot().reservations;
    checks.expect(external && viewer.actualDisplayDescriptor() && before.admittedPhases == 0 &&
                      before.ownedPackedHostAllocations == 1 && before.ownedPackedHostBytes > 0 &&
                      before.ownedPackedHostMetadataBytes > 0,
                  "real painted production output owns payload and admitted retirement metadata");
    if (!external)
        return 1;
    auto* outgoing = new ui::EditorArea(registry, "bloom.viewer", "outgoing-packed", &splitter);
    splitter.addWidget(outgoing);
    outgoing->show();
    QPointer<ui::EditorArea> deferredArea(outgoing);
    auto* outgoingViewer = outgoing->findChild<ui::ViewerEditor*>();
    checks.expect(outgoingViewer && waitUntil(
                                        [&] {
                                            const auto painted =
                                                outgoingViewer->actualDisplayDescriptor();
                                            return painted && painted->frame &&
                                                   painted->frame->displayOnlyFrame() ==
                                                       external->displayOnlyFrame();
                                        },
                                        3s),
                  "outgoing real Viewer has painted and retained the same owned backing");
    outgoing->setParent(nullptr);
    PackedHostFreeGate freeGate;
    {
        auto state = owner->producerState();
        std::lock_guard lock(state->mutex);
        state->beforeFree = &PackedHostFreeGate::hold;
        state->testContext = &freeGate;
    }
    ui::ApplicationShutdownCoordinator shutdown(controller, bridge);
    shutdown.setNativeSurfaceSource(
        [&] { return std::vector<ui::EditorNativeSurface*>{area.nativeSurface()}; });
    bool preHost = false, quiescent = false, deferredAtHostBegin = false;
    shutdown.setPackedHostRetirement(
        [&] {
            preHost = true;
            checks.expect(shutdown.nativeSurfaceRetirementSatisfied() &&
                              shutdown.gpuExportRetirementSatisfied(),
                          "pin release follows native and export prerequisites");
            service.beginShutdown();
            viewer.releasePackedFramesForShutdown();
            controller.releasePublishedFramesForShutdown();
            outgoing->deleteLater(); // Still retained outside the surviving workspace tree.
            deferredAtHostBegin = !deferredArea.isNull();
            owner->beginShutdown();
        },
        [&] { return owner->exitAcknowledged(); });
    QObject::connect(&shutdown, &ui::ApplicationShutdownCoordinator::shutdownQuiescent, &shutdown,
                     [&] { quiescent = true; });
    {
        // Start shutdown and post deletion FROM this real loop. A nested loop cannot stand in
        // for the originating event-loop boundary that Qt requires for deleteLater delivery.
        QEventLoop loop;
        QTimer poll;
        QObject::connect(&poll, &QTimer::timeout, &loop, [&] {
            if (preHost &&
                service.status().state == runtime::GpuPreviewDisplayServiceState::Stopped)
                loop.quit();
        });
        poll.start(1);
        QTimer::singleShot(0, &loop, [&] { shutdown.beginShutdown(); });
        QTimer::singleShot(3000, &loop, &QEventLoop::quit);
        loop.exec();
    }
    checks.expect(
        preHost && deferredAtHostBegin && deferredArea.isNull() &&
            service.status().state == runtime::GpuPreviewDisplayServiceState::Stopped &&
            !owner->exitAcknowledged() && external && !quiescent,
        "host close starts with a detached deleteLater Viewer still pending; the same "
        "active loop destroys it and retires producer while external pin keeps host alive");
    const auto releaseRevision = policy->snapshot().reservations.admissionRevision;
    external.reset(); // Actual final owner on the UI thread: enqueue only.
    checks.expect(waitUntil([&] { return freeGate.hasEntered(); }, 3s),
                  "real last-alias backing deletion reaches the retirement worker");
    std::size_t heartbeats = 0;
    QTimer heartbeat;
    QObject::connect(&heartbeat, &QTimer::timeout, &shutdown, [&] { ++heartbeats; });
    heartbeat.start(1);
    checks.expect(waitUntil([&] { return heartbeats >= 5; }, 1s) && preHost && !quiescent &&
                      !shutdown.packedHostRetirementSatisfied() && !owner->exitAcknowledged() &&
                      !viewer.actualDisplayDescriptor() && !controller.state().frame &&
                      controller.frameCache().size() == 0,
                  "active Qt loop remains responsive with released Viewer/cache pins while free "
                  "and actual exit are withheld");
    const auto held = policy->snapshot().reservations;
    checks.expect(held.ownedPackedHostBytes == before.ownedPackedHostBytes &&
                      held.ownedPackedHostMetadataBytes == before.ownedPackedHostMetadataBytes &&
                      held.ownedPackedHostAllocations == 1 &&
                      held.admissionRevision == releaseRevision &&
                      held.totalHostClaimBytes ==
                          held.ownedPackedHostBytes + held.ownedPackedHostMetadataBytes,
                  "queued and in-progress backing remains exactly charged before actual free");
    freeGate.release();
    checks.expect(waitUntil([&] { return quiescent; }, 3s) && owner->exitAcknowledged() &&
                      !owner->snapshot().collected,
                  "GUI final quiescence requires actual worker exit but performs no join");
    heartbeat.stop();
    const auto final = policy->snapshot().reservations;
    checks.expect(final.admittedPhases == 0 && final.hostReservedBytes == 0 &&
                      final.deviceReservedBytes == 0 && final.unprovenPhases == 0 &&
                      final.ownedPackedHostBytes == 0 && final.ownedPackedHostMetadataBytes == 0 &&
                      final.ownedPackedHostAllocations == 0 && final.totalHostClaimBytes == 0 &&
                      final.admissionRevision == releaseRevision + 1,
                  "actual worker free releases payload and metadata once with one revision");
    // Test's Qt event processing has ended; collection is an explicit teardown operation.
    checks.expect(owner->collect() && owner->snapshot().collected,
                  "post-eventprocessing collection joins the already exited worker");
    std::cout << "PACKED_HOST_RETIREMENT bytes=" << before.ownedPackedHostBytes
              << " metadata=" << before.ownedPackedHostMetadataBytes << " heartbeats=" << heartbeats
              << " finalClaims=" << final.totalHostClaimBytes << '\n'
              << (checks.failures() ? "FAIL" : "PASS")
              << ": actual packed Viewer host retirement\n";
    return checks.failures() ? 1 : 0;
}
} // namespace
