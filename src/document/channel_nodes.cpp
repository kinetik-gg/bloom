#include <bloom/document/channel_nodes.hpp>

#include <bloom/core/utf8.hpp>

#include <string>
#include <utility>

namespace bloom::document {
namespace {

[[nodiscard]] bool boundedText(const ParameterValue& value, const std::size_t limit) noexcept {
    const auto* text = std::get_if<std::string>(&value);
    return text != nullptr && text->size() <= limit && text->find('\0') == std::string::npos &&
           core::isValidUtf8(*text);
}

[[nodiscard]] ParameterDefinition text(const std::string_view typeId, std::string role,
                                       std::string value) {
    auto schemaKey = std::string(typeId) + "." + role;
    return {std::move(role), std::move(schemaKey), ParameterValueKind::String, true, false,
            std::move(value)};
}

} // namespace

NodeDefinition channelShuffleDefinition() {
    const auto type = kChannelShuffleNodeType;
    NodeDefinition node{{std::string(type), kChannelNodeSchemaVersion},
                        NodeLoweringKind::Channel,
                        {{std::string(kChannelSetPort), SocketValueKind::ChannelSet, true}},
                        {{std::string(kChannelShuffleImagePort), SocketValueKind::Image},
                         {std::string(kChannelSetPort), SocketValueKind::ChannelSet}},
                        {},
                        std::nullopt,
                        NodeCardinality::Many,
                        NodeCategory::Compositing};
    // A slot names a source channel, or the constants "0" and "1".
    node.parameters.push_back(text(type, "red", "R"));
    node.parameters.push_back(text(type, "green", "G"));
    node.parameters.push_back(text(type, "blue", "B"));
    node.parameters.push_back(text(type, "alpha", "1"));
    node.parameters.push_back({"mode", std::string(type) + ".mode", ParameterValueKind::Integer,
                               true, false, kChannelShuffleColourMode});
    // Whether the mapped RGB is already associated (premultiplied) with the mapped alpha, as
    // OpenEXR stores it; straight sources such as PNG set it off.
    node.parameters.push_back({"associated", std::string(type) + ".associated",
                               ParameterValueKind::Boolean, true, false, true});
    // The layer the `channels` output names its R, G, B and A in; empty is the root layer.
    node.parameters.push_back(text(type, "layer", ""));
    return node;
}

NodeDefinition channelCopyDefinition() {
    const auto type = kChannelCopyNodeType;
    NodeDefinition node{{std::string(type), kChannelNodeSchemaVersion},
                        NodeLoweringKind::Channel,
                        {{std::string(kChannelSetPort), SocketValueKind::ChannelSet, true},
                         {std::string(kChannelCopyFromPort), SocketValueKind::ChannelSet, true}},
                        {{std::string(kChannelSetPort), SocketValueKind::ChannelSet}},
                        {},
                        std::nullopt,
                        NodeCardinality::Many,
                        NodeCategory::Compositing};
    // Comma-separated channel patterns (names, `layer.*` globs or `@role`) copied from `from`.
    node.parameters.push_back(text(type, "select", "*"));
    return node;
}

NodeDefinition channelRemoveDefinition() {
    const auto type = kChannelRemoveNodeType;
    NodeDefinition node{{std::string(type), kChannelNodeSchemaVersion},
                        NodeLoweringKind::Channel,
                        {{std::string(kChannelSetPort), SocketValueKind::ChannelSet, true}},
                        {{std::string(kChannelSetPort), SocketValueKind::ChannelSet}},
                        {},
                        std::nullopt,
                        NodeCardinality::Many,
                        NodeCategory::Compositing};
    node.parameters.push_back(text(type, "select", ""));
    // Keep only the matching channels instead of removing them.
    node.parameters.push_back({"keep", std::string(type) + ".keep", ParameterValueKind::Boolean,
                               true, false, false});
    return node;
}

bool isChannelParameterSchemaKey(const std::string_view schemaKey) noexcept {
    return schemaKey == kImagePartParameterSchemaKey ||
           schemaKey == kImageLayerParameterSchemaKey ||
           schemaKey == kCompositionOutputLayersParameterSchemaKey ||
           schemaKey.starts_with("bloom.channel-shuffle.") ||
           schemaKey == "bloom.channel-copy.select" || schemaKey == "bloom.channel-remove.select" ||
           schemaKey == "bloom.channel-remove.keep";
}

bool isChannelParameterValueValid(const std::string_view schemaKey,
                                  const ParameterValue& value) noexcept {
    if (schemaKey == kImagePartParameterSchemaKey || schemaKey == kImageLayerParameterSchemaKey)
        return boundedText(value, kMaximumChannelNameBytes);
    if (schemaKey == "bloom.channel-shuffle.red" || schemaKey == "bloom.channel-shuffle.green" ||
        schemaKey == "bloom.channel-shuffle.blue" || schemaKey == "bloom.channel-shuffle.alpha" ||
        schemaKey == "bloom.channel-shuffle.layer")
        return boundedText(value, kMaximumChannelNameBytes);
    if (schemaKey == "bloom.channel-shuffle.mode") {
        const auto* mode = std::get_if<std::int64_t>(&value);
        return mode != nullptr &&
               (*mode == kChannelShuffleColourMode || *mode == kChannelShuffleRawMode);
    }
    if (schemaKey == "bloom.channel-shuffle.associated" || schemaKey == "bloom.channel-remove.keep")
        return std::holds_alternative<bool>(value);
    if (schemaKey == "bloom.channel-copy.select" || schemaKey == "bloom.channel-remove.select" ||
        schemaKey == kCompositionOutputLayersParameterSchemaKey)
        return boundedText(value, kMaximumChannelPatternBytes);
    return false;
}

} // namespace bloom::document
