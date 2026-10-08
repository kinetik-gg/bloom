// Private compositor proof over the actual EditorArea/Viewer and adjacent widget panels.
namespace {

constexpr QColor kResizeLeftColor{235, 37, 19};
constexpr QColor kResizeRightColor{23, 229, 47};

QSplitter* makeResizeProofLayout(QWidget& host, QVBoxLayout& layout, ui::EditorArea& area) {
    auto* splitter = new QSplitter(Qt::Horizontal, &host);
    splitter->setChildrenCollapsible(false);
    const auto sentinel = [splitter](QColor color, const QString& name) {
        auto* panel = new QWidget(splitter);
        panel->setObjectName(name);
        auto palette = panel->palette();
        palette.setColor(QPalette::Window, color);
        panel->setPalette(palette);
        panel->setAutoFillBackground(true);
        panel->setMinimumWidth(40);
        return panel;
    };
    splitter->addWidget(sentinel(kResizeLeftColor, QStringLiteral("resizeLeftSentinel")));
    splitter->addWidget(&area);
    splitter->addWidget(sentinel(kResizeRightColor, QStringLiteral("resizeRightSentinel")));
    area.setMinimumWidth(0);
    layout.addWidget(splitter);
    splitter->setSizes({100, 650, 200});
    return splitter;
}

struct ResizeOwnerPause final {
    std::atomic_bool entered{false}, released{false};
    ~ResizeOwnerPause() { released.store(true, std::memory_order_release); }
};

std::shared_ptr<ResizeOwnerPause>
pauseResizeOwner(Expectations& checks, TaskScheduler& scheduler,
                 const std::shared_ptr<runtime::detail::PreviewDisplayServiceCore>& core) {
    auto pause = std::make_shared<ResizeOwnerPause>();
    auto submitted = scheduler.submitGpu<int>(
        TaskRequest("Private resize observation barrier",
                    {.kind = runtime::TaskOwnerKind::Application,
                     .id = runtime::TaskOwnerId::fromRaw(core->generation.value())},
                    runtime::TaskPriority::Interactive, runtime::TaskExecutor::Gpu),
        core->generation, runtime::GpuTaskAdmission{0, 0},
        [pause](TaskContext&, runtime::GpuTaskCompletion<int> completion) {
            pause->entered.store(true, std::memory_order_release);
            const auto deadline = std::chrono::steady_clock::now() + 8s;
            while (!pause->released.load(std::memory_order_acquire) &&
                   std::chrono::steady_clock::now() < deadline)
                std::this_thread::sleep_for(1ms);
            static_cast<void>(std::move(completion).succeed(0));
        });
    checks.expect(submitted.accepted() &&
                      waitUntil([&] { return pause->entered.load(std::memory_order_acquire); }, 3s),
                  "the bounded private barrier holds genuine owner progress");
    return pause;
}

void observeResizePhase(const char* phase, ui::ViewerEditor& viewer, QSplitter& splitter,
                        QWidget& host, const std::shared_ptr<GpuPresentationClient>& client) {
    const auto rect = [&](QWidget& widget) {
        const auto origin = widget.mapTo(&host, QPoint{});
        std::cout << '[' << origin.x() << ',' << origin.y() << ',' << widget.width() << ','
                  << widget.height() << ']';
    };
    const auto actual = viewer.displayedPreviewState();
    std::cout << "RESIZE_OBSERVATION {\"phase\":\"" << phase << "\",\"host\":[" << host.width()
              << ',' << host.height() << "],\"viewer\":";
    rect(viewer);
    std::cout << ",\"left\":";
    rect(*splitter.widget(0));
    std::cout << ",\"right\":";
    rect(*splitter.widget(2));
    std::cout << ",\"content\":[" << viewer.contentRectForTest().x() << ','
              << viewer.contentRectForTest().y() << ',' << viewer.contentRectForTest().width()
              << ',' << viewer.contentRectForTest().height() << ']';
    if (auto* container = viewer.gpuNativeContainerForTest()) {
        std::cout << ",\"container\":";
        rect(*container);
        auto* native = nativeVulkanWindow(container, &host);
        std::cout << ",\"nativeVisible\":" << (native && native->isVisible())
                  << ",\"nativeExposed\":" << (native && native->isExposed());
        std::cout << ",\"nativeAncestry\":[";
        bool first = true;
        for (auto* ancestor = native; ancestor; ancestor = ancestor->parent()) {
            if (!first)
                std::cout << ',';
            first = false;
            std::cout << "{\"name\":\"" << ancestor->objectName().toStdString() << "\",\"rect\":["
                      << ancestor->x() << ',' << ancestor->y() << ',' << ancestor->width() << ','
                      << ancestor->height() << "]}";
        }
        std::cout << ']';
    }
    std::cout << ",\"covered\":" << viewer.gpuCpuCoverVisibleForTest()
              << ",\"retirePending\":" << viewer.internalNativeRetirePendingForTest()
              << ",\"retireGeneration\":" << viewer.internalNativeRetireGenerationForTest()
              << ",\"freshness\":" << static_cast<int>(actual.freshness)
              << ",\"ownerApplied\":" << actual.ownerAppliedDisplay
              << ",\"appliedSequence\":" << viewer.gpuNativeAppliedSequenceForTest()
              << ",\"enqueuedSequence\":" << viewer.gpuNativeLastEnqueuedSequenceForTest()
              << ",\"ownerAlive\":" << client->ownerAlive() << "}\n";
    std::cout << "PHASE_RESIZE_" << phase << '\n' << std::flush;
    holdEditorOutlinePhase();
}

void exerciseViewerResizeProof(
    Expectations& checks, ui::ViewerEditor& viewer, ui::EditorArea& area, QSplitter& splitter,
    QWidget& host, TaskScheduler& scheduler,
    const std::shared_ptr<runtime::detail::PreviewDisplayServiceCore>& core,
    const std::shared_ptr<GpuPresentationClient>& client) {
    const auto recover = [&] {
        return waitUntil(
            [&] {
                return viewer.residentPresentationActiveForTest() &&
                       !viewer.gpuCpuCoverVisibleForTest() &&
                       viewer.displayedPreviewState().freshness == ui::FrameFreshness::Current;
            },
            20s);
    };
    checks.expect(recover(), "the resize baseline is actually current and uncovered");
    observeResizePhase("BASE", viewer, splitter, host, client);
    auto pause = pauseResizeOwner(checks, scheduler, core);
    splitter.setSizes({160, 500, 280});
    QApplication::processEvents();
    splitter.setSizes({220, 400, 320});
    QApplication::processEvents();
    splitter.setSizes({220, 300, 430});
    QApplication::processEvents();
    observeResizePhase("HOLD_SHRINK", viewer, splitter, host, client);
    pause->released.store(true, std::memory_order_release);
    checks.expect(recover(), "rapid shrink recovers at the newest Viewer geometry");
    observeResizePhase("SMALL", viewer, splitter, host, client);
    const auto width = area.width();
    pause = pauseResizeOwner(checks, scheduler, core);
    splitter.setSizes({320, width, splitter.width() - 2 * splitter.handleWidth() - 320 - width});
    QApplication::processEvents();
    observeResizePhase("HOLD_MOVE", viewer, splitter, host, client);
    pause->released.store(true, std::memory_order_release);
    checks.expect(recover(), "same-size ancestor move recovers current pixels");
    observeResizePhase("MOVED", viewer, splitter, host, client);
}

// Qualification only: this deliberately does not alter production WorkspaceHost. The real
// retirement gate admits at most one pending transaction; its commit reads the newest intent.
void exerciseGeometryAdmissionProof(
    Expectations& checks, ui::ViewerEditor& viewer, ui::EditorArea& area, QSplitter& splitter,
    QWidget& host, TaskScheduler& scheduler,
    const std::shared_ptr<runtime::detail::PreviewDisplayServiceCore>& core,
    const std::shared_ptr<GpuPresentationClient>& client, const bool availabilityOnly = false) {
    ui::NativeSurfaceRetirementGate gate;
    std::function<void()> newestCommit;
    std::uint64_t intent = 0, committedIntent = 0;
    std::size_t starts = 0, commits = 0;
    const auto request = [&](std::function<void()> commit) {
        newestCommit = std::move(commit);
        ++intent;
        if (gate.isPending())
            return;
        ++starts;
        static_cast<void>(gate.begin(
            {&viewer},
            [&] {
                committedIntent = intent;
                ++commits;
                auto commitNewest = std::move(newestCommit);
                newestCommit = {};
                commitNewest();
            },
            [&](const ui::NativeSurfaceRetirementGate::Result& result) {
                checks.expect(result.committed, "geometry admission gets a real safe retirement");
            }));
    };
    const auto recover = [&] {
        return waitUntil(
            [&] {
                return !gate.isPending() && viewer.residentPresentationActiveForTest() &&
                       !viewer.gpuCpuCoverVisibleForTest() &&
                       viewer.displayedPreviewState().freshness == ui::FrameFreshness::Current;
            },
            20s);
    };
    checks.expect(recover(), "admission starts from genuinely displayed current pixels");
    observeResizePhase("GATE_BASE", viewer, splitter, host, client);
    const auto baseFrame = viewer.displayedPreviewState().frame;
    const auto oldSizes = splitter.sizes();
    auto pause = pauseResizeOwner(checks, scheduler, core);
    request([&] { splitter.setSizes({160, 500, 280}); });
    request([&] { splitter.setSizes({220, 400, 320}); });
    request([&] { splitter.setSizes({220, 300, 430}); });
    checks.expect(gate.isPending() && starts == 1U && commits == 0U && splitter.sizes() == oldSizes,
                  "three authored intents keep one retirement and the old admitted layout");
    std::cout << "GEOMETRY_ADMISSION Pending intent=" << intent << " starts=" << starts
              << " commits=" << commits << '\n';
    const auto pendingDisplay = viewer.displayedPreviewState();
    checks.expect(pendingDisplay.frame == baseFrame && pendingDisplay.ownerAppliedDisplay &&
                      pendingDisplay.freshness == ui::FrameFreshness::Stale,
                  "the visible old coherent pair remains actual Stale during held retirement");
    observeResizePhase("GATE_SPLITTER_PENDING", viewer, splitter, host, client);
    pause->released.store(true, std::memory_order_release);
    checks.expect(recover() && committedIntent == 3U && commits == 1U,
                  "only the newest authored layout commits after real retirement");
    observeResizePhase("GATE_SPLITTER_APPLIED", viewer, splitter, host, client);
    if (availabilityOnly)
        return;

    const auto width = area.width();
    pause = pauseResizeOwner(checks, scheduler, core);
    request([&] {
        splitter.setSizes(
            {320, width, splitter.width() - 2 * splitter.handleWidth() - 320 - width});
    });
    observeResizePhase("GATE_MOVE_PENDING", viewer, splitter, host, client);
    pause->released.store(true, std::memory_order_release);
    checks.expect(recover() && area.width() == width,
                  "same-size ancestor movement commits without stale geometry recovery");
    observeResizePhase("GATE_MOVE_APPLIED", viewer, splitter, host, client);

    // Restore a wide Viewer so shrinking the actual top-level cuts through the previously admitted
    // native extent. Freeze the workspace before configure; this is a stronger case than receiving
    // an OS resize notification and only then beginning retirement.
    request([&] { splitter.setSizes({100, 650, 200}); });
    checks.expect(recover(), "the wide top-level qualification baseline is current");
    observeResizePhase("GATE_TOP_BASE", viewer, splitter, host, client);
    const auto oldSplitterSize = splitter.size();
    host.layout()->setEnabled(false);
    pause = pauseResizeOwner(checks, scheduler, core);
    request([&] {
        host.layout()->setEnabled(true);
        host.layout()->activate();
    });
    host.resize(600, 640);
    QApplication::processEvents();
    checks.expect(gate.isPending() && splitter.size() == oldSplitterSize,
                  "actual top-level shrink leaves workspace geometry frozen until safe");
    observeResizePhase("GATE_TOP_PENDING", viewer, splitter, host, client);
    pause->released.store(true, std::memory_order_release);
    checks.expect(recover(), "actual top-level shrink recovers after safe layout admission");
    observeResizePhase("GATE_TOP_APPLIED", viewer, splitter, host, client);
}

} // namespace
