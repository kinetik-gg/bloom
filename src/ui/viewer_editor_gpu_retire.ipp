// ViewerEditor native-surface retirement (included at the end of viewer_editor.cpp).
//
// The blank/no-frame transition and the host's EditorNativeSurface mutation gate must never unmap
// or destroy a live native presentation surface before the adapter has genuinely retired it. This
// file owns that bounded retire-before-unmap machinery: the internal blank retirement, the external
// gate passthrough and its fan-in, and the queued completion that resets the adapter on a clean
// stack. Definitions only; ViewerEditor declares every entry point.

EditorNativeSurface::PrepareOutcome
ViewerEditor::prepareNativeSurfaceMutation(const std::uint64_t generation,
                                           PrepareCallback completion) {
    if (gpuResident_ == nullptr) {
        if (completion) {
            completion(generation, PrepareResult{true, "no native surface implementation"});
        }
        return EditorNativeSurface::PrepareOutcome::NoLiveTarget;
    }
    if (gpuNativeRetirePending_) {
        if (gpuHostMutationWaiter_.has_value()) {
            // The EditorNativeSurface contract admits one pending mutation per editor. A second
            // concurrent host request is refused explicitly; it is never silently coerced into the
            // first or dropped, and no safety is claimed for it.
            return EditorNativeSurface::PrepareOutcome::Refused;
        }
        // Fold this host request into the in-flight internal retirement and answer it with that
        // retirement's truthful result instead of falsely refusing a mutation that is genuinely in
        // flight. The presenter has exactly one mutation callback, so a second request would
        // otherwise answer Refused and make the host gate fail despite a successful retirement.
        gpuHostMutationPending_ = true;
        gpuHostMutationWaiter_ = GpuHostMutationWaiter{generation, std::move(completion)};
        return EditorNativeSurface::PrepareOutcome::RetirePending;
    }
    if (gpuHostMutationPending_) {
        // An external retirement already owns the presenter's single retire slot. Refuse the
        // duplicate BEFORE touching gpuHostMutationGeneration_: superseding the first request would
        // discard its queued completion and hang the host gate.
        return EditorNativeSurface::PrepareOutcome::Refused;
    }
    if (!gpuResident_->hasLiveTarget()) {
        // No live target: answer synchronously, exactly like the controller's NoLiveTarget path, so
        // the host may mutate now and a later request can never supersede this answer.
        if (completion) {
            completion(generation, PrepareResult{true, "no live presentation target"});
        }
        return EditorNativeSurface::PrepareOutcome::NoLiveTarget;
    }
    // The presenter invokes its mutation callback inline from applySnapshot() and then reclaims the
    // terminal owner record on that same stack, so running the host callback there could commit a
    // tree mutation that destroys the presenter mid-applySnapshot. Deliver every live external
    // completion through a queued, generation-guarded, `this`-context call instead.
    const std::uint64_t hostGeneration = ++gpuHostMutationGeneration_;
    PrepareCallback queued = [this, generation, hostGeneration,
                              completion](const std::uint64_t, const PrepareResult& result) {
        QMetaObject::invokeMethod(
            this,
            [this, generation, hostGeneration, safe = result.safeToMutate,
             diagnostic = result.diagnostic, completion] {
                onExternalNativeRetireResult(generation, hostGeneration, safe, diagnostic,
                                             completion);
            },
            Qt::QueuedConnection);
    };
    const auto outcome = gpuResident_->prepareForMutation(hostGeneration, queued);
    if (outcome == EditorNativeSurface::PrepareOutcome::RetirePending) {
        gpuHostMutationPending_ = true;
    }
    return outcome;
}

void ViewerEditor::clearGpuContainer() {
    // The presenter owns the container (it is parented to this widget). The container handle must
    // be dropped BEFORE the adapter is reset, because resetting a proven-Retired presenter deletes
    // the container through its Impl; dereferencing the stale pointer afterwards would be a UAF.
    gpuContainer_ = nullptr;
}

void ViewerEditor::resumeNativeSurfaceAfterMutation() {
    if (gpuResident_ == nullptr) {
        return;
    }
    // Invalidate any queued internal or external completion FIRST so neither can reset the
    // presenter reentrantly after the host already completed its mutation, and drop the folded-in
    // host waiter (the host is resolving this generation itself now).
    ++gpuNativeRetireGeneration_;
    ++gpuHostMutationGeneration_;
    gpuNativeRetirePending_ = false;
    gpuHostMutationPending_ = false;
    gpuHostMutationWaiter_.reset();
    // The host resumes only after a proven SafeToMutate, so resumeAfterMutation() destroys the
    // (Retired) presenter and the container it owns. Drop our raw handle BEFORE that reset; when
    // the presenter is not in a resettable state it stays mapped and keeps its handle.
    if (!gpuResident_->hasLiveTarget()) {
        clearGpuContainer();
    }
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
    if (!gpuResident_->hasLiveTarget()) {
        // Nothing native remains: expose the host's own CPU paint instead of leaving the blank
        // cover snapshot on top of a now-destroyed container.
        gpuResident_->concealCpuCover();
    }
}

void ViewerEditor::requestResidentNativeRetire() {
    if (gpuResident_ == nullptr || gpuNativeRetirePending_ || !gpuResident_->hasLiveTarget()) {
        return;
    }
    // Keep the last native image hidden without unmapping: raise the host's CURRENT CPU paint as an
    // opaque native cover over the still-mapped container before the retire starts. The container
    // and its QWindow stay mapped, so Qt/Wayland never recreates the VkSurfaceKHR.
    const QRect containerRect = contentRect().toRect();
    if (!containerRect.isEmpty()) {
        gpuResident_->revealCpuCover(containerRect);
    }
    if (gpuResident_->presenterState() == ViewerGpuPresenter::State::Retained) {
        // A Retained target is terminal: no later retire can ever be proven safe. Never start a
        // retry loop against it (and never recapture the cover every tick); keep the target mapped
        // and the current cover raised until the host tears the tree down.
        return;
    }
    if (gpuHostMutationPending_) {
        // An external EditorNativeSurface mutation already owns the presenter's single retire slot.
        // Do not issue a conflicting retire; the external flow resolves and resumes. The target
        // stays mapped with the cover raised meanwhile.
        return;
    }
    gpuNativeRetirePending_ = true;
    const std::uint64_t generation = ++gpuNativeRetireGeneration_;
    const auto outcome = gpuResident_->prepareForMutation(
        generation, [this](const std::uint64_t callbackGeneration, const PrepareResult& result) {
            // The presenter invokes this from inside its own poll tick (and, in the terminal
            // branches, from the very stack frame that reclaims the owner record). Resetting the
            // adapter inline would destroy that stack; marshal the result to the event loop so the
            // retire is always observed on a clean stack. The generation guards a stale completion
            // after a newer retire (or after teardown), and the `this` context object cancels the
            // queued call outright if this editor is destroyed first.
            QMetaObject::invokeMethod(
                this,
                [this, callbackGeneration, safe = result.safeToMutate,
                 diagnostic = result.diagnostic] {
                    onResidentNativeRetireResult(callbackGeneration, safe, diagnostic);
                },
                Qt::QueuedConnection);
        });
    if (outcome == EditorNativeSurface::PrepareOutcome::Refused) {
        // Quarantined/unproven target: keep everything mapped and the cover raised, and never
        // invent safety. A later tick retries once a genuine release becomes possible.
        gpuNativeRetirePending_ = false;
    } else if (outcome == EditorNativeSurface::PrepareOutcome::NoLiveTarget) {
        // No live target after all: hiding the container mutates no surface.
        gpuNativeRetirePending_ = false;
        if (gpuContainer_ != nullptr) {
            gpuContainer_->hide();
        }
        if (gpuResident_ != nullptr) {
            gpuResident_->concealCpuCover();
        }
    }
}

void ViewerEditor::onResidentNativeRetireResult(const std::uint64_t generation,
                                                const bool safeToMutate,
                                                const std::string& diagnostic) {
    if (generation != gpuNativeRetireGeneration_) {
        return; // stale completion from a superseded retire (or before teardown)
    }
    gpuNativeRetirePending_ = false;
    // Any host commit resolved below may tear this editor down; guard every later member access.
    QPointer<ViewerEditor> self(this);
    if (!safeToMutate) {
        // The owner could not prove the surface safe: leave the container mapped and the CURRENT
        // CPU cover raised. No false SafeToMutate, no unmapping of a live target. A later tick may
        // retry once a genuine release becomes possible.
        if (gpuResident_ != nullptr) {
            const QRect containerRect = contentRect().toRect();
            if (!containerRect.isEmpty()) {
                gpuResident_->revealCpuCover(containerRect);
            }
        }
        resolveGpuHostMutationWaiters(false, diagnostic);
        if (self.isNull()) {
            return;
        }
        update();
        return;
    }
    // Proven Retired: the presenter may now be destroyed, which deletes the container it owns.
    // Bump the generation first so a second queued completion cannot reset the presenter again,
    // and drop the raw container handle BEFORE the reset.
    ++gpuNativeRetireGeneration_;
    clearGpuContainer();
    if (gpuResident_ != nullptr) {
        gpuResident_->resumeAfterMutation();
    }
    residentActive_ = false;
    gpuPresentedFrame_.reset();
    resolveGpuHostMutationWaiters(true, diagnostic);
    if (self.isNull()) {
        return;
    }
    update();
    // Re-evaluate on the next event-loop turn, never inline: if a new resident frame already
    // arrived while the retire was in flight this builds a fresh presenter/target.
    QMetaObject::invokeMethod(
        this, [this] { updateGpuResidentPresentation(); }, Qt::QueuedConnection);
}

void ViewerEditor::onExternalNativeRetireResult(const std::uint64_t generation,
                                                const std::uint64_t hostGeneration,
                                                const bool safeToMutate,
                                                const std::string& diagnostic,
                                                const PrepareCallback& completion) {
    if (hostGeneration != gpuHostMutationGeneration_) {
        return; // superseded by a resume or a newer external request
    }
    if (!safeToMutate) {
        // A standalone external retirement owns the presenter's retire slot, and the host gate
        // deliberately never resumes an unsafe (Retained/unproven) entry, so nothing else will ever
        // clear this flag. Clear it BEFORE the host completion: that completion may destroy this
        // editor or start another generation, and a latched gate would refuse every later mutation
        // and blank retire. The target stays mapped with its cover raised; a Retained presenter
        // keeps refusing new retires, which is the correct terminal safety, not a recovery.
        gpuHostMutationPending_ = false;
    }
    if (completion) {
        completion(generation, PrepareResult{safeToMutate, diagnostic});
    }
}

void ViewerEditor::resolveGpuHostMutationWaiters(const bool safeToMutate,
                                                 const std::string& diagnostic) {
    gpuHostMutationPending_ = false;
    if (!gpuHostMutationWaiter_.has_value()) {
        return;
    }
    // Move the single waiter out before invoking: the host completion may re-enter this editor (or
    // destroy it), and it must never observe or mutate the member again.
    const GpuHostMutationWaiter waiter = std::move(*gpuHostMutationWaiter_);
    gpuHostMutationWaiter_.reset();
    if (waiter.completion) {
        waiter.completion(waiter.generation, PrepareResult{safeToMutate, diagnostic});
    }
}

void ViewerEditor::simulateNativeRetireInFlightForTest() {
    gpuNativeRetirePending_ = true;
    gpuHostMutationPending_ = false;
    gpuHostMutationWaiter_.reset();
    ++gpuNativeRetireGeneration_;
}

void ViewerEditor::finishSimulatedNativeRetireForTest(const bool safeToMutate,
                                                      const std::string& diagnostic) {
    onResidentNativeRetireResult(gpuNativeRetireGeneration_, safeToMutate, diagnostic);
}

void ViewerEditor::simulateExternalRetireInFlightForTest() {
    gpuNativeRetirePending_ = false;
    gpuHostMutationPending_ = true;
    gpuHostMutationWaiter_.reset();
    ++gpuHostMutationGeneration_;
}

void ViewerEditor::finishSimulatedExternalRetireForTest(const std::uint64_t hostGeneration,
                                                        const bool safeToMutate,
                                                        const PrepareCallback& completion,
                                                        const std::string& diagnostic) {
    // Drives the exact body the queued external completion runs (onExternalNativeRetireResult), so
    // the unsafe-clear / safe-preserve / stale-generation contract is testable without a presenter.
    onExternalNativeRetireResult(hostGeneration, hostGeneration, safeToMutate, diagnostic,
                                 completion);
}

std::uint64_t ViewerEditor::hostMutationGenerationForTest() const noexcept {
    return gpuHostMutationGeneration_;
}
