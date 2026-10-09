#pragma once

#include <bloom/render/gpu_processing_phase_budget.hpp>
#include <bloom/render/image_types.hpp>

#include <cstddef>
#include <memory>
#include <new>
#include <span>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace bloom::render {
namespace detail {
class PackedHostCapacityError final : public std::bad_alloc {};
class PackedHostMissingRetirementError final : public std::bad_alloc {};

// This allocator admits the actual standard-library request before allocating its payload.
// The private allocation header owns the token; no borrowed span or vector alias owns a charge.
[[nodiscard]] Rgba8* allocatePackedRgba8(const std::shared_ptr<PackedHostStorageAuthority>&,
                                         std::size_t count);
void deallocatePackedRgba8(Rgba8*) noexcept;
[[nodiscard]] std::size_t packedHostAllocationMetadataBytes() noexcept;

template <class T> class PackedStorageAllocator {
  public:
    using value_type = T;
    using propagate_on_container_move_assignment = std::true_type;
    using propagate_on_container_swap = std::true_type;
    using is_always_equal = std::false_type;
    template <class U> struct rebind {
        using other = PackedStorageAllocator<U>;
    };

    PackedStorageAllocator() = default;
    explicit PackedStorageAllocator(std::shared_ptr<PackedHostStorageAuthority> authority) noexcept
        : authority_(std::move(authority)) {}
    template <class U>
    PackedStorageAllocator(const PackedStorageAllocator<U>& other) noexcept
        : authority_(other.authority()) {}
    [[nodiscard]] T* allocate(const std::size_t count) {
        if constexpr (std::is_same_v<T, Rgba8>) {
            return allocatePackedRgba8(authority_, count);
        } else {
            // Standard-library proxy/node metadata is outside the packed-payload accounting
            // claim. Rebinding remains conforming on implementations such as MSVC's vector.
            return std::allocator<T>{}.allocate(count);
        }
    }
    void deallocate(T* const block, const std::size_t count) noexcept {
        if constexpr (std::is_same_v<T, Rgba8>) {
            deallocatePackedRgba8(block);
        } else {
            std::allocator<T>{}.deallocate(block, count);
        }
    }
    [[nodiscard]] const std::shared_ptr<PackedHostStorageAuthority>& authority() const noexcept {
        return authority_;
    }
    template <class U>
    [[nodiscard]] bool operator==(const PackedStorageAllocator<U>& other) const noexcept {
        return authority_ == other.authority();
    }

  private:
    std::shared_ptr<PackedHostStorageAuthority> authority_;
};
using PackedRgba8Allocator = PackedStorageAllocator<Rgba8>;
} // namespace detail

enum class PackedRgba8Ownership { LegacyUnowned, RootedProvisional, RootedPublished };

// Moves the complete backing allocation, including its token. Growth is deliberately unavailable:
// rooted construction makes one vector and every actual allocator request is admitted separately.
// The legacy arm preserves existing unrooted callers without implying physical ownership proof.
class PackedRgba8Storage final {
  public:
    PackedRgba8Storage() = default;
    PackedRgba8Storage(const PackedRgba8Storage&) = delete;
    PackedRgba8Storage& operator=(const PackedRgba8Storage&) = delete;
    PackedRgba8Storage(PackedRgba8Storage&&) noexcept;
    PackedRgba8Storage& operator=(PackedRgba8Storage&&) noexcept;
    ~PackedRgba8Storage() = default;

    [[nodiscard]] static PackedRgba8Storage legacy(std::vector<Rgba8>&&) noexcept;
    [[nodiscard]] static ImageResult<PackedRgba8Storage>
    create(std::size_t pixelCount, const std::shared_ptr<GpuProcessingPhaseLease>&,
           std::size_t simultaneousByteLimit) noexcept;
    [[nodiscard]] static ImageResult<PackedRgba8Storage>
    copy(std::span<const Rgba8>, const std::shared_ptr<GpuProcessingPhaseLease>&,
         std::size_t simultaneousByteLimit) noexcept;

    [[nodiscard]] std::span<const Rgba8> pixels() const noexcept;
    [[nodiscard]] std::span<Rgba8> mutablePixels() noexcept;
    [[nodiscard]] std::size_t size() const noexcept;
    [[nodiscard]] std::size_t capacity() const noexcept;
    [[nodiscard]] std::size_t byteCost() const noexcept;
    [[nodiscard]] PackedRgba8Ownership ownership() const noexcept;
    // Promote only at the complete successful terminal product boundary. This transfers no native
    // retirement authority. The locked handoff changes no total/revision; dropping the final
    // origin alias can then release the remaining phase/count reservation normally.
    [[nodiscard]] bool promote() const noexcept;
    void reset() noexcept;

  private:
    using OwnedVector = std::vector<Rgba8, detail::PackedRgba8Allocator>;
    explicit PackedRgba8Storage(std::vector<Rgba8>&&) noexcept;
    explicit PackedRgba8Storage(OwnedVector&&) noexcept;
    std::variant<std::monostate, std::vector<Rgba8>, OwnedVector> storage_;
};
} // namespace bloom::render
