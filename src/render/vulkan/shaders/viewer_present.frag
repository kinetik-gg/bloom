#version 460
// Bloom viewer presentation fragment shader (offline compiled; do not load at runtime).
//
// Reproduces the current viewer's paintEvent composition in the target's pixel space:
//   - display-space background (solid / checkerboard / black / white),
//   - the resident packed-RGBA8 display image sampled through an explicit destination rectangle and
//     source image window, with the viewer's channel remap,
//   - straight-alpha source-over onto the background (the display buffer is straight RGBA8 sRGB),
//   - an optional premultiplied RGBA8 overlay composited last.
//
// The display image is STRAIGHT RGBA8. QPainter's SmoothPixmapTransform premultiplies before it
// interpolates, so this shader fetches four texel taps, applies the channel remap to each tap, then
// premultiplies each tap BEFORE the bilinear weights (RGBA mode); the opaque channel modes are
// already premultiplied by their forced alpha 1. Interpolating straight RGB against straight alpha
// would produce coloured halos at transparent edges and is deliberately not done.
//
// The display texture holds already sRGB-encoded bytes. The pipeline targets a UNORM swapchain
// (BGRA8/RGBA8 UNORM); an _SRGB attachment would encode again and is rejected by the caller. The
// attachment format owns channel packing: the fragment output is always the RGBA semantic vec4 and
// the driver maps it into the BGRA8_UNORM texel bytes; there is no manual swizzle.

layout(location = 0) in vec2 vTargetUv01;
layout(location = 0) out vec4 outColor;

layout(set = 0, binding = 0) uniform sampler2D uDisplayImage;
layout(set = 0, binding = 1) uniform sampler2D uOverlayImage;

layout(set = 0, binding = 2) uniform BloomPresentParams {
    vec4 targetExtent;      // xy = target pixels
    vec4 destinationRect;   // xy, wh = destination rectangle in target pixels
    vec4 imageWindow;       // xy, wh = source window in display-image pixels
    vec4 sourceExtent;      // xy = full display-image extent in pixels
    vec4 backgroundColor;   // solid background (straight, opaque)
    vec4 checkerColorA;     // base tile color
    vec4 checkerColorB;     // raised tile color
    vec4 checkerParams;     // x = tile pixels, z,w = checker origin in target pixels
    uvec4 modes;            // x = channel, y = background, z = hasOverlay, w = flags
} present;

const uint kChannelRgba = 0u;
const uint kChannelRgb = 1u;
const uint kChannelRed = 2u;
const uint kChannelGreen = 3u;
const uint kChannelBlue = 4u;
const uint kChannelAlpha = 5u;

const uint kBackgroundSolid = 0u;
const uint kBackgroundCheckerboard = 1u;
const uint kBackgroundBlack = 2u;
const uint kBackgroundWhite = 3u;

vec4 remapChannel(vec4 source, uint channel)
{
    switch (channel) {
    case kChannelRgb:
        return vec4(source.rgb, 1.0);
    case kChannelRed:
        return vec4(source.r, source.r, source.r, 1.0);
    case kChannelGreen:
        return vec4(source.g, source.g, source.g, 1.0);
    case kChannelBlue:
        return vec4(source.b, source.b, source.b, 1.0);
    case kChannelAlpha:
        return vec4(source.a, source.a, source.a, 1.0);
    case kChannelRgba:
    default:
        return source;
    }
}

// Straight RGBA tap -> premultiplied, after the viewer channel remap. Opaque remapped modes are
// unchanged by the multiply.
vec4 premultipliedTap(ivec2 coordinate, ivec2 extent, uint channel)
{
    const ivec2 clamped = clamp(coordinate, ivec2(0), max(extent - 1, ivec2(0)));
    const vec4 raw = texelFetch(uDisplayImage, clamped, 0);
    const vec4 remapped = remapChannel(raw, channel);
    return vec4(remapped.rgb * remapped.a, remapped.a);
}

vec4 samplePremultiplied(vec2 sourcePixel, ivec2 extent, uint channel)
{
    // Half-texel-centred bilinear, matching QPainter's smooth transform sampling positions.
    const vec2 base = sourcePixel - vec2(0.5);
    const ivec2 lower = ivec2(floor(base));
    const vec2 fraction = base - vec2(lower);
    const vec4 tap00 = premultipliedTap(lower, extent, channel);
    const vec4 tap10 = premultipliedTap(lower + ivec2(1, 0), extent, channel);
    const vec4 tap01 = premultipliedTap(lower + ivec2(0, 1), extent, channel);
    const vec4 tap11 = premultipliedTap(lower + ivec2(1, 1), extent, channel);
    const vec4 top = mix(tap00, tap10, fraction.x);
    const vec4 bottom = mix(tap01, tap11, fraction.x);
    return mix(top, bottom, fraction.y);
}

void main()
{
    const vec2 targetPixel = clamp(vTargetUv01, vec2(0.0), vec2(1.0)) * present.targetExtent.xy;

    vec4 background = present.backgroundColor;
    const uint backgroundMode = present.modes.y;
    if (backgroundMode == kBackgroundBlack) {
        background = vec4(0.0, 0.0, 0.0, 1.0);
    } else if (backgroundMode == kBackgroundWhite) {
        background = vec4(1.0, 1.0, 1.0, 1.0);
    } else if (backgroundMode == kBackgroundCheckerboard) {
        const float tile = max(present.checkerParams.x, 1.0);
        const ivec2 cell = ivec2(floor((targetPixel - present.checkerParams.zw) / tile));
        // The viewer raises (row + column) even; see viewer_editor.cpp drawCheckerboard().
        const bool raised = ((cell.x + cell.y) & 1) == 0;
        background = raised ? present.checkerColorB : present.checkerColorA;
    }

    vec3 color = background.rgb;
    float alpha = background.a;

    const vec4 destination = present.destinationRect;
    const bool inside = destination.z > 0.0 && destination.w > 0.0 &&
                        targetPixel.x >= destination.x &&
                        targetPixel.x < destination.x + destination.z &&
                        targetPixel.y >= destination.y &&
                        targetPixel.y < destination.y + destination.w;
    if (inside) {
        const vec2 local = (targetPixel - destination.xy) / destination.zw;
        const vec2 sourcePixel = present.imageWindow.xy + local * present.imageWindow.zw;
        const ivec2 extent = ivec2(max(present.sourceExtent.xy, vec2(1.0)));
        const vec4 source = samplePremultiplied(sourcePixel, extent, present.modes.x);
        // Premultiplied source-over onto the opaque background.
        color = source.rgb + background.rgb * (1.0 - source.a);
        alpha = source.a + background.a * (1.0 - source.a);
    }

    if (present.modes.z != 0u) {
        // The overlay is already premultiplied; filtering it directly is correct.
        const vec4 overlay = texture(uOverlayImage, vTargetUv01);
        color = overlay.rgb + color * (1.0 - overlay.a);
        alpha = overlay.a + alpha * (1.0 - overlay.a);
    }

    // The attachment format owns channel packing (BGRA8_UNORM attachment packs these RGBA semantics
    // into BGRA bytes); no manual swizzle.
    outColor = vec4(color, alpha);
}
