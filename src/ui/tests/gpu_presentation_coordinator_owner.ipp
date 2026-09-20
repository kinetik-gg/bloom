void ownerMain(Shared& shared, const TestOptions& options, Expectations& expectations) {
    GpuDeviceCreationOptions creation;
    creation.loader_path = options.loader_path;
    creation.request_presentation = true;
    creation.presentation_platform = GpuPresentationPlatform::Wayland;
    auto created = GpuDevice::create(creation);
    if (!created) {
        std::lock_guard lock(shared.mutex);
        shared.reason = "no presentable device: " + created.diagnostic.message;
        shared.initDone = true;
        shared.cv.notify_all();
        return;
    }
    auto device = std::move(created.device);
    if (device->presentationStatus().availability !=
        bloom::render::GpuPresentationAvailability::Ready) {
        std::lock_guard lock(shared.mutex);
        shared.reason = "presentation not ready: " + device->presentationStatus().detail;
        shared.initDone = true;
        shared.cv.notify_all();
        return;
    }
    auto solidResult = GpuSolid::create(*device);
    auto displayResult = GpuResidentDisplay::create(*device);
    auto registry = GpuResidentFrameLeaseRegistry::create(*device);
    auto foreignRegistry = GpuResidentFrameLeaseRegistry::create(*device);
    if (!solidResult || !displayResult || !registry || !foreignRegistry) {
        std::lock_guard lock(shared.mutex);
        shared.reason = "the native pipelines or lease registries could not be created";
        shared.initDone = true;
        shared.cv.notify_all();
        return;
    }
    std::unique_ptr<GpuSolid> solid = std::move(solidResult.solid);
    std::unique_ptr<GpuResidentDisplay> display = std::move(displayResult.display);
    GpuPresentationCoordinatorOptions coordinatorOptions;
    coordinatorOptions.maxTargets = 3U;
    auto coordinator =
        std::make_unique<GpuPresentationCoordinator>(*device, *registry, coordinatorOptions);
    {
        std::lock_guard lock(shared.mutex);
        shared.client = coordinator->client();
        shared.ran = shared.client != nullptr;
        shared.initDone = true;
        shared.cv.notify_all();
    }

    const auto publishLease = [&](GpuResidentFrameLeaseRegistry& targetRegistry) {
        auto image = produceDisplay(*solid, *display, expectations);
        if (image == nullptr) {
            return GpuResidentFrameLease{};
        }
        auto published = targetRegistry.publish(std::move(image));
        expectations.expect(published.hasValue(), "the resident image is published as a lease");
        return published.lease;
    };

    bool gated = false;
    for (;;) {
        Cmd command = Cmd::None;
        bool gateValue = false;
        {
            std::unique_lock lock(shared.mutex);
            shared.cv.wait_for(lock, std::chrono::milliseconds(2),
                               [&] { return shared.stop || shared.cmd != Cmd::None; });
            if (shared.stop) {
                break;
            }
            command = shared.cmd;
            gateValue = shared.gateValue;
            shared.cmd = Cmd::None;
        }
        bool acknowledged = false;
        switch (command) {
        case Cmd::SetGate:
            gated = gateValue;
            acknowledged = true;
            break;
        case Cmd::Barrier:
            acknowledged = true;
            break;
        case Cmd::ProduceLease: {
            auto leaseValue = publishLease(*registry);
            std::lock_guard lock(shared.mutex);
            shared.lease = leaseValue;
            acknowledged = true;
            break;
        }
        case Cmd::ProduceForeignLease: {
            auto leaseValue = publishLease(*foreignRegistry);
            std::lock_guard lock(shared.mutex);
            shared.foreignLease = leaseValue;
            acknowledged = true;
            break;
        }
        case Cmd::None:
            break;
        }
        if (acknowledged) {
            std::lock_guard lock(shared.mutex);
            shared.cmdDone = true;
            shared.cv.notify_all();
        }
        if (!gated) {
            coordinator->pump();
        }
    }

    coordinator->beginShutdown();
    for (int attempt = 0; attempt < 600 && !coordinator->shutdownStatus().drained; ++attempt) {
        coordinator->pump();
    }
    const auto drain = coordinator->shutdownStatus();
    expectations.expect(drain.drained, "the owner shutdown drained every target");
    std::lock_guard lock(shared.mutex);
    shared.reason = drain.message;
}

[[nodiscard]] int skipOrFail(const TestOptions& options) { return options.require_device ? 1 : 0; }
