#include <bloom/commands/video_source_operations.hpp>

#include <bloom/document/document.hpp>
#include <bloom/document/parameter.hpp>

#include <algorithm>
#include <charconv>
#include <string>

namespace bloom::commands {

std::string_view SetVideoAudioStream::typeId() const noexcept {
    return "bloom.video.set-audio-stream";
}

OperationResult SetVideoAudioStream::apply(document::Draft& draft) const {
    auto* composition = draft.project().findComposition(composition_);
    if (composition == nullptr)
        return OperationResult::rejected(OperationIssueCode::InvalidTarget,
                                         "Composition does not exist");
    auto* node = composition->graph().findNode(node_);
    if (node == nullptr || node->typeId != "bloom.video-source")
        return OperationResult::rejected(OperationIssueCode::InvalidTarget,
                                         "The node is not a Video source");
    if (composition->nodeLocked(node_))
        return OperationResult::rejected(OperationIssueCode::InvalidValue, "Layer is locked");
    // The selected stream must exist on the node's asset.
    const auto assetBinding = std::ranges::find(node->parameters, std::string_view("asset"),
                                                &document::ParameterBinding::role);
    const auto* assetParameter = assetBinding == node->parameters.end()
                                     ? nullptr
                                     : composition->parameters().find(assetBinding->parameterId);
    const auto* constant = assetParameter
                               ? std::get_if<document::ConstantValueSource>(&assetParameter->source)
                               : nullptr;
    const auto* text = constant ? std::get_if<std::string>(&constant->value) : nullptr;
    std::uint64_t raw = 0;
    if (text == nullptr ||
        std::from_chars(text->data(), text->data() + text->size(), raw).ec != std::errc{})
        return OperationResult::rejected(OperationIssueCode::InvalidValue,
                                         "The Video node has no asset");
    const auto* asset = draft.project().findAsset(document::AssetId::fromRaw(raw));
    const auto audioStreams =
        asset == nullptr
            ? 0
            : std::ranges::count(asset->videoStreams, 2U, &document::AssetVideoStream::kind);
    if (audioStream_ >= static_cast<std::uint32_t>(audioStreams))
        return OperationResult::rejected(OperationIssueCode::InvalidValue,
                                         "The asset has no such audio stream");
    const document::ParameterValue value = static_cast<std::int64_t>(audioStream_);
    const auto binding =
        std::ranges::find(node->parameters, document::kVideoAudioStreamParameterRole,
                          &document::ParameterBinding::role);
    if (binding != node->parameters.end()) {
        const auto* parameter = composition->parameters().find(binding->parameterId);
        if (parameter != nullptr &&
            parameter->source == document::ParameterSource{document::ConstantValueSource{value}})
            return OperationResult::noChange();
        if (!composition->parameters().setSource(binding->parameterId,
                                                 document::ConstantValueSource{value}))
            return OperationResult::rejected(OperationIssueCode::InvalidValue,
                                             "Audio stream parameter is invalid");
        return OperationResult::applied();
    }
    const auto parameterId = draft.ids().allocateParameter();
    if (!parameterId)
        return OperationResult::rejected(OperationIssueCode::InvalidValue,
                                         "Parameter identifiers are exhausted");
    if (!composition->parameters().insert(
            {*parameterId, std::string(document::kVideoAudioStreamParameterSchemaKey),
             document::ConstantValueSource{value}}))
        return OperationResult::rejected(OperationIssueCode::InvalidValue,
                                         "Audio stream parameter could not be inserted");
    node->parameters.push_back(
        {std::string(document::kVideoAudioStreamParameterRole), *parameterId});
    return OperationResult::applied();
}

} // namespace bloom::commands
