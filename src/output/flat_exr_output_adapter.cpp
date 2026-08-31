#include "flat_exr_preset_contract.hpp"
#include "output_analysis_numeric.hpp"
// output_semantic_identity.hpp is module-private (no public bloom/output/ path exists for it --
// see docs/architecture/frame-output.md's "module-private Output Semantic Identity" note); this
// translation unit lives inside src/output/ itself, so the quoted include resolves relative to
// this file exactly like every other src/output/*.cpp that touches it (e.g.
// output_semantic_identity_ownership.cpp). Only kFlatExrRec709D65ChromaticitiesBitsV1 is used
// here; this header's own public declaration never names the type.
#include "output_semantic_identity.hpp"

#include <bloom/output/flat_exr_output_adapter.hpp>
#include <bloom/output/output_limits.hpp>
#include <bloom/render/image.hpp>

// OpenEXR/Imath types are private to this translation unit only -- design decision 1
// (docs/architecture/frame-output.md "Flat OpenEXR Preset Version 1"): no OpenEXR/Imath type,
// enum, pointer, or exception may appear in a public bloom/output header. The classic C++ API
// (Imf::Header + Imf::OutputFile) is chosen over the OpenEXRCore C API specifically because a
// plain, non-multipart, non-deep Header constructed here inserts EXACTLY the eight base
// attributes its constructor writes (displayWindow, dataWindow, pixelAspectRatio,
// screenWindowCenter, screenWindowWidth, lineOrder, compression, channels) plus the two this
// writer inserts itself (chromaticities, colorInteropID) -- ten total, nothing else. Verified
// against the OpenEXR 3.4.15 source: Header::setVersion()/"version" is only inserted for deep
// data (never reached by a flat scanline Header); Header::setChunkCount()/"chunkCount" is only
// called by the deep and multipart output paths, never by the classic scanline OutputFile; and
// OutputFile::writeMagicNumberAndVersionField() only sets Header::setType("scanlineimage") when
// header.hasType() is already true, which it never is for a Header built through this
// constructor. A plain scanline OutputFile therefore writes precisely the attributes inserted
// into its Header -- the exact control design decision 2 requires -- which the header-conformance
// test proves by independently decoding this writer's own output.
#include <ImathBox.h>
#include <ImathVec.h>
#include <ImfChannelList.h>
#include <ImfChromaticities.h>
#include <ImfChromaticitiesAttribute.h>
#include <ImfFrameBuffer.h>
#include <ImfHeader.h>
#include <ImfOutputFile.h>
#include <ImfStandardAttributes.h>
#include <ImfStringAttribute.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <optional>
#include <string>
#include <system_error>

namespace {

namespace output = bloom::output;
namespace render = bloom::render;
namespace runtime = bloom::runtime;

// Best-effort cleanup after a non-Written outcome. Never throws; reports whether the destination
// is now confirmed absent.
[[nodiscard]] bool removeDestinationBestEffort(const std::filesystem::path& destination) noexcept {
    std::error_code removeError;
    std::filesystem::remove(destination, removeError);
    std::error_code existsError;
    const bool stillExists = std::filesystem::exists(destination, existsError);
    return !existsError && !stillExists;
}

[[nodiscard]] output::FlatExrWriteResultV1 fail(const output::FlatExrWriteErrorCodeV1 error,
                                                const std::filesystem::path& destination,
                                                const bool destinationWasCreated) noexcept {
    const bool removed = !destinationWasCreated || removeDestinationBestEffort(destination);
    return output::FlatExrWriteResultV1::failed(error, removed);
}

struct PreparedGeometry final {
    Imath::Box2i dataWindow;
    Imath::Box2i displayWindow;
    float pixelAspectRatio = 1.0F;
};

// Checked signed-32 inclusive fit AND the real, narrower OpenEXR library ceiling
// (kFlatExrLibraryCoordinateCeilingV1 -- see flat_exr_preset_contract.hpp for the discrepancy this
// documents against the doc's literal "signed-32" language).
[[nodiscard]] bool windowsFitWriterDomain(const render::ImageWindow dataWindow,
                                          const render::ImageWindow displayWindow) noexcept {
    return output::detail::outputAnalysisInclusiveAxisFitsSigned32V1(dataWindow.originX(),
                                                                     dataWindow.extent().width()) &&
           output::detail::outputAnalysisInclusiveAxisFitsSigned32V1(
               dataWindow.originY(), dataWindow.extent().height()) &&
           output::detail::outputAnalysisInclusiveAxisFitsSigned32V1(
               displayWindow.originX(), displayWindow.extent().width()) &&
           output::detail::outputAnalysisInclusiveAxisFitsSigned32V1(
               displayWindow.originY(), displayWindow.extent().height()) &&
           output::detail::flatExrWindowWithinLibraryCeilingV1(dataWindow.originX(),
                                                               dataWindow.extent().width()) &&
           output::detail::flatExrWindowWithinLibraryCeilingV1(dataWindow.originY(),
                                                               dataWindow.extent().height()) &&
           output::detail::flatExrWindowWithinLibraryCeilingV1(displayWindow.originX(),
                                                               displayWindow.extent().width()) &&
           output::detail::flatExrWindowWithinLibraryCeilingV1(displayWindow.originY(),
                                                               displayWindow.extent().height());
}

// All checked, before-staging conversions: signed-32 inclusive windows (within the writer domain
// above) and the single round-to-nearest-ties-even binary32 pixel-aspect conversion (reused from
// the exact function the existing digest preflight already validates against, so a writer and a
// later semantic-identity preflight can never silently disagree).
[[nodiscard]] std::optional<PreparedGeometry>
prepareGeometry(const render::Rgba32fImageDescriptor& descriptor) noexcept {
    const auto dataWindow = descriptor.dataWindow();
    const auto displayWindow = descriptor.displayWindow();
    if (!::windowsFitWriterDomain(dataWindow, displayWindow)) {
        return std::nullopt;
    }
    const auto pixelAspect = descriptor.pixelAspect();
    const auto rounded = output::detail::roundOutputAnalysisPositiveRationalToBinary32V1(
        {.numerator = pixelAspect.numerator(), .denominator = pixelAspect.denominator()});
    if (!rounded) {
        return std::nullopt;
    }

    PreparedGeometry geometry;
    geometry.dataWindow = Imath::Box2i(
        Imath::V2i(static_cast<int>(dataWindow.originX()), static_cast<int>(dataWindow.originY())),
        Imath::V2i(static_cast<int>(dataWindow.originX() + dataWindow.extent().width() - 1),
                   static_cast<int>(dataWindow.originY() + dataWindow.extent().height() - 1)));
    geometry.displayWindow = Imath::Box2i(
        Imath::V2i(static_cast<int>(displayWindow.originX()),
                   static_cast<int>(displayWindow.originY())),
        Imath::V2i(
            static_cast<int>(displayWindow.originX() + displayWindow.extent().width() - 1),
            static_cast<int>(displayWindow.originY() + displayWindow.extent().height() - 1)));
    geometry.pixelAspectRatio = std::bit_cast<float>(rounded->bits);
    return geometry;
}

void reportProgress(const output::FlatExrWriteProgressCallbackV1& callback,
                    const output::FlatExrWriteProgressV1& progress) noexcept {
    if (!callback) {
        return;
    }
    try {
        callback(progress);
    } catch (...) {
        // Monitoring cannot change a portable write outcome.
        return;
    }
}

[[nodiscard]] bool withinResourceLimits(const render::Rgba32fImageDescriptor& descriptor) noexcept {
    const auto dataExtent = descriptor.dataWindow().extent();
    const auto displayExtent = descriptor.displayWindow().extent();
    if (dataExtent.width() > output::kOutputAnalysisMaximumDimensionV1 ||
        dataExtent.height() > output::kOutputAnalysisMaximumDimensionV1 ||
        displayExtent.width() > output::kOutputAnalysisMaximumDimensionV1 ||
        displayExtent.height() > output::kOutputAnalysisMaximumDimensionV1) {
        return false;
    }
    const auto pixelCount = descriptor.layout().pixelCount;
    if (pixelCount > output::kOutputAnalysisMaximumPixelCountV1 ||
        descriptor.layout().pixelStorageBytes > output::kOutputAnalysisMaximumProcessPixelBytesV1) {
        return false;
    }
    return true;
}

} // namespace

namespace bloom::output {

FlatExrWriteResultV1::FlatExrWriteResultV1(const FlatExrWriteStatusV1 status,
                                           const FlatExrWriteErrorCodeV1 error,
                                           const bool destinationRemoved) noexcept
    : status_(status), error_(error), destinationRemoved_(destinationRemoved) {}

FlatExrWriteResultV1 FlatExrWriteResultV1::written() noexcept {
    return {FlatExrWriteStatusV1::Written, FlatExrWriteErrorCodeV1::None, true};
}

FlatExrWriteResultV1 FlatExrWriteResultV1::cancelled(const bool destinationRemoved) noexcept {
    return {FlatExrWriteStatusV1::Cancelled, FlatExrWriteErrorCodeV1::None, destinationRemoved};
}

FlatExrWriteResultV1 FlatExrWriteResultV1::failed(const FlatExrWriteErrorCodeV1 error,
                                                  const bool destinationRemoved) noexcept {
    return {FlatExrWriteStatusV1::Failed,
            error == FlatExrWriteErrorCodeV1::None ? FlatExrWriteErrorCodeV1::InternalInvariant
                                                   : error,
            destinationRemoved};
}

FlatExrWriteResultV1 FlatExrRgba32fLinRec709SceneWriterV1::write(
    const runtime::ProcessFrame& frame, const std::filesystem::path& destination,
    const runtime::CancellationToken& cancellation,
    const FlatExrWriteProgressCallbackV1& progress) const noexcept {
    const auto& image = frame.processImage();
    if (!image.isValid()) {
        return ::fail(FlatExrWriteErrorCodeV1::InternalInvariant, destination, false);
    }
    const auto* descriptor = image.descriptor();
    if (descriptor == nullptr) {
        return ::fail(FlatExrWriteErrorCodeV1::InternalInvariant, destination, false);
    }
    if (!::withinResourceLimits(*descriptor)) {
        return ::fail(FlatExrWriteErrorCodeV1::ResourceLimitExceeded, destination, false);
    }
    const auto geometry = ::prepareGeometry(*descriptor);
    if (!geometry) {
        // prepareGeometry only reports WindowOutOfRange or InvalidPixelAspectRatio; distinguish
        // by re-checking the window fit alone (cheap, deterministic).
        const bool windowsFit =
            ::windowsFitWriterDomain(descriptor->dataWindow(), descriptor->displayWindow());
        return ::fail(windowsFit ? FlatExrWriteErrorCodeV1::InvalidPixelAspectRatio
                                 : FlatExrWriteErrorCodeV1::WindowOutOfRange,
                      destination, false);
    }
    if (cancellation.isCancellationRequested()) {
        return FlatExrWriteResultV1::cancelled(true);
    }

    const auto pixels = image.pixels();
    const auto rowStrideBytes = descriptor->layout().rowStrideBytes;
    const auto height = descriptor->dataWindow().extent().height();
    if (height == 0 || pixels.empty()) {
        return ::fail(FlatExrWriteErrorCodeV1::InternalInvariant, destination, false);
    }

    bool destinationCreated = false;
    try {
        Imf::Header header(geometry->displayWindow, geometry->dataWindow,
                           geometry->pixelAspectRatio, Imath::V2f(0.0F, 0.0F), 1.0F,
                           Imf::INCREASING_Y, Imf::ZIP_COMPRESSION);
        header.channels().insert("R", Imf::Channel(Imf::FLOAT));
        header.channels().insert("G", Imf::Channel(Imf::FLOAT));
        header.channels().insert("B", Imf::Channel(Imf::FLOAT));
        header.channels().insert("A", Imf::Channel(Imf::FLOAT));

        std::array<Imath::V2f, 4> chromaBits{};
        for (std::size_t index = 0; index < 4; ++index) {
            chromaBits[index] = Imath::V2f(
                std::bit_cast<float>(kFlatExrRec709D65ChromaticitiesBitsV1[index * 2]),
                std::bit_cast<float>(kFlatExrRec709D65ChromaticitiesBitsV1[index * 2 + 1]));
        }
        Imf::addChromaticities(header, Imf::Chromaticities(chromaBits[0], chromaBits[1],
                                                           chromaBits[2], chromaBits[3]));
        header.insert("colorInteropID",
                      Imf::StringAttribute(std::string(detail::kFlatExrColorInteropIdV1)));

        destinationCreated = true;
        Imf::OutputFile outputFile(destination.string().c_str(), header, 1);

        const auto* base = reinterpret_cast<const char*>(pixels.data());
        constexpr std::size_t xStride = sizeof(render::Rgba32f);
        Imf::FrameBuffer frameBuffer;
        frameBuffer.insert("R", Imf::Slice::Make(Imf::FLOAT, base + 0 * sizeof(float),
                                                 geometry->dataWindow, xStride, rowStrideBytes));
        frameBuffer.insert("G", Imf::Slice::Make(Imf::FLOAT, base + 1 * sizeof(float),
                                                 geometry->dataWindow, xStride, rowStrideBytes));
        frameBuffer.insert("B", Imf::Slice::Make(Imf::FLOAT, base + 2 * sizeof(float),
                                                 geometry->dataWindow, xStride, rowStrideBytes));
        frameBuffer.insert("A", Imf::Slice::Make(Imf::FLOAT, base + 3 * sizeof(float),
                                                 geometry->dataWindow, xStride, rowStrideBytes));
        outputFile.setFrameBuffer(frameBuffer);

        const auto rowsPerChunk = static_cast<std::uint32_t>(
            std::max<std::size_t>(1, kOutputAdapterMaximumStreamingChunkBytesV1 /
                                         std::max<std::size_t>(rowStrideBytes, 1)));
        std::uint32_t remaining = height;
        std::uint32_t completed = 0;
        ::reportProgress(progress, {completed, height});
        while (remaining > 0) {
            if (cancellation.isCancellationRequested()) {
                return FlatExrWriteResultV1::cancelled(::removeDestinationBestEffort(destination));
            }
            const auto chunk = std::min(rowsPerChunk, remaining);
            outputFile.writePixels(static_cast<int>(chunk));
            remaining -= chunk;
            completed += chunk;
            ::reportProgress(progress, {completed, height});
        }
    } catch (const std::bad_alloc&) {
        return ::fail(FlatExrWriteErrorCodeV1::AllocationFailure, destination, destinationCreated);
    } catch (const std::exception&) {
        return ::fail(destinationCreated ? FlatExrWriteErrorCodeV1::IoFailure
                                         : FlatExrWriteErrorCodeV1::DestinationUnavailable,
                      destination, destinationCreated);
    } catch (...) {
        return ::fail(FlatExrWriteErrorCodeV1::InternalInvariant, destination, destinationCreated);
    }

    return FlatExrWriteResultV1::written();
}

} // namespace bloom::output
