#pragma once
#include <bloom/document/asset.hpp>
#include <bloom/media/provider/contract.hpp>
namespace bloom::runtime::video {
namespace provider = media::provider;
[[nodiscard]] provider::ProbeResult probeMetadata(const document::AssetRecord& asset);
} // namespace bloom::runtime::video
