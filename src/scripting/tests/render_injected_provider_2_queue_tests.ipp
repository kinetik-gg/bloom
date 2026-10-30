// Test group for render_injected_provider_test.cpp: the headless export queue (bloom-cli export and
// scripted export.* calls). A display-referred video output must run its display/view transform on
// the GPU in the frame's combined readback, exactly as the desktop queue does. Before the fix the
// queue composed its own packaged-tool resolver inside the scripting library, which carries no
// packaging definitions, so every ProRes/DNxHR/H.264 frame took the CPU reference display.
// Issue #396: the job also starts while the provider's bootstrap is still held (its native gate is
// locked), as an export started right after the queue opens does; no frame may start before the
// evaluator and the shared OCIO context are published, or it would run on the CPU.

// Starts `job` on the queue and pumps it to a terminal state (bounded). With a locked `gate`, the
// job starts while the provider bootstrap is held; `framesBeforeBootstrap` receives the frames that
// progressed in the next 500 ms, then the gate is released.
[[nodiscard]] std::optional<host::ExportJobView>
runQueuedJob(host::ExportQueueService& service, const document::ExportJobId job,
             std::timed_mutex* gate = nullptr, std::uint64_t* framesBeforeBootstrap = nullptr) {
    const auto ready = std::chrono::steady_clock::now() + 60s;
    while (std::chrono::steady_clock::now() < ready) {
        service.pump();
        bool pending = false;
        for (const auto& view : service.snapshot()->jobs)
            pending = pending || view.commands[host::ExportCommand::Start].code ==
                                     host::ExportUnavailableCode::DestinationCheckPending;
        if (!pending)
            break;
        std::this_thread::sleep_for(5ms);
    }
    const std::array<document::ExportJobId, 1> jobs{job};
    if (!service.start(jobs).accepted)
        return std::nullopt;
    if (gate != nullptr) {
        const auto held = std::chrono::steady_clock::now() + 500ms;
        std::uint64_t frames = 0;
        while (std::chrono::steady_clock::now() < held) {
            service.pump();
            for (const auto& view : service.snapshot()->jobs)
                if (view.id == job)
                    frames = std::max(frames, view.progress.framesCompleted);
            std::this_thread::sleep_for(5ms);
        }
        if (framesBeforeBootstrap != nullptr)
            *framesBeforeBootstrap = frames;
        gate->unlock();
    }
    const auto deadline = std::chrono::steady_clock::now() + 240s;
    while (std::chrono::steady_clock::now() < deadline) {
        service.pump();
        for (const auto& view : service.snapshot()->jobs)
            if (view.id == job && !host::isActive(view.state))
                return view;
        std::this_thread::sleep_for(5ms);
    }
    return std::nullopt;
}

GpuProofOutcome testNativeHeadlessQueueVideoDisplay(Expectations& expectations,
                                                    const bool requireDevice) {
    const char* loader = std::getenv("BLOOM_TEST_VULKAN_LOADER");
    if (loader == nullptr || *loader == '\0') {
        expectations.expect(!requireDevice,
                            "headless queue: --require-device needs BLOOM_TEST_VULKAN_LOADER");
        std::cout << "NOTE: BLOOM_TEST_VULKAN_LOADER unset; skipping the headless queue\n";
        return GpuProofOutcome::Skipped;
    }
    // Probe the device and the packaged tools the way the queue's own provider will see them.
    {
        runtime::TaskScheduler scheduler;
        auto probe = host::GpuExportProvider::create(optionsFor(loader));
        probe->prepare(scheduler);
        const auto deadline = std::chrono::steady_clock::now() + 30s;
        while (!probe->prepared() && std::chrono::steady_clock::now() < deadline)
            std::this_thread::sleep_for(1ms);
        const bool device = probe->deviceAvailable();
        const bool tools = probe->gpuDisplayPreparationAvailable();
        while (!probe->shutdownAndWait(10s)) {
        }
        if (!device || !tools) {
            expectations.expect(!requireDevice,
                                "headless queue: a device and the packaged tools are required");
            std::cout << "NOTE: no device or packaged GPU shader tools; skipping the headless "
                         "queue\n";
            return GpuProofOutcome::Skipped;
        }
    }
    // 1280x720: DNxHR needs a broadcast-sized picture. ProRes in MOV carries every colour tag;
    // DNxHR in MXF carries only the matrix, and its ColourTags check must expect exactly that.
    auto session = buildSolidSession(expectations, 1280, 720);
    TempDirectory directory;
    expectations.expect(session != nullptr && directory.isValid(),
                        "headless queue: the session and temp directory exist");
    if (session == nullptr || !directory.isValid())
        return GpuProofOutcome::Ran;
    const auto outputJson = [&](const std::string_view format, const std::string_view profile,
                                const std::string_view file) -> std::optional<std::string> {
        const auto destination = bloom::core::fileUriFromAbsolutePath(directory.path() / file);
        if (!destination)
            return std::nullopt;
        return std::string(R"({"preset":"","settings":{"format":")") + std::string(format) +
               R"(","codecProfile":")" + std::string(profile) +
               R"(","compression":"","outputColorSpace":"","display":"","view":"",)"
               R"("bakeLook":true,"includeAudio":false,"useCachedFrames":false,)"
               R"("audioSampleRate":48000,"encoder":"automatic",)"
               R"("naming":{"framePadding":4,"pattern":"<base>.####.<ext>"}},)"
               R"("destination":{"portability":"external","path":")" +
               *destination +
               R"(","overwrite":"replace"},"enabled":true,"importIntoAssets":false})";
    };
    const auto prores = outputJson("prores-mov", "hq", "queue.mov");
    const auto dnxhr = outputJson("dnxhr-mxf", "hqx", "queue.mxf");
    expectations.expect(prores && dnxhr, "headless queue: the destinations are file URIs");
    if (!prores || !dnxhr)
        return GpuProofOutcome::Ran;
    const auto composition = session->snapshot().project().compositions().front().id();
    const auto added = session->executeJsonOperation(
        "bloom.export.add-job",
        {{"composition", scripting::StableId{composition.value()}},
         {"name", "queue"},
         {"range", "composition"},
         {"outputs", scripting::ValueArray{scripting::Value(*prores), scripting::Value(*dnxhr)}}});
    const auto job = added.succeeded() ? added.command->outputId<document::ExportJobId>("exportJob")
                                       : std::nullopt;
    expectations.expect(job.has_value(), "headless queue: the ProRes and DNxHR job is added");
    if (!job)
        return GpuProofOutcome::Ran;

    std::optional<host::ExportJobView> finished;
    std::uint64_t framesBeforeBootstrap = 0;
    {
        auto gate = std::make_shared<std::timed_mutex>();
        gate->lock();
        scripting::HeadlessExportQueue queue(
            *session, host::makePackagedGpuOcioResolver(host::currentExecutablePath()), loader,
            gate);
        finished = runQueuedJob(queue.service(), *job, gate.get(), &framesBeforeBootstrap);
    }
    expectations.expect(framesBeforeBootstrap == 0,
                        "headless queue: no frame runs before the provider bootstrap completes");
    expectations.expect(
        finished.has_value() && finished->state == host::ExportJobState::Done &&
            finished->outputs.size() == 2,
        "headless queue: the ProRes and DNxHR outputs finish and pass verification");
    if (!finished)
        return GpuProofOutcome::Ran;
    const auto frames = finished->resolvedRange
                            ? finished->resolvedRange->last - finished->resolvedRange->first + 1
                            : 0;
    for (const auto& output : finished->outputs) {
        expectations.expect(
            output.verification.has_value() &&
                output.verification->overall == host::ExportCheckOutcome::Passed,
            "headless queue: each output passes verification, colour tags included");
        if (!output.verification)
            continue;
        const auto& provenance = output.verification->provenance;
        // Every frame is evaluated on the GPU and its display transform runs in the GPU combined
        // readback; none takes the CPU reference path.
        expectations.expect(frames > 0 && provenance.gpuFrames == frames &&
                                provenance.cpuReferenceFrames == 0,
                            "headless queue: every frame is evaluated on the GPU");
        expectations.expect(provenance.gpuDisplayFrames == frames &&
                                provenance.cpuDisplayFrames == 0,
                            "headless queue: every display transform runs on the GPU");
        if (provenance.cpuDisplayFrames != 0 || provenance.gpuFrames != frames ||
            output.verification->overall != host::ExportCheckOutcome::Passed)
            std::cerr << "headless queue output " << output.record.settings.codecProfile
                      << ": overall=" << static_cast<int>(output.verification->overall)
                      << " frames=" << frames << " gpuFrames=" << provenance.gpuFrames
                      << " cpuReferenceFrames=" << provenance.cpuReferenceFrames
                      << " gpuDisplayFrames=" << provenance.gpuDisplayFrames
                      << " cpuDisplayFrames=" << provenance.cpuDisplayFrames << '\n';
    }
    return GpuProofOutcome::Ran;
}
