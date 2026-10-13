#pragma once

#include <bloom/render/gpu_processing_phase_budget.hpp>

#include <cstddef>
#include <limits>
#include <memory>
#include <new>
#include <optional>
#include <utility>

namespace bloom::render {
struct ProcessingHostStorageGrant;
struct ProcessingHostStorageRefusal final {
    GpuProcessingPhaseAdmission status = GpuProcessingPhaseAdmission::InvalidRequest;
    GpuProcessingPhaseDomain domain = GpuProcessingPhaseDomain::None;
    std::uint64_t phaseId = 0;
    std::uint64_t revisionAfterOwnRelease = 0;
};

// Scratch never publishes. Exact allocator-requested payload/header bytes require fresh additive
// host headroom and keep their admitted phase alive through actual backing destruction.
class ProcessingHostStorageToken final {
  public:
    ProcessingHostStorageToken() = default;
    ProcessingHostStorageToken(const ProcessingHostStorageToken&) = delete;
    ProcessingHostStorageToken& operator=(const ProcessingHostStorageToken&) = delete;
    ProcessingHostStorageToken(ProcessingHostStorageToken&&) noexcept;
    ProcessingHostStorageToken& operator=(ProcessingHostStorageToken&&) noexcept;
    ~ProcessingHostStorageToken();
    [[nodiscard]] static ProcessingHostStorageGrant
    reserve(const std::shared_ptr<GpuProcessingPhaseLease>& phase, std::uint64_t payloadBytes,
            std::uint64_t metadataBytes = 0,
            std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now()) noexcept;
    [[nodiscard]] std::uint64_t bytes() const noexcept { return bytes_; }
    [[nodiscard]] std::uint64_t metadataBytes() const noexcept { return metadataBytes_; }

  private:
    ProcessingHostStorageToken(std::shared_ptr<GpuProcessingPhaseLease>, std::uint64_t,
                               std::uint64_t) noexcept;
    void reset() noexcept;
    std::shared_ptr<detail::GpuProcessingPhaseBudgetState> state_;
    std::shared_ptr<GpuProcessingPhaseLease> origin_;
    std::uint64_t bytes_ = 0;
    std::uint64_t metadataBytes_ = 0;
};
struct ProcessingHostStorageGrant final {
    GpuProcessingPhaseAdmission status = GpuProcessingPhaseAdmission::InvalidRequest;
    GpuProcessingPhaseDomain domain = GpuProcessingPhaseDomain::None;
    std::optional<ProcessingHostStorageToken> token;
    std::uint64_t revisionAfterOwnRelease = 0;
};

// A vector's real allocator request is admitted before new, and released after delete. Null phase
// is explicitly legacy/unrooted; a rooted refusal never switches to untracked storage.
template <class T> class ProcessingHostAllocator {
    static_assert(alignof(T) <= alignof(std::max_align_t));
    struct alignas(std::max_align_t) Header {
        ProcessingHostStorageToken token;
    };

  public:
    using value_type = T;
    using propagate_on_container_move_assignment = std::true_type;
    template <class U> friend class ProcessingHostAllocator;
    [[nodiscard]] static constexpr std::size_t allocationHeaderBytes() noexcept {
        return sizeof(Header);
    }
    ProcessingHostAllocator() = default;
    explicit ProcessingHostAllocator(
        std::shared_ptr<GpuProcessingPhaseLease> phase,
        std::optional<ProcessingHostStorageRefusal>* failure = nullptr) noexcept
        : phase_(std::move(phase)), failure_(failure) {}
    template <class U>
    ProcessingHostAllocator(const ProcessingHostAllocator<U>& other) noexcept
        : phase_(other.phase_), failure_(other.failure_) {}
    [[nodiscard]] T* allocate(std::size_t count) {
        if (failure_)
            failure_->reset();
        if (count > (std::numeric_limits<std::size_t>::max() - sizeof(Header)) / sizeof(T))
            throw std::bad_array_new_length();
        const auto payload = count * sizeof(T);
        ProcessingHostStorageToken token;
        if (phase_) {
            auto grant = ProcessingHostStorageToken::reserve(phase_, payload, sizeof(Header));
            if (!grant.token) {
                if (failure_)
                    *failure_ = {grant.status, grant.domain, phase_->id(),
                                 grant.revisionAfterOwnRelease};
                throw std::bad_alloc();
            }
            token = std::move(*grant.token);
        }
        auto* block = static_cast<Header*>(::operator new(sizeof(Header) + payload));
        ::new (static_cast<void*>(block)) Header{std::move(token)};
        return reinterpret_cast<T*>(block + 1);
    }
    void deallocate(T* pointer, std::size_t) noexcept {
        auto* header = reinterpret_cast<Header*>(pointer) - 1;
        auto token = std::move(header->token);
        header->~Header();
        ::operator delete(header);
        // Token credit follows actual backing free, outside any native/ledger handoff lock.
    }
    template <class U> bool operator==(const ProcessingHostAllocator<U>& other) const noexcept {
        return phase_ == other.phase_ && failure_ == other.failure_;
    }

  private:
    std::shared_ptr<GpuProcessingPhaseLease> phase_;
    std::optional<ProcessingHostStorageRefusal>* failure_ = nullptr;
};
} // namespace bloom::render
