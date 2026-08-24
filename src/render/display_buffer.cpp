#include <bloom/render/display_buffer.hpp>

#include <new>
#include <stdexcept>
#include <utility>

namespace bloom::render {

ImageResult<ReferenceDisplayBufferDescriptor>
ReferenceDisplayBufferDescriptor::create(const ImageWindow displayWindow,
                                         const PixelAspectRatio pixelAspect) noexcept {
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
        return ImageResult<ReferenceDisplayBufferView>::failure(
            ImageError::codeOnly(ImageErrorCode::InvalidState));
    }
    return ImageResult<ReferenceDisplayBufferView>::success(
        ReferenceDisplayBufferView(*displayDescriptor, pixels_));
}

void PreparedReferenceDisplayBuffer::reset() noexcept {
    descriptor_.reset();
    pixels_.clear();
}

} // namespace bloom::render
