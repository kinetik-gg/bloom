// Bloom's private loader shim. It is intentionally not an OpenH264 codec
// implementation and contains only the public ABI declarations needed by the
// FFmpeg libopenh264 wrapper. The Cisco binary, when consented to and verified,
// is placed before this shim in the worker's loader path.

#include <cstdint>

namespace {
constexpr unsigned kShimMarker = 0x5348494dU; // "SHIM"
}

extern "C" {
struct ISVCEncoder;
struct ISVCDecoder;
struct OpenH264Version {
    unsigned uMajor;
    unsigned uMinor;
    unsigned uRevision;
    unsigned uReserved;
};

#if defined(__GNUC__)
#define BLOOM_OPENH264_SHIM_EXPORT __attribute__((visibility("default")))
#else
#define BLOOM_OPENH264_SHIM_EXPORT
#endif

BLOOM_OPENH264_SHIM_EXPORT int WelsCreateSVCEncoder(ISVCEncoder** encoder) {
    if (encoder != nullptr)
        *encoder = nullptr;
    return 1;
}

BLOOM_OPENH264_SHIM_EXPORT void WelsDestroySVCEncoder(ISVCEncoder*) {}

BLOOM_OPENH264_SHIM_EXPORT long WelsCreateDecoder(ISVCDecoder** decoder) {
    if (decoder != nullptr)
        *decoder = nullptr;
    return 1;
}

BLOOM_OPENH264_SHIM_EXPORT void WelsDestroyDecoder(ISVCDecoder*) {}

BLOOM_OPENH264_SHIM_EXPORT OpenH264Version WelsGetCodecVersion() {
    return {0U, 0U, 0U, kShimMarker};
}

BLOOM_OPENH264_SHIM_EXPORT void WelsGetCodecVersionEx(OpenH264Version* version) {
    if (version != nullptr)
        *version = WelsGetCodecVersion();
}
}

#undef BLOOM_OPENH264_SHIM_EXPORT
