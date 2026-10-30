// Test group for render_injected_provider_test.cpp: the headless export queue (bloom-cli export and
// scripted export.* calls). A display-referred video output must run its display/view transform on
// the GPU in the frame's combined readback, exactly as the desktop queue does. Before the fix the
// queue composed its own packaged-tool resolver inside the scripting library, which carries no
// packaging definitions, so every ProRes/DNxHR/H.264 frame took the CPU reference display.

// Starts `job` on the queue and pumps it to a terminal state (bounded).
[[nodiscard]] std::optional<host::ExportJobView> runQueuedJob(host::ExportQueueService& service,
                                                              const document::ExportJobId job) {
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
    auto session = buildSolidSession(expectations);
    TempDirectory directory;
    expectations.expect(session != nullptr && directory.isValid(),
                        "headless queue: the session and temp directory exist");
    if (session == nullptr || !directory.isValid())
        return GpuProofOutcome::Ran;
    const auto destination = bloom::core::fileUriFromAbsolutePath(directory.path() / "queue.mov");
    expectations.expect(destination.has_value(), "headless queue: the destination is a file URI");
    if (!destination)
        return GpuProofOutcome::Ran;
    const auto output =
        R"({"preset":"","settings":{"format":"prores-mov","codecProfile":"hq","compression":"",)"
        R"("outputColorSpace":"","display":"","view":"","bakeLook":true,"includeAudio":false,)"
        R"("useCachedFrames":false,"audioSampleRate":48000,"encoder":"automatic",)"
        R"("naming":{"framePadding":4,"pattern":"<base>.####.<ext>"}},)"
        R"("destination":{"portability":"external","path":")" +
        *destination + R"(","overwrite":"replace"},"enabled":true,"importIntoAssets":false})";
    const auto composition = session->snapshot().project().compositions().front().id();
    const auto added = session->executeJsonOperation(
        "bloom.export.add-job", {{"composition", scripting::StableId{composition.value()}},
                                 {"name", "queue"},
                                 {"range", "composition"},
                                 {"outputs", scripting::ValueArray{scripting::Value(output)}}});
    const auto job = added.succeeded() ? added.command->outputId<document::ExportJobId>("exportJob")
                                       : std::nullopt;
    expectations.expect(job.has_value(), "headless queue: the ProRes job is added");
    if (!job)
        return GpuProofOutcome::Ran;

    std::optional<host::ExportJobView> finished;
    {
        scripting::HeadlessExportQueue queue(
            *session, host::makePackagedGpuOcioResolver(host::currentExecutablePath()), loader);
        finished = runQueuedJob(queue.service(), *job);
    }
    expectations.expect(finished.has_value() && finished->state == host::ExportJobState::Done &&
                            finished->outputs.size() == 1 &&
                            finished->outputs.front().verification.has_value(),
                        "headless queue: the ProRes job finishes and is verified");
    if (!finished || finished->outputs.empty() || !finished->outputs.front().verification)
        return GpuProofOutcome::Ran;
    const auto& provenance = finished->outputs.front().verification->provenance;
    // Every frame of the composition range is evaluated on the GPU and its display transform runs
    // in the GPU combined readback; none takes the CPU reference path.
    const auto frames = finished->resolvedRange
                            ? finished->resolvedRange->last - finished->resolvedRange->first + 1
                            : 0;
    expectations.expect(frames > 0 && provenance.gpuFrames == frames &&
                            provenance.cpuReferenceFrames == 0,
                        "headless queue: every frame is evaluated on the GPU");
    expectations.expect(provenance.gpuDisplayFrames == frames && provenance.cpuDisplayFrames == 0,
                        "headless queue: every display transform runs on the GPU");
    if (provenance.cpuDisplayFrames != 0 || provenance.gpuFrames != frames)
        std::cerr << "headless queue provenance: frames=" << frames
                  << " gpuFrames=" << provenance.gpuFrames
                  << " cpuReferenceFrames=" << provenance.cpuReferenceFrames
                  << " gpuDisplayFrames=" << provenance.gpuDisplayFrames
                  << " cpuDisplayFrames=" << provenance.cpuDisplayFrames << '\n';
    return GpuProofOutcome::Ran;
}
