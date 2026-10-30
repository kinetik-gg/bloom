#include "ffmpeg_reopen_compare.hpp"
#include <algorithm>
#include <memory>
extern "C" {
#include <libavutil/pixdesc.h>
}
namespace bloom::media::ffmpeg {
namespace {
using Scale = std::unique_ptr<SwsContext, decltype(&sws_freeContext)>;
bool rgb(AVPixelFormat format) {
    const auto* layout = av_pix_fmt_desc_get(format);
    return layout != nullptr && (layout->flags & AV_PIX_FMT_FLAG_RGB) != 0;
}
// swscale chroma positions are in 1/256 luma sample; an axis without subsampling keeps the
// default, because an explicit position there would shift full-resolution chroma.
void sitePlanes(AVPixelFormat format, AVChromaLocation siting, int& horizontal, int& vertical) {
    const auto* layout = av_pix_fmt_desc_get(format);
    if (layout == nullptr || rgb(format))
        return;
    int x = 0, y = 0;
    if (av_chroma_location_enum_to_pos(&x, &y, siting) < 0)
        return;
    if (layout->log2_chroma_w != 0)
        horizontal = x;
    if (layout->log2_chroma_h != 0)
        vertical = y;
}
unsigned sample(std::span<const std::byte> bytes, std::size_t offset) {
    return std::to_integer<unsigned>(bytes[offset]) |
           (std::to_integer<unsigned>(bytes[offset + 1]) << 8U);
}
} // namespace
AVChromaLocation effectiveSiting(const AVChromaLocation siting) noexcept {
    return siting == AVCHROMA_LOC_UNSPECIFIED ? AVCHROMA_LOC_LEFT : siting;
}
int swsCoefficients(const AVColorSpace matrix) noexcept {
    switch (matrix) {
    case AVCOL_SPC_BT709:
    case AVCOL_SPC_UNSPECIFIED:
        return SWS_CS_ITU709;
    case AVCOL_SPC_BT470BG:
    case AVCOL_SPC_SMPTE170M:
        return SWS_CS_ITU601;
    case AVCOL_SPC_FCC:
        return SWS_CS_FCC;
    case AVCOL_SPC_SMPTE240M:
        return SWS_CS_SMPTE240M;
    case AVCOL_SPC_BT2020_NCL:
    case AVCOL_SPC_BT2020_CL:
        return SWS_CS_BT2020;
    default:
        return -1;
    }
}
SwsContext* declaredConversion(const int width, const int height, const AVPixelFormat from,
                               const AVPixelFormat to, const DeclaredSampling& sampling,
                               const unsigned flags) {
    const bool yuv = !rgb(from) || !rgb(to);
    const auto coefficients = swsCoefficients(sampling.matrix);
    if (yuv && coefficients < 0)
        return nullptr;
    Scale scale(sws_alloc_context(), sws_freeContext);
    if (!scale)
        return nullptr;
    auto& s = *scale;
    s.src_w = s.dst_w = width;
    s.src_h = s.dst_h = height;
    s.src_format = from;
    s.dst_format = to;
    s.flags = flags;
    const auto siting = effectiveSiting(sampling.siting);
    sitePlanes(from, siting, s.src_h_chr_pos, s.src_v_chr_pos);
    sitePlanes(to, siting, s.dst_h_chr_pos, s.dst_v_chr_pos);
    if (sws_init_context(scale.get(), nullptr, nullptr) < 0)
        return nullptr;
    if (yuv) {
        const int full = sampling.range == AVCOL_RANGE_JPEG ? 1 : 0;
        const auto* table = sws_getCoefficients(coefficients);
        if (sws_setColorspaceDetails(scale.get(), table, rgb(from) ? 1 : full, table,
                                     rgb(to) ? 1 : full, 0, 1 << 16, 1 << 16) < 0)
            return nullptr;
    }
    return scale.release();
}
ReopenMeasure measureReopen(const AVFrame& decoded, const std::span<const std::byte> reference,
                            const DeclaredSampling& declared) {
    ReopenMeasure result;
    const auto width = decoded.width, height = decoded.height;
    const auto format = static_cast<AVPixelFormat>(decoded.format);
    const auto bytes = static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 8U;
    if (width <= 0 || height <= 0 || reference.size() != bytes) {
        result.failure = "Reopen reference size mismatch";
        return result;
    }
    int rgbaStride = width * 8;
    const auto reconstruct = [&](const std::uint8_t* const* planes, const int* strides,
                                 std::vector<std::byte>& out) {
        Scale scale(declaredConversion(width, height, format, AV_PIX_FMT_RGBA64LE, declared,
                                       kReconstructionFlags),
                    sws_freeContext);
        out.assign(bytes, std::byte{});
        std::uint8_t* target[4]{reinterpret_cast<std::uint8_t*>(out.data()), nullptr, nullptr,
                                nullptr};
        return scale &&
               sws_scale(scale.get(), planes, strides, 0, height, target, &rgbaStride) == height;
    };
    if (!reconstruct(decoded.data, decoded.linesize, result.rgba)) {
        result.failure = "Reopen conversion unavailable for the declared sampling";
        return result;
    }
    // The codec-sampled reference: the encoder's own conversion into this sample format under
    // the declared sampling, then the same reconstruction as the decoded frame.
    std::vector<std::byte> sampled;
    std::span<const std::byte> expected = reference;
    if (!rgb(format)) {
        std::unique_ptr<AVFrame, void (*)(AVFrame*)> planes(av_frame_alloc(),
                                                            [](AVFrame* f) { av_frame_free(&f); });
        Scale toSamples(declaredConversion(width, height, AV_PIX_FMT_RGBA64LE, format, declared,
                                           kOutputConversionFlags),
                        sws_freeContext);
        bool ok = planes && toSamples;
        if (ok) {
            planes->format = format;
            planes->width = width;
            planes->height = height;
            ok = av_frame_get_buffer(planes.get(), 32) >= 0;
        }
        const std::uint8_t* source[4]{reinterpret_cast<const std::uint8_t*>(reference.data()),
                                      nullptr, nullptr, nullptr};
        ok = ok &&
             sws_scale(toSamples.get(), source, &rgbaStride, 0, height, planes->data,
                       planes->linesize) == height &&
             reconstruct(planes->data, planes->linesize, sampled);
        if (!ok) {
            result.failure = "Reopen reference unavailable for the declared sampling";
            return result;
        }
        expected = sampled;
    }
    for (std::size_t i = 0; i < bytes; i += 2) {
        const auto a = sample(result.rgba, i);
        if (i % 8 == 6) {
            const auto b = sample(reference, i);
            result.alphaMaximum = std::max(result.alphaMaximum, a > b ? a - b : b - a);
            continue;
        }
        const auto b = sample(expected, i);
        const auto error = a > b ? a - b : b - a;
        result.maximum = std::max(result.maximum, error);
        result.sum += error;
        ++result.count;
    }
    return result;
}
} // namespace bloom::media::ffmpeg
