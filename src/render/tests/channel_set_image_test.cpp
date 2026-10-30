#include <bloom/core/pixel_aspect_ratio.hpp>
#include <bloom/render/channel_set_image.hpp>

#include <bit>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <optional>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using bloom::core::PixelAspectRatio;
using bloom::render::ChannelDescriptor;
using bloom::render::ChannelRole;
using bloom::render::ChannelSampleKind;
using bloom::render::ChannelSetImage;
using bloom::render::ChannelValueEncoding;
using bloom::render::describeChannel;
using bloom::render::ImageErrorCode;
using bloom::render::ImageWindow;

int failures = 0;

void expect(const bool condition, const std::string_view message) {
    if (!condition) {
        ++failures;
        std::cerr << "FAIL: " << message << '\n';
    }
}

[[nodiscard]] ImageWindow window(const std::int64_t x, const std::int64_t y,
                                 const std::uint64_t width, const std::uint64_t height) {
    auto result = ImageWindow::create(x, y, width, height);
    if (!result.hasValue())
        std::abort();
    return *result.value();
}

void roleInference() {
    const auto role = [](const std::string_view name,
                         const ChannelSampleKind kind = ChannelSampleKind::Half) {
        return describeChannel(name, kind).role;
    };
    expect(role("R") == ChannelRole::ColorR, "root R is color");
    expect(role("A") == ChannelRole::Alpha, "root A is alpha");
    expect(role("Y") == ChannelRole::Luminance, "root Y is luminance");
    expect(role("Z", ChannelSampleKind::Float) == ChannelRole::Depth, "root Z is depth");
    expect(role("diffuse.R") == ChannelRole::ColorR, "layer R is color");
    expect(role("diffuse.B") == ChannelRole::ColorB, "layer B is color");
    expect(role("depth.Z") == ChannelRole::Depth, "depth layer");
    expect(role("N.X") == ChannelRole::NormalX, "normal x");
    expect(role("normal.Z") == ChannelRole::NormalZ, "normal z");
    expect(role("P.Y") == ChannelRole::PositionY, "position y");
    expect(role("motion.u") == ChannelRole::MotionU, "motion u");
    expect(role("forward.V") == ChannelRole::MotionV, "motion v");
    expect(role("uv.s") == ChannelRole::UvU, "uv s");
    expect(role("CryptoObject00.r", ChannelSampleKind::Float) == ChannelRole::Id,
           "crypto rank id");
    expect(role("CryptoObject00.g", ChannelSampleKind::Float) == ChannelRole::Coverage,
           "crypto rank coverage");
    expect(role("CryptoMaterial01.b", ChannelSampleKind::Float) == ChannelRole::Id,
           "crypto second rank id");
    expect(role("objectId", ChannelSampleKind::Uint) == ChannelRole::Data, "plain data");
    expect(role("id.R", ChannelSampleKind::Uint) == ChannelRole::Data,
           "uint samples are never color");
    expect(role("diffuse.r") == ChannelRole::Data, "lower-case suffix is not color");

    const auto colour = describeChannel("beauty.light.G", ChannelSampleKind::Half);
    expect(colour.layer == "beauty.light", "nested layer name");
    expect(colour.encoding == ChannelValueEncoding::Colour, "color encoding");
    const auto depth = describeChannel("depth.Z", ChannelSampleKind::Float);
    expect(depth.encoding == ChannelValueEncoding::Data, "data encoding");
    expect(describeChannel("A", ChannelSampleKind::Half).encoding == ChannelValueEncoding::Data,
           "alpha is not color-managed");
}

void creation() {
    const auto data = window(-1, 2, 2, 1);
    const auto display = window(0, 0, 4, 4);
    std::vector<ChannelDescriptor> channels{describeChannel("depth.Z", ChannelSampleKind::Float),
                                            describeChannel("id", ChannelSampleKind::Uint)};
    const auto bits = std::bit_cast<float>(std::uint32_t{0x7fc01234U});
    std::vector<std::vector<float>> planes{{-5.0F, std::numeric_limits<float>::infinity()},
                                           {bits, 0.0F}};
    auto result = ChannelSetImage::create(data, display, PixelAspectRatio::square(), channels,
                                          planes, {{"name", "beauty"}}, 1U << 20U);
    expect(result.hasValue(), "valid channel set");
    if (const auto* image = result.value()) {
        expect(image->channels().size() == 2, "two channels");
        expect(image->find("id") == std::optional<std::size_t>{1}, "find by name");
        expect(!image->find("missing").has_value(), "missing channel");
        expect(image->plane(0)[0] == -5.0F, "negative data survives");
        expect(std::bit_cast<std::uint32_t>(image->plane(1)[0]) == 0x7fc01234U,
               "bit pattern survives");
        expect(image->plane(9).empty(), "out of range plane");
        expect(image->metadata().size() == 1, "metadata kept");
        expect(image->byteCost() >= 4 * sizeof(float), "byte cost counts planes");
        expect(image->dataWindow() == data && image->displayWindow() == display, "windows");
    }

    auto duplicate = ChannelSetImage::create(
        data, display, PixelAspectRatio::square(),
        {describeChannel("Z", ChannelSampleKind::Float),
         describeChannel("Z", ChannelSampleKind::Float)},
        {{0.0F, 0.0F}, {0.0F, 0.0F}}, {}, 1U << 20U);
    expect(!duplicate.hasValue() &&
               duplicate.error()->code == ImageErrorCode::InvalidParameter,
           "duplicate names rejected");
    auto shortPlane = ChannelSetImage::create(data, display, PixelAspectRatio::square(),
                                              {describeChannel("Z", ChannelSampleKind::Float)},
                                              {{0.0F}}, {}, 1U << 20U);
    expect(!shortPlane.hasValue() &&
               shortPlane.error()->code == ImageErrorCode::InvalidStorageSize,
           "plane size checked");
    auto empty = ChannelSetImage::create(data, display, PixelAspectRatio::square(), {}, {}, {},
                                         1U << 20U);
    expect(!empty.hasValue(), "empty channel set rejected");
    auto overBudget = ChannelSetImage::create(data, display, PixelAspectRatio::square(),
                                              {describeChannel("Z", ChannelSampleKind::Float)},
                                              {{0.0F, 0.0F}}, {}, 7U);
    expect(!overBudget.hasValue() &&
               overBudget.error()->code == ImageErrorCode::PixelStorageBudgetExceeded,
           "byte budget enforced");
}

} // namespace

int main() {
    roleInference();
    creation();
    if (failures != 0) {
        std::cerr << failures << " channel set expectation(s) failed\n";
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}
