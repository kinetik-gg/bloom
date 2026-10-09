// A distinct 100-member 1080 EXR sequence through the real app stage, RAM cache and Viewer.
// This is a capacity/identity/replay proof, not SMB/EXR production-throughput qualification.
namespace {
int runPackedCache100(const TestOptions& options, QTemporaryDir& temporary, const bool half,
                      const bool partial, const std::string& manifestPath) {
    Expectations checks;
    QSettings().setValue("viewer/resolution", half ? "Half" : "Full");
    Fixture fixture(temporary.path().toStdString());
    auto sequence = fixture.mediaAsset;
    sequence.kind = document::AssetKind::Sequence;
    std::uint32_t compositionWidth = 1920, compositionHeight = 1080, controlledRate = 25;
    sequence.width = 1920;
    sequence.height = 1080;
    sequence.frames = 100;
    sequence.rate = controlledRate;
    sequence.manifest.pattern = "packed.####.exr";
    sequence.manifest.padding = 4;
    sequence.manifest.first = 0;
    sequence.manifest.last = 99;
    if (!manifestPath.empty()) {
        // All descriptor I/O, strong digest verification and header probing is off the UI thread.
        auto loaded = std::async(std::launch::async, [&] {
            QFile file(QString::fromStdString(manifestPath));
            if (!file.open(QIODevice::ReadOnly))
                throw std::runtime_error("Cannot open private sequence descriptor");
            QJsonParseError error;
            const auto json = QJsonDocument::fromJson(file.readAll(), &error);
            if (error.error != QJsonParseError::NoError || !json.isObject())
                throw std::runtime_error("Invalid private sequence JSON");
            const auto object = json.object();
            compositionWidth = static_cast<std::uint32_t>(object["compositionWidth"].toInt());
            compositionHeight = static_cast<std::uint32_t>(object["compositionHeight"].toInt());
            controlledRate = static_cast<std::uint32_t>(object["controlledRate"].toInt());
            sequence.width = static_cast<std::uint32_t>(object["sourceWidth"].toInt());
            sequence.height = static_cast<std::uint32_t>(object["sourceHeight"].toInt());
            sequence.manifest.first = object["first"].toInteger(-1);
            const auto members = object["members"].toArray();
            if (compositionWidth != 1920 || compositionHeight != 1080 || controlledRate != 24 ||
                sequence.width != 1920 || sequence.height != 804 || members.size() != 100 ||
                sequence.manifest.first != 1001 || object["sourceRate"].toString() != "24000/1001")
                throw std::runtime_error(
                    "Descriptor does not match the controlled A qualification");
            sequence.frames = static_cast<std::uint64_t>(members.size());
            sequence.rate = controlledRate;
            sequence.manifest.last = sequence.manifest.first + members.size() - 1;
            sequence.manifest.pattern = object["pattern"].toString().toStdString();
            for (qsizetype index = 0; index < members.size(); ++index) {
                const auto member = members[index].toObject();
                const auto frame = member["frame"].toInteger(-1);
                const auto path = std::filesystem::path(member["path"].toString().toStdString());
                const auto digest =
                    core::Sha256Digest::fromLowercaseHex(member["sha256"].toString().toStdString());
                if (frame != sequence.manifest.first + index || !path.is_absolute() || !digest)
                    throw std::runtime_error(
                        "Sequence members must be exact contiguous absolute inputs");
                const auto probe = media::probeImage(path);
                if (!probe.value || probe.value->contentDigest != *digest ||
                    probe.value->width != sequence.width || probe.value->height != sequence.height)
                    throw std::runtime_error(
                        "Sequence digest or source geometry verification failed");
                document::AssetLocator locator{
                    .kind = "file",
                    .portability = "project-relative",
                    .path = path.filename().string(),
                    .relinkHint = QUrl::fromLocalFile(QString::fromStdString(path.string()))
                                      .toEncoded()
                                      .toStdString()};
                sequence.manifest.members.push_back(
                    {frame, std::move(locator), probe.value->contentDigest});
                std::cout << "PACKED_INPUT member=" << frame << " path=" << path
                          << " sha256=" << member["sha256"].toString().toStdString()
                          << " sourceColorTag=" << probe.value->colorSpaceTag
                          << " interpretationAssumption=" << probe.value->interpretationAssumption
                          << '\n';
            }
            std::cout << "PACKED_INPUT controlledRate=24 descriptorSourceRate=24000/1001"
                      << " composition=1920x1080 source=1920x804 centered=960,540"
                      << " background=opaqueBlack interpretation=Auto"
                      << " sourceIOBytes=unmeasured decodeBytes=unmeasured SMBCold=unqualified\n";
        });
        try {
            loaded.get();
        } catch (const std::exception& error) {
            std::cerr << "Invalid controlled input: " << error.what() << '\n';
            return 2;
        }
    } else {
        for (std::int64_t index = 0; index < 100; ++index) {
            const auto path = std::filesystem::path(temporary.path().toStdString()) /
                              ("packed." + std::to_string(index) + ".exr");
            writeExrRgba(path, 1920, 1080,
                         std::vector<ExrPixel>(
                             1920ULL * 1080,
                             ExrPixel{static_cast<float>(index + 1) / 128.0F, 0.3F, 0.7F, 1.0F}));
            const auto member = imageAsset(path, "Distinct packed member", 901);
            sequence.manifest.members.push_back({index, member.locator, member.contentDigest});
        }
    }
    sequence.locator = sequence.manifest.members.front().locator;
    sequence.contentDigest = sequence.manifest.members.front().contentDigest;
    fixture.evaluator.operationCache()->setByteBudget(256ULL << 20);
    const auto rate = document::FrameRate::create(controlledRate, 1);
    if (!rate)
        return 2;
    const auto format = document::CompositionFormat::create(
        compositionWidth, compositionHeight, core::PixelAspectRatio::square(), *rate);
    if (!format)
        return 2;
    const auto duration =
        core::RationalTime::create(static_cast<std::int64_t>(sequence.frames), controlledRate);
    if (!duration)
        return 2;
    auto project = document::makeNewProject("Packed100", "Main", *duration, *format);
    checks.expect(project.project.addAsset(sequence), "genuine 100-member sequence validates");
    document::Document document(std::move(project.project));
    commands::CommandStack commands(document);
    ui::CompositionSession session(document, commands, project.initialCompositionId);
    if (!manifestPath.empty())
        checks.expect(session.addSolidLayer("Explicit black background", {0, 0, 0, 1}),
                      "opaque black background uses production authoring");
    commands::Transaction add("Add packed sequence");
    add.emplace<commands::AddImageLayer>(project.initialCompositionId, sequence.id);
    checks.expect(commands.execute(std::move(add)).succeeded(), "real sequence layer is authored");
    std::atomic<std::uint64_t> preparations{0};
    runtime::CpuReferenceDisplayPreparer displayPreparer;
    runtime::TaskScheduler scheduler(schedulerConfig());
    auto resolver =
        host::makePackagedGpuOcioResolver(QCoreApplication::applicationFilePath().toStdString());
    checks.expect(resolver != nullptr, "packaged normal application resolver is available");
    const auto retirementOwner = render::PackedHostRetirementOwner::create();
    PackedHostTestTeardown retirementTeardown(retirementOwner);
    const auto processingPolicy =
        std::make_shared<runtime::GpuProcessingPhasePolicy>(4, retirementOwner);
    const runtime::GpuScenePreparationPhase processingPhase{
        .policy = processingPolicy,
        .envelope = {.metadataBytes = 1024ULL * 1024,
                     .retainedCoverageByteLimit = 16ULL * 1024 * 1024,
                     .requestedExecutorBytes = kBudget,
                     .deviceWorkingBytes = 832ULL * 1024 * 1024}};
    auto stage = ui::makeSessionRefreshingGpuSceneStage(
        fixture.compiler, fixture.evaluator, fixture.provider,
        std::make_shared<runtime::GpuSceneCoverageCache>(),
        std::make_shared<runtime::GpuPreparedUploadCache>(), nullptr, resolver, processingPhase);
    runtime::MemoryBudgetLedger ledger(0, 0);
    ledger.beginProcessSampling();
    ui::TaskUiBridge bridge(scheduler);
    ui::MemoryBudgetController memoryController(scheduler, bridge, ledger);
    QObject::connect(&memoryController, &ui::MemoryBudgetController::sampleApplied, &bridge,
                     [processingPolicy, &ledger] {
                         processingPolicy->applyHostSample(ledger.state(),
                                                           ledger.physicalByteBudget());
                     });
    checks.expect(
        waitUntil([&] { return processingPolicy->snapshot().reservations.hostReady; }, 30s),
        "production off-UI sampler delivers genuine first shared-root readiness");
    stage = [base = std::move(stage), &preparations](
                const document::Snapshot& snapshot, const runtime::PreviewRequestIdentity& identity,
                const std::size_t bytes,
                const std::vector<runtime::SnapshotParameterOverride>& overrides,
                runtime::TaskContext& context) {
        ++preparations;
        auto result = base(snapshot, identity, bytes, overrides, context);
        if (result.state() != runtime::TaskState::Succeeded || preparations.load() <= 3) {
            for (const auto& diagnostic : result.diagnostics())
                std::cout << "PACKED_STAGE diagnostic=" << diagnostic.code << " "
                          << diagnostic.summary << " " << diagnostic.detail << '\n';
            if (result.value() && *result.value()) {
                std::cout << "PACKED_STAGE status=" << static_cast<int>((*result.value())->status)
                          << '\n';
                for (const auto& diagnostic : (*result.value())->diagnostics)
                    std::cout << "PACKED_STAGE diagnostic=" << diagnostic.code << " "
                              << diagnostic.summary << " " << diagnostic.detail << '\n';
            }
        }
        return result;
    };
    auto configured = serviceOptions(options.loader_path);
    configured.processingPhasePolicy = processingPolicy;
    configured.product = runtime::GpuPreviewDisplayProductMode::GpuPreparedPacked;
    configured.presentation = runtime::GpuPreviewDisplayServicePresentationMode::Disabled;
    runtime::GpuPreviewDisplayService service(scheduler, stage, fixture.productionCpuStage(),
                                              fixture.productionCpuFallback(displayPreparer),
                                              configured);
    checks.expect(waitUntil(
                      [&] {
                          return service.status().state !=
                                 runtime::GpuPreviewDisplayServiceState::Initializing;
                      },
                      90s),
                  "genuine offscreen GPU service initializes");
    if (service.status().state != runtime::GpuPreviewDisplayServiceState::Ready) {
        std::cout << "Unsupported: " << service.status().residentDetail << '\n';
        return options.require_device ? 1 : 77;
    }
    bool insideCompletion = false;
    std::uint64_t completionSignals = 0, jobsPolls = 0;
    std::vector<std::uint64_t> measuredTerminals;
    std::vector<double> terminalDeliveryMicros;
    QObject::connect(&bridge, &ui::TaskUiBridge::resultsAvailable, &bridge, [&] {
        insideCompletion = true;
        ++completionSignals;
        for (const auto& task : scheduler.snapshots()) {
            if (task.name == "Cache RAM preview frame" && task.finishedAt &&
                std::ranges::find(measuredTerminals, task.id.value()) == measuredTerminals.end()) {
                measuredTerminals.push_back(task.id.value());
                terminalDeliveryMicros.push_back(
                    std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() -
                                                              *task.finishedAt)
                        .count());
            }
        }
    });
    QObject::connect(&bridge, &ui::TaskUiBridge::snapshotsPolled, &bridge, [&] { ++jobsPolls; });
    auto pipeline = ui::makeCompositionPreviewPipeline(fixture.compiler, fixture.evaluator,
                                                       displayPreparer, fixture.provider);
    const ui::PreviewPreparationSubmitter submitter =
        [&service](runtime::TaskRequest request, const document::Snapshot& snapshot,
                   const runtime::PreviewRequestIdentity& identity, std::size_t bytes,
                   const std::vector<runtime::SnapshotParameterOverride>& overrides) {
            return service.submit(std::move(request), snapshot, identity, bytes, overrides);
        };
    const std::size_t hostBudget = partial ? 64ULL << 20 : 1ULL << 30;
    auto cache = std::make_shared<ui::PreviewFrameCache>(hostBudget);
    const auto ownerLimits = service.status().residentCapacityPlan;
    cache->setGpuResidentLimits(ownerLimits.cacheBytes, ownerLimits.cacheEntries);
    ui::CompositionPreviewSettings settings;
    settings.pixelStorageByteLimit = kBudget;
    settings.resolutionPolicy =
        half ? runtime::PreviewResolutionPolicy::Half : runtime::PreviewResolutionPolicy::Full;
    ui::CompositionPreviewController controller(session, scheduler, bridge, pipeline, settings,
                                                cache, nullptr, submitter, nullptr,
                                                processingPolicy);
    ui::RamPreviewController ram(session, controller, scheduler, bridge, pipeline, nullptr,
                                 submitter);
    std::uint64_t observedPrepared = 0, fromCompletion = 0, outsideCompletion = 0;
    QObject::connect(&ram, &ui::RamPreviewController::stateChanged, &bridge, [&] {
        const auto prepared = ram.preparedFrameCount();
        if (prepared > observedPrepared) {
            (insideCompletion ? fromCompletion : outsideCompletion) += prepared - observedPrepared;
            observedPrepared = prepared;
        }
    });
    QObject::connect(&bridge, &ui::TaskUiBridge::resultsAvailable, &bridge,
                     [&] { insideCompletion = false; });
    ui::ViewerEditor viewer(session, controller);
    viewer.resize(600, 420);
    viewer.show();
    checks.expect(
        waitUntil(
            [&] { return viewer.displayedPreviewState().freshness == ui::FrameFreshness::Current; },
            30s),
        "foreground paints genuine GPU-packed sequence before RAM fill");
    checks.expect(viewer.displayedFrameForTest() &&
                      viewer.displayedFrameForTest()->provenance().provider ==
                          runtime::PreviewDisplayProvider::GpuPreparedPacked,
                  "actual normal Viewer foreground is GPU-prepared/CPU-painted");
    if (!viewer.displayedFrameForTest() || viewer.displayedFrameForTest()->provenance().provider !=
                                               runtime::PreviewDisplayProvider::GpuPreparedPacked) {
        for (const auto& diagnostic : controller.state().diagnostics)
            std::cout << "PACKED_FOREGROUND diagnostic=" << diagnostic.code << " "
                      << diagnostic.summary << " " << diagnostic.detail << '\n';
        const auto status = service.status();
        std::cout << "PACKED_FOREGROUND detail=" << status.residentDetail
                  << " graphJobs=" << status.counters.residentGraphJobs
                  << " nativeDispatches=" << status.counters.nativeDispatches
                  << " cpuFallbacks=" << status.counters.cpuFallbacks
                  << " admissionRefusals=" << status.counters.gpuAdmissionRefusals << '\n';
        return 1;
    }
    const auto before = service.status().counters;
    const auto fillStarted = std::chrono::steady_clock::now();
    ram.start();
    checks.expect(waitUntil([&] { return !ram.isCaching(); }, 110s), "100-frame range terminates");
    if (ram.lastCompletion() != ui::RamPreviewController::Completion::Complete && !partial) {
        for (const auto& task : scheduler.snapshots()) {
            if (task.name != "Cache RAM preview frame")
                continue;
            std::cout << "PACKED_RAM task=" << task.id.value()
                      << " state=" << static_cast<int>(task.state) << '\n';
            for (const auto& diagnostic : task.diagnostics)
                std::cout << "PACKED_RAM diagnostic=" << diagnostic.code << " "
                          << diagnostic.summary << " " << diagnostic.detail << '\n';
        }
        const auto claims = processingPolicy->snapshot().reservations;
        std::cout << "PACKED_RAM completion="
                  << (ram.lastCompletion() ? static_cast<int>(*ram.lastCompletion()) : -1)
                  << " hostReady=" << claims.hostReady << " deviceReady=" << claims.deviceReady
                  << " hostCapacity=" << claims.hostCapacityBytes.value_or(0)
                  << " deviceCapacity=" << claims.deviceCapacityBytes.value_or(0)
                  << " totalHost=" << claims.totalHostClaimBytes
                  << " phaseCount=" << claims.admittedPhases
                  << " admissionRevision=" << claims.admissionRevision
                  << " preparations=" << preparations.load()
                  << " serviceState=" << static_cast<int>(service.status().state)
                  << " gpuAvailable=" << service.status().gpuAvailable << '\n';
    }
    checks.expect(
        fromCompletion == ram.preparedFrameCount() && outsideCompletion == 0,
        "every actual RAM result/refill is driven by completion, not Jobs snapshot polling");
    std::ranges::sort(terminalDeliveryMicros);
    std::cout << "PACKED_COMPLETION_REFILL prepared=" << ram.preparedFrameCount()
              << " insideCompletion=" << fromCompletion
              << " outsideCompletion=" << outsideCompletion
              << " completionSignals=" << completionSignals
              << " bridgeWakes=" << bridge.completionWakeCount() << " jobsPolls=" << jobsPolls
              << " fillWallMs="
              << std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() -
                                                           fillStarted)
                     .count();
    if (!terminalDeliveryMicros.empty())
        std::cout << " measuredTerminals=" << terminalDeliveryMicros.size()
                  << " terminalToUiMedianMicros="
                  << terminalDeliveryMicros[terminalDeliveryMicros.size() / 2]
                  << " terminalToUiMaxMicros=" << terminalDeliveryMicros.back();
    std::cout << '\n';
    checks.expect(ram.totalFrameCount() == sequence.frames &&
                      ram.lastCompletion() ==
                          (partial ? ui::RamPreviewController::Completion::PartialBudget
                                   : ui::RamPreviewController::Completion::Complete),
                  "actual budget yields truthful full/partial completion");
    checks.expect(partial ? !ram.canAutoPlay() : ram.canAutoPlay(),
                  "only actual full retention permits autoplay");
    checks.expect(partial ? ram.cachedFrameCount() < sequence.frames
                          : ram.cachedFrameCount() == sequence.frames,
                  "live retained exact-time count matches the measured capacity");
    checks.expect(cache->gpuResidentBytes() == 0 && cache->gpuResidentEntryCount() == 0 &&
                      cache->residentBytes() <= hostBudget,
                  "packed retained storage is host-only and fits its actual byte budget");
    std::cout << "PACKED100 resolution=" << (half ? "Half" : "Full") << " hostBudget=" << hostBudget
              << " requested=" << ram.totalFrameCount() << " prepared=" << ram.preparedFrameCount()
              << " retained=" << ram.cachedFrameCount() << " charged=" << cache->residentBytes()
              << " nativeRetained=" << cache->gpuResidentBytes()
              << " ownerCacheBytes=" << ownerLimits.cacheBytes
              << " ownerLeaseBytes=" << ownerLimits.leaseBytes
              << " ownerEntries=" << ownerLimits.cacheEntries
              << " samplerDeliveries=" << ledger.state().sampleSerial << '\n';
    if (ram.playableSpan())
        std::cout << "PACKED_SPAN start=" << ram.playableSpan()->start.toSeconds()
                  << " endExclusive=" << ram.playableSpan()->end.toSeconds()
                  << " frames=" << ram.playableSpan()->frameCount
                  << " autoplay=" << ram.canAutoPlay() << '\n';
    std::vector<core::RationalTime> retainedTimes;
    std::size_t measured = 0;
    const auto retainedIndices = ram.retainedFrameIndices();
    for (const auto index : retainedIndices) {
        if (index >= sequence.manifest.members.size()) {
            checks.expect(false, "retained index belongs to the checked manifest");
            continue;
        }
        const auto time =
            core::RationalTime::create(static_cast<std::int64_t>(index), controlledRate);
        if (!time) {
            checks.expect(false, "retained frame time validates");
            continue;
        }
        const auto key = controller.cacheKeyForTime(*time);
        if (!key) {
            checks.expect(false, "exact retained frame has an identity");
            continue;
        }
        // Transport requests the real accepted exact key; no fabricated prepared frame or time
        // stamp.
        checks.expect(session.currentTime() == *time || session.setCurrentTime(*time),
                      "retained exact time seeks");
        checks.expect(waitUntil(
                          [&] {
                              const auto actual = viewer.displayedPreviewState();
                              return actual.freshness == ui::FrameFreshness::Current &&
                                     actual.frame && actual.frame->desiredIdentity().time == *time;
                          },
                          5s),
                      "each retained exact time really paints in normal Viewer");
        const auto frame = viewer.displayedFrameForTest();
        if (!frame ||
            frame->provenance().provider != runtime::PreviewDisplayProvider::GpuPreparedPacked) {
            checks.expect(false, "retained time preserves genuine GPU-packed provenance");
            continue;
        }
        const auto view = frame->displayBufferView();
        if (!view) {
            checks.expect(false, "packed frame exposes display bytes");
            continue;
        }
        const auto extent = view->dataWindow.value_or(view->displayWindow).extent();
        const auto width = half ? compositionWidth / 2 : compositionWidth;
        const auto height = half ? compositionHeight / 2 : compositionHeight;
        if (extent.width() != width || extent.height() != height ||
            view->pixels.size() != static_cast<std::size_t>(width) * height) {
            checks.expect(false, "retained physical span matches expected output geometry");
            continue;
        }
        const auto bytes = frame->displayOnlyFrame()->displayByteCost();
        checks.expect(extent.width() == width && extent.height() == height &&
                          frame->provenance().gpuPreparedPacked->transfer.hostStorageBytes ==
                              static_cast<std::uint64_t>(width) * height * 4 &&
                          key->resolutionPolicy == settings.resolutionPolicy,
                      "physical dimensions/capacity and exact Full/Half cache identity agree");
        if (!manifestPath.empty()) {
            const auto margin = (compositionHeight - sequence.height) / (half ? 4U : 2U);
            bool blackMargins = true;
            for (std::uint32_t y = 0; y < height; ++y) {
                if (y >= margin && y < height - margin)
                    continue;
                for (std::uint32_t x = 0; x < width; ++x) {
                    const auto pixel = view->pixels[static_cast<std::size_t>(y) * width + x];
                    blackMargins = blackMargins && pixel.red == 0 && pixel.green == 0 &&
                                   pixel.blue == 0 && pixel.alpha == 255;
                }
            }
            checks.expect(blackMargins, "every explicit black margin pixel is opaque black");
            const auto firstSourcePixel = view->pixels[static_cast<std::size_t>(margin) * width];
            bool nonuniform = false;
            for (std::uint32_t y = margin; y < height - margin; ++y)
                for (std::uint32_t x = 0; x < width; ++x)
                    nonuniform =
                        nonuniform ||
                        view->pixels[static_cast<std::size_t>(y) * width + x] != firstSourcePixel;
            checks.expect(nonuniform, "mounted source region contains genuine nonuniform footage");
        }
        measured += bytes;
        const auto snapshot = session.evaluationSnapshotForTime(*time);
        const auto expectedMember = sequence.manifest.members.at(static_cast<std::size_t>(index));
        auto oracle = scheduler.submit<ui::PreparedPreviewFrameHandle>(
            runtime::TaskRequest("CPU sequence parity",
                                 {.kind = runtime::TaskOwnerKind::PanelRequest,
                                  .id = runtime::TaskOwnerId::fromRaw(999)}),
            [&fixture, snapshot, identity = frame->desiredIdentity(), expectedMember,
             rate = *rate](runtime::TaskContext& context) {
                const auto compiled =
                    fixture.compiler.compile({.snapshot = snapshot,
                                              .compositionId = identity.compositionId,
                                              .parameterOverrides = {}},
                                             context.cancellation());
                if (!compiled.plan)
                    return runtime::TaskResult<ui::PreparedPreviewFrameHandle>::succeeded(nullptr);
                bool selectedExactMember = false;
                auto independent = compiled.plan->copyDefinition();
                std::size_t sourceCount = 0;
                for (auto& operation : independent.operations) {
                    if (auto* source = std::get_if<runtime::CompiledImageSource>(&operation)) {
                        ++sourceCount;
                        const auto selected = runtime::detail::selectImageSource(
                            *source, identity.time, rate, fixture.mediaDirectory,
                            context.cancellation());
                        selectedExactMember =
                            selected.available && selected.digest == expectedMember.contentDigest &&
                            selected.path ==
                                media::resolveImagePath(expectedMember.locator.path,
                                                        expectedMember.locator.relinkHint,
                                                        fixture.mediaDirectory);
                        std::cout << "PACKED_SELECTION member=" << expectedMember.frame
                                  << " exact=" << selectedExactMember
                                  << " input=" << selected.resolvedInputColorSpaceName
                                  << " automatic=" << selected.inputColorSpaceAutomatic
                                  << " working=" << selected.workingColorSpaceId
                                  << " warning=" << selected.inputColorSpaceWarning << '\n';
                        // The oracle is independent of sequence time selection: use the
                        // descriptor's expected still member, preserving all authored transforms
                        // and colour.
                        if (!source->asset)
                            return runtime::TaskResult<ui::PreparedPreviewFrameHandle>::succeeded(
                                nullptr);
                        source->asset->kind = document::AssetKind::Image;
                        source->asset->locator = expectedMember.locator;
                        source->asset->contentDigest = expectedMember.contentDigest;
                        source->asset->manifest = {};
                        source->asset->frames = 0;
                    }
                }
                return runtime::TaskResult<ui::PreparedPreviewFrameHandle>::succeeded(
                    selectedExactMember && sourceCount == 1
                        ? planCpuReference(fixture,
                                           std::make_shared<const runtime::CompiledCompositionPlan>(
                                               std::move(independent)),
                                           identity, identity.resolution)
                        : nullptr);
            });
        const auto reference = awaitResult(oracle.handle, 15s);
        checks.expect(reference && reference->value() && samePixels(frame, *reference->value()),
                      "every actual retained GPU image equals its real CPU reference");
        std::cout << "PACKED_FRAME member="
                  << sequence.manifest.first + static_cast<std::int64_t>(index)
                  << " index=" << index << " width=" << width << " height=" << height
                  << " host=" << frame->provenance().gpuPreparedPacked->transfer.hostStorageBytes
                  << " actualCharge=" << bytes
                  << " red=" << static_cast<int>(view->pixels.front().red) << '\n';
        retainedTimes.push_back(*time);
    }
    checks.expect(measured == cache->residentBytes(),
                  "measured product charges equal the live cache ledger");
    const auto preparationsBeforeWarm = preparations.load();
    const auto beforeWarm = service.status().counters;
    const auto cacheBeforeWarm = cache->residentBytes();
    const auto ownedBeforeWarm = processingPolicy->snapshot().reservations;
    std::size_t replayed = 0;
    for (int direction = 0; direction < 2; ++direction) {
        for (std::size_t step = 0; step < retainedTimes.size(); ++step) {
            const auto time = retainedTimes[direction ? retainedTimes.size() - 1 - step : step];
            checks.expect(session.currentTime() == time || session.setCurrentTime(time),
                          "warm transport seeks cached time");
            if (waitUntil(
                    [&] {
                        const auto actual = viewer.displayedPreviewState();
                        return actual.freshness == ui::FrameFreshness::Current && actual.frame &&
                               actual.frame->desiredIdentity().time == time;
                    },
                    5s))
                ++replayed;
        }
    }
    const auto afterWarm = service.status().counters;
    const auto ownedAfterWarm = processingPolicy->snapshot().reservations;
    checks.expect(
        replayed == retainedTimes.size() * 2 && preparations.load() == preparationsBeforeWarm &&
            afterWarm.residentGraphJobs == beforeWarm.residentGraphJobs &&
            afterWarm.nativeDispatches == beforeWarm.nativeDispatches &&
            afterWarm.packedTransfers == beforeWarm.packedTransfers &&
            cache->residentBytes() == cacheBeforeWarm &&
            ownedAfterWarm.ownedPackedHostBytes == ownedBeforeWarm.ownedPackedHostBytes &&
            ownedAfterWarm.ownedPackedHostMetadataBytes ==
                ownedBeforeWarm.ownedPackedHostMetadataBytes &&
            ownedAfterWarm.ownedPackedHostAllocations == ownedBeforeWarm.ownedPackedHostAllocations,
        "forward/reverse exact-time Current observations add no GPU "
        "evaluation/dispatch/transfer or cache growth");
    checks.expect(afterWarm.cpuFallbacks == 0 && afterWarm.packedTransfers > before.packedTransfers,
                  "cold RAM path genuinely dispatches GPU and transfers, without CPU fallback");
    std::cout << "PACKED_WARM replayed=" << replayed
              << " preparationDelta=" << preparations.load() - preparationsBeforeWarm
              << " ownedPayload=" << ownedBeforeWarm.ownedPackedHostBytes
              << " ownedHeader=" << ownedBeforeWarm.ownedPackedHostMetadataBytes
              << " ownedAllocations=" << ownedBeforeWarm.ownedPackedHostAllocations
              << " evalDelta=" << afterWarm.residentGraphJobs - beforeWarm.residentGraphJobs
              << " dispatchDelta=" << afterWarm.nativeDispatches - beforeWarm.nativeDispatches
              << " transferDelta=" << afterWarm.packedTransfers - beforeWarm.packedTransfers
              << " transferSource=" << afterWarm.packedRetainedSourceBytes
              << " transferStaging=" << afterWarm.packedRetainedStagingBytes
              << " transferHost=" << afterWarm.packedRetainedHostBytes << '\n';
    ram.beginShutdown();
    controller.beginShutdown();
    memoryController.beginShutdown();
    bridge.beginShutdown();
    service.beginShutdown();
    checks.expect(waitUntil(
                      [&] {
                          return scheduler.isQuiescent() &&
                                 service.status().state ==
                                     runtime::GpuPreviewDisplayServiceState::Stopped;
                      },
                      30s),
                  "RAM/Viewer/service retire asynchronously");
    viewer.releasePackedFramesForShutdown();
    controller.releasePublishedFramesForShutdown();
    cache->clear();
    retirementOwner->beginShutdown();
    checks.expect(waitUntil([&] { return retirementOwner->exitAcknowledged(); }, 30s),
                  "all retained host backings actually free before retirement exit");
    const auto finalClaims = processingPolicy->snapshot().reservations;
    checks.expect(
        finalClaims.ownedPackedHostBytes == 0 && finalClaims.ownedPackedHostMetadataBytes == 0 &&
            finalClaims.ownedPackedHostAllocations == 0 && finalClaims.admittedPhases == 0 &&
            finalClaims.hostReservedBytes == 0 && finalClaims.totalHostClaimBytes == 0 &&
            finalClaims.deviceReservedBytes == 0 && finalClaims.unprovenPhases == 0,
        "published payload/header and phases conserve to zero");
    std::cout << (checks.failures() ? "FAIL" : "PASS") << ": production packed cache100 "
              << (half ? "Half" : "Full") << '\n';
    return checks.failures() ? 1 : 0;
}
} // namespace
