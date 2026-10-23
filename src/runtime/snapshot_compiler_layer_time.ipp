// Included inside the snapshot compiler class (snapshot_compiler.cpp). ADR 0024 layer-owned time:
// lowers a boundary's LayerTiming and Time Remap binding into CompiledLayerTime, resolving a Source
// content extent from the referenced node's natural media or composition length.

[[nodiscard]] const document::AssetRecord* assetOfSource(const document::NodeRecord& node) {
    const auto* asset = parameterConstant<std::string>(findParameterBinding(node, "asset"));
    if (asset == nullptr)
        return nullptr;
    std::uint64_t raw = 0;
    const auto parsed = std::from_chars(asset->data(), asset->data() + asset->size(), raw);
    if (parsed.ec != std::errc{} || parsed.ptr != asset->data() + asset->size())
        return nullptr;
    return request_.snapshot.project().findAsset(document::AssetId::fromRaw(raw));
}

// The content extent a Source reference resolves to, or nullopt when the source has no natural
// length (a still image, a missing asset, a source with its own non-identity timing).
[[nodiscard]] std::optional<std::pair<core::RationalTime, document::FrameRate>>
sourceExtent(const document::NodeId sourceNodeId) {
    const auto* node = composition_->graph().findNode(sourceNodeId);
    if (node == nullptr)
        return std::nullopt;
    const auto rate = composition_->format().frameRate();
    const auto frames = [](const std::uint64_t count, const document::FrameRate frameRate)
        -> std::optional<core::RationalTime> {
        if (count == 0 || count > static_cast<std::uint64_t>(std::numeric_limits<std::int32_t>::max()))
            return std::nullopt;
        return core::RationalTime::create(static_cast<std::int64_t>(count) * frameRate.denominator(),
                                          frameRate.numerator());
    };
    const auto identityStart = [&] {
        const auto* start =
            parameterConstant<std::int64_t>(findParameterBinding(*node, "startFrame"));
        return start == nullptr || *start == 0;
    };
    if (node->typeId == "bloom.image-source") {
        const auto* asset = assetOfSource(*node);
        if (asset == nullptr || asset->kind != document::AssetKind::Sequence || !identityStart() ||
            asset->manifest.last < asset->manifest.first)
            return std::nullopt;
        const auto length = static_cast<std::uint64_t>(asset->manifest.last) -
                            static_cast<std::uint64_t>(asset->manifest.first) + 1;
        const auto duration = frames(length, rate);
        return duration ? std::optional{std::pair{*duration, rate}} : std::nullopt;
    }
    if (node->typeId == "bloom.video-source") {
        const auto* asset = assetOfSource(*node);
        if (asset == nullptr || asset->kind != document::AssetKind::Video || !identityStart())
            return std::nullopt;
        const auto stream =
            std::ranges::find(asset->videoStreams, 1U, &document::AssetVideoStream::kind);
        if (stream == asset->videoStreams.end() || stream->framePeriod.numerator() <= 0 ||
            stream->framePeriod.denominator() > std::numeric_limits<std::uint32_t>::max() ||
            stream->framePeriod.numerator() > std::numeric_limits<std::uint32_t>::max())
            return std::nullopt;
        const auto streamRate = document::FrameRate::create(
            static_cast<std::uint32_t>(stream->framePeriod.denominator()),
            static_cast<std::uint32_t>(stream->framePeriod.numerator()));
        const auto duration = streamRate ? frames(asset->frames, *streamRate) : std::nullopt;
        return duration ? std::optional{std::pair{*duration, *streamRate}} : std::nullopt;
    }
    if (node->typeId == "bloom.audio-source") {
        const auto* asset = assetOfSource(*node);
        if (asset == nullptr || !identityStart() || asset->duration <= core::RationalTime{})
            return std::nullopt;
        return std::pair{asset->duration, rate};
    }
    if (node->typeId == std::string_view(document::kCompositionSourceNodeType)) {
        const auto* id =
            parameterConstant<std::int64_t>(findParameterBinding(*node, "composition"));
        const auto* offset = parameterConstant<double>(findParameterBinding(*node, "timeOffset"));
        const auto* scale = parameterConstant<double>(findParameterBinding(*node, "timeScale"));
        if (id == nullptr || *id <= 0 || offset == nullptr || scale == nullptr || *offset != 0.0 ||
            *scale != 1.0)
            return std::nullopt;
        const auto* nested = request_.snapshot.project().findComposition(
            document::CompositionId::fromRaw(static_cast<std::uint64_t>(*id)));
        if (nested == nullptr)
            return std::nullopt;
        return std::pair{nested->duration(), nested->format().frameRate()};
    }
    return std::nullopt;
}

// nullopt is a topology failure (already reported); an engaged empty optional is identity timing.
[[nodiscard]] std::optional<std::optional<runtime::CompiledLayerTime>>
lowerLayerTime(const document::NodeRecord& node, const document::LayerOutputBoundary& boundary) {
    const auto& timing = boundary.timing;
    const auto* remapBinding = findParameterBinding(node, document::kTimeRemapParameterRole);
    std::optional<runtime::CompiledScalarParameter> remap;
    if (remapBinding != nullptr) {
        remap = compiledScalarParameter(remapBinding);
        if (!remap) {
            addTopologyFailure(node.id, "Validated Layer Output Time Remap could not be lowered.");
            return std::nullopt;
        }
    }
    if (timing.isIdentityMapping() && !remap)
        return std::optional<runtime::CompiledLayerTime>{};
    runtime::CompiledLayerTime result;
    result.startTime = timing.startTime;
    result.speed = timing.speed;
    result.timeRemap = remap;
    result.beforeMode = timing.beforeMode;
    result.afterMode = timing.afterMode;
    if (const auto* fixed = std::get_if<document::FixedLayerExtent>(&timing.contentExtent)) {
        result.extentDuration = fixed->duration;
        result.extentFrameRate = fixed->frameRate;
    } else if (const auto* source =
                   std::get_if<document::SourceLayerExtent>(&timing.contentExtent)) {
        if (const auto resolved = sourceExtent(source->sourceNodeId)) {
            result.extentDuration = resolved->first;
            result.extentFrameRate = resolved->second;
        } else {
            auto diagnosticSubject = subject(node.id, "layerTime.contentExtent");
            diagnosticSubject.layerId = boundary.layerId;
            addWarning(runtime::CompileDiagnosticCode::LayerExtentUnresolved,
                       std::move(diagnosticSubject),
                       "Layer content extent has no media length; it is unbounded",
                       "The extent source is missing, a still image, or has its own timing.");
        }
    }
    return std::optional{result};
}
