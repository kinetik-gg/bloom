void testQueuedColorIntentOwnership(Expectations& expectations) {
    using namespace bloom;
    auto project = makeTestProject("Queued colour capture");
    const auto compositionId = project.initialCompositionId;
    document::Document document(std::move(project.project));
    commands::CommandStack commands(document);
    ui::CompositionSession session(document, commands, compositionId);
    runtime::TaskScheduler scheduler(testSchedulerConfig());
    ui::TaskUiBridge bridge(scheduler, nullptr, 1ms);
    WorkerGate gate;
    std::atomic<int> calls = 0;
    std::atomic<bool> originalSurvived = false;
    std::string working(runtime::kLinearRec709SceneColorSpaceId);
    std::string uri(runtime::kBloomNeutralOcioConfigUri);
    ui::CompositionPreviewController controller(
        session, scheduler, bridge,
        [&](const document::Snapshot&, const runtime::PreviewRequestIdentity& identity, std::size_t,
            const std::vector<runtime::SnapshotParameterOverride>&, runtime::TaskContext&) {
            if (calls.fetch_add(1) == 0) {
                gate.enterAndWait();
                originalSurvived.store(identity.colorIntent.storage &&
                                       identity.colorIntent.workingColorSpaceId ==
                                           runtime::kLinearRec709SceneColorSpaceId &&
                                       identity.colorIntent.ocioConfigUri ==
                                           runtime::kBloomNeutralOcioConfigUri);
            }
            return PipelineResult::cancelled();
        },
        {.colorIntent = {.workingColorSpaceId = working, .ocioConfigUri = uri},
         .displayName = {},
         .viewName = {}});
    expectations.expect(waitUntil([&] { return gate.entered(); }),
                        "colour capture task enters before producer mutation");
    const auto retainedIdentity = controller.state().desiredIdentity;
    working.assign(4096, 'x');
    uri.assign(4096, 'y');
    auto changed = session.colorSettings();
    std::get<document::BuiltInOcioConfigLocator>(changed.ocioConfig.locator).uri =
        std::string(256, 'z');
    session.setColorSettings(std::move(changed));
    expectations.expect(controller.settings().colorIntent.storage &&
                            controller.settings().colorIntent.ocioConfigUri ==
                                std::string(256, 'z'),
                        "colour update stores owning intent before next asynchronous request");
    gate.release();
    expectations.expect(waitUntil([&] { return originalSurvived.load(); }),
                        "cancelled queued worker still reads its original exact colour strings");
    expectations.expect(
        retainedIdentity.has_value() && retainedIdentity->colorIntent.storage &&
            retainedIdentity->colorIntent.workingColorSpaceId ==
                runtime::kLinearRec709SceneColorSpaceId &&
            retainedIdentity->colorIntent.ocioConfigUri == runtime::kBloomNeutralOcioConfigUri,
        "retained preview identity survives producer strings and session settings replacement");
    reachQuiescence(controller, bridge, scheduler, expectations);
}
