// Included inside canonical_document.cpp's private namespace: one bounded writer walk.
[[nodiscard]] bool emitAssetLocator(EmitState& state, const bloom::document::AssetLocator& locator) noexcept {
    auto& writer = state.writer;
    if (!state.ok(writer.beginObject())) return false;
    for (const auto& [key, value] : {std::pair{"kind", locator.kind}, {"portability", locator.portability},
                                    {"path", locator.path}, {"relinkHint", locator.relinkHint}})
        if (!state.ok(writer.memberName(key)) || !state.ok(writer.stringValue(value))) return false;
    return state.ok(writer.endObject());
}
[[nodiscard]] bool emitAssetDigest(EmitState& state, const bloom::core::Sha256Digest digest) noexcept {
    const auto text = digest.toLowercaseHex();
    return state.ok(state.writer.stringValue(std::string_view(text.data(), text.size())));
}
[[nodiscard]] bool emitAssets(EmitState& state) noexcept {
    auto& writer = state.writer;
    if (!state.ok(writer.memberName("assets")) || !state.ok(writer.beginArray())) return false;
    for (const auto& asset : state.project.assets()) {
        if (!state.ok(writer.beginObject()) || !emitNamedId(state,"id",asset.id.value()) ||
            !state.ok(writer.memberName("kind")) ||
            !state.ok(writer.stringValue(asset.kind == bloom::document::AssetKind::Image
                                             ? "image"
                                             : asset.kind == bloom::document::AssetKind::Sequence
                                                   ? "sequence"
                                                   : asset.kind == bloom::document::AssetKind::Audio
                                                         ? "audio"
                                                         : asset.kind == bloom::document::AssetKind::Video ? "video" : asset.kind == bloom::document::AssetKind::Lut ? "lut" : "font")) ||
            !state.ok(writer.memberName("locator")) || !emitAssetLocator(state, asset.locator) ||
            !state.ok(writer.memberName("contentDigest")) || !emitAssetDigest(state, asset.contentDigest) ||
            !state.ok(writer.memberName("interpretation")) || !state.ok(writer.beginObject()) ||
            !state.ok(writer.memberName("colorSpace")) || !state.ok(writer.integerValue(static_cast<std::uint32_t>(asset.interpretation.colorSpace))) ||
            !state.ok(writer.memberName("inputColorSpaceId")) || !state.ok(writer.stringValue(asset.interpretation.inputColorSpaceId)) ||
            !state.ok(writer.memberName("alphaAssociation")) || !state.ok(writer.integerValue(static_cast<std::uint32_t>(asset.interpretation.alphaAssociation))) ||
            !state.ok(writer.endObject()) || !state.ok(writer.memberName("width")) || !state.ok(writer.integerValue(asset.width)) ||
            !state.ok(writer.memberName("height")) || !state.ok(writer.integerValue(asset.height)) ||
            !state.ok(writer.memberName("manifest")) || !state.ok(writer.beginObject()) ||
            !state.ok(writer.memberName("pattern")) || !state.ok(writer.stringValue(asset.manifest.pattern)) ||
            !state.ok(writer.memberName("padding")) || !state.ok(writer.integerValue(asset.manifest.padding)) ||
            !emitNamedSigned(state, "first", asset.manifest.first) || !emitNamedSigned(state,"last",asset.manifest.last) ||
            !state.ok(writer.memberName("members")) || !state.ok(writer.beginArray())) return false;
        for (const auto& member : asset.manifest.members) {
            if (!state.ok(writer.beginObject()) || !emitNamedSigned(state,"frame",member.frame) ||
                !state.ok(writer.memberName("locator")) || !emitAssetLocator(state,member.locator) ||
                !state.ok(writer.memberName("contentDigest")) || !emitAssetDigest(state,member.contentDigest) ||
                !state.ok(writer.endObject())) return false;
        }
        if (!state.ok(writer.endArray()) || !state.ok(writer.memberName("gaps")) || !state.ok(writer.beginArray())) return false;
        for (const auto frame : asset.manifest.gaps) {
            const auto text = bloom::project::formatCanonicalInt64(frame);
            if (!state.ok(writer.stringValue(text.view()))) return false;
        }
        if (!state.ok(writer.endArray()) || !state.ok(writer.endObject())) return false;
        if (asset.kind == bloom::document::AssetKind::Font) {
            if (!state.ok(writer.memberName("font")) || !state.ok(writer.beginObject()) ||
                !state.ok(writer.memberName("family")) ||
                !state.ok(writer.stringValue(asset.fontFamily)) ||
                !state.ok(writer.memberName("style")) ||
                !state.ok(writer.stringValue(asset.fontStyle)) ||
                !state.ok(writer.memberName("faceIndex")) ||
                !state.ok(writer.integerValue(asset.fontIndex)) || !state.ok(writer.endObject()))
                return false;
        }
        if (asset.kind == bloom::document::AssetKind::Audio) {
            if (!state.ok(writer.memberName("audio")) || !state.ok(writer.beginObject()) ||
                !state.ok(writer.memberName("rate")) || !state.ok(writer.integerValue(asset.rate)) ||
                !state.ok(writer.memberName("channels")) || !state.ok(writer.integerValue(asset.channels)) ||
                !emitNamedId(state, "frames", asset.frames) ||
                !state.ok(writer.memberName("duration")) ||
                !emitRational(state, asset.duration.numerator(), asset.duration.denominator()) ||
                !state.ok(writer.endObject())) return false;
        }
        if (asset.kind == bloom::document::AssetKind::Video) {
            if (!state.ok(writer.memberName("video")) || !state.ok(writer.beginObject()) ||
                !emitNamedId(state,"frames",asset.frames) || !state.ok(writer.memberName("duration")) ||
                !emitRational(state,asset.duration.numerator(),asset.duration.denominator()) ||
                !state.ok(writer.memberName("streams")) || !state.ok(writer.beginArray())) return false;
            for (const auto& stream : asset.videoStreams) {
                if (!state.ok(writer.beginObject())) return false;
                if (!state.ok(writer.memberName("id")) || !state.ok(writer.integerValue(stream.id))) return false;
                if (!state.ok(writer.memberName("kind")) || !state.ok(writer.integerValue(stream.kind))) return false;
                if (!state.ok(writer.memberName("codec")) || !state.ok(writer.stringValue(stream.codec))) return false;
                if (!state.ok(writer.memberName("profile")) || !state.ok(writer.stringValue(stream.profile))) return false;
                if (!state.ok(writer.memberName("pixelFormat")) || !state.ok(writer.stringValue(stream.pixelFormat))) return false;
                if (!state.ok(writer.memberName("timecode")) || !state.ok(writer.stringValue(stream.timecode))) return false;
                if (!state.ok(writer.memberName("timebase")) || !emitRational(state,stream.timebase.numerator(),stream.timebase.denominator())) return false;
                if (!state.ok(writer.memberName("framePeriod")) || !emitRational(state,stream.framePeriod.numerator(),stream.framePeriod.denominator())) return false;
                if (!state.ok(writer.memberName("duration")) || !emitRational(state,stream.duration.numerator(),stream.duration.denominator())) return false;
                if (!state.ok(writer.memberName("width")) || !state.ok(writer.integerValue(stream.width))) return false;
                if (!state.ok(writer.memberName("height")) || !state.ok(writer.integerValue(stream.height))) return false;
                if (!state.ok(writer.memberName("sampleRate")) || !state.ok(writer.integerValue(stream.sampleRate))) return false;
                if (!emitNamedSigned(state,"primaries",stream.primaries)) return false;
                if (!emitNamedSigned(state,"transfer",stream.transfer)) return false;
                if (!emitNamedSigned(state,"matrix",stream.matrix)) return false;
                if (!emitNamedSigned(state,"range",stream.range)) return false;
                if (!state.ok(writer.memberName("channelLayout")) || !state.ok(writer.beginArray())) return false;
                for (const auto& channel : stream.channelLayout)
                    if (!state.ok(writer.stringValue(channel))) return false;
                if (!state.ok(writer.endArray()) || !state.ok(writer.endObject())) return false;
            }
            if (!state.ok(writer.endArray()) || !state.ok(writer.endObject())) return false;
        }
        if (!state.ok(writer.memberName("name")) || !state.ok(writer.stringValue(asset.name))) return false;
        const auto assetFolder = asset.folder;
        if (assetFolder && !emitNamedId(state, "folder", assetFolder->value())) return false;
        if (!state.ok(writer.memberName("tags")) || !state.ok(writer.beginArray())) return false;
        for (const auto& tag : asset.tags)
            if (!state.ok(writer.stringValue(tag))) return false;
        if (!state.ok(writer.endArray()) || !emitNamedId(state, "order", asset.order)) return false;
        if (!state.ok(writer.endObject())) return false;
    }
    return state.ok(writer.endArray());
}

[[nodiscard]] bool emitAssetFolders(EmitState& state) noexcept {
    if (state.project.assetFolders().empty()) return true;
    auto& writer = state.writer;
    if (!state.ok(writer.memberName("assetFolders")) || !state.ok(writer.beginArray())) return false;
    for (const auto& folder : state.project.assetFolders()) {
        if (!state.ok(writer.beginObject()) || !emitNamedId(state, "id", folder.id.value()) ||
            !state.ok(writer.memberName("name")) || !state.ok(writer.stringValue(folder.name))) return false;
        const auto parent = folder.parent;
        if (parent && !emitNamedId(state, "parent", parent->value())) return false;
        if (!state.ok(writer.endObject())) return false;
    }
    return state.ok(writer.endArray());
}
