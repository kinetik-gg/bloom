#include <bloom/output/tiff_export_write.hpp>

namespace bloom::output {

namespace {

[[nodiscard]] TiffExportWriteErrorCodeV1
mapImageDiagnostic(const media::ImageDiagnosticCode diagnostic) noexcept {
    switch (diagnostic) {
    case media::ImageDiagnosticCode::ProviderMissing:
        return TiffExportWriteErrorCodeV1::ProviderMissing;
    case media::ImageDiagnosticCode::Cancelled:
        return TiffExportWriteErrorCodeV1::Cancelled;
    case media::ImageDiagnosticCode::None:
        return TiffExportWriteErrorCodeV1::None;
    default:
        return TiffExportWriteErrorCodeV1::ProviderFailed;
    }
}

} // namespace

TiffExportWriteResultV1
TiffExportWriterV1::run(const OutputAnalysisAttemptV1& attempt,
                        const std::filesystem::path& destination,
                        const media::ImageProvider* provider) const noexcept {
    if (attempt.preset() != OutputPresetV1::TiffRgba16SrgbV1 || attempt.frame() == nullptr)
        return TiffExportWriteResultV1::failed(TiffExportWriteErrorCodeV1::ProviderFailed);
    if (provider == nullptr || !provider->encode)
        return TiffExportWriteResultV1::failed(TiffExportWriteErrorCodeV1::ProviderMissing);

    const std::shared_ptr<const render::Rgba32fImage> image(attempt.frame(),
                                                            &attempt.frame()->processImage());
    const auto result = media::encodeImage({.destination = destination, .image = image}, provider);
    if (result.cancelled)
        return TiffExportWriteResultV1::cancelledResult();
    if (!result.value.has_value())
        return TiffExportWriteResultV1::failed(mapImageDiagnostic(result.code));
    return result.value->written
               ? TiffExportWriteResultV1::writtenResult()
               : TiffExportWriteResultV1::failed(TiffExportWriteErrorCodeV1::ProviderFailed);
}

} // namespace bloom::output
