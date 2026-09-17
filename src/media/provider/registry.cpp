#include <algorithm>
#include <bloom/media/provider/registry.hpp>

namespace bloom::media::provider {
namespace {
bool eligible(const ProviderDeclaration& p) {
    if (p.execution.availability != Availability::Available || p.evidence.result != QcResult::Pass)
        return false;
    switch (p.capability.purpose) {
    case Purpose::Preview:
    case Purpose::Proxy:
        return p.evidence.qualification == Qualification::PreviewQualified;
    case Purpose::Conform:
        return p.evidence.qualification == Qualification::ConformIngestQualified;
    case Purpose::Export:
        return p.evidence.qualification == Qualification::ExportQualified;
    case Purpose::Delivery:
        return p.evidence.qualification == Qualification::DeliveryQualified;
    }
    return false;
}
bool matches(const ProviderDeclaration& d, const PipelineStepV1& s) {
    return digest(d.capability) == Result<Digest>(s.capability) &&
           digest(d.execution) == Result<Digest>(s.execution) &&
           digest(d.evidence) == Result<Digest>(s.evidence);
}
} // namespace
bool MediaAttempt::accepts(std::size_t step, const ProviderExecutionKeyV1& execution) const {
    return !terminal_->load() && step < pipeline_->providers.size() &&
           pipeline_->providers[step].execution == execution;
}
Result<Digest> CapabilityRegistry::registerProvider(ProviderDeclaration d) {
    const auto c = digest(d.capability), e = digest(d.execution), q = digest(d.evidence);
    if (std::holds_alternative<Unavailable>(c) || std::holds_alternative<Unavailable>(e) ||
        std::holds_alternative<Unavailable>(q))
        return Unavailable{Error::InvalidValue, "Invalid provider declaration"};
    std::lock_guard lock(mutex_);
    if (providers_.size() >= Limits::entries)
        return Unavailable{Error::Oversized, "Registry capacity"};
    for (const auto& existing : providers_)
        if (existing.capability == d.capability && existing.execution == d.execution)
            return Unavailable{Error::InvalidValue, "Duplicate immutable tuple"};
    providers_.push_back(std::move(d));
    return std::get<Digest>(e);
}
Result<Digest> CapabilityRegistry::qualifyPipeline(PipelineQualificationV1 p) {
    auto key = digest(p);
    if (std::holds_alternative<Unavailable>(key) || p.result != QcResult::Pass)
        return Unavailable{Error::Unavailable, "Pipeline fixtures have not passed"};
    std::lock_guard lock(mutex_);
    if (pipelines_.size() >= Limits::entries)
        return Unavailable{Error::Oversized, "Pipeline capacity"};
    auto pinned = std::make_shared<PinnedPipeline>();
    pinned->qualification = p;
    for (const auto& step : p.steps) {
        const auto it = std::ranges::find_if(providers_, [&](const auto& d) {
            return matches(d, step) && d.capability.purpose == p.purpose && eligible(d);
        });
        if (it == providers_.end())
            return Unavailable{Error::Unavailable, "Exact qualified component unavailable"};
        pinned->providers.push_back(*it);
    }
    pipelines_.push_back(std::move(pinned));
    return key;
}
Result<MediaAttempt> CapabilityRegistry::begin(const PipelineQualificationV1& p,
                                               std::string_view authorityScope) const {
    std::lock_guard lock(mutex_);
    for (const auto& pipeline : pipelines_) {
        if (pipeline->qualification != p)
            continue;
        for (const auto& d : pipeline->providers) {
            if (std::ranges::find(providers_, d) == providers_.end())
                return Unavailable{Error::Unavailable, "Provider generation revoked"};
            if (!authorityScope.empty() && (d.evidence.authority.issuer.empty() ||
                                            d.evidence.authority.scope != authorityScope))
                return Unavailable{Error::Unavailable, "Attributed authority scope unavailable"};
        }
        return MediaAttempt(pipeline);
    }
    return Unavailable{Error::Unavailable, "Exact pipeline qualification unavailable"};
}
void CapabilityRegistry::revoke(const ProviderExecutionKeyV1& execution) {
    std::lock_guard lock(mutex_);
    std::erase_if(providers_, [&](const auto& p) { return p.execution == execution; });
}
} // namespace bloom::media::provider
