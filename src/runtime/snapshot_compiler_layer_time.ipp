// Included inside the snapshot compiler class (snapshot_compiler.cpp). ADR 0024 layer-owned time:
// lowers a boundary's LayerTiming and Time Remap binding into CompiledLayerTime, resolving a Source
// content extent through runtime::resolveSourceExtent (layer_time_projection).

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
        if (const auto resolved = runtime::resolveSourceExtent(request_.snapshot.project(),
                                                               *composition_,
                                                               source->sourceNodeId)) {
            result.extentDuration = resolved->duration;
            result.extentFrameRate = resolved->frameRate;
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
