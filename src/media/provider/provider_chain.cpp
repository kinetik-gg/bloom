#include <bloom/media/provider/provider_chain.hpp>

#include <algorithm>
#include <utility>

namespace bloom::media::provider {
namespace {
std::string_view causeName(const MediaFallbackCauseV1 cause) {
    switch (cause) {
    case MediaFallbackCauseV1::NotInstalled:
        return "not installed";
    case MediaFallbackCauseV1::NotDeclared:
        return "does not declare the operation";
    case MediaFallbackCauseV1::Failed:
        return "failed";
    }
    return "skipped";
}
bool decodeRole(const Role role) { return role == Role::VideoDecode || role == Role::AudioDecode; }
bool encodeRole(const Role role) { return role == Role::VideoEncode || role == Role::AudioEncode; }
} // namespace

std::string mediaCodecFamilyV1(const std::string_view codec) {
    if (codec == "prores_ks" || codec == "prores_aw" || codec == "prores")
        return "prores";
    if (codec.starts_with("dnxhr") || codec == "dnxhd")
        return "dnxhd";
    if (codec.starts_with("pcm_"))
        return "pcm";
    if (codec == "libopenh264" || codec == "h264_nvenc" || codec == "h264_vaapi" ||
        codec == "h264_videotoolbox" || codec == "h264_mf")
        return "h264";
    if (codec == "libdav1d" || codec == "libsvtav1")
        return "av1";
    if (codec == "libvpx-vp9")
        return "vp9";
    if (codec == "libopus")
        return "opus";
    if (codec == "libkvazaar" || codec == "hevc_nvenc" || codec == "hevc_vaapi" ||
        codec == "hevc_videotoolbox" || codec == "hevc_mf")
        return "hevc";
    return std::string(codec);
}

bool declaresMediaOperationV1(const Handshake& handshake, const MediaOperationV1& operation) {
    const auto family = mediaCodecFamilyV1(operation.codec);
    const auto& declarations = handshake.declarations;
    if (operation.direction == MediaDirectionV1::Decode)
        return std::ranges::any_of(declarations, [&](const ProviderDeclaration& d) {
            return decodeRole(d.capability.role) &&
                   mediaCodecFamilyV1(d.capability.codec) == family;
        });
    const bool codec =
        family.empty() || std::ranges::any_of(declarations, [&](const ProviderDeclaration& d) {
            return encodeRole(d.capability.role) &&
                   mediaCodecFamilyV1(d.capability.codec) == family;
        });
    const bool container =
        operation.container.empty() ||
        std::ranges::any_of(declarations, [&](const ProviderDeclaration& d) {
            return d.capability.role == Role::Mux && d.capability.container == operation.container;
        });
    return codec && container;
}

bool mediaFallbackPermittedV1(const Unavailable& error) noexcept {
    return error.reason != Error::Cancelled && error.reason != Error::Shutdown &&
           error.reason != Error::SourceChanged;
}

std::string MediaProviderProvenanceV1::diagnostic() const {
    std::string text = provider.empty() ? std::string("no provider") : provider;
    if (!adjustment.empty())
        text += " [delivered: " + adjustment + "]";
    if (fallbacks.empty())
        return text;
    text += " (after";
    for (std::size_t i = 0; i < fallbacks.size(); ++i) {
        const auto& fallback = fallbacks[i];
        text += i == 0 ? " " : "; ";
        text += fallback.provider;
        text += ": ";
        text += causeName(fallback.cause);
        if (fallback.cause == MediaFallbackCauseV1::Failed) {
            text += " [";
            text += std::to_string(static_cast<int>(fallback.error));
            text += "]";
        }
        if (!fallback.detail.empty()) {
            text += " ";
            text += fallback.detail;
        }
    }
    text += ")";
    return text;
}

MediaProviderChainV1::MediaProviderChainV1(std::vector<MediaProviderCandidateV1> candidates)
    : candidates_(std::move(candidates)) {}

std::optional<std::size_t> MediaProviderChainV1::next(const MediaOperationV1& operation,
                                                      const std::size_t from,
                                                      MediaProviderProvenanceV1& provenance) const {
    for (auto index = from; index < candidates_.size(); ++index) {
        const auto& candidate = candidates_[index];
        if (!candidate.installed || !candidate.installed()) {
            provenance.fallbacks.push_back(
                {candidate.id, MediaFallbackCauseV1::NotInstalled, Error::Unavailable, {}});
            continue;
        }
        if (candidate.declares && !candidate.declares(operation)) {
            provenance.fallbacks.push_back(
                {candidate.id, MediaFallbackCauseV1::NotDeclared, Error::Unavailable, {}});
            continue;
        }
        return index;
    }
    return std::nullopt;
}

std::string MediaProviderChainV1::describe(const MediaOperationV1& operation) {
    std::string text = operation.direction == MediaDirectionV1::Decode ? "decode " : "encode ";
    text += operation.codec.empty() ? std::string("(no codec)") : operation.codec;
    if (!operation.container.empty())
        text += " in " + operation.container;
    return text;
}

} // namespace bloom::media::provider
