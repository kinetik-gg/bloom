// Included inside canonical_document.cpp's private namespace: the document 1.29 export queue
// (ADR 0033). Jobs are written in run order (vector order), never sorted by id.
[[nodiscard]] bool emitDecimalMember(EmitState& state, const std::string_view name,
                                     const std::uint64_t value) noexcept {
    const auto text = bloom::project::formatCanonicalUInt64(value);
    return state.ok(state.writer.memberName(name)) &&
           state.ok(state.writer.stringValue(text.view()));
}

[[nodiscard]] bool emitStringMember(EmitState& state, const std::string_view name,
                                    const std::string_view value) noexcept {
    return state.ok(state.writer.memberName(name)) && state.ok(state.writer.stringValue(value));
}

[[nodiscard]] bool emitExportOutput(EmitState& state,
                                    const bloom::document::ExportOutput& output) noexcept {
    using namespace bloom::document;
    auto& writer = state.writer;
    const auto& settings = output.settings;
    if (!state.ok(writer.beginObject()) || !emitNamedId(state, "id", output.id.value()) ||
        !emitStringMember(state, "preset", output.presetId) ||
        !state.ok(writer.memberName("settings")) || !state.ok(writer.beginObject()) ||
        !emitStringMember(state, "format", exportFormatToken(settings.format)) ||
        !emitStringMember(state, "codecProfile", settings.codecProfile))
        return false;
    if (settings.videoBitrateKbps && (!state.ok(writer.memberName("videoBitrateKbps")) ||
                                      !state.ok(writer.integerValue(*settings.videoBitrateKbps))))
        return false;
    if (!emitStringMember(state, "compression", settings.compression) ||
        !emitStringMember(state, "outputColorSpace", settings.outputColorSpaceId) ||
        !emitStringMember(state, "display", settings.displayName) ||
        !emitStringMember(state, "view", settings.viewName) ||
        !state.ok(writer.memberName("bakeLook")) ||
        !state.ok(writer.booleanValue(settings.bakeLook)) ||
        !state.ok(writer.memberName("includeAudio")) ||
        !state.ok(writer.booleanValue(settings.includeAudio)))
        return false;
    // Optional in 1.29 (absent means true), so files saved before it existed stay canonical.
    if (!settings.useCachedFrames &&
        (!state.ok(writer.memberName("useCachedFrames")) || !state.ok(writer.booleanValue(false))))
        return false;
    if (!state.ok(writer.memberName("audioSampleRate")) ||
        !state.ok(writer.integerValue(settings.audioSampleRate)) ||
        !emitStringMember(state, "encoder", exportEncoderToken(settings.encoder)) ||
        !state.ok(writer.memberName("naming")) || !state.ok(writer.beginObject()))
        return false;
    if (settings.naming.startFrame &&
        !emitDecimalMember(state, "startFrame", *settings.naming.startFrame))
        return false;
    if (!state.ok(writer.memberName("framePadding")) ||
        !state.ok(writer.integerValue(settings.naming.framePadding)) ||
        !emitStringMember(state, "pattern", settings.naming.pattern) ||
        !state.ok(writer.endObject()) || !state.ok(writer.endObject()))
        return false;
    const auto& destination = output.destination;
    return state.ok(writer.memberName("destination")) && state.ok(writer.beginObject()) &&
           emitStringMember(state, "portability", destination.portability) &&
           emitStringMember(state, "path", destination.path) &&
           emitStringMember(state, "overwrite", exportOverwriteToken(destination.overwrite)) &&
           state.ok(writer.endObject()) && state.ok(writer.memberName("enabled")) &&
           state.ok(writer.booleanValue(output.enabled)) &&
           state.ok(writer.memberName("importIntoAssets")) &&
           state.ok(writer.booleanValue(output.importIntoAssets)) && state.ok(writer.endObject());
}

[[nodiscard]] bool emitExportJob(EmitState& state, const bloom::document::ExportJob& job) noexcept {
    using namespace bloom::document;
    auto& writer = state.writer;
    const auto& range = job.render.range;
    if (!state.ok(writer.beginObject()) || !emitNamedId(state, "id", job.id.value()) ||
        !emitNamedId(state, "composition", job.composition.value()) ||
        !emitStringMember(state, "name", job.name) || !state.ok(writer.memberName("range")) ||
        !state.ok(writer.beginObject()) ||
        !emitStringMember(state, "kind", exportRangeKindToken(range.kind)) ||
        !emitDecimalMember(state, "first", range.first))
        return false;
    if (range.kind == ExportRangeKind::Custom && !emitDecimalMember(state, "last", range.last))
        return false;
    if (!state.ok(writer.endObject()) || !state.ok(writer.memberName("outputs")) ||
        !state.ok(writer.beginArray()))
        return false;
    for (const auto& output : job.outputs)
        if (!emitExportOutput(state, output))
            return false;
    if (!state.ok(writer.endArray()) || !state.ok(writer.memberName("enabled")) ||
        !state.ok(writer.booleanValue(job.enabled)))
        return false;
    if (job.labelColor) {
        if (!state.ok(writer.memberName("labelColor")) || !state.ok(writer.beginArray()))
            return false;
        for (const auto channel : *job.labelColor)
            if (!state.ok(writer.integerValue(channel)))
                return false;
        if (!state.ok(writer.endArray()))
            return false;
    }
    return state.ok(writer.endObject());
}

[[nodiscard]] bool emitExportQueue(EmitState& state) noexcept {
    auto& writer = state.writer;
    if (!state.ok(writer.memberName("exportQueue")) || !state.ok(writer.beginObject()) ||
        !state.ok(writer.memberName("jobs")) || !state.ok(writer.beginArray()))
        return false;
    for (const auto& job : state.project.exportJobs())
        if (!emitExportJob(state, job))
            return false;
    return state.ok(writer.endArray()) && state.ok(writer.endObject());
}
