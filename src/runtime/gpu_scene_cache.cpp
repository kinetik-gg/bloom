#include <bloom/runtime/gpu_scene_cache.hpp>

#include <list>
#include <new>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <utility>

namespace bloom::runtime {
namespace {

[[nodiscard]] GpuSceneCacheDiagnostic makeDiagnostic(const GpuSceneCacheDiagnosticCode code,
                                                     std::string message) {
    return GpuSceneCacheDiagnostic{code, std::move(message)};
}

} // namespace

struct GpuSceneCache::Impl final {
    Impl() = default;
    Impl(const Impl&) = delete;
    Impl& operator=(const Impl&) = delete;

    [[nodiscard]] bool onOwnerThread() const noexcept {
        return device != nullptr && owner == std::this_thread::get_id();
    }

    struct Entry final {
        std::shared_ptr<const render::GpuImage> image;
        std::uint64_t bytes = 0;
        std::list<std::string>::iterator position;
    };

    void removeAt(const std::string_view key) {
        const auto found = entries.find(key);
        if (found == entries.end()) {
            return;
        }
        retainedBytes -= found->second.bytes;
        order.erase(found->second.position);
        entries.erase(found);
    }

    // The least-recently used entry that is not pinned by an external shared_ptr and is not the key
    // currently being replaced. Returns end() when every remaining entry is pinned.
    [[nodiscard]] std::list<std::string>::iterator findEvictable(const std::string_view skip) {
        for (auto candidate = order.end(); candidate != order.begin();) {
            --candidate;
            if (skip == *candidate) {
                continue;
            }
            const auto entry = entries.find(*candidate);
            if (entry != entries.end() && entry->second.image.use_count() == 1) {
                return candidate;
            }
        }
        return order.end();
    }

    render::GpuDevice* device = nullptr;
    std::thread::id owner;
    GpuSceneCacheBudgets budgets;

    // LRU order, most-recent at the front; each map key is a view into its order node's string.
    std::list<std::string> order;
    std::unordered_map<std::string_view, Entry> entries;

    std::uint64_t retainedBytes = 0;
    std::uint64_t invalidationEpoch = 1;
    GpuSceneCacheCounters counters;
};

GpuSceneCache::GpuSceneCache(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}

GpuSceneCache::GpuSceneCache(GpuSceneCache&& other) noexcept = default;

GpuSceneCache& GpuSceneCache::operator=(GpuSceneCache&& other) noexcept {
    if (this != &other) {
        releaseImpl();
        impl_ = std::move(other.impl_);
    }
    return *this;
}

GpuSceneCache::~GpuSceneCache() { releaseImpl(); }

void GpuSceneCache::releaseImpl() noexcept {
    // The cache holds no Vulkan handle. Dropping its shared_ptr<GpuImage> entries is safe from any
    // thread because GpuImage's release path never calls Vulkan on a foreign thread and retains an
    // unretired generation instead; there is no owner-thread drain to fail.
    impl_.reset();
}

GpuSceneCacheCreateResult GpuSceneCache::create(render::GpuDevice& device,
                                                const GpuSceneCacheBudgets budgets) {
    if (budgets.maxRetainedBytes == 0) {
        return {nullptr, makeDiagnostic(GpuSceneCacheDiagnosticCode::InvalidArgument,
                                        "the scene cache budget is out of range")};
    }
    if (device.state() != render::GpuDeviceState::Ready) {
        return {nullptr, makeDiagnostic(GpuSceneCacheDiagnosticCode::DeviceUnavailable,
                                        "the GPU device is not Ready")};
    }
    // state() has no owner guard, so a foreign thread could otherwise create a cache and bind it to
    // itself. Fail closed before any bookkeeping is created.
    if (!device.isOwnerThread()) {
        return {nullptr, makeDiagnostic(GpuSceneCacheDiagnosticCode::WrongThread,
                                        "the scene cache must be created on the device owner "
                                        "thread")};
    }
    auto impl = std::make_unique<Impl>();
    impl->device = &device;
    impl->owner = std::this_thread::get_id();
    impl->budgets = budgets;
    return {std::unique_ptr<GpuSceneCache>(new GpuSceneCache(std::move(impl))),
            GpuSceneCacheDiagnostic{}};
}

bool GpuSceneCache::isValid() const noexcept { return impl_ != nullptr; }

bool GpuSceneCache::isBoundTo(render::GpuDevice& device) const noexcept {
    return impl_ != nullptr && impl_->onOwnerThread() && impl_->device == &device;
}

std::shared_ptr<const render::GpuImage>
GpuSceneCache::find(const std::string& semanticKey) noexcept {
    if (impl_ == nullptr || !impl_->onOwnerThread()) {
        return nullptr;
    }
    Impl& impl = *impl_;
    const auto found = impl.entries.find(semanticKey);
    if (found == impl.entries.end()) {
        ++impl.counters.misses;
        return nullptr;
    }
    impl.order.splice(impl.order.begin(), impl.order, found->second.position);
    ++impl.counters.hits;
    return found->second.image;
}

GpuSceneCacheInsertResult
GpuSceneCache::insert(std::string semanticKey,
                      std::shared_ptr<const render::GpuImage> image) noexcept {
    if (impl_ == nullptr || !impl_->onOwnerThread()) {
        return GpuSceneCacheInsertResult::WrongThread;
    }
    Impl& impl = *impl_;
    if (image == nullptr || !image->isValid()) {
        ++impl.counters.refusals;
        return GpuSceneCacheInsertResult::InvalidArgument;
    }
    // Actual device ownership, not a name or the numeric generation. A foreign device's image fails
    // here even when it has the same extent, driver, and generation number.
    if (!image->isBoundTo(*impl.device)) {
        ++impl.counters.refusals;
        return GpuSceneCacheInsertResult::ForeignImage;
    }
    const std::uint64_t bytes = image->allocationBytes();
    if (bytes == 0) {
        ++impl.counters.refusals;
        return GpuSceneCacheInsertResult::InvalidArgument;
    }
    if (bytes > impl.budgets.maxRetainedBytes) {
        ++impl.counters.refusals;
        return GpuSceneCacheInsertResult::RefusedOverBudget;
    }

    const std::string_view key{semanticKey};
    const auto existing = impl.entries.find(key);
    if (existing != impl.entries.end() && existing->second.image == image) {
        // Same semantic key and the same resident image is a hit/touch, never a replacement.
        impl.order.splice(impl.order.begin(), impl.order, existing->second.position);
        return GpuSceneCacheInsertResult::Inserted;
    }
    // Replacing a key with a DIFFERENT image is only safe when the cache alone owns the old image,
    // because only then does dropping the cache's reference actually free its VRAM. A pinned old
    // image stays alive for its external holder, so replacing it would silently drop the charge for
    // memory that is still allocated. Refuse and preserve the original entry.
    if (existing != impl.entries.end() && existing->second.image.use_count() > 1) {
        ++impl.counters.refusals;
        return GpuSceneCacheInsertResult::RefusedPinnedReplacement;
    }

    // Subtraction-only comparators: never `a + b > max`, which wraps for very large values.
    const std::uint64_t maxBytes = impl.budgets.maxRetainedBytes;
    const auto wouldExceed = [](const std::uint64_t retained, const std::uint64_t add,
                                const std::uint64_t max) noexcept {
        return retained > max || add > max - retained;
    };

    // Phase 1: prove eviction can make room, without evicting anything, so a refusal is clean.
    const std::uint64_t replacedBytes = existing != impl.entries.end() ? existing->second.bytes : 0;
    const std::uint64_t currentWithoutReplaced =
        impl.retainedBytes >= replacedBytes ? impl.retainedBytes - replacedBytes : 0;
    if (wouldExceed(currentWithoutReplaced, bytes, maxBytes)) {
        const std::uint64_t need = bytes - (maxBytes - currentWithoutReplaced);
        std::uint64_t reclaimable = 0;
        for (auto candidate = impl.order.rbegin(); candidate != impl.order.rend(); ++candidate) {
            if (key == *candidate) {
                continue;
            }
            const auto entry = impl.entries.find(*candidate);
            if (entry == impl.entries.end() || entry->second.image.use_count() > 1) {
                continue;
            }
            const std::uint64_t freed = entry->second.bytes;
            if (freed >= need - reclaimable) {
                reclaimable = need;
                break;
            }
            reclaimable += freed;
        }
        if (reclaimable < need) {
            ++impl.counters.refusals;
            return GpuSceneCacheInsertResult::RefusedOverBudget;
        }
    }

    // Phase 2: commit. Drop the replaced entry (proved unpinned above), then evict unpinned LRU
    // entries until it fits. `wouldExceed` is subtraction-only, so the loop cannot wrap.
    if (existing != impl.entries.end()) {
        impl.removeAt(key);
    }
    while (wouldExceed(impl.retainedBytes, bytes, maxBytes)) {
        const auto victim = impl.findEvictable(key);
        if (victim == impl.order.end()) {
            // Phase 1 proved this cannot happen; fail closed rather than corrupt accounting.
            ++impl.counters.refusals;
            return GpuSceneCacheInsertResult::RefusedOverBudget;
        }
        impl.removeAt(*victim);
        ++impl.counters.evictions;
    }

    bool pushed = false;
    try {
        impl.order.push_front(std::move(semanticKey));
        pushed = true;
        const auto position = impl.order.begin();
        const auto placed = impl.entries.emplace(std::string_view{*position},
                                                 Impl::Entry{std::move(image), bytes, position});
        if (!placed.second) {
            impl.order.pop_front();
            ++impl.counters.refusals;
            return GpuSceneCacheInsertResult::RefusedBookkeeping;
        }
    } catch (const std::bad_alloc&) {
        if (pushed) {
            impl.order.pop_front();
        }
        ++impl.counters.refusals;
        return GpuSceneCacheInsertResult::RefusedBookkeeping;
    }
    impl.retainedBytes += bytes;
    ++impl.counters.insertions;
    return GpuSceneCacheInsertResult::Inserted;
}

bool GpuSceneCache::erase(const std::string& semanticKey) noexcept {
    if (impl_ == nullptr || !impl_->onOwnerThread()) {
        return false;
    }
    const bool present = impl_->entries.find(semanticKey) != impl_->entries.end();
    if (present) {
        impl_->removeAt(semanticKey);
    }
    return present;
}

void GpuSceneCache::clear() noexcept {
    if (impl_ == nullptr || !impl_->onOwnerThread()) {
        return;
    }
    impl_->entries.clear();
    impl_->order.clear();
    impl_->retainedBytes = 0;
}

void GpuSceneCache::invalidateDevice() noexcept {
    if (impl_ == nullptr || !impl_->onOwnerThread()) {
        return;
    }
    impl_->entries.clear();
    impl_->order.clear();
    impl_->retainedBytes = 0;
    ++impl_->invalidationEpoch;
}

std::uint64_t GpuSceneCache::retainedBytes() const noexcept {
    return impl_ != nullptr ? impl_->retainedBytes : 0;
}

std::size_t GpuSceneCache::entryCount() const noexcept {
    return impl_ != nullptr ? impl_->entries.size() : 0;
}

GpuSceneCacheCounters GpuSceneCache::counters() const noexcept {
    return impl_ != nullptr ? impl_->counters : GpuSceneCacheCounters{};
}

std::uint64_t GpuSceneCache::invalidationEpoch() const noexcept {
    return impl_ != nullptr ? impl_->invalidationEpoch : 0;
}

} // namespace bloom::runtime
