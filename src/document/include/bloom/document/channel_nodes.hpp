#pragma once

#include <bloom/document/node_definition_registry.hpp>

#include <string_view>

// ADR 0026 multi-channel nodes: the image source's channel selection parameters, the Shuffle,
// Channel Copy and Remove definitions, and the Composition Output's channel layer list.
namespace bloom::document {

inline constexpr std::string_view kImageSourceNodeType = "bloom.image-source";
inline constexpr std::uint32_t kImageSourceNodeSchemaVersion = 2;
inline constexpr std::string_view kImageSourceImageOutputPort = "image";
inline constexpr std::string_view kImageSourceChannelsOutputPort = "channels";
inline constexpr std::string_view kImagePartParameterRole = "part";
inline constexpr std::string_view kImagePartParameterSchemaKey = "bloom.image.part";
inline constexpr std::string_view kImageLayerParameterRole = "layer";
inline constexpr std::string_view kImageLayerParameterSchemaKey = "bloom.image.layer";

inline constexpr std::string_view kChannelShuffleNodeType = "bloom.channel-shuffle";
inline constexpr std::string_view kChannelCopyNodeType = "bloom.channel-copy";
inline constexpr std::string_view kChannelRemoveNodeType = "bloom.channel-remove";
inline constexpr std::uint32_t kChannelNodeSchemaVersion = 1;
// Every channel node reads its primary set from `channels` and publishes a set on `channels`.
inline constexpr std::string_view kChannelSetPort = "channels";
inline constexpr std::string_view kChannelCopyFromPort = "from";
inline constexpr std::string_view kChannelShuffleImagePort = "image";

// Shuffle mapping encodings (ADR 0026 §4). Colour converts the RGB triple from the channel set's
// input colour space to the working space; Raw places the values in working-space RGB unchanged.
inline constexpr std::int64_t kChannelShuffleColourMode = 0;
inline constexpr std::int64_t kChannelShuffleRawMode = 1;

inline constexpr std::string_view kCompositionOutputChannelsInputPort = "channels";
inline constexpr std::string_view kCompositionOutputLayersParameterRole = "layers";
inline constexpr std::string_view kCompositionOutputLayersParameterSchemaKey =
    "bloom.composition-output.layers";

inline constexpr std::size_t kMaximumChannelNameBytes = 256;
inline constexpr std::size_t kMaximumChannelPatternBytes = 4096;

// #364 Cryptomatte (Nuke-compatible): mattes picked by name from a Cryptomatte layer of the
// connected channel set. `mask` is the ADR 0028 Mask (premultiplied white x coverage); `image` is
// the optional input image multiplied by coverage (matte in alpha), or the matte as a white image.
inline constexpr std::string_view kCryptomatteNodeType = "bloom.cryptomatte";
inline constexpr std::uint32_t kCryptomatteNodeSchemaVersion = 1;
inline constexpr std::string_view kCryptomatteImageInputPort = "image";
inline constexpr std::string_view kCryptomatteMaskOutputPort = "mask";
inline constexpr std::string_view kCryptomatteImageOutputPort = "image";
// #364: not a socket. A Viewer inspection of this port shows the ID-colour preview (every object
// in its own colour, the selected mattes brightened) over the input image.
inline constexpr std::string_view kCryptomattePreviewInspectionPort = "preview";
inline constexpr std::string_view kCryptomatteLayerParameterRole = "layer";
inline constexpr std::string_view kCryptomatteLayerParameterSchemaKey = "bloom.cryptomatte.layer";
inline constexpr std::string_view kCryptomatteMattesParameterRole = "mattes";
inline constexpr std::string_view kCryptomatteMattesParameterSchemaKey = "bloom.cryptomatte.mattes";
inline constexpr std::size_t kMaximumCryptomatteMatteListBytes = 65536;

[[nodiscard]] NodeDefinition cryptomatteDefinition();

[[nodiscard]] NodeDefinition channelShuffleDefinition();
[[nodiscard]] NodeDefinition channelCopyDefinition();
[[nodiscard]] NodeDefinition channelRemoveDefinition();

// Whether `schemaKey` is one of the channel nodes' parameter schemas, and whether `value` is a
// constant inside its domain.
[[nodiscard]] bool isChannelParameterSchemaKey(std::string_view schemaKey) noexcept;
[[nodiscard]] bool isChannelParameterValueValid(std::string_view schemaKey,
                                                const ParameterValue& value) noexcept;

} // namespace bloom::document
