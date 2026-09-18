#include <bloom/output/media_output.hpp>
#include <bloom/output/tiff_export_write.hpp>
#include <fstream>

namespace bloom::output {
TiffExportWriteResultV1
TiffExportWriterV1::run(const OutputAnalysisAttemptV1& attempt,
                        const std::filesystem::path& destination,
                        const media::ImageProvider* provider,
                        const runtime::CancellationToken& cancellation) const noexcept {
    using namespace media::provider;
    if (attempt.preset() != OutputPresetV1::TiffRgba16SrgbV1 || !attempt.frame() ||
        !attempt.approvable())
        return TiffExportWriteResultV1::failed(TiffExportWriteErrorCodeV1::ProviderFailed);
    // Retain the explicit injection seam for callers with their own qualified provider.
    if (provider && provider->encode) {
        const std::shared_ptr<const render::Rgba32fImage> image(attempt.frame(),
                                                                &attempt.frame()->processImage());
        const auto encoded =
            media::encodeImage({.destination = destination, .image = image}, provider);
        if (encoded.cancelled)
            return TiffExportWriteResultV1::cancelledResult();
        // A bare write callback supplies no reopen evidence; it cannot authorize publication.
        return TiffExportWriteResultV1::failed(TiffExportWriteErrorCodeV1::ProviderFailed);
    }
    try {
        auto fail = [](const Unavailable& e) {
            return e.reason == Error::Cancelled
                       ? TiffExportWriteResultV1::cancelledResult()
                       : TiffExportWriteResultV1::failed(
                             e.reason == Error::Unavailable
                                 ? TiffExportWriteErrorCodeV1::ProviderMissing
                                 : TiffExportWriteErrorCodeV1::ProviderFailed);
        };
        const auto cancel = [&] { return cancellation.isCancellationRequested(); };
        auto prepared = prepareMediaRgba16V1(attempt.frame()->processImage(), {0, 1}, cancel);
        if (const auto* e = std::get_if<Unavailable>(&prepared))
            return fail(*e);
        auto frame = std::get<FrameProduct>(std::move(prepared));
        EncodeSettingsV1 settings;
        settings.container = "tiff";
        settings.videoCodec = "tiff";
        settings.profile = "rgba16";
        settings.width = frame.planes[0].width;
        settings.height = frame.planes[0].height;
        EncodeSessionV1 session({}, cancel);
        if (const auto e = session.begin(settings))
            return fail(*e);
        if (const auto e = session.video(std::move(frame)))
            return fail(*e);
        auto verified = session.finish();
        if (const auto* e = std::get_if<Unavailable>(&verified))
            return fail(*e);
        const auto qc = std::get<EncodeQcV1>(verified);
        std::ofstream file(destination, std::ios::binary | std::ios::trunc);
        if (!file)
            return TiffExportWriteResultV1::failed(TiffExportWriteErrorCodeV1::ProviderFailed);
        core::Sha256Hasher hasher;
        for (std::uint64_t offset = 0; offset < qc.bytes;) {
            auto reply = session.read(offset);
            if (const auto* e = std::get_if<Unavailable>(&reply))
                return fail(*e);
            const auto& chunk = std::get<EncodedChunkV1>(reply);
            if (chunk.bytes.size() > qc.bytes - offset || !hasher.update(chunk.bytes))
                return TiffExportWriteResultV1::failed(TiffExportWriteErrorCodeV1::ProviderFailed);
            file.write(reinterpret_cast<const char*>(chunk.bytes.data()),
                       static_cast<std::streamsize>(chunk.bytes.size()));
            offset += chunk.bytes.size();
        }
        file.close();
        if (!file || hasher.finalize() != qc.artifact)
            return TiffExportWriteResultV1::failed(TiffExportWriteErrorCodeV1::ProviderFailed);
        if (const auto e = session.close())
            return fail(*e);
        auto result = TiffExportWriteResultV1::writtenResult();
        result.artifactDigest = qc.artifact;
        result.semanticDigest = qc.firstFrame;
        result.artifactBytes = qc.bytes;
        return result;
    } catch (const std::exception&) {
        return TiffExportWriteResultV1::failed(TiffExportWriteErrorCodeV1::ProviderFailed);
    }
}
} // namespace bloom::output
