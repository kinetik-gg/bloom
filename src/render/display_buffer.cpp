#include <bloom/render/display_buffer.hpp>

#include <new>
#include <stdexcept>
#include <utility>

namespace {

using bloom::render::ImageError;
using bloom::render::ImageErrorCode;
using bloom::render::ImageWindow;

[[nodiscard]] ImageError invalidStateError() noexcept {
    return ImageError::codeOnly(ImageErrorCode::InvalidState);
}

[[nodiscard]] ImageError coordinateError() noexcept {
    return ImageError::codeOnly(ImageErrorCode::CoordinateOutOfBounds);
}

[[nodiscard]] std::size_t rowOffset(const ImageWindow displayWindow,
                                    const std::int64_t y) noexcept {
    const auto relativeY = static_cast<std::uint64_t>(y - displayWindow.originY());
    return static_cast<std::size_t>(relativeY) * displayWindow.extent().width();
}

} // namespace

namespace bloom::render {

ImageResult<ReferenceDisplayBufferDescriptor>
ReferenceDisplayBufferDescriptor::create(const ImageWindow displayWindow,
                                         const core::PixelAspectRatio pixelAspect) noexcept {
    const auto layoutResult = checkedRgba8Layout(displayWindow.extent());
    if (!layoutResult) {
        return ImageResult<ReferenceDisplayBufferDescriptor>::failure(*layoutResult.error());
    }
    return ImageResult<ReferenceDisplayBufferDescriptor>::success(
        ReferenceDisplayBufferDescriptor(displayWindow, pixelAspect, *layoutResult.value()));
}

PreparedReferenceDisplayBuffer::PreparedReferenceDisplayBuffer(
    const ReferenceDisplayBufferDescriptor descriptor, PackedRgba8Storage pixels) noexcept
    : descriptor_(descriptor), pixels_(std::move(pixels)) {}

PreparedReferenceDisplayBuffer::PreparedReferenceDisplayBuffer(
    PreparedReferenceDisplayBuffer&& other) noexcept
    : descriptor_(other.descriptor_), pixels_(std::move(other.pixels_)) {
    other.reset();
}

PreparedReferenceDisplayBuffer&
PreparedReferenceDisplayBuffer::operator=(PreparedReferenceDisplayBuffer&& other) noexcept {
    if (this != &other) {
        descriptor_ = other.descriptor_;
        pixels_ = std::move(other.pixels_);
        other.reset();
    }
    return *this;
}

ImageResult<PreparedReferenceDisplayBuffer>
PreparedReferenceDisplayBuffer::create(const ReferenceDisplayBufferDescriptor descriptor,
                                       const std::span<const Rgba8> pixels,
                                       const std::size_t pixelStorageByteLimit) noexcept {
    const auto requiredBytes = descriptor.layout().pixelStorageBytes;
    if (pixels.size() != descriptor.layout().pixelCount) {
        return ImageResult<PreparedReferenceDisplayBuffer>::failure(
            ImageError::storageSizeMismatch(pixels.size_bytes(), requiredBytes));
    }
    if (requiredBytes > pixelStorageByteLimit) {
        return ImageResult<PreparedReferenceDisplayBuffer>::failure(
            ImageError::pixelStorageBudgetExceeded(requiredBytes, pixelStorageByteLimit));
    }

    try {
        std::vector<Rgba8> storage(pixels.begin(), pixels.end());
        return ImageResult<PreparedReferenceDisplayBuffer>::success(PreparedReferenceDisplayBuffer(
            descriptor, PackedRgba8Storage::legacy(std::move(storage))));
    } catch (const std::bad_alloc&) {
        return ImageResult<PreparedReferenceDisplayBuffer>::failure(
            ImageError::allocationFailure(requiredBytes));
    } catch (const std::length_error&) {
        return ImageResult<PreparedReferenceDisplayBuffer>::failure(
            ImageError::allocationFailure(requiredBytes));
    }
}

ImageResult<PreparedReferenceDisplayBuffer>
PreparedReferenceDisplayBuffer::create(const ReferenceDisplayBufferDescriptor descriptor,
                                       const std::span<const Rgba8> pixels,
                                       const std::shared_ptr<GpuProcessingPhaseLease>& phase,
                                       const std::size_t pixelStorageByteLimit) noexcept {
    if (pixels.size() != descriptor.layout().pixelCount) {
        return ImageResult<PreparedReferenceDisplayBuffer>::failure(ImageError::storageSizeMismatch(
            pixels.size_bytes(), descriptor.layout().pixelStorageBytes));
    }
    auto storage = PackedRgba8Storage::copy(pixels, phase, pixelStorageByteLimit);
    if (!storage) {
        return ImageResult<PreparedReferenceDisplayBuffer>::failure(*storage.error());
    }
    return adopt(descriptor, std::move(*storage.value()), pixelStorageByteLimit);
}

ImageResult<PreparedReferenceDisplayBuffer>
PreparedReferenceDisplayBuffer::adopt(const ReferenceDisplayBufferDescriptor descriptor,
                                      std::vector<Rgba8>&& pixels,
                                      const std::size_t pixelStorageByteLimit) noexcept {
    const auto requiredBytes = descriptor.layout().pixelStorageBytes;
    const auto ownedBytes = pixels.size() * sizeof(Rgba8);
    if (pixels.size() != descriptor.layout().pixelCount) {
        return ImageResult<PreparedReferenceDisplayBuffer>::failure(
            ImageError::storageSizeMismatch(ownedBytes, requiredBytes));
    }
    // Minimal zero-copy policy: the adopted vector must own exactly its pixel count. A vector with
    // spare capacity would retain unaccounted bytes for the frame's whole lifetime, so it is
    // rejected without moving the caller's buffer (which still owns its pixels).
    if (pixels.capacity() != pixels.size()) {
        return ImageResult<PreparedReferenceDisplayBuffer>::failure(
            ImageError::storageSizeMismatch(ownedBytes, requiredBytes));
    }
    if (requiredBytes > pixelStorageByteLimit) {
        return ImageResult<PreparedReferenceDisplayBuffer>::failure(
            ImageError::pixelStorageBudgetExceeded(requiredBytes, pixelStorageByteLimit));
    }
    // Every check has passed, so the payload is moved now and no allocation can fail: the private
    // constructor is noexcept. A rejected adoption above returned before this point, leaving the
    // caller's vector untouched.
    return ImageResult<PreparedReferenceDisplayBuffer>::success(
        PreparedReferenceDisplayBuffer(descriptor, PackedRgba8Storage::legacy(std::move(pixels))));
}

ImageResult<PreparedReferenceDisplayBuffer>
PreparedReferenceDisplayBuffer::adopt(const ReferenceDisplayBufferDescriptor descriptor,
                                      PackedRgba8Storage&& pixels,
                                      const std::size_t pixelStorageByteLimit) noexcept {
    const auto requiredBytes = descriptor.layout().pixelStorageBytes;
    if (pixels.size() != descriptor.layout().pixelCount ||
        (pixels.ownership() == PackedRgba8Ownership::LegacyUnowned &&
         pixels.capacity() != pixels.size())) {
        return ImageResult<PreparedReferenceDisplayBuffer>::failure(
            ImageError::storageSizeMismatch(pixels.byteCost(), requiredBytes));
    }
    if (pixels.byteCost() > pixelStorageByteLimit) {
        return ImageResult<PreparedReferenceDisplayBuffer>::failure(
            ImageError::pixelStorageBudgetExceeded(pixels.byteCost(), pixelStorageByteLimit));
    }
    return ImageResult<PreparedReferenceDisplayBuffer>::success(
        PreparedReferenceDisplayBuffer(descriptor, std::move(pixels)));
}

bool PreparedReferenceDisplayBuffer::isValid() const noexcept {
    return descriptor_.has_value() && pixels_.size() == descriptor_->layout().pixelCount;
}

const ReferenceDisplayBufferDescriptor*
PreparedReferenceDisplayBuffer::descriptor() const& noexcept {
    return descriptor_ ? &*descriptor_ : nullptr;
}

ImageResult<ReferenceDisplayBufferView> PreparedReferenceDisplayBuffer::view() const& noexcept {
    const auto* displayDescriptor = descriptor();
    if (displayDescriptor == nullptr || pixels_.size() != displayDescriptor->layout().pixelCount) {
        return ImageResult<ReferenceDisplayBufferView>::failure(invalidStateError());
    }
    return ImageResult<ReferenceDisplayBufferView>::success(
        ReferenceDisplayBufferView(*displayDescriptor, pixels_.pixels()));
}

void PreparedReferenceDisplayBuffer::reset() noexcept {
    descriptor_.reset();
    pixels_.reset();
}

ReferenceDisplayBufferBuilder::ReferenceDisplayBufferBuilder(
    const ReferenceDisplayBufferDescriptor descriptor, PackedRgba8Storage pixels) noexcept
    : descriptor_(descriptor), pixels_(std::move(pixels)) {}

ReferenceDisplayBufferBuilder::ReferenceDisplayBufferBuilder(
    ReferenceDisplayBufferBuilder&& other) noexcept
    : descriptor_(other.descriptor_), pixels_(std::move(other.pixels_)) {
    other.reset();
}

ReferenceDisplayBufferBuilder&
ReferenceDisplayBufferBuilder::operator=(ReferenceDisplayBufferBuilder&& other) noexcept {
    if (this != &other) {
        descriptor_ = other.descriptor_;
        pixels_ = std::move(other.pixels_);
        other.reset();
    }
    return *this;
}

ImageResult<ReferenceDisplayBufferBuilder>
ReferenceDisplayBufferBuilder::create(const ReferenceDisplayBufferDescriptor descriptor,
                                      const std::size_t pixelStorageByteLimit) noexcept {
    const auto requiredBytes = descriptor.layout().pixelStorageBytes;
    if (requiredBytes > pixelStorageByteLimit) {
        return ImageResult<ReferenceDisplayBufferBuilder>::failure(
            ImageError::pixelStorageBudgetExceeded(requiredBytes, pixelStorageByteLimit));
    }

    try {
        std::vector<Rgba8> pixels(descriptor.layout().pixelCount, Rgba8{0, 0, 0, 0});
        return ImageResult<ReferenceDisplayBufferBuilder>::success(ReferenceDisplayBufferBuilder(
            descriptor, PackedRgba8Storage::legacy(std::move(pixels))));
    } catch (const std::bad_alloc&) {
        return ImageResult<ReferenceDisplayBufferBuilder>::failure(
            ImageError::allocationFailure(requiredBytes));
    } catch (const std::length_error&) {
        return ImageResult<ReferenceDisplayBufferBuilder>::failure(
            ImageError::allocationFailure(requiredBytes));
    }
}

ImageResult<ReferenceDisplayBufferBuilder>
ReferenceDisplayBufferBuilder::create(const ReferenceDisplayBufferDescriptor descriptor,
                                      const std::shared_ptr<GpuProcessingPhaseLease>& phase,
                                      const std::size_t pixelStorageByteLimit) noexcept {
    auto pixels =
        PackedRgba8Storage::create(descriptor.layout().pixelCount, phase, pixelStorageByteLimit);
    if (!pixels) {
        return ImageResult<ReferenceDisplayBufferBuilder>::failure(*pixels.error());
    }
    return ImageResult<ReferenceDisplayBufferBuilder>::success(
        ReferenceDisplayBufferBuilder(descriptor, std::move(*pixels.value())));
}

bool ReferenceDisplayBufferBuilder::isValid() const noexcept {
    return descriptor_.has_value() && pixels_.size() == descriptor_->layout().pixelCount;
}

const ReferenceDisplayBufferDescriptor*
ReferenceDisplayBufferBuilder::descriptor() const& noexcept {
    return descriptor_ ? &*descriptor_ : nullptr;
}

ImageResult<ReferenceDisplayBufferView> ReferenceDisplayBufferBuilder::view() const& noexcept {
    const auto* displayDescriptor = descriptor();
    if (displayDescriptor == nullptr || pixels_.size() != displayDescriptor->layout().pixelCount) {
        return ImageResult<ReferenceDisplayBufferView>::failure(invalidStateError());
    }
    return ImageResult<ReferenceDisplayBufferView>::success(
        ReferenceDisplayBufferView(*displayDescriptor, pixels_.pixels()));
}

ImageResult<std::span<Rgba8>> ReferenceDisplayBufferBuilder::row(const std::int64_t y) & noexcept {
    const auto* displayDescriptor = descriptor();
    if (displayDescriptor == nullptr || pixels_.size() != displayDescriptor->layout().pixelCount) {
        return ImageResult<std::span<Rgba8>>::failure(invalidStateError());
    }
    const auto displayWindow = displayDescriptor->displayWindow();
    if (y < displayWindow.originY() || y >= displayWindow.maxYExclusive()) {
        return ImageResult<std::span<Rgba8>>::failure(coordinateError());
    }
    const auto offset = rowOffset(displayWindow, y);
    return ImageResult<std::span<Rgba8>>::success(
        pixels_.mutablePixels().subspan(offset, displayWindow.extent().width()));
}

ImageResult<PreparedReferenceDisplayBuffer> ReferenceDisplayBufferBuilder::freeze() && noexcept {
    const auto* displayDescriptor = descriptor();
    if (displayDescriptor == nullptr || pixels_.size() != displayDescriptor->layout().pixelCount) {
        return ImageResult<PreparedReferenceDisplayBuffer>::failure(invalidStateError());
    }
    PreparedReferenceDisplayBuffer buffer(*displayDescriptor, std::move(pixels_));
    reset();
    return ImageResult<PreparedReferenceDisplayBuffer>::success(std::move(buffer));
}

void ReferenceDisplayBufferBuilder::reset() noexcept {
    descriptor_.reset();
    pixels_.reset();
}

} // namespace bloom::render
