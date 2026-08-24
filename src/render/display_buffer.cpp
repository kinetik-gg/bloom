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
    const ReferenceDisplayBufferDescriptor descriptor, std::vector<Rgba8> pixels) noexcept
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
        return ImageResult<PreparedReferenceDisplayBuffer>::success(
            PreparedReferenceDisplayBuffer(descriptor, std::move(storage)));
    } catch (const std::bad_alloc&) {
        return ImageResult<PreparedReferenceDisplayBuffer>::failure(
            ImageError::allocationFailure(requiredBytes));
    } catch (const std::length_error&) {
        return ImageResult<PreparedReferenceDisplayBuffer>::failure(
            ImageError::allocationFailure(requiredBytes));
    }
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
        ReferenceDisplayBufferView(*displayDescriptor, pixels_));
}

void PreparedReferenceDisplayBuffer::reset() noexcept {
    descriptor_.reset();
    pixels_.clear();
}

ReferenceDisplayBufferBuilder::ReferenceDisplayBufferBuilder(
    const ReferenceDisplayBufferDescriptor descriptor, std::vector<Rgba8> pixels) noexcept
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
        return ImageResult<ReferenceDisplayBufferBuilder>::success(
            ReferenceDisplayBufferBuilder(descriptor, std::move(pixels)));
    } catch (const std::bad_alloc&) {
        return ImageResult<ReferenceDisplayBufferBuilder>::failure(
            ImageError::allocationFailure(requiredBytes));
    } catch (const std::length_error&) {
        return ImageResult<ReferenceDisplayBufferBuilder>::failure(
            ImageError::allocationFailure(requiredBytes));
    }
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
        ReferenceDisplayBufferView(*displayDescriptor, pixels_));
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
        std::span<Rgba8>(pixels_).subspan(offset, displayWindow.extent().width()));
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
    pixels_.clear();
}

} // namespace bloom::render
