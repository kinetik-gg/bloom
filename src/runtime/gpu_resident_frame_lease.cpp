#include <bloom/runtime/gpu_resident_frame_lease.hpp>

#include <bloom/render/gpu_device.hpp>
#include <bloom/render/gpu_resident_display.hpp>

#include <atomic>
#include <cstdint>
#include <memory>
#include <new>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace bloom::runtime {

// Token shared state. Deliberately carries no native image, no device, and no Vulkan object: only
// immutable metadata plus one atomic validity flag. Actual ownership is distinguished by the
// registry epoch and instance, never by an image generation number.
struct GpuResidentFrameLeaseState final {
    std::uint64_t id = 0;
    std::uint64_t registryEpoch = 0;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::optional<render::ImageWindow> displayWindow;
    core::PixelAspectRatio pixelAspect = core::PixelAspectRatio::square();
    std::uint64_t allocationBytes = 0;
    std::atomic<bool> valid{false};
};

// Registry-internal ownership record. It is the only place (besides an owner-thread pin) that holds
// the native image strongly.
struct GpuResidentFrameLeaseEntry final {
    std::uint64_t id = 0;
    std::weak_ptr<GpuResidentFrameLeaseState> token;
    std::shared_ptr<const render::GpuDisplayImage> image;
    std::uint64_t bytes = 0;
    std::atomic<int> pins{0};
    bool released = false;
};

namespace {

[[nodiscard]] std::uint64_t allocateRegistryEpoch() noexcept {
    static std::atomic<std::uint64_t> counter{1};
    std::uint64_t epoch = counter.fetch_add(1, std::memory_order_relaxed);
    if (epoch == 0) {
        epoch = counter.fetch_add(1, std::memory_order_relaxed);
    }
    return epoch;
}

[[nodiscard]] GpuResidentFrameLeasePublishResult
publishRefusal(const GpuResidentFrameLeaseCode code, std::string message) noexcept {
    GpuResidentFrameLeasePublishResult result;
    result.diagnostic.code = code;
    result.diagnostic.message = std::move(message);
    return result;
}

[[nodiscard]] GpuResidentFramePinResult pinRefusal(const GpuResidentFrameLeaseCode code,
                                                   std::string message) noexcept {
    GpuResidentFramePinResult result;
    result.diagnostic.code = code;
    result.diagnostic.message = std::move(message);
    return result;
}

} // namespace

struct GpuResidentFrameLeaseRegistry::Impl final {
    void requestWake() noexcept {
        if (wakeGuard || !wake) {
            return;
        }
        wakeGuard = true;
        try {
            wake();
        } catch (...) { // NOLINT(bugprone-empty-catch)
            // A host wake callback must never terminate a lease-registry operation; the exception
            // is deliberately contained and the guard is cleared below.
        }
        wakeGuard = false;
    }

    render::GpuDevice* device = nullptr;
    std::thread::id owner;
    std::uint64_t epoch = 0;
    std::uint64_t nextId = 1;
    GpuResidentFrameLeaseBudgets budgets;
    std::vector<std::shared_ptr<GpuResidentFrameLeaseEntry>> entries;
    std::uint64_t charged = 0;
    bool shuttingDown = false;
    bool wakeGuard = false;
    std::function<void()> wake;
};

// --- Token ---------------------------------------------------------------------------------------

GpuResidentFrameLease::GpuResidentFrameLease() noexcept = default;
GpuResidentFrameLease::GpuResidentFrameLease(const GpuResidentFrameLease&) noexcept = default;
GpuResidentFrameLease&
GpuResidentFrameLease::operator=(const GpuResidentFrameLease&) noexcept = default;
GpuResidentFrameLease::GpuResidentFrameLease(GpuResidentFrameLease&&) noexcept = default;
GpuResidentFrameLease& GpuResidentFrameLease::operator=(GpuResidentFrameLease&&) noexcept = default;
GpuResidentFrameLease::~GpuResidentFrameLease() = default;

GpuResidentFrameLease::GpuResidentFrameLease(
    std::shared_ptr<const GpuResidentFrameLeaseState> state) noexcept
    : state_(std::move(state)) {}

bool GpuResidentFrameLease::isValid() const noexcept {
    return state_ != nullptr && state_->valid.load(std::memory_order_acquire);
}

std::uint64_t GpuResidentFrameLease::id() const noexcept {
    return state_ != nullptr ? state_->id : 0;
}
std::uint32_t GpuResidentFrameLease::width() const noexcept {
    return state_ != nullptr ? state_->width : 0;
}
std::uint32_t GpuResidentFrameLease::height() const noexcept {
    return state_ != nullptr ? state_->height : 0;
}
std::optional<render::ImageWindow> GpuResidentFrameLease::displayWindow() const noexcept {
    return state_ != nullptr ? state_->displayWindow : std::nullopt;
}
core::PixelAspectRatio GpuResidentFrameLease::pixelAspect() const noexcept {
    return state_ != nullptr ? state_->pixelAspect : core::PixelAspectRatio::square();
}
std::uint64_t GpuResidentFrameLease::allocationBytes() const noexcept {
    return state_ != nullptr ? state_->allocationBytes : 0;
}
std::uint64_t GpuResidentFrameLease::registryEpoch() const noexcept {
    return state_ != nullptr ? state_->registryEpoch : 0;
}

// --- Pin -----------------------------------------------------------------------------------------

GpuResidentFramePin::GpuResidentFramePin() noexcept = default;
GpuResidentFramePin::GpuResidentFramePin(GpuResidentFramePin&& other) noexcept
    : image_(std::move(other.image_)), entry_(std::move(other.entry_)) {}
GpuResidentFramePin& GpuResidentFramePin::operator=(GpuResidentFramePin&& other) noexcept {
    if (this != &other) {
        if (entry_ != nullptr) {
            entry_->pins.fetch_sub(1, std::memory_order_acq_rel);
        }
        image_ = std::move(other.image_);
        entry_ = std::move(other.entry_);
    }
    return *this;
}
GpuResidentFramePin::~GpuResidentFramePin() {
    if (entry_ != nullptr) {
        entry_->pins.fetch_sub(1, std::memory_order_acq_rel);
    }
}

bool GpuResidentFramePin::isValid() const noexcept { return image_ != nullptr; }
const render::GpuDisplayImage& GpuResidentFramePin::image() const noexcept { return *image_; }
std::uint64_t GpuResidentFramePin::leaseId() const noexcept {
    return entry_ != nullptr ? entry_->id : 0;
}

// --- Registry ------------------------------------------------------------------------------------

GpuResidentFrameLeaseRegistry::GpuResidentFrameLeaseRegistry(std::unique_ptr<Impl> impl) noexcept
    : impl_(std::move(impl)) {}

GpuResidentFrameLeaseRegistry::~GpuResidentFrameLeaseRegistry() {
    if (impl_ == nullptr) {
        return;
    }
    impl_->shuttingDown = true;
    // Every surviving token becomes invalid regardless of the destroying thread. Setting the atomic
    // state is thread-safe and never touches Vulkan.
    for (const auto& entry : impl_->entries) {
        if (auto state = entry->token.lock()) {
            state->valid.store(false, std::memory_order_release);
        }
    }
    // Owner-thread destroy releases the native images here. A wrong-thread destroy releases only
    // the registry's own references: GpuDisplayImage's owner-thread check then quarantines
    // (retains) the native implementation rather than calling Vulkan from the wrong thread. Pins
    // returned to the caller keep their own strong references either way.
    impl_->entries.clear();
    impl_->charged = 0;
}

std::unique_ptr<GpuResidentFrameLeaseRegistry>
GpuResidentFrameLeaseRegistry::create(render::GpuDevice& device,
                                      const GpuResidentFrameLeaseBudgets& budgets) {
    if (budgets.maxBytes == 0 || budgets.maxEntries == 0) {
        return nullptr;
    }
    // Bind only to a live, Ready device generation from its own owner thread. This is checked
    // BEFORE any allocation or native call: a foreign thread, a moved-from/unavailable/stub device,
    // a lost device, or a device still initializing yields no registry. GpuDevice reports both
    // facts without touching the driver.
    if (!device.isOwnerThread() || device.state() != render::GpuDeviceState::Ready) {
        return nullptr;
    }
    auto impl = std::make_unique<Impl>();
    impl->device = &device;
    impl->owner = std::this_thread::get_id();
    impl->epoch = allocateRegistryEpoch();
    impl->budgets = budgets;
    return std::unique_ptr<GpuResidentFrameLeaseRegistry>(
        new GpuResidentFrameLeaseRegistry(std::move(impl)));
}

bool GpuResidentFrameLeaseRegistry::isBoundTo(render::GpuDevice& device) const noexcept {
    return impl_ != nullptr && impl_->device == &device;
}

std::uint64_t GpuResidentFrameLeaseRegistry::epoch() const noexcept {
    return impl_ != nullptr ? impl_->epoch : 0;
}

GpuResidentFrameLeaseBudgets GpuResidentFrameLeaseRegistry::budgets() const noexcept {
    return impl_ != nullptr ? impl_->budgets : GpuResidentFrameLeaseBudgets{};
}

std::uint64_t GpuResidentFrameLeaseRegistry::chargedBytes() const noexcept {
    return impl_ != nullptr ? impl_->charged : 0;
}

std::size_t GpuResidentFrameLeaseRegistry::entryCount() const noexcept {
    return impl_ != nullptr ? impl_->entries.size() : 0;
}

void GpuResidentFrameLeaseRegistry::setWakeCallback(std::function<void()> callback) {
    if (impl_ == nullptr || std::this_thread::get_id() != impl_->owner) {
        return;
    }
    impl_->wake = std::move(callback);
}

GpuResidentFrameLeasePublishResult
GpuResidentFrameLeaseRegistry::publish(std::shared_ptr<const render::GpuDisplayImage> image) {
    if (impl_ == nullptr) {
        return publishRefusal(GpuResidentFrameLeaseCode::ShuttingDown,
                              "the lease registry is gone");
    }
    if (std::this_thread::get_id() != impl_->owner) {
        return publishRefusal(GpuResidentFrameLeaseCode::WrongThread,
                              "publish must run on the device owner thread");
    }
    if (impl_->shuttingDown) {
        return publishRefusal(GpuResidentFrameLeaseCode::ShuttingDown,
                              "the lease registry is shutting down");
    }
    // Reclaim dead metadata before applying the budget so a burst of dropped tokens does not force
    // a spurious refusal. This is the documented deferred collection point.
    collectExpired();
    if (image == nullptr || !image->isValid()) {
        return publishRefusal(GpuResidentFrameLeaseCode::DeviceUnavailable,
                              "the display image is null, invalid, or device-lost");
    }
    if (impl_->device == nullptr || !image->isBoundTo(*impl_->device)) {
        return publishRefusal(GpuResidentFrameLeaseCode::ForeignDevice,
                              "the display image is not bound to this registry's device");
    }
    const std::uint64_t bytes = image->allocationBytes();
    if (bytes == 0) {
        return publishRefusal(GpuResidentFrameLeaseCode::InvalidArgument,
                              "the display image reports no native allocation bytes");
    }
    if (impl_->entries.size() >= impl_->budgets.maxEntries) {
        return publishRefusal(GpuResidentFrameLeaseCode::TooManyLeases,
                              "the lease metadata cap is exhausted");
    }
    if (impl_->charged > impl_->budgets.maxBytes ||
        bytes > impl_->budgets.maxBytes - impl_->charged) {
        return publishRefusal(GpuResidentFrameLeaseCode::OverBudget,
                              "the publication would exceed the lease byte budget");
    }

    auto state = std::make_shared<GpuResidentFrameLeaseState>();
    state->id = impl_->nextId++;
    state->registryEpoch = impl_->epoch;
    state->width = image->width();
    state->height = image->height();
    state->displayWindow = image->displayWindow();
    state->pixelAspect = image->pixelAspect();
    state->allocationBytes = bytes;

    auto entry = std::make_shared<GpuResidentFrameLeaseEntry>();
    entry->id = state->id;
    entry->token = state;
    entry->image = std::move(image);
    entry->bytes = bytes;

    state->valid.store(true, std::memory_order_release);
    impl_->entries.push_back(entry);
    impl_->charged += bytes;
    impl_->requestWake();

    GpuResidentFrameLeasePublishResult result;
    result.lease = GpuResidentFrameLease(std::move(state));
    return result;
}

GpuResidentFramePinResult GpuResidentFrameLeaseRegistry::pin(const GpuResidentFrameLease& lease) {
    if (impl_ == nullptr) {
        return pinRefusal(GpuResidentFrameLeaseCode::ShuttingDown, "the lease registry is gone");
    }
    if (std::this_thread::get_id() != impl_->owner) {
        return pinRefusal(GpuResidentFrameLeaseCode::WrongThread,
                          "owner lookup must run on the device owner thread");
    }
    const auto& state = lease.state_;
    if (state == nullptr) {
        return pinRefusal(GpuResidentFrameLeaseCode::InvalidArgument, "the lease is empty");
    }
    if (state->registryEpoch != impl_->epoch) {
        return pinRefusal(GpuResidentFrameLeaseCode::ForeignRegistry,
                          "the lease belongs to a different registry");
    }
    if (!state->valid.load(std::memory_order_acquire)) {
        return pinRefusal(GpuResidentFrameLeaseCode::StaleLease, "the lease is no longer valid");
    }

    std::shared_ptr<GpuResidentFrameLeaseEntry> found;
    for (const auto& entry : impl_->entries) {
        if (entry->id == state->id) {
            found = entry;
            break;
        }
    }
    if (found == nullptr || found->released || found->image == nullptr) {
        return pinRefusal(GpuResidentFrameLeaseCode::StaleLease, "the lease entry was reclaimed");
    }
    if (!found->image->isValid()) {
        if (auto mutableState = found->token.lock()) {
            mutableState->valid.store(false, std::memory_order_release);
        }
        return pinRefusal(GpuResidentFrameLeaseCode::DeviceUnavailable,
                          "the leased display image was lost");
    }

    found->pins.fetch_add(1, std::memory_order_acq_rel);
    GpuResidentFramePinResult result;
    result.pin.image_ = found->image;
    result.pin.entry_ = std::move(found);
    return result;
}

void GpuResidentFrameLeaseRegistry::invalidateAll() noexcept {
    if (impl_ == nullptr || std::this_thread::get_id() != impl_->owner) {
        return;
    }
    for (const auto& entry : impl_->entries) {
        if (auto state = entry->token.lock()) {
            state->valid.store(false, std::memory_order_release);
        }
        if (entry->pins.load(std::memory_order_acquire) == 0 && !entry->released) {
            entry->image.reset();
            impl_->charged -= entry->bytes;
            entry->bytes = 0;
            entry->released = true;
        }
    }
    impl_->requestWake();
}

void GpuResidentFrameLeaseRegistry::collectExpired() noexcept {
    if (impl_ == nullptr || impl_->shuttingDown) {
        return;
    }
    if (std::this_thread::get_id() != impl_->owner) {
        return;
    }
    auto& entries = impl_->entries;
    for (auto it = entries.begin(); it != entries.end();) {
        auto& entry = **it;
        // Acquire ONE strong snapshot and derive both facts from it. A separate expired() then
        // lock() would race a concurrent UI drop of the last token: lock() could return null
        // between the two calls and be dereferenced. The snapshot keeps the token state alive
        // through the decision, and lock() is itself safe against a concurrent last release.
        const std::shared_ptr<GpuResidentFrameLeaseState> token = entry.token.lock();
        const bool tokenAlive = token != nullptr;
        const bool valid = tokenAlive && token->valid.load(std::memory_order_acquire);
        const bool pinned = entry.pins.load(std::memory_order_acquire) > 0;

        if (!tokenAlive && !pinned) {
            if (!entry.released) {
                impl_->charged -= entry.bytes;
            }
            it = entries.erase(it);
            continue;
        }
        if (!valid && !pinned && !entry.released) {
            entry.image.reset();
            impl_->charged -= entry.bytes;
            entry.bytes = 0;
            entry.released = true;
        }
        ++it;
    }
}

} // namespace bloom::runtime
