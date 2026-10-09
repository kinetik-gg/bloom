#include <bloom/ui/preview_frame_trace.hpp>

#include <algorithm>
#include <bit>
#include <ostream>

namespace bloom::ui {
namespace {
void mix(std::uint64_t& hash, const std::uint64_t value) noexcept {
    hash ^= value;
    hash *= 1099511628211ULL;
}
void mix(std::uint64_t& hash, const std::string_view value) noexcept {
    mix(hash, value.size());
    for (const char byte : value)
        mix(hash, static_cast<unsigned char>(byte));
}
const char* stageName(const PreviewTraceStage stage) noexcept {
    switch (stage) {
    case PreviewTraceStage::Desired:
        return "desired";
    case PreviewTraceStage::Queued:
        return "queued";
    case PreviewTraceStage::Delivered:
        return "delivered";
    case PreviewTraceStage::Published:
        return "published";
    case PreviewTraceStage::CacheLookup:
        return "cache_lookup";
    case PreviewTraceStage::CacheRetention:
        return "cache_retention";
    case PreviewTraceStage::CacheRemoval:
        return "cache_removal";
    case PreviewTraceStage::PresentSubmitted:
        return "present_submitted";
    case PreviewTraceStage::PresentAcknowledged:
        return "present_acknowledged";
    case PreviewTraceStage::CpuPaint:
        return "cpu_paint";
    }
    return "unknown";
}
const char* outcomeName(const PreviewTraceOutcome outcome) noexcept {
    switch (outcome) {
    case PreviewTraceOutcome::Requested:
        return "requested";
    case PreviewTraceOutcome::Accepted:
        return "accepted";
    case PreviewTraceOutcome::Rejected:
        return "rejected";
    case PreviewTraceOutcome::Current:
        return "current";
    case PreviewTraceOutcome::Stale:
        return "stale";
    case PreviewTraceOutcome::Cancelled:
        return "cancelled";
    case PreviewTraceOutcome::Failed:
        return "failed";
    case PreviewTraceOutcome::Unsupported:
        return "unsupported";
    case PreviewTraceOutcome::Hit:
        return "hit";
    case PreviewTraceOutcome::Miss:
        return "miss";
    case PreviewTraceOutcome::Retained:
        return "retained";
    case PreviewTraceOutcome::Reused:
        return "reused";
    case PreviewTraceOutcome::Removed:
        return "removed";
    case PreviewTraceOutcome::Late:
        return "late";
    case PreviewTraceOutcome::Invalid:
        return "invalid";
    case PreviewTraceOutcome::OutOfRange:
        return "out_of_range";
    case PreviewTraceOutcome::WrongRevision:
        return "wrong_revision";
    case PreviewTraceOutcome::OverBudget:
        return "over_budget";
    case PreviewTraceOutcome::AllocationFailure:
        return "allocation_failure";
    }
    return "unknown";
}
} // namespace

PreviewFrameTrace::PreviewFrameTrace(const std::size_t capacity)
    : buffer_(std::clamp(capacity, std::size_t{1}, std::size_t{8192})) {}

PreviewTraceIdentity
PreviewFrameTrace::identity(const runtime::PreviewRequestIdentity& request) noexcept {
    std::uint64_t hash = 14695981039346656037ULL;
    mix(hash, request.projectId.value());
    mix(hash, request.compositionId.value());
    mix(hash, request.sourceRevision.value());
    mix(hash, static_cast<std::uint64_t>(request.time.numerator()));
    mix(hash, static_cast<std::uint64_t>(request.time.denominator()));
    mix(hash, static_cast<std::uint64_t>(request.output));
    mix(hash, request.resolution.index());
    if (const auto* proxy = std::get_if<runtime::ProxyResolution>(&request.resolution)) {
        mix(hash, proxy->extent.width());
        mix(hash, proxy->extent.height());
    }
    mix(hash, static_cast<std::uint64_t>(request.quality));
    mix(hash, request.colorIntent.workingColorSpaceId);
    mix(hash, request.colorIntent.ocioConfigUri);
    for (const auto byte : request.colorIntent.ocioConfigRevision.bytes())
        mix(hash, byte);
    mix(hash, static_cast<std::uint64_t>(request.resolutionPolicy));
    mix(hash, request.roi.has_value());
    if (request.roi) {
        mix(hash, static_cast<std::uint64_t>(request.roi->originX()));
        mix(hash, static_cast<std::uint64_t>(request.roi->originY()));
        mix(hash, request.roi->extent().width());
        mix(hash, request.roi->extent().height());
    }
    mix(hash, std::bit_cast<std::uint64_t>(request.viewAdjust.exposure));
    mix(hash, std::bit_cast<std::uint64_t>(request.viewAdjust.gamma));
    mix(hash, request.displayName);
    mix(hash, request.viewName);
    mix(hash, request.showLook);
    return {.project = request.projectId.value(),
            .composition = request.compositionId.value(),
            .revision = request.sourceRevision.value(),
            .generation = request.requestGeneration,
            .intent = hash,
            .timeNumerator = request.time.numerator(),
            .timeDenominator = request.time.denominator()};
}

void PreviewFrameTrace::record(const PreviewTraceIdentity identity, const PreviewTraceStage stage,
                               const PreviewTraceOutcome outcome, const std::uint8_t provider,
                               const std::uint64_t task, const std::uint64_t sequence,
                               const std::uint64_t appliedSequence,
                               const std::uint64_t nativePresentCount,
                               const std::uint64_t presenter) noexcept {
    buffer_[next_] = {.ordinal = ++ordinal_,
                      .wallNanoseconds = std::chrono::duration_cast<std::chrono::nanoseconds>(
                                             std::chrono::steady_clock::now() - started_)
                                             .count(),
                      .identity = identity,
                      .stage = stage,
                      .outcome = outcome,
                      .provider = provider,
                      .phase = phase_,
                      .task = task,
                      .sequence = sequence,
                      .appliedSequence = appliedSequence,
                      .nativePresentCount = nativePresentCount,
                      .presenter = presenter};
    if (stage == PreviewTraceStage::PresentAcknowledged)
        nativeAck_ = buffer_[next_];
    next_ = (next_ + 1) % buffer_.size();
    if (size_ < buffer_.size())
        ++size_;
    else
        ++overwritten_;
}
void PreviewFrameTrace::record(const runtime::PreviewRequestIdentity& request,
                               const PreviewTraceStage stage, const PreviewTraceOutcome outcome,
                               const std::uint8_t provider, const std::uint64_t task) noexcept {
    record(identity(request), stage, outcome, provider, task);
}
void PreviewFrameTrace::record(const runtime::PreparedPreviewFrame& frame,
                               const PreviewTraceStage stage,
                               const PreviewTraceOutcome outcome) noexcept {
    record(frame.desiredIdentity(), stage, outcome,
           static_cast<std::uint8_t>(frame.provenance().provider));
}
std::vector<PreviewTraceEvent> PreviewFrameTrace::events() const {
    std::vector<PreviewTraceEvent> result;
    result.reserve(size_);
    const auto first = size_ == buffer_.size() ? next_ : 0;
    for (std::size_t i = 0; i < size_; ++i)
        result.push_back(buffer_[(first + i) % buffer_.size()]);
    return result;
}
void PreviewFrameTrace::setPollingIntervals(const std::chrono::milliseconds bridge,
                                            const std::chrono::milliseconds foreground) noexcept {
    bridgeInterval_ = bridge;
    foregroundInterval_ = foreground;
}
void PreviewFrameTrace::writeJson(std::ostream& output) const {
    output << "{\"schema\":1,\"clock\":\"cpu_steady_wall\",\"identity_kind\":\"request_intent_"
              "fingerprint\","
              "\"acknowledgement\":\"native_owner_applied_sequence_not_scanout\","
              "\"unavailable\":[\"scanout\",\"gpu_timestamps\",\"source_validation_read_decode\","
              "\"plan_upload_dispatch_display_stage_times\",\"canonical_pixel_cache_key\","
              "\"fallback_diagnostic_text\",\"worker_prepared_timestamp\",\"native_host_polls\"],"
              "\"provider_codes\":{\"0\":\"cpu_reference\",\"1\":\"cpu_ocio\","
              "\"2\":\"gpu_readback\",\"3\":\"gpu_resident\"},\"bridge_poll_ms\":"
           << bridgeInterval_.count() << ",\"foreground_poll_ms\":" << foregroundInterval_.count()
           << ",\"bridge_polls\":" << bridgePolls_
           << ",\"foreground_mailbox_polls\":" << foregroundPolls_
           << ",\"completion_wakes\":" << completionWakes_
           << ",\"completion_serial\":" << completionSerial_
           << ",\"foreground_deliveries\":" << foregroundDeliveries_
           << ",\"capacity\":" << buffer_.size() << ",\"overwritten\":" << overwritten_
           << ",\"events\":[";
    bool first = true;
    for (const auto& event : events()) {
        if (!first)
            output << ',';
        first = false;
        const auto& id = event.identity;
        output << "{\"ordinal\":" << event.ordinal << ",\"wall_ns\":" << event.wallNanoseconds
               << ",\"phase\":" << static_cast<unsigned>(event.phase) << ",\"stage\":\""
               << stageName(event.stage) << "\",\"outcome\":\"" << outcomeName(event.outcome)
               << "\",\"project\":" << id.project
               << ",\"identity_available\":" << (id.project != 0 ? "true" : "false")
               << ",\"composition\":" << id.composition << ",\"revision\":" << id.revision
               << ",\"generation\":" << id.generation << ",\"intent\":" << id.intent
               << ",\"time_numerator\":" << id.timeNumerator
               << ",\"time_denominator\":" << id.timeDenominator << ",\"provider\":";
        if (event.provider == 255)
            output << "null";
        else
            output << static_cast<unsigned>(event.provider);
        output << ",\"task\":" << event.task << ",\"sequence\":" << event.sequence
               << ",\"applied_sequence\":" << event.appliedSequence
               << ",\"native_present_count\":" << event.nativePresentCount
               << ",\"presenter\":" << event.presenter << '}';
    }
    output << "]}\n";
}
} // namespace bloom::ui
