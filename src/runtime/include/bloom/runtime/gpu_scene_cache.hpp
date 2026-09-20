#pragma once

// Bounded, owner-thread content cache for GPU-resident intermediate images.
//
// It stores already-computed semantic identities to already-computed resident images. It does NOT
// build keys: the caller (the future evaluator) hands it a semantic digest string, and this class
// never inspects document revisions, layout, layer IDs, operation indices, or node IDs. Two
// requests with the same digest hit even if those non-pixel facts changed.
//
// Ownership and identity: every entry is a shared_ptr<const render::GpuImage>. The cache is bound
// to exactly one GpuDevice ownership generation (the DeviceAllocatorState behind the GpuDevice) and
// rejects an image that is not bound to it, so a numeric device generation of 1 or an identical
// driver/device name can never let a stale cross-device image in. The cache holds native GPU
// ownership and must run only on the device owner thread; UI code must never receive these
// pointers.
//
// Pinning: find() returns a shared_ptr that keeps the image alive. An entry whose shared_ptr is
// held outside the cache (use_count > 1) is treated as pinned/in-flight and is NEVER evicted, so
// eviction never pretends to reclaim VRAM it cannot free. When only pinned entries remain and a new
// image does not fit, the insertion is refused and the cache is unchanged.
//
// Budget: `maxRetainedBytes` bounds the retained intermediate images only, charged by each image's
// actual VMA allocation size (allocator rounding included), not a requested extent. Per-operation
// allocation and transient peak budgets (GpuSolid/GpuComposite/GpuImageUpload/GpuResidentDisplay)
// are separate and are NOT charged here.
//
// No service, no thread, no framework: the evaluator owns the instance and calls it on the device
// owner thread.

#include <bloom/render/gpu_device.hpp>
#include <bloom/render/gpu_image.hpp>
#include <bloom/runtime/gpu_memory_budget.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

namespace bloom::runtime {

struct GpuSceneCacheBudgets final {
    // Capacity-aware retained-byte ceiling: sized from the operation-cache allocation so a large
    // converted source and its produced intermediates can be retained across a scrub when the
    // machine has room, while a small machine keeps the 512 MiB floor. The cache still evicts
    // unpinned entries and never invalidates a live pin.
    std::uint64_t maxRetainedBytes = gpuSceneCacheRetainedByteBudget();
};

enum class GpuSceneCacheDiagnosticCode : std::uint8_t {
    None,
    WrongThread,
    DeviceUnavailable,
    InvalidArgument,
};

struct GpuSceneCacheDiagnostic final {
    GpuSceneCacheDiagnosticCode code = GpuSceneCacheDiagnosticCode::None;
    std::string message;

    friend bool operator==(const GpuSceneCacheDiagnostic&,
                           const GpuSceneCacheDiagnostic&) = default;
};

enum class GpuSceneCacheInsertResult : std::uint8_t {
    Inserted,
    // The image does not fit the retained-byte budget even after evicting every unpinned entry, or
    // the image alone is larger than the whole budget. Nothing was inserted.
    RefusedOverBudget,
    // The key already holds a DIFFERENT image that an external shared_ptr still pins. Replacing it
    // would drop the charge for memory that is still allocated, so the replacement is refused and
    // the original entry (and its bytes) is preserved.
    RefusedPinnedReplacement,
    // The image is bound to a different device ownership generation.
    ForeignImage,
    // Null/invalid image, or an image whose actual allocation size could not be read.
    InvalidArgument,
    WrongThread,
    // The bookkeeping allocation failed; the entry was dropped and the cache still renders.
    RefusedBookkeeping,
};

struct GpuSceneCacheCounters final {
    std::uint64_t hits = 0;
    std::uint64_t misses = 0;
    std::uint64_t insertions = 0;
    std::uint64_t evictions = 0;
    // Insertions refused by budget, foreign image, invalid argument, or bookkeeping failure. Upload
    // counts are NOT here: the number of GPU uploads is external to the cache and belongs to the
    // evaluator that decides to call the upload operation.
    std::uint64_t refusals = 0;

    friend bool operator==(const GpuSceneCacheCounters&, const GpuSceneCacheCounters&) = default;
};

struct GpuSceneCacheCreateResult;

class GpuSceneCache final {
  public:
    GpuSceneCache(const GpuSceneCache&) = delete;
    GpuSceneCache& operator=(const GpuSceneCache&) = delete;
    GpuSceneCache(GpuSceneCache&& other) noexcept;
    GpuSceneCache& operator=(GpuSceneCache&& other) noexcept;
    ~GpuSceneCache();

    // Called on the device owner thread. Binds the cache to that GpuDevice's ownership generation;
    // the GpuDevice must outlive the cache.
    [[nodiscard]] static GpuSceneCacheCreateResult create(render::GpuDevice& device,
                                                          GpuSceneCacheBudgets budgets = {});

    [[nodiscard]] bool isValid() const noexcept;
    [[nodiscard]] bool isBoundTo(render::GpuDevice& device) const noexcept;

    // Owner-thread lookup. Returns the retained image on a hit (the returned shared_ptr is the pin
    // that protects it from eviction) and nullptr on a miss or from a foreign thread.
    [[nodiscard]] std::shared_ptr<const render::GpuImage>
    find(const std::string& semanticKey) noexcept;

    // Owner-thread insertion. Replaces an existing key. Charges image->allocationBytes(). If the
    // budget would be exceeded, unpinned LRU entries are evicted; if the image still does not fit,
    // the insertion is refused and no entry is added.
    [[nodiscard]] GpuSceneCacheInsertResult
    insert(std::string semanticKey, std::shared_ptr<const render::GpuImage> image) noexcept;

    [[nodiscard]] bool erase(const std::string& semanticKey) noexcept;
    void clear() noexcept;

    // Device loss/recreation: drop every entry and advance the invalidation epoch. Owner thread
    // only; a foreign thread is a no-op, so loss handling can never run Vulkan on the wrong thread.
    void invalidateDevice() noexcept;

    [[nodiscard]] std::uint64_t retainedBytes() const noexcept;
    [[nodiscard]] std::size_t entryCount() const noexcept;
    [[nodiscard]] GpuSceneCacheCounters counters() const noexcept;
    [[nodiscard]] std::uint64_t invalidationEpoch() const noexcept;

  private:
    struct Impl;
    explicit GpuSceneCache(std::unique_ptr<Impl> impl) noexcept;
    void releaseImpl() noexcept;

    std::unique_ptr<Impl> impl_;
};

struct GpuSceneCacheCreateResult final {
    std::unique_ptr<GpuSceneCache> cache;
    GpuSceneCacheDiagnostic diagnostic;

    [[nodiscard]] bool hasValue() const noexcept { return cache != nullptr; }
    explicit operator bool() const noexcept { return hasValue(); }
};

} // namespace bloom::runtime
