#include <bloom/render/display_buffer.hpp>
#include <bloom/render/image.hpp>

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <limits>
#include <source_location>
#include <stdexcept>
#include <string_view>
#include <type_traits>
#include <utility>

namespace {

using bloom::render::AlphaAssociation;
using bloom::render::ColorEncoding;
using bloom::render::ImageErrorCode;
using bloom::render::ImageExtent;
using bloom::render::ImageResult;
using bloom::render::ImageWindow;
using bloom::render::PixelAspectRatio;
using bloom::render::PixelPacking;
using bloom::render::PreparedReferenceDisplayBuffer;
using bloom::render::ReferenceDisplayBufferDescriptor;
using bloom::render::Rgba32f;
using bloom::render::Rgba32fImage;
using bloom::render::Rgba32fImageBuilder;
using bloom::render::Rgba32fImageDescriptor;
using bloom::render::Rgba8;

class ExpectationContext final {
  public:
    void expect(const bool condition, const std::string_view message,
                const std::source_location location = std::source_location::current()) {
        if (condition) {
            return;
        }
        ++failures_;
        std::cerr << location.file_name() << ':' << location.line() << ": " << message << '\n';
    }

    [[nodiscard]] bool ok() const noexcept { return failures_ == 0; }

  private:
    std::size_t failures_ = 0;
};

template <typename T>
concept BorrowsPixelsFromRvalue = requires(T value) { std::move(value).pixels(); };

template <typename T>
concept BorrowsViewFromRvalue = requires(T value) { std::move(value).view(); };

template <typename T>
concept BorrowsDescriptorFromRvalue = requires(T value) { std::move(value).descriptor(); };

template <typename T>
concept BorrowsMutableRowFromRvalue = requires(T value) { std::move(value).row(0); };

static_assert(!BorrowsPixelsFromRvalue<Rgba32fImage>);
static_assert(!BorrowsViewFromRvalue<Rgba32fImage>);
static_assert(!BorrowsDescriptorFromRvalue<Rgba32fImage>);
static_assert(!BorrowsPixelsFromRvalue<Rgba32fImageBuilder>);
static_assert(!BorrowsViewFromRvalue<Rgba32fImageBuilder>);
static_assert(!BorrowsDescriptorFromRvalue<Rgba32fImageBuilder>);
static_assert(!BorrowsMutableRowFromRvalue<Rgba32fImageBuilder>);
static_assert(!BorrowsPixelsFromRvalue<PreparedReferenceDisplayBuffer>);
static_assert(!BorrowsViewFromRvalue<PreparedReferenceDisplayBuffer>);
static_assert(!BorrowsDescriptorFromRvalue<PreparedReferenceDisplayBuffer>);

template <typename T>
[[nodiscard]] bool hasError(const ImageResult<T>& result, const ImageErrorCode code) {
    return !result && result.error() != nullptr && result.error()->code == code;
}

[[nodiscard]] ImageWindow window(const std::int64_t originX, const std::int64_t originY,
                                 const std::uint64_t width, const std::uint64_t height) {
    const auto result = ImageWindow::create(originX, originY, width, height);
    if (!result) {
        throw std::logic_error("Invalid image window fixture");
    }
    return *result.value();
}

[[nodiscard]] PixelAspectRatio aspect(const std::uint64_t numerator,
                                      const std::uint64_t denominator) {
    const auto result = PixelAspectRatio::create(numerator, denominator);
    if (!result) {
        throw std::logic_error("Invalid pixel aspect fixture");
    }
    return *result.value();
}

[[nodiscard]] Rgba32f pixel(const float red, const float green, const float blue,
                            const float alpha) {
    const auto result = Rgba32f::fromPremultiplied(red, green, blue, alpha);
    if (!result) {
        throw std::logic_error("Invalid RGBA32F fixture");
    }
    return *result.value();
}

[[nodiscard]] Rgba32fImageDescriptor descriptor(const ImageWindow dataWindow,
                                                const ImageWindow displayWindow,
                                                const PixelAspectRatio pixelAspect) {
    const auto result = Rgba32fImageDescriptor::create(dataWindow, displayWindow, pixelAspect);
    if (!result) {
        throw std::logic_error("Invalid image descriptor fixture");
    }
    return *result.value();
}

void testGeometryAndTypedErrors(ExpectationContext& expectations) {
    constexpr auto beyondDimension =
        static_cast<std::uint64_t>(std::numeric_limits<std::uint32_t>::max()) + 1;
    expectations.expect(
        hasError(ImageExtent::create(0, 1), ImageErrorCode::InvalidExtent) &&
            hasError(ImageExtent::create(1, beyondDimension), ImageErrorCode::InvalidExtent),
        "invalid extents have a specific error category");
    expectations.expect(hasError(ImageWindow::create(0, 0, 0, 1), ImageErrorCode::InvalidWindow),
                        "invalid window dimensions have a specific error category");
    expectations.expect(
        hasError(ImageWindow::create(std::numeric_limits<std::int64_t>::max(), 0, 1, 1),
                 ImageErrorCode::ArithmeticOverflow),
        "window coordinate arithmetic is checked before construction");

    const auto minimumOrigin = ImageWindow::create(
        std::numeric_limits<std::int64_t>::min(), std::numeric_limits<std::int64_t>::min(),
        std::numeric_limits<std::uint32_t>::max(), std::numeric_limits<std::uint32_t>::max());
    expectations.expect(
        minimumOrigin &&
            minimumOrigin.value()->originX() == std::numeric_limits<std::int64_t>::min() &&
            minimumOrigin.value()->originY() == std::numeric_limits<std::int64_t>::min() &&
            minimumOrigin.value()->contains(std::numeric_limits<std::int64_t>::min(),
                                            std::numeric_limits<std::int64_t>::min()),
        "minimum signed origins remain valid for the largest supported extent");

    const auto highestNonOverflowingOrigin =
        ImageWindow::create(std::numeric_limits<std::int64_t>::max() - 1, 0, 1, 1);
    expectations.expect(highestNonOverflowingOrigin &&
                            highestNonOverflowingOrigin.value()->maxXExclusive() ==
                                std::numeric_limits<std::int64_t>::max() &&
                            highestNonOverflowingOrigin.value()->contains(
                                std::numeric_limits<std::int64_t>::max() - 1, 0),
                        "the exact highest non-overflowing window boundary remains representable");
    expectations.expect(
        hasError(PixelAspectRatio::create(0, 1), ImageErrorCode::InvalidPixelAspect),
        "zero pixel-aspect terms are rejected with a specific error");

    const auto reducedAspect = aspect(8, 6);
    expectations.expect(reducedAspect.numerator() == 4 && reducedAspect.denominator() == 3,
                        "non-square pixel aspect is stored as a normalized rational");

    const auto dataWindow = window(-10, -5, 2, 2);
    const auto displayWindow = window(100, 200, 1, 1);
    const auto imageDescriptor = descriptor(dataWindow, displayWindow, reducedAspect);
    expectations.expect(imageDescriptor.dataWindow() == dataWindow &&
                            imageDescriptor.displayWindow() == displayWindow &&
                            imageDescriptor.pixelAspect() == reducedAspect,
                        "data and display windows remain independent descriptor semantics");
    expectations.expect(imageDescriptor.layout().pixelCount == 4 &&
                            imageDescriptor.layout().rowStrideBytes == 2 * sizeof(Rgba32f) &&
                            imageDescriptor.layout().pixelStorageBytes == 4 * sizeof(Rgba32f),
                        "descriptor carries checked packed RGBA32F layout");
    expectations.expect(imageDescriptor.alphaAssociation() == AlphaAssociation::Premultiplied &&
                            imageDescriptor.pixelPacking() == PixelPacking::PackedRgba32f &&
                            imageDescriptor.colorEncoding() == ColorEncoding::ReferenceLinearSrgb &&
                            bloom::render::colorEncodingId(imageDescriptor.colorEncoding()) ==
                                bloom::render::kReferenceLinearSrgbEncodingId,
                        "descriptor fixes alpha, packing, and unambiguous reference encoding");

    constexpr auto maximum = std::numeric_limits<std::uint32_t>::max();
    const auto overflowingDescriptor = Rgba32fImageDescriptor::create(
        window(0, 0, maximum, maximum), window(0, 0, 1, 1), PixelAspectRatio::square());
    expectations.expect(hasError(overflowingDescriptor, ImageErrorCode::ArithmeticOverflow),
                        "pixel-storage arithmetic overflow is distinguishable from allocation");
}

void testPixelContract(ExpectationContext& expectations) {
    const auto infinity = std::numeric_limits<float>::infinity();
    const auto nan = std::numeric_limits<float>::quiet_NaN();
    expectations.expect(
        hasError(Rgba32f::fromPremultiplied(infinity, 0.0F, 0.0F, 1.0F),
                 ImageErrorCode::InvalidPixel) &&
            hasError(Rgba32f::fromPremultiplied(0.0F, nan, 0.0F, 1.0F),
                     ImageErrorCode::InvalidPixel) &&
            hasError(Rgba32f::fromPremultiplied(0.0F, 0.0F, 0.0F, -0.1F),
                     ImageErrorCode::InvalidPixel) &&
            hasError(Rgba32f::fromPremultiplied(0.0F, 0.0F, 0.0F, 1.1F),
                     ImageErrorCode::InvalidPixel),
        "non-finite components and alpha outside the closed unit interval are typed errors");

    const auto transparent = Rgba32f::fromPremultiplied(-4.0F, 8.0F, 2.0F, -0.0F);
    expectations.expect(transparent && *transparent.value() == Rgba32f::transparent() &&
                            !std::signbit(transparent.value()->red()) &&
                            !std::signbit(transparent.value()->alpha()),
                        "fully transparent pixels canonicalize every component to positive zero");

    const auto wideRange = pixel(-2.5F, 8.0F, 0.25F, 0.5F);
    expectations.expect(wideRange.red() == -2.5F && wideRange.green() == 8.0F &&
                            wideRange.blue() == 0.25F && wideRange.alpha() == 0.5F,
                        "finite negative and HDR RGB values are preserved exactly");
}

void testBudgetAndMutableConstruction(ExpectationContext& expectations) {
    const auto imageDescriptor =
        descriptor(window(-10, -5, 2, 2), window(0, 0, 1920, 1080), aspect(4, 3));
    const auto requiredBytes = imageDescriptor.layout().pixelStorageBytes;
    const auto belowBudget = Rgba32fImageBuilder::create(imageDescriptor, requiredBytes - 1);
    expectations.expect(hasError(belowBudget, ImageErrorCode::PixelStorageBudgetExceeded) &&
                            belowBudget.error()->requestedPixelStorageBytes == requiredBytes &&
                            belowBudget.error()->pixelStorageByteLimit == requiredBytes - 1,
                        "pixel-storage budget errors report requested and limiting byte counts");

    auto exactBudgetResult = Rgba32fImageBuilder::create(imageDescriptor, requiredBytes);
    if (!exactBudgetResult) {
        expectations.expect(false, "an exact pixel-storage byte budget succeeds");
        return;
    }
    auto builder = std::move(*exactBudgetResult.value());
    expectations.expect(builder.isValid() && builder.pixels().size() == 4,
                        "mutable construction owns the exact packed pixel storage");

    const auto wideRange = pixel(-2.5F, 8.0F, 0.25F, 0.5F);
    expectations.expect(!builder.write(-10, -5, wideRange).has_value() &&
                            builder.read(-10, -5).value() != nullptr &&
                            *builder.read(-10, -5).value() == wideRange,
                        "data-window origin participates in coordinate access");
    const auto firstRow = builder.row(-5);
    expectations.expect(firstRow && firstRow.value()->size() == 2 &&
                            firstRow.value()->data() == builder.pixels().data(),
                        "negative-origin rows map to the packed first row");
    const auto outOfBoundsWrite = builder.write(-8, -5, wideRange);
    expectations.expect(hasError(builder.row(-6), ImageErrorCode::CoordinateOutOfBounds) &&
                            outOfBoundsWrite.has_value() &&
                            outOfBoundsWrite->code == ImageErrorCode::CoordinateOutOfBounds,
                        "coordinates outside the data window are rejected without mutation");

    const auto clearValue = pixel(3.0F, -1.0F, 0.5F, 1.0F);
    expectations.expect(!builder.clear(clearValue).has_value(), "valid builders can be cleared");
    bool allCleared = true;
    for (const auto stored : builder.pixels()) {
        allCleared = allCleared && stored == clearValue;
    }
    expectations.expect(allCleared, "clear fills every packed pixel with one validated value");

    const auto builderView = builder.view();
    expectations.expect(builderView && builderView.value()->descriptor().has_value() &&
                            builderView.value()->descriptor()->dataWindow() ==
                                imageDescriptor.dataWindow() &&
                            builderView.value()->pixels().data() == builder.pixels().data(),
                        "const non-owning views retain descriptor semantics and storage identity");
}

void testFreezePublicationAndMoves(ExpectationContext& expectations) {
    const auto imageDescriptor =
        descriptor(window(-2, 3, 2, 1), window(0, 0, 8, 8), PixelAspectRatio::square());
    auto builderResult =
        Rgba32fImageBuilder::create(imageDescriptor, imageDescriptor.layout().pixelStorageBytes,
                                    pixel(2.0F, -1.0F, 0.5F, 1.0F));
    if (!builderResult) {
        expectations.expect(false, "builder fixture succeeds");
        return;
    }

    auto sourceBuilder = std::move(*builderResult.value());
    auto movedBuilder = std::move(sourceBuilder);
    // Bloom defines moved-from image objects as a queryable invalid state by contract.
    expectations.expect(
        !sourceBuilder.isValid() && // NOLINT(bugprone-use-after-move,clang-analyzer-cplusplus.Move)
            sourceBuilder.descriptor() == nullptr &&
            hasError(sourceBuilder.view(), ImageErrorCode::InvalidState) && movedBuilder.isValid(),
        "builder moves leave a coherent and safely queryable source");

    auto imageResult = std::move(movedBuilder).freeze();
    expectations.expect(
        !movedBuilder.isValid() && // NOLINT(bugprone-use-after-move,clang-analyzer-cplusplus.Move)
            movedBuilder.descriptor() == nullptr &&
            hasError(movedBuilder.view(), ImageErrorCode::InvalidState),
        "freeze consumes and invalidates the mutable builder");
    if (!imageResult) {
        expectations.expect(false, "freezing a valid builder publishes an immutable image");
        return;
    }
    auto sourceImage = std::move(*imageResult.value());
    static_assert(!std::is_copy_constructible_v<Rgba32fImage>);
    static_assert(
        std::is_same_v<decltype(std::as_const(sourceImage).pixels()), std::span<const Rgba32f>>);
    const auto publishedView = sourceImage.view();
    expectations.expect(sourceImage.isValid() && publishedView &&
                            publishedView.value()->read(-2, 3).value() != nullptr &&
                            publishedView.value()->read(-2, 3).value()->red() == 2.0F,
                        "published owners and their views expose immutable pixel access");

    auto movedImage = std::move(sourceImage);
    expectations.expect(
        !sourceImage.isValid() && // NOLINT(bugprone-use-after-move,clang-analyzer-cplusplus.Move)
            sourceImage.descriptor() == nullptr && sourceImage.pixels().empty() &&
            hasError(sourceImage.view(), ImageErrorCode::InvalidState) && movedImage.isValid() &&
            movedImage.read(-1, 3).value() != nullptr,
        "immutable image moves leave a coherent source and preserve the owner");
    const auto ownerBackedView = movedImage.view();
    expectations.expect(ownerBackedView &&
                            ownerBackedView.value()->pixels().data() == movedImage.pixels().data(),
                        "a non-owning view remains valid while its immutable owner lives");
}

void testPreparedReferenceDisplayBuffer(ExpectationContext& expectations) {
    const auto descriptorResult =
        ReferenceDisplayBufferDescriptor::create(window(4, -3, 2, 1), aspect(4, 3));
    if (!descriptorResult) {
        expectations.expect(false, "reference display descriptor fixture succeeds");
        return;
    }
    const auto displayDescriptor = *descriptorResult.value();
    expectations.expect(displayDescriptor.layout().pixelStorageBytes == 2 * sizeof(Rgba8) &&
                            displayDescriptor.pixelPacking() == PixelPacking::PackedRgba8 &&
                            displayDescriptor.alphaAssociation() == AlphaAssociation::Straight &&
                            displayDescriptor.referenceDisplayPipelineId() ==
                                "bloom.reference.linear-srgb-to-srgb" &&
                            !displayDescriptor.isOcioQualified(),
                        "display handoff is packed RGBA8 and explicitly reference-only, not OCIO");

    constexpr std::array pixels{Rgba8{255, 0, 4, 255}, Rgba8{1, 2, 3, 128}};
    const auto wrongSize = PreparedReferenceDisplayBuffer::create(
        displayDescriptor, std::span<const Rgba8>(pixels).first(1), pixels.size() * sizeof(Rgba8));
    expectations.expect(hasError(wrongSize, ImageErrorCode::InvalidStorageSize) &&
                            wrongSize.error()->actualPixelStorageBytes == sizeof(Rgba8) &&
                            wrongSize.error()->expectedPixelStorageBytes ==
                                pixels.size() * sizeof(Rgba8),
                        "display handoff rejects storage that does not match its descriptor");
    const auto underBudget = PreparedReferenceDisplayBuffer::create(
        displayDescriptor, pixels, pixels.size() * sizeof(Rgba8) - 1);
    expectations.expect(
        hasError(underBudget, ImageErrorCode::PixelStorageBudgetExceeded) &&
            underBudget.error()->requestedPixelStorageBytes == pixels.size() * sizeof(Rgba8) &&
            underBudget.error()->pixelStorageByteLimit == pixels.size() * sizeof(Rgba8) - 1,
        "display storage uses the same typed exact-byte budget contract");

    auto bufferResult = PreparedReferenceDisplayBuffer::create(displayDescriptor, pixels,
                                                               pixels.size() * sizeof(Rgba8));
    if (!bufferResult) {
        expectations.expect(false, "exact-budget display handoff succeeds");
        return;
    }
    auto sourceBuffer = std::move(*bufferResult.value());
    const auto view = sourceBuffer.view();
    expectations.expect(view && view.value()->descriptor().has_value() &&
                            view.value()->descriptor()->displayWindow().originX() == 4 &&
                            view.value()->pixels()[1] == pixels[1],
                        "display views preserve descriptor and packed immutable pixels");

    auto movedBuffer = std::move(sourceBuffer);
    expectations.expect(
        !sourceBuffer.isValid() && // NOLINT(bugprone-use-after-move,clang-analyzer-cplusplus.Move)
            sourceBuffer.descriptor() == nullptr && sourceBuffer.pixels().empty() &&
            hasError(sourceBuffer.view(), ImageErrorCode::InvalidState) && movedBuffer.isValid(),
        "prepared display-buffer moves leave a coherent source");
}

} // namespace

int main() {
    try {
        ExpectationContext expectations;
        testGeometryAndTypedErrors(expectations);
        testPixelContract(expectations);
        testBudgetAndMutableConstruction(expectations);
        testFreezePublicationAndMoves(expectations);
        testPreparedReferenceDisplayBuffer(expectations);
        return expectations.ok() ? 0 : 1;
    } catch (const std::exception& exception) {
        std::cerr << "Unexpected test exception: " << exception.what() << '\n';
        return 1;
    }
}
