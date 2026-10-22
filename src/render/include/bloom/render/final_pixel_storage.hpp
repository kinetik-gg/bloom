#pragma once

#include <bloom/render/packed_rgba8_storage.hpp>

#include <algorithm>
#include <limits>
#include <stdexcept>

namespace bloom::render {
namespace detail {
template <class T> class FinalPixelAllocator {
  public:
    using value_type = T;
    using propagate_on_container_move_assignment = std::true_type;
    using propagate_on_container_swap = std::true_type;
    using is_always_equal = std::false_type;
    template <class U> struct rebind {
        using other = FinalPixelAllocator<U>;
    };
    FinalPixelAllocator() = default;
    explicit FinalPixelAllocator(std::shared_ptr<PackedHostStorageAuthority> authority) noexcept
        : authority_(std::move(authority)) {}
    template <class U>
    FinalPixelAllocator(const FinalPixelAllocator<U>& other) noexcept
        : authority_(other.authority()) {}
    [[nodiscard]] T* allocate(std::size_t count) {
        if constexpr (std::is_same_v<T, Rgba32f> || std::is_same_v<T, Rgba8>) {
            static_assert(alignof(T) <= alignof(std::max_align_t));
            return static_cast<T*>(allocateHostPixels(authority_, count, sizeof(T)));
        } else {
            return std::allocator<T>{}.allocate(count);
        }
    }
    void deallocate(T* block, std::size_t count) noexcept {
        if constexpr (std::is_same_v<T, Rgba32f> || std::is_same_v<T, Rgba8>)
            deallocateHostPixels(block);
        else
            std::allocator<T>{}.deallocate(block, count);
    }
    [[nodiscard]] const std::shared_ptr<PackedHostStorageAuthority>& authority() const noexcept {
        return authority_;
    }
    template <class U>
    [[nodiscard]] bool operator==(const FinalPixelAllocator<U>& other) const noexcept {
        return authority_ == other.authority();
    }

  private:
    std::shared_ptr<PackedHostStorageAuthority> authority_;
};
} // namespace detail

// Final output payload only. Moves retain the complete backing, exact domain and originating root.
// Legacy vectors remain unowned. Borrowed spans never hold a charge; rooted growth/copy is absent.
template <class T> class FinalPixelStorage final {
    static_assert(std::is_same_v<T, Rgba32f> || std::is_same_v<T, Rgba8>);
    using OwnedVector = std::vector<T, detail::FinalPixelAllocator<T>>;

    [[nodiscard]] static constexpr T clearPixel() noexcept {
        if constexpr (std::is_same_v<T, Rgba32f>)
            return Rgba32f::transparent();
        else
            return T{};
    }

  public:
    FinalPixelStorage() = default;
    FinalPixelStorage(const FinalPixelStorage&) = delete;
    FinalPixelStorage& operator=(const FinalPixelStorage&) = delete;
    FinalPixelStorage(FinalPixelStorage&& other) noexcept : storage_(std::move(other.storage_)) {
        other.reset();
    }
    FinalPixelStorage& operator=(FinalPixelStorage&& other) noexcept {
        if (this != &other) {
            reset();
            storage_ = std::move(other.storage_);
            other.reset();
        }
        return *this;
    }
    [[nodiscard]] static FinalPixelStorage legacy(std::vector<T>&& pixels) noexcept {
        return FinalPixelStorage(std::move(pixels));
    }
    [[nodiscard]] static ImageResult<FinalPixelStorage>
    create(std::size_t count, const std::shared_ptr<GpuProcessingPhaseLease>& phase,
           std::size_t byteLimit, HostPixelStorageDomain domain, T clear = clearPixel()) noexcept {
        constexpr auto max = std::numeric_limits<std::size_t>::max();
        if (!phase || domain == HostPixelStorageDomain::PackedPreview ||
            (std::is_same_v<T, Rgba8> && domain == HostPixelStorageDomain::FinalProcess))
            return ImageResult<FinalPixelStorage>::failure(
                ImageError::codeOnly(ImageErrorCode::InvalidState));
        if (count > max / sizeof(T) || count * sizeof(T) > byteLimit)
            return ImageResult<FinalPixelStorage>::failure(ImageError::pixelStorageBudgetExceeded(
                count > max / sizeof(T) ? max : count * sizeof(T), byteLimit));
        try {
            auto authority = PackedHostStorageAuthority::create(phase, byteLimit, domain);
            OwnedVector pixels(count, clear, detail::FinalPixelAllocator<T>(std::move(authority)));
            if (!pixels.empty() && pixels.size() == pixels.capacity())
                detail::markHostPixelsMaterialized(pixels.data());
            return ImageResult<FinalPixelStorage>::success(FinalPixelStorage(std::move(pixels)));
        } catch (const detail::PackedHostCapacityError&) {
            return ImageResult<FinalPixelStorage>::failure(
                ImageError::pixelStorageBudgetExceeded(count * sizeof(T), byteLimit));
        } catch (const detail::PackedHostMissingRetirementError&) {
            return ImageResult<FinalPixelStorage>::failure(
                ImageError::codeOnly(ImageErrorCode::InvalidState));
        } catch (const std::bad_alloc&) {
            return ImageResult<FinalPixelStorage>::failure(
                ImageError::allocationFailure(count * sizeof(T)));
        } catch (const std::length_error&) {
            return ImageResult<FinalPixelStorage>::failure(
                ImageError::allocationFailure(count * sizeof(T)));
        }
    }
    [[nodiscard]] std::span<const T> pixels() const noexcept {
        if (const auto* p = std::get_if<std::vector<T>>(&storage_))
            return *p;
        if (const auto* p = std::get_if<OwnedVector>(&storage_))
            return *p;
        return {};
    }
    [[nodiscard]] std::span<T> mutablePixels() noexcept {
        if (auto* p = std::get_if<std::vector<T>>(&storage_))
            return *p;
        if (auto* p = std::get_if<OwnedVector>(&storage_))
            return *p;
        return {};
    }
    [[nodiscard]] std::shared_ptr<PackedHostStorageAuthority> authority() const noexcept {
        if (const auto* p = std::get_if<OwnedVector>(&storage_))
            return p->get_allocator().authority();
        return {};
    }
    [[nodiscard]] std::size_t size() const noexcept { return pixels().size(); }
    [[nodiscard]] bool empty() const noexcept { return size() == 0; }
    [[nodiscard]] std::size_t capacity() const noexcept {
        if (const auto* p = std::get_if<std::vector<T>>(&storage_))
            return p->capacity();
        if (const auto* p = std::get_if<OwnedVector>(&storage_))
            return p->capacity();
        return 0;
    }
    [[nodiscard]] std::size_t byteCost() const noexcept { return capacity() * sizeof(T); }
    [[nodiscard]] const T* data() const noexcept { return pixels().data(); }
    [[nodiscard]] T* data() noexcept { return mutablePixels().data(); }
    [[nodiscard]] auto begin() const noexcept { return pixels().begin(); }
    [[nodiscard]] auto end() const noexcept { return pixels().end(); }
    [[nodiscard]] auto begin() noexcept { return mutablePixels().begin(); }
    [[nodiscard]] auto end() noexcept { return mutablePixels().end(); }
    [[nodiscard]] const T& operator[](std::size_t i) const noexcept { return pixels()[i]; }
    [[nodiscard]] T& operator[](std::size_t i) noexcept { return mutablePixels()[i]; }
    void reset() noexcept {
        std::destroy_at(&storage_);
        std::construct_at(&storage_);
    }

  private:
    explicit FinalPixelStorage(std::vector<T>&& pixels) noexcept
        : storage_(std::in_place_type<std::vector<T>>, std::move(pixels)) {}
    explicit FinalPixelStorage(OwnedVector&& pixels) noexcept
        : storage_(std::in_place_type<OwnedVector>, std::move(pixels)) {}
    std::variant<std::monostate, std::vector<T>, OwnedVector> storage_;
};
using FinalRgba32fStorage = FinalPixelStorage<Rgba32f>;
using FinalRgba8Storage = FinalPixelStorage<Rgba8>;
} // namespace bloom::render
