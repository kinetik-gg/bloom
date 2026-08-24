#include <bloom/render/image.hpp>

#include <algorithm>
#include <new>
#include <stdexcept>
#include <utility>

namespace {

using bloom::render::ImageError;
using bloom::render::ImageErrorCode;
using bloom::render::ImageResult;
using bloom::render::ImageWindow;

[[nodiscard]] ImageError invalidStateError() noexcept {
    return ImageError::codeOnly(ImageErrorCode::InvalidState);
}

[[nodiscard]] ImageError coordinateError() noexcept {
    return ImageError::codeOnly(ImageErrorCode::CoordinateOutOfBounds);
}

[[nodiscard]] std::size_t rowOffset(const ImageWindow dataWindow, const std::int64_t y) noexcept {
    const auto relativeY = static_cast<std::uint64_t>(y - dataWindow.originY());
    return static_cast<std::size_t>(relativeY) * dataWindow.extent().width();
}

[[nodiscard]] std::size_t pixelOffset(const ImageWindow dataWindow, const std::int64_t x,
                                      const std::int64_t y) noexcept {
    const auto relativeX = static_cast<std::uint64_t>(x - dataWindow.originX());
    return rowOffset(dataWindow, y) + static_cast<std::size_t>(relativeX);
}

} // namespace

namespace bloom::render {

ImageResult<Rgba32fImageDescriptor>
Rgba32fImageDescriptor::create(const ImageWindow dataWindow, const ImageWindow displayWindow,
                               const PixelAspectRatio pixelAspect) noexcept {
    const auto layoutResult = checkedRgba32fLayout(dataWindow.extent());
    if (!layoutResult) {
        return ImageResult<Rgba32fImageDescriptor>::failure(*layoutResult.error());
    }
    return ImageResult<Rgba32fImageDescriptor>::success(
        Rgba32fImageDescriptor(dataWindow, displayWindow, pixelAspect, *layoutResult.value()));
}

ImageResult<std::span<const Rgba32f>> Rgba32fImageView::row(const std::int64_t y) const noexcept {
    if (!descriptor_.has_value() || pixels_.size() != descriptor_->layout().pixelCount) {
        return ImageResult<std::span<const Rgba32f>>::failure(invalidStateError());
    }
    const auto dataWindow = descriptor_->dataWindow();
    if (y < dataWindow.originY() || y >= dataWindow.maxYExclusive()) {
        return ImageResult<std::span<const Rgba32f>>::failure(coordinateError());
    }
    const auto offset = rowOffset(dataWindow, y);
    return ImageResult<std::span<const Rgba32f>>::success(
        pixels_.subspan(offset, dataWindow.extent().width()));
}

ImageResult<Rgba32f> Rgba32fImageView::read(const std::int64_t x,
                                            const std::int64_t y) const noexcept {
    if (!descriptor_.has_value() || pixels_.size() != descriptor_->layout().pixelCount) {
        return ImageResult<Rgba32f>::failure(invalidStateError());
    }
    const auto dataWindow = descriptor_->dataWindow();
    if (!dataWindow.contains(x, y)) {
        return ImageResult<Rgba32f>::failure(coordinateError());
    }
    return ImageResult<Rgba32f>::success(pixels_[pixelOffset(dataWindow, x, y)]);
}

Rgba32fImage::Rgba32fImage(const Rgba32fImageDescriptor descriptor,
                           std::vector<Rgba32f> pixels) noexcept
    : descriptor_(descriptor), pixels_(std::move(pixels)) {}

Rgba32fImage::Rgba32fImage(Rgba32fImage&& other) noexcept
    : descriptor_(other.descriptor_), pixels_(std::move(other.pixels_)) {
    other.reset();
}

Rgba32fImage& Rgba32fImage::operator=(Rgba32fImage&& other) noexcept {
    if (this != &other) {
        descriptor_ = other.descriptor_;
        pixels_ = std::move(other.pixels_);
        other.reset();
    }
    return *this;
}

bool Rgba32fImage::isValid() const noexcept {
    return descriptor_.has_value() && pixels_.size() == descriptor_->layout().pixelCount;
}

const Rgba32fImageDescriptor* Rgba32fImage::descriptor() const& noexcept {
    return descriptor_ ? &*descriptor_ : nullptr;
}

ImageResult<Rgba32fImageView> Rgba32fImage::view() const& noexcept {
    const auto* imageDescriptor = descriptor();
    if (imageDescriptor == nullptr || pixels_.size() != imageDescriptor->layout().pixelCount) {
        return ImageResult<Rgba32fImageView>::failure(invalidStateError());
    }
    return ImageResult<Rgba32fImageView>::success(Rgba32fImageView(*imageDescriptor, pixels_));
}

ImageResult<Rgba32f> Rgba32fImage::read(const std::int64_t x, const std::int64_t y) const noexcept {
    const auto imageView = view();
    if (!imageView) {
        return ImageResult<Rgba32f>::failure(*imageView.error());
    }
    return imageView.value()->read(x, y);
}

void Rgba32fImage::reset() noexcept {
    descriptor_.reset();
    pixels_.clear();
}

Rgba32fImageBuilder::Rgba32fImageBuilder(const Rgba32fImageDescriptor descriptor,
                                         std::vector<Rgba32f> pixels) noexcept
    : descriptor_(descriptor), pixels_(std::move(pixels)) {}

Rgba32fImageBuilder::Rgba32fImageBuilder(Rgba32fImageBuilder&& other) noexcept
    : descriptor_(other.descriptor_), pixels_(std::move(other.pixels_)) {
    other.reset();
}

Rgba32fImageBuilder& Rgba32fImageBuilder::operator=(Rgba32fImageBuilder&& other) noexcept {
    if (this != &other) {
        descriptor_ = other.descriptor_;
        pixels_ = std::move(other.pixels_);
        other.reset();
    }
    return *this;
}

ImageResult<Rgba32fImageBuilder>
Rgba32fImageBuilder::create(const Rgba32fImageDescriptor descriptor,
                            const std::size_t pixelStorageByteLimit,
                            const Rgba32f clearValue) noexcept {
    const auto requiredBytes = descriptor.layout().pixelStorageBytes;
    if (requiredBytes > pixelStorageByteLimit) {
        return ImageResult<Rgba32fImageBuilder>::failure(
            ImageError::pixelStorageBudgetExceeded(requiredBytes, pixelStorageByteLimit));
    }

    try {
        std::vector<Rgba32f> pixels(descriptor.layout().pixelCount, clearValue);
        return ImageResult<Rgba32fImageBuilder>::success(
            Rgba32fImageBuilder(descriptor, std::move(pixels)));
    } catch (const std::bad_alloc&) {
        return ImageResult<Rgba32fImageBuilder>::failure(
            ImageError::allocationFailure(requiredBytes));
    } catch (const std::length_error&) {
        return ImageResult<Rgba32fImageBuilder>::failure(
            ImageError::allocationFailure(requiredBytes));
    }
}

bool Rgba32fImageBuilder::isValid() const noexcept {
    return descriptor_.has_value() && pixels_.size() == descriptor_->layout().pixelCount;
}

const Rgba32fImageDescriptor* Rgba32fImageBuilder::descriptor() const& noexcept {
    return descriptor_ ? &*descriptor_ : nullptr;
}

ImageResult<Rgba32fImageView> Rgba32fImageBuilder::view() const& noexcept {
    const auto* imageDescriptor = descriptor();
    if (imageDescriptor == nullptr || pixels_.size() != imageDescriptor->layout().pixelCount) {
        return ImageResult<Rgba32fImageView>::failure(invalidStateError());
    }
    return ImageResult<Rgba32fImageView>::success(Rgba32fImageView(*imageDescriptor, pixels_));
}

ImageResult<std::span<Rgba32f>> Rgba32fImageBuilder::row(const std::int64_t y) & noexcept {
    const auto* imageDescriptor = descriptor();
    if (imageDescriptor == nullptr || pixels_.size() != imageDescriptor->layout().pixelCount) {
        return ImageResult<std::span<Rgba32f>>::failure(invalidStateError());
    }
    const auto dataWindow = imageDescriptor->dataWindow();
    if (y < dataWindow.originY() || y >= dataWindow.maxYExclusive()) {
        return ImageResult<std::span<Rgba32f>>::failure(coordinateError());
    }
    const auto offset = rowOffset(dataWindow, y);
    return ImageResult<std::span<Rgba32f>>::success(
        std::span<Rgba32f>(pixels_).subspan(offset, dataWindow.extent().width()));
}

ImageResult<Rgba32f> Rgba32fImageBuilder::read(const std::int64_t x,
                                               const std::int64_t y) const noexcept {
    const auto imageView = view();
    if (!imageView) {
        return ImageResult<Rgba32f>::failure(*imageView.error());
    }
    return imageView.value()->read(x, y);
}

ImageStatus Rgba32fImageBuilder::write(const std::int64_t x, const std::int64_t y,
                                       const Rgba32f value) noexcept {
    const auto* imageDescriptor = descriptor();
    if (imageDescriptor == nullptr || pixels_.size() != imageDescriptor->layout().pixelCount) {
        return invalidStateError();
    }
    const auto dataWindow = imageDescriptor->dataWindow();
    if (!dataWindow.contains(x, y)) {
        return coordinateError();
    }
    pixels_[pixelOffset(dataWindow, x, y)] = value;
    return std::nullopt;
}

ImageStatus Rgba32fImageBuilder::clear(const Rgba32f value) noexcept {
    const auto* imageDescriptor = descriptor();
    if (imageDescriptor == nullptr || pixels_.size() != imageDescriptor->layout().pixelCount) {
        return invalidStateError();
    }
    std::ranges::fill(pixels_, value);
    return std::nullopt;
}

ImageResult<Rgba32fImage> Rgba32fImageBuilder::freeze() && noexcept {
    const auto* imageDescriptor = descriptor();
    if (imageDescriptor == nullptr || pixels_.size() != imageDescriptor->layout().pixelCount) {
        return ImageResult<Rgba32fImage>::failure(invalidStateError());
    }
    Rgba32fImage image(*imageDescriptor, std::move(pixels_));
    reset();
    return ImageResult<Rgba32fImage>::success(std::move(image));
}

void Rgba32fImageBuilder::reset() noexcept {
    descriptor_.reset();
    pixels_.clear();
}

} // namespace bloom::render
