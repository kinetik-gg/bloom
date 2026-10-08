#include <bloom/ui/preview_frame_trace.hpp>

#include <iostream>
#include <sstream>

int main() {
    using namespace bloom;
    using namespace bloom::ui;
    int failures = 0;
    const auto expect = [&failures](const bool condition, const char* message) {
        if (!condition) {
            ++failures;
            std::cerr << message << '\n';
        }
    };
    runtime::PreviewRequestIdentity request{.projectId = document::ProjectId::fromRaw(1),
                                            .compositionId = document::CompositionId::fromRaw(2),
                                            .sourceRevision = document::Revision::fromRaw(3),
                                            .requestGeneration = 4,
                                            .time = core::RationalTime::fromInteger(1),
                                            .resolution = runtime::CompositionFormatResolution{},
                                            .displayName = "/private/customer/secret.exr",
                                            .viewName = "private customer view"};
    const auto original = PreviewFrameTrace::identity(request);
    request.requestGeneration = 5;
    const auto warm = PreviewFrameTrace::identity(request);
    expect(original.intent == warm.intent && original.generation != warm.generation,
           "warm restamping preserves intent while publication generation changes");
    request.viewName = "another view";
    expect(PreviewFrameTrace::identity(request).intent != warm.intent,
           "display/view intent distinguishes otherwise identical requests");
    request.viewName = "private customer view";
    const auto roi = render::ImageWindow::create(1, 2, 4, 8);
    request.roi = *roi.value();
    expect(PreviewFrameTrace::identity(request).intent != warm.intent,
           "ROI changes request intent");

    PreviewFrameTrace trace(3);
    trace.setPollingIntervals(std::chrono::milliseconds{100}, std::chrono::milliseconds{4});
    trace.record(original, PreviewTraceStage::Desired, PreviewTraceOutcome::Requested);
    trace.record(original, PreviewTraceStage::PresentSubmitted, PreviewTraceOutcome::Accepted, 3, 0,
                 10, 0, 0, 1);
    // Newer desired intent does not change the identity of the owner's older applied sequence.
    trace.record(warm, PreviewTraceStage::Desired, PreviewTraceOutcome::Requested);
    trace.record(original, PreviewTraceStage::PresentAcknowledged, PreviewTraceOutcome::Accepted, 3,
                 0, 10, 10, 1, 1);
    const auto events = trace.events();
    expect(events.size() == 3 && trace.overwritten() == 1, "overflow is bounded and counted");
    expect(events.front().ordinal == 2 && events.back().ordinal == 4,
           "the newest bounded history remains chronological");
    expect(events[1].identity.generation == 5 && events.back().identity.generation == 4,
           "submitted/acknowledged identity remains separate from newer desired intent");
    expect(events.back().sequence == events.front().sequence &&
               events.back().identity == events.front().identity,
           "acknowledgement correlates by presenter and sequence");
    std::ostringstream output;
    trace.writeJson(output);
    const auto json = output.str();
    expect(json.find("/private/") == std::string::npos &&
               json.find("customer") == std::string::npos,
           "publishable records contain no raw private labels or locators");
    expect(json.find("not_scanout") != std::string::npos &&
               json.find("\"bridge_poll_ms\":100") != std::string::npos,
           "native acknowledgement and actual polling defaults are explicit");
    PreviewFrameTrace unknown(0);
    unknown.record(PreviewTraceIdentity{}, PreviewTraceStage::PresentAcknowledged,
                   PreviewTraceOutcome::Accepted);
    std::ostringstream unavailable;
    unknown.writeJson(unavailable);
    expect(unavailable.str().find("\"identity_available\":false") != std::string::npos &&
               unavailable.str().find("\"provider\":null") != std::string::npos,
           "unavailable identity/provider fields are not fabricated");
    if (failures)
        return 1;
    std::cout << "PASS: bounded private frame trace\n";
    return 0;
}
