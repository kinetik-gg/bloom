#pragma once
#include <atomic>
#include <bloom/media/provider/contract.hpp>
#include <memory>
#include <mutex>

namespace bloom::media::provider {
struct ProviderDeclaration {
    MediaCapabilityKeyV1 capability;
    ProviderExecutionKeyV1 execution;
    QualificationEvidenceV1 evidence;
    friend bool operator==(const ProviderDeclaration&, const ProviderDeclaration&) = default;
};
struct PinnedPipeline {
    PipelineQualificationV1 qualification;
    std::vector<ProviderDeclaration> providers;
};
class MediaAttempt final {
  public:
    [[nodiscard]] const PinnedPipeline& pipeline() const { return *pipeline_; }
    // Once terminated, this handle can never accept another product or replacement execution.
    [[nodiscard]] bool accepts(std::size_t step, const ProviderExecutionKeyV1& execution) const;
    void terminate() const { terminal_->store(true); }

  private:
    friend class CapabilityRegistry;
    explicit MediaAttempt(std::shared_ptr<const PinnedPipeline> pipeline)
        : pipeline_(std::move(pipeline)) {}
    std::shared_ptr<const PinnedPipeline> pipeline_;
    std::shared_ptr<std::atomic<bool>> terminal_ = std::make_shared<std::atomic<bool>>(false);
};
class CapabilityRegistry final {
  public:
    [[nodiscard]] Result<Digest> registerProvider(ProviderDeclaration declaration);
    [[nodiscard]] Result<Digest> qualifyPipeline(PipelineQualificationV1 pipeline);
    [[nodiscard]] Result<MediaAttempt> begin(const PipelineQualificationV1& exactPipeline,
                                             std::string_view requiredAuthorityScope = {}) const;
    void revoke(const ProviderExecutionKeyV1& execution);

  private:
    mutable std::mutex mutex_;
    std::vector<ProviderDeclaration> providers_;
    std::vector<std::shared_ptr<const PinnedPipeline>> pipelines_;
};
} // namespace bloom::media::provider
