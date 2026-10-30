#include <bloom/output/display_output.hpp>

namespace bloom::output {
std::shared_ptr<const PreparedOutputDisplayV1>
PreparedOutputDisplayV1::prepare(const runtime::EvaluationColorIntent& intent,
                                 std::string_view display, std::string_view view) {
    const auto revision = intent.ocioConfigRevision == core::Sha256Digest{}
                              ? color::kBloomNeutralV1ConfigDigest
                              : intent.ocioConfigRevision;
    const auto uri =
        revision == color::kBloomNeutralV1ConfigDigest ? color::kBloomNeutralV1ConfigUri
        : intent.ocioConfigUri == runtime::kBloomNeutralOcioConfigUri ? color::kAcesCgV1ConfigUri
                                                                      : intent.ocioConfigUri;
    auto resolution = color::resolveOcioBuiltIn(color::OcioConfigLocatorKind::BloomBuiltIn, uri,
                                                revision, intent.workingColorSpaceId);
    auto config = std::move(resolution).takeResolved();
    if (!config)
        return {};
    if (display.empty())
        display = config->displayName();
    if (view.empty())
        view = config->viewName();
    auto built = color::buildCpuDisplayProcessorForView(*config, display, view);
    auto handle = std::move(built).takeHandle();
    if (!handle)
        return {};
    auto result = std::shared_ptr<PreparedOutputDisplayV1>(new PreparedOutputDisplayV1());
    result->processor_ =
        std::make_shared<const color::PreparedCpuDisplayProcessorHandle>(std::move(*handle));
    result->display_ = std::string(display);
    result->view_ = std::string(view);
    result->description_ = "display: " + std::string(display) + "; view: " + std::string(view);
    const auto hex = revision.toLowercaseHex();
    result->description_ += "; OCIO: " + std::string(hex.data(), hex.size());
    core::Sha256Hasher hasher;
    (void)hasher.update(result->processor_->identity().canonicalBytes());
    result->digest_ = hasher.finalize();
    return result;
}
OutputColourTagsV1 outputColourTagsV1(const std::string_view display) noexcept {
    const auto has = [display](const std::string_view text) {
        return display.find(text) != std::string_view::npos;
    };
    // H.273: primaries 1 BT.709, 9 BT.2020, 12 P3-D65; transfer 1 BT.709, 13 sRGB, 16 PQ,
    // 18 HLG; matrix 1 BT.709, 9 BT.2020 non-constant.
    if (has("2100") && has("PQ"))
        return {9, 16, 9, 1};
    if (has("2100") && has("HLG"))
        return {9, 18, 9, 1};
    if (has("P3-D65") || has("Display P3"))
        return {12, has("1886") || has("Gamma 2.6") ? 1 : 13, 1, 1};
    if (has("1886") || has("Rec.709") || has("BT.709") || has("Rec709"))
        return {1, 1, 1, 1};
    if (has("sRGB"))
        return {1, 13, 1, 1};
    return {2, 2, 1, 1};
}
std::uint64_t outputLookEffectCountV1(const runtime::CompiledCompositionPlan& plan) noexcept {
    std::uint64_t result = 0;
    for (const auto& operation : plan.operations())
        if (const auto* effect = std::get_if<runtime::CompiledImageEffect>(&operation);
            effect && effect->look && !effect->bypass)
            ++result;
    for (const auto& nested : plan.nestedPlans())
        if (nested)
            result += outputLookEffectCountV1(*nested);
    return result;
}
std::string outputLookDescriptionV1(const runtime::ProcessFrameIdentity& identity) {
    if (identity.bypassLookNodes)
        return "Look: OFF (handoff)";
    return "look: baked (" +
           std::to_string(identity.plan ? outputLookEffectCountV1(*identity.plan) : 0) +
           " look-tagged effects)";
}
} // namespace bloom::output
