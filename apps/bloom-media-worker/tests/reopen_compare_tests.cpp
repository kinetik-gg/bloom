// Tolerance profile v2: the reopen comparison models the codec's own chroma sampling, yet still
// fails a real encoding error (corrupted luma, a wrong matrix or range).
#include "ffmpeg_reopen_compare.hpp"
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>
#include <vector>
extern "C" {
#include <libavutil/pixdesc.h>
}
using namespace bloom::media::ffmpeg;
namespace {
constexpr int kWidth = 640, kHeight = 360;
// The profile's acceptance bounds in unsigned 16-bit units (kVideoToleranceV2).
constexpr std::uint32_t kMaximum = 22938, kMean = 1967;
int failures = 0;
void check(bool ok, const std::string& what) {
    std::cout << (ok ? "PASS: " : "FAIL: ") << what << '\n';
    if (!ok)
        ++failures;
}
using Frame = std::unique_ptr<AVFrame, void (*)(AVFrame*)>;
Frame frame() {
    return {av_frame_alloc(), [](AVFrame* f) { av_frame_free(&f); }};
}
struct Rgb {
    double r, g, b;
};
// Black with a centred 10x10 square: the MCP repro (its left edge is an odd column).
std::vector<std::byte> square(Rgb colour) {
    std::vector<std::byte> bytes(static_cast<std::size_t>(kWidth) * kHeight * 8U);
    for (int y = 0; y < kHeight; ++y)
        for (int x = 0; x < kWidth; ++x) {
            const bool inside = x >= kWidth / 2 - 5 && x < kWidth / 2 + 5 && y >= kHeight / 2 - 5 &&
                                y < kHeight / 2 + 5;
            const double values[4]{inside ? colour.r : 0.0, inside ? colour.g : 0.0,
                                   inside ? colour.b : 0.0, 1.0};
            for (unsigned c = 0; c < 4; ++c) {
                const auto value = static_cast<unsigned>(values[c] * 65535.0 + 0.5);
                const auto offset =
                    (static_cast<std::size_t>(y) * kWidth + static_cast<std::size_t>(x)) * 8U +
                    c * 2U;
                bytes[offset] = static_cast<std::byte>(value & 255U);
                bytes[offset + 1] = static_cast<std::byte>(value >> 8U);
            }
        }
    return bytes;
}
// Full-frame vertical bars of saturated green and magenta, where BT.601 and BT.709 disagree most.
std::vector<std::byte> bars() {
    auto bytes = square({0, 0, 0});
    for (int y = 0; y < kHeight; ++y)
        for (int x = 0; x < kWidth; ++x) {
            const bool green = (x / 32) % 2 == 0;
            const unsigned values[3]{green ? 0U : 65535U, green ? 65535U : 0U, green ? 0U : 65535U};
            for (unsigned c = 0; c < 3; ++c) {
                const auto offset =
                    (static_cast<std::size_t>(y) * kWidth + static_cast<std::size_t>(x)) * 8U +
                    c * 2U;
                bytes[offset] = static_cast<std::byte>(values[c] & 255U);
                bytes[offset + 1] = static_cast<std::byte>(values[c] >> 8U);
            }
        }
    return bytes;
}
// A perfect codec: the reference written through `format` with the given sampling.
Frame encode(const std::vector<std::byte>& reference, AVPixelFormat format,
             const DeclaredSampling& sampling) {
    auto out = frame();
    out->format = format;
    out->width = kWidth;
    out->height = kHeight;
    std::unique_ptr<SwsContext, decltype(&sws_freeContext)> scale(
        declaredConversion(kWidth, kHeight, AV_PIX_FMT_RGBA64LE, format, sampling,
                           kOutputConversionFlags),
        sws_freeContext);
    const std::uint8_t* source[4]{reinterpret_cast<const std::uint8_t*>(reference.data()), nullptr,
                                  nullptr, nullptr};
    const int stride = kWidth * 8;
    if (!scale || av_frame_get_buffer(out.get(), 32) < 0 ||
        sws_scale(scale.get(), source, &stride, 0, kHeight, out->data, out->linesize) != kHeight) {
        std::cerr << "cannot build " << av_get_pix_fmt_name(format) << '\n';
        std::exit(1);
    }
    return out;
}
std::uint32_t mean(const ReopenMeasure& m) {
    return m.count == 0 ? 0U : static_cast<std::uint32_t>((m.sum + m.count - 1) / m.count);
}
bool accepted(const ReopenMeasure& m) {
    return m.failure.empty() && m.maximum <= kMaximum && mean(m) <= kMean;
}
// The v1 rule: decoded pixels against the unsampled reference.
std::uint32_t unsampledMaximum(const ReopenMeasure& m, const std::vector<std::byte>& reference) {
    std::uint32_t maximum = 0;
    for (std::size_t i = 0; i < reference.size(); i += 2) {
        if (i % 8 == 6)
            continue;
        const auto a =
            std::to_integer<unsigned>(m.rgba[i]) | (std::to_integer<unsigned>(m.rgba[i + 1]) << 8U);
        const auto b = std::to_integer<unsigned>(reference[i]) |
                       (std::to_integer<unsigned>(reference[i + 1]) << 8U);
        maximum = std::max(maximum, a > b ? a - b : b - a);
    }
    return maximum;
}
std::string describe(const ReopenMeasure& m) {
    return " (max=" + std::to_string(m.maximum) + " mean=" + std::to_string(mean(m)) +
           (m.failure.empty() ? "" : " failure=" + m.failure) + ")";
}
// Adds `delta` to the luma samples of [x0,x1)x[y0,y1), clamped to the sample's depth.
void shiftLuma(AVFrame& f, int x0, int x1, int y0, int y1, int delta) {
    const auto* layout = av_pix_fmt_desc_get(static_cast<AVPixelFormat>(f.format));
    const int peak = (1 << layout->comp[0].depth) - 1;
    for (int y = y0; y < y1; ++y)
        for (int x = x0; x < x1; ++x) {
            if (layout->comp[0].depth > 8) {
                auto* row = reinterpret_cast<std::uint16_t*>(f.data[0] + y * f.linesize[0]);
                row[x] = static_cast<std::uint16_t>(std::clamp(row[x] + delta, 0, peak));
            } else {
                auto* row = f.data[0] + y * f.linesize[0];
                row[x] = static_cast<std::uint8_t>(std::clamp(row[x] + delta, 0, peak));
            }
        }
}
} // namespace
// Issue #394: lossy codecs write footroom (Y' below 16) beside saturated chroma. swscale's Y'CbCr
// to RGBA64 path wrapped such samples (the VA-API H.264 red square reconstructed a near-black
// pixel, Y' 12 Cb 126 Cr 125, as G = 65535), so the verifier failed a correct encode. The
// reconstruction clips: exact BT.709 limited-range values for uniform 8-bit 4:2:0 frames.
void footroomClips() {
    const DeclaredSampling bt709{AVCOL_SPC_BT709, AVCOL_RANGE_MPEG, AVCHROMA_LOC_LEFT};
    struct Case {
        int y, cb, cr;
        unsigned r, g, b;
    };
    // R = (Y' - 16) / 219 + 1.5748 (Cr - 128) / 224, clipped; 12/90/240 is red with footroom luma.
    for (const auto& [y, cb, cr, r, g, b] :
         {Case{12, 90, 240, 50405, 0, 0}, Case{16, 128, 128, 0, 0, 0},
          Case{235, 128, 128, 65535, 65535, 65535}, Case{126, 128, 128, 32917, 32917, 32917},
          Case{4, 128, 128, 0, 0, 0}, Case{250, 128, 128, 65535, 65535, 65535}}) {
        auto f = frame();
        f->format = AV_PIX_FMT_YUV420P;
        f->width = 64;
        f->height = 16;
        if (av_frame_get_buffer(f.get(), 32) < 0)
            std::exit(1);
        for (int row = 0; row < 16; ++row)
            std::fill_n(f->data[0] + row * f->linesize[0], 64, static_cast<std::uint8_t>(y));
        for (int row = 0; row < 8; ++row) {
            std::fill_n(f->data[1] + row * f->linesize[1], 32, static_cast<std::uint8_t>(cb));
            std::fill_n(f->data[2] + row * f->linesize[2], 32, static_cast<std::uint8_t>(cr));
        }
        std::vector<std::byte> rgba;
        const bool ok =
            reconstructYuv(64, 16, AV_PIX_FMT_YUV420P, f->data, f->linesize, bt709, rgba);
        const auto at = [&](const std::size_t channel) {
            return ok ? std::to_integer<unsigned>(rgba[channel * 2]) |
                            (std::to_integer<unsigned>(rgba[channel * 2 + 1]) << 8U)
                      : 0U;
        };
        check(ok && at(0) == r && at(1) == g && at(2) == b && at(3) == 65535,
              "Y'CbCr " + std::to_string(y) + "/" + std::to_string(cb) + "/" + std::to_string(cr) +
                  " reconstructs clipped to " + std::to_string(r) + "/" + std::to_string(g) + "/" +
                  std::to_string(b) + " (got " + std::to_string(at(0)) + "/" +
                  std::to_string(at(1)) + "/" + std::to_string(at(2)) + ")");
    }
}
int main() {
    footroomClips();
    const DeclaredSampling bt709{AVCOL_SPC_BT709, AVCOL_RANGE_MPEG, AVCHROMA_LOC_LEFT};
    const auto red = square({1, 0, 0});
    // Every sample layout the lossy presets write or read back: ProRes/DNxHR 4:2:2 and 4:4:4,
    // DNxHD/DNxHR 8-bit 4:2:2, H.264/HEVC 4:2:0 (software and VA-API uploads), ProRes 4444.
    for (const auto format :
         {AV_PIX_FMT_YUV422P10LE, AV_PIX_FMT_YUV422P, AV_PIX_FMT_YUV420P, AV_PIX_FMT_YUV420P10LE,
          AV_PIX_FMT_NV12, AV_PIX_FMT_P010LE, AV_PIX_FMT_YUV444P10LE, AV_PIX_FMT_YUVA444P10LE,
          AV_PIX_FMT_YUVA444P12LE}) {
        const std::string name = av_get_pix_fmt_name(format);
        const auto* layout = av_pix_fmt_desc_get(format);
        const bool subsampled = layout->log2_chroma_w != 0 || layout->log2_chroma_h != 0;
        // Unspecified siting must mean what the encoder wrote (co-sited left).
        for (const auto siting : {AVCHROMA_LOC_LEFT, AVCHROMA_LOC_UNSPECIFIED}) {
            auto declared = bt709;
            declared.siting = siting;
            const auto decoded = encode(red, format, bt709);
            const auto m = measureReopen(*decoded, red, declared);
            check(m.failure.empty() && m.maximum == 0 && m.alphaMaximum <= 64,
                  name + (siting == AVCHROMA_LOC_LEFT ? " left" : " unspecified") +
                      " siting: a lossless codec of the tiny red square measures zero error" +
                      describe(m));
        }
        const auto m = measureReopen(*encode(red, format, bt709), red, bt709);
        const auto v1 = unsampledMaximum(m, red);
        // Evidence only: what the v1 rule (an unsampled reference) measured for the same frame.
        std::cout << "INFO: " << name << " v1 unsampled maximum=" << v1
                  << (subsampled && v1 > kMaximum ? " (over the bound: the MCP repro)" : "")
                  << '\n';
        if (!subsampled)
            check(v1 < 1024, name + ": 4:4:4 needs no sampling model (" + std::to_string(v1) + ")");
    }
    const auto format = AV_PIX_FMT_YUV422P10LE;
    {
        auto decoded = encode(red, format, bt709);
        shiftLuma(*decoded, 64, 80, 64, 80, 1023);
        const auto m = measureReopen(*decoded, red, bt709);
        check(!accepted(m), "a corrupted 16x16 luma block fails" + describe(m));
    }
    {
        auto decoded = encode(red, format, bt709);
        shiftLuma(*decoded, 0, kWidth, 0, kHeight, 48);
        const auto m = measureReopen(*decoded, red, bt709);
        check(!accepted(m), "a 5% luma drift over the frame fails" + describe(m));
    }
    {
        const auto reference = bars();
        auto declared = bt709;
        check(accepted(measureReopen(*encode(reference, format, bt709), reference, bt709)),
              "saturated bars written as declared pass");
        declared.matrix = AVCOL_SPC_SMPTE170M;
        const auto m = measureReopen(*encode(reference, format, declared), reference, bt709);
        check(!accepted(m), "BT.601 samples declared BT.709 fail" + describe(m));
        const auto tagged = measureReopen(*encode(reference, format, bt709), reference, declared);
        check(!accepted(tagged), "BT.709 samples declared BT.601 fail" + describe(tagged));
    }
    {
        auto full = bt709;
        full.range = AVCOL_RANGE_JPEG;
        const auto m = measureReopen(*encode(red, format, bt709), red, full);
        check(!accepted(m), "limited-range samples declared full range fail" + describe(m));
    }
    {
        auto centre = bt709;
        centre.siting = AVCHROMA_LOC_CENTER;
        const auto m = measureReopen(*encode(red, format, bt709), red, centre);
        check(m.failure.empty() && m.maximum > 0,
              "left-sited samples declared centre-sited are measured as error" + describe(m));
    }
    {
        auto rgb = bt709;
        rgb.matrix = AVCOL_SPC_RGB;
        const auto m = measureReopen(*encode(red, format, bt709), red, rgb);
        check(!m.failure.empty(), "a Y'CbCr stream declaring a non-Y'CbCr matrix is refused");
    }
    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
