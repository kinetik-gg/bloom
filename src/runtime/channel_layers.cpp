#include <bloom/runtime/channel_layers.hpp>

#include "channel_resolution.hpp"
#include "image_source.hpp"

#include <algorithm>
#include <limits>

namespace bloom::runtime {
namespace {

[[nodiscard]] std::string_view trim(std::string_view text) noexcept {
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t'))
        text.remove_prefix(1);
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t'))
        text.remove_suffix(1);
    return text;
}

[[nodiscard]] bool layerSelected(const std::string& selection, const std::string& layer) {
    if (trim(selection).empty())
        return true;
    std::size_t start = 0;
    while (start <= selection.size()) {
        const auto comma = selection.find(',', start);
        const auto token = trim(std::string_view(selection).substr(
            start, comma == std::string::npos ? std::string::npos : comma - start));
        if (token == layer)
            return true;
        if (comma == std::string::npos)
            break;
        start = comma + 1;
    }
    return false;
}

} // namespace

ChannelLayerAssembly
assembleOutputChannelLayers(const CompiledCompositionPlan& plan, const core::RationalTime time,
                            const std::filesystem::path& assetBase,
                            const EvaluationColorIntent& colorIntent,
                            const render::ImageWindow dataWindow,
                            const render::ImageWindow displayWindow, const std::size_t byteBudget,
                            const CancellationToken& cancellation) {
    ChannelLayerAssembly result;
    const auto* output = std::get_if<CompiledCompositionOutput>(
        &plan.operations()[plan.output().value()]);
    if (output == nullptr || !output->channelLayers)
        return result;
    const auto& expression = *output->channelLayers;
    std::vector<detail::ImageSourceSelection> selections;
    std::vector<const media::ImagePartInfo*> parts;
    for (const auto& source : expression.sources) {
        const CompiledImageSource image{source.sourceNodeId, source.asset,      source.startFrame,
                                        source.loopMode,     source.colorSpace, source.inputColorSpaceId,
                                        false,               source.part,       {}};
        auto selected = detail::selectImageSource(image, time, plan.format().frameRate(), assetBase,
                                                  cancellation, colorIntent);
        if (selected.cancelled) {
            result.cancelled = true;
            return result;
        }
        const auto* part = selected.available && selected.sourceRevision
                               ? selected.sourceRevision->probe().findPart(source.part)
                               : nullptr;
        if (part == nullptr) {
            result.diagnostic = selected.warning.empty() ? "A channel layer source is unavailable"
                                                         : selected.warning;
            return result;
        }
        parts.push_back(part);
        selections.push_back(std::move(selected));
    }
    const auto resolution = detail::resolveChannelExpression(expression, parts);
    if (!resolution.ok()) {
        result.diagnostic = resolution.error;
        return result;
    }
    std::vector<detail::ResolvedChannel> written;
    for (const auto& channel : resolution.channels) {
        if (channel.name == "R" || channel.name == "G" || channel.name == "B" ||
            channel.name == "A")
            continue;
        if (!layerSelected(output->layers, channel.descriptor.layer))
            continue;
        written.push_back(channel);
    }
    if (written.empty()) {
        result.diagnostic = "The output channel set has no layer to write";
        return result;
    }
    std::vector<std::vector<std::string>> names(selections.size());
    for (const auto& channel : written)
        if (channel.source && std::ranges::find(names[*channel.source], channel.sourceChannel) ==
                                  names[*channel.source].end())
            names[*channel.source].push_back(channel.sourceChannel);
    std::vector<std::shared_ptr<const render::ChannelSetImage>> sets(selections.size());
    std::size_t remaining = byteBudget;
    for (std::size_t index = 0; index < selections.size(); ++index) {
        if (names[index].empty())
            continue;
        media::ImageChannelSelection request;
        request.part = selections[index].interpretation.part;
        request.channels = names[index];
        auto decoded = media::decodeImageChannels(
            *selections[index].sourceRevision, request,
            [&cancellation] { return cancellation.isCancellationRequested(); }, {}, remaining);
        if (!decoded.value.has_value()) {
            result.cancelled = decoded.cancelled;
            result.diagnostic = decoded.cancelled ? std::string{} : decoded.diagnostic;
            return result;
        }
        const auto cost = (*decoded.value)->byteCost();
        remaining = cost < remaining ? remaining - cost : 0;
        sets[index] = std::move(*decoded.value);
    }
    const auto width = static_cast<std::size_t>(dataWindow.extent().width());
    const auto height = static_cast<std::size_t>(dataWindow.extent().height());
    std::vector<render::ChannelDescriptor> descriptors;
    std::vector<std::vector<float>> planes;
    for (const auto& channel : written) {
        if (cancellation.isCancellationRequested()) {
            result.cancelled = true;
            return result;
        }
        std::vector<float> plane(width * height, channel.constant);
        if (channel.source) {
            const auto& set = sets[*channel.source];
            const auto found = set->find(channel.sourceChannel);
            if (!found) {
                result.diagnostic = "A channel layer was not decoded";
                return result;
            }
            const auto values = set->plane(*found);
            const auto window = set->dataWindow();
            for (std::size_t row = 0; row < height; ++row) {
                const auto y = dataWindow.originY() + static_cast<std::int64_t>(row);
                for (std::size_t column = 0; column < width; ++column) {
                    const auto x = dataWindow.originX() + static_cast<std::int64_t>(column);
                    plane[row * width + column] =
                        window.contains(x, y)
                            ? values[static_cast<std::size_t>(y - window.originY()) *
                                         window.extent().width() +
                                     static_cast<std::size_t>(x - window.originX())]
                            : 0.0F;
                }
            }
        }
        descriptors.push_back(channel.descriptor);
        descriptors.back().name = channel.name;
        planes.push_back(std::move(plane));
    }
    auto created = render::ChannelSetImage::create(dataWindow, displayWindow,
                                                   core::PixelAspectRatio::square(),
                                                   std::move(descriptors), std::move(planes), {},
                                                   remaining);
    if (!created) {
        result.diagnostic = "The output channel layers exceed the byte budget";
        return result;
    }
    result.layers = std::make_shared<const render::ChannelSetImage>(std::move(*created.value()));
    return result;
}

} // namespace bloom::runtime
