#include <bloom/render/packed_host_retirement.hpp>
#include <bloom/render/packed_rgba8_storage.hpp>

#include <algorithm>
#include <limits>
#include <stdexcept>
#include <utility>

namespace bloom::render {
namespace {
struct AllocationHeader final : detail::PackedHostRetirementNode {
    PackedHostStorageToken token;
    std::shared_ptr<detail::PackedHostRetirementState> retirement;
};
static_assert(alignof(Rgba8) <= alignof(AllocationHeader));
static_assert(alignof(AllocationHeader) <= alignof(std::max_align_t));
void destroyAllocation(detail::PackedHostRetirementNode* const node) noexcept {
    auto* header = static_cast<AllocationHeader*>(node);
    auto token = std::move(header->token);
    header->~AllocationHeader();
    ::operator delete(header);
    // Token releases payload and metadata together only after backing is actually gone.
}
} // namespace

std::size_t detail::packedHostAllocationMetadataBytes() noexcept {
    return sizeof(AllocationHeader);
}

Rgba8* detail::allocatePackedRgba8(const std::shared_ptr<PackedHostStorageAuthority>& authority,
                                   const std::size_t count) {
    constexpr auto max = std::numeric_limits<std::size_t>::max();
    if (!authority || count == 0 || count > (max - sizeof(AllocationHeader)) / sizeof(Rgba8)) {
        throw PackedHostCapacityError{};
    }
    const auto bytes = count * sizeof(Rgba8);
    const auto retirement = authority->retirementState();
    if (!packedHostRetirementAvailable(retirement)) {
        throw PackedHostMissingRetirementError{};
    }
    if (!preparePackedHostRetirement(retirement)) {
        if (!packedHostRetirementAvailable(retirement)) {
            throw PackedHostMissingRetirementError{};
        }
        throw std::bad_alloc{};
    }
    auto token = authority->reserve(bytes, sizeof(AllocationHeader));
    if (!token) {
        throw PackedHostCapacityError{};
    }
    if (!registerPackedHostAllocation(retirement)) {
        throw PackedHostMissingRetirementError{};
    }
    try {
        // Payload AND the exact requested header/node are charged before backing allocation.
        // Opaque malloc/stdlib/thread overhead remains outside this explicit claim.
        void* block = ::operator new(sizeof(AllocationHeader) + bytes);
        auto* header = ::new (block) AllocationHeader;
        header->destroy = &destroyAllocation;
        header->token = std::move(*token);
        header->retirement = retirement;
        return reinterpret_cast<Rgba8*>(header + 1);
    } catch (...) {
        releasePackedHostRegistration(retirement);
        throw; // The provisional token atomically folds both debits back.
    }
}
void detail::deallocatePackedRgba8(Rgba8* const pixels) noexcept {
    if (!pixels) {
        return;
    }
    auto* header = reinterpret_cast<AllocationHeader*>(pixels) - 1;
    const auto retirement = header->retirement;
    if (header->token.published()) {
        // No allocation, pixel work, backing free or ledger release on the dropping thread.
        enqueuePackedHostRetirement(retirement, header);
    } else {
        // Provisional attempt-worker cleanup preserves the one own-phase R+1 watermark.
        destroyAllocation(header);
        releasePackedHostRegistration(retirement);
    }
}

PackedRgba8Storage::PackedRgba8Storage(OwnedVector&& pixels) noexcept
    : storage_(std::in_place_type<OwnedVector>, std::move(pixels)) {}
PackedRgba8Storage::PackedRgba8Storage(std::vector<Rgba8>&& pixels) noexcept
    : storage_(std::in_place_type<std::vector<Rgba8>>, std::move(pixels)) {}
PackedRgba8Storage::PackedRgba8Storage(PackedRgba8Storage&& other) noexcept
    : storage_(std::move(other.storage_)) {
    other.reset();
}
PackedRgba8Storage& PackedRgba8Storage::operator=(PackedRgba8Storage&& other) noexcept {
    if (this != &other) {
        // Destination backing must disappear before its token releases. Destroying the vector
        // invokes the allocator's exact ordering; then the source allocation is stolen unchanged.
        reset();
        storage_ = std::move(other.storage_);
        other.reset();
    }
    return *this;
}
PackedRgba8Storage PackedRgba8Storage::legacy(std::vector<Rgba8>&& pixels) noexcept {
    return PackedRgba8Storage(std::move(pixels));
}
ImageResult<PackedRgba8Storage>
PackedRgba8Storage::create(const std::size_t pixelCount,
                           const std::shared_ptr<GpuProcessingPhaseLease>& phase,
                           const std::size_t byteLimit) noexcept {
    constexpr auto max = std::numeric_limits<std::size_t>::max();
    if (pixelCount > max / sizeof(Rgba8)) {
        return ImageResult<PackedRgba8Storage>::failure(
            ImageError::pixelStorageBudgetExceeded(max, byteLimit));
    }
    const auto bytes = pixelCount * sizeof(Rgba8);
    if (!phase) {
        return ImageResult<PackedRgba8Storage>::failure(
            ImageError::codeOnly(ImageErrorCode::InvalidState));
    }
    if (bytes > byteLimit) {
        return ImageResult<PackedRgba8Storage>::failure(
            ImageError::pixelStorageBudgetExceeded(bytes, byteLimit));
    }
    try {
        auto authority = PackedHostStorageAuthority::create(phase, byteLimit);
        OwnedVector pixels(pixelCount, Rgba8{}, detail::PackedRgba8Allocator(std::move(authority)));
        return ImageResult<PackedRgba8Storage>::success(PackedRgba8Storage(std::move(pixels)));
    } catch (const detail::PackedHostCapacityError&) {
        return ImageResult<PackedRgba8Storage>::failure(
            ImageError::pixelStorageBudgetExceeded(bytes, byteLimit));
    } catch (const detail::PackedHostMissingRetirementError&) {
        return ImageResult<PackedRgba8Storage>::failure(
            ImageError::codeOnly(ImageErrorCode::InvalidState));
    } catch (const std::bad_alloc&) {
        return ImageResult<PackedRgba8Storage>::failure(ImageError::allocationFailure(bytes));
    } catch (const std::length_error&) {
        return ImageResult<PackedRgba8Storage>::failure(ImageError::allocationFailure(bytes));
    }
}
ImageResult<PackedRgba8Storage>
PackedRgba8Storage::copy(const std::span<const Rgba8> pixels,
                         const std::shared_ptr<GpuProcessingPhaseLease>& phase,
                         const std::size_t byteLimit) noexcept {
    auto result = create(pixels.size(), phase, byteLimit);
    if (result) {
        std::copy(pixels.begin(), pixels.end(), result.value()->mutablePixels().begin());
    }
    return result;
}
std::span<const Rgba8> PackedRgba8Storage::pixels() const noexcept {
    if (const auto* pixels = std::get_if<std::vector<Rgba8>>(&storage_)) {
        return *pixels;
    }
    if (const auto* pixels = std::get_if<OwnedVector>(&storage_)) {
        return *pixels;
    }
    return {};
}
std::span<Rgba8> PackedRgba8Storage::mutablePixels() noexcept {
    if (auto* pixels = std::get_if<std::vector<Rgba8>>(&storage_)) {
        return *pixels;
    }
    if (auto* pixels = std::get_if<OwnedVector>(&storage_)) {
        return *pixels;
    }
    return {};
}
std::size_t PackedRgba8Storage::size() const noexcept { return pixels().size(); }
std::size_t PackedRgba8Storage::capacity() const noexcept {
    if (const auto* pixels = std::get_if<std::vector<Rgba8>>(&storage_)) {
        return pixels->capacity();
    }
    if (const auto* pixels = std::get_if<OwnedVector>(&storage_)) {
        return pixels->capacity();
    }
    return 0;
}
std::size_t PackedRgba8Storage::byteCost() const noexcept { return capacity() * sizeof(Rgba8); }
PackedRgba8Ownership PackedRgba8Storage::ownership() const noexcept {
    const auto* pixels = std::get_if<OwnedVector>(&storage_);
    if (!pixels) {
        return PackedRgba8Ownership::LegacyUnowned;
    }
    return pixels->get_allocator().authority()->snapshot().published
               ? PackedRgba8Ownership::RootedPublished
               : PackedRgba8Ownership::RootedProvisional;
}
bool PackedRgba8Storage::promote() const noexcept {
    const auto* pixels = std::get_if<OwnedVector>(&storage_);
    return pixels && pixels->get_allocator().authority()->promote();
}
void PackedRgba8Storage::reset() noexcept {
    static_assert(std::is_nothrow_default_constructible_v<decltype(storage_)>);
    std::destroy_at(&storage_);
    std::construct_at(&storage_);
}
} // namespace bloom::render
