#include "flat_exr_preset_contract.hpp"
#include "output_analysis_numeric.hpp"
// output_semantic_identity.hpp is module-private (no public bloom/output/ path exists for it --
// docs/architecture/frame-output.md calls it "the module-private Output Semantic Identity version
// 1 streaming serializer/preparer"); this translation unit lives inside src/output/ itself, so the
// quoted include resolves relative to this file, matching every other src/output/*.cpp that
// touches it. This is also where the friend seam class below (forward-declared in that header as
// `detail::FlatExrRgba32fLinRec709SceneSemanticPayloadVerifierV1`) is defined.
#include "output_semantic_identity.hpp"

#include <bloom/output/flat_exr_reopen_verifier.hpp>
#include <bloom/output/output_limits.hpp>
#include <bloom/render/image.hpp>

// OpenEXR/Imath types are private to this translation unit only -- see flat_exr_output_adapter.cpp
// for the API-choice rationale shared by the writer and this verifier. `Imf::Header::begin()`/
// `end()` give exhaustive attribute enumeration (used to prove the closed ten-entry allowlist
// admits no extra attribute) and `Imf::InputFile` gives typed access to every standard attribute
// plus `findTypedAttribute<T>` for the one custom attribute (`colorInteropID`), all without this
// verifier ever exposing an OpenEXR/Imath type in a public header.
#include <ImathBox.h>
#include <ImathVec.h>
#include <ImfChannelList.h>
#include <ImfChromaticities.h>
#include <ImfChromaticitiesAttribute.h>
#include <ImfFrameBuffer.h>
#include <ImfHeader.h>
#include <ImfInputFile.h>
#include <ImfStandardAttributes.h>
#include <ImfStringAttribute.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <fstream>
#include <iterator>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace output = bloom::output;
namespace render = bloom::render;
namespace runtime = bloom::runtime;

// Positional aggregate construction (never designated) so every FlatExrVerifyDiagnosticV1 site
// stays a one-line call regardless of how many trailing fields default.
[[nodiscard]] output::FlatExrVerifyDiagnosticV1
diag(const output::FlatExrVerifyErrorCodeV1 code, std::string attributeName = {},
     const std::optional<std::int64_t> scanlineY = std::nullopt,
     const std::optional<std::uint8_t> channelIndex = std::nullopt) noexcept {
    return output::FlatExrVerifyDiagnosticV1{code, std::move(attributeName), scanlineY,
                                             channelIndex};
}

void reportScanProgress(const output::FlatExrVerifyScanProgressCallbackV1& callback,
                        const output::FlatExrVerifyScanProgressV1& progress) noexcept {
    if (!callback) {
        return;
    }
    try {
        callback(progress);
    } catch (...) {
        // Monitoring cannot change a portable verification outcome.
        return;
    }
}

[[nodiscard]] constexpr std::optional<std::uint64_t>
multiplyChecked(const std::uint64_t left, const std::uint64_t right) noexcept {
    if (right != 0 && left > std::numeric_limits<std::uint64_t>::max() / right) {
        return std::nullopt;
    }
    return left * right;
}

struct VersionFieldCheck final {
    bool ok = false;
    output::FlatExrVerifyErrorCodeV1 error = output::FlatExrVerifyErrorCodeV1::InternalInvariant;
};

// Reads the first 8 bytes directly (magic number, then the 8-bit version number and 24-bit
// feature-flags field) without involving OpenEXR at all, so this specific check can never be
// softened by a library's own tolerant interpretation of a malformed version field.
[[nodiscard]] VersionFieldCheck checkVersionField(const std::filesystem::path& path) noexcept {
    try {
        std::ifstream stream(path, std::ios::binary);
        if (!stream) {
            return {false, output::FlatExrVerifyErrorCodeV1::SourceUnavailable};
        }
        std::array<unsigned char, 8> header{};
        stream.read(reinterpret_cast<char*>(header.data()),
                    static_cast<std::streamsize>(header.size()));
        if (!stream || stream.gcount() != static_cast<std::streamsize>(header.size())) {
            return {false, output::FlatExrVerifyErrorCodeV1::TruncatedFile};
        }
        const auto magic = static_cast<std::uint32_t>(header[0]) |
                           (static_cast<std::uint32_t>(header[1]) << 8U) |
                           (static_cast<std::uint32_t>(header[2]) << 16U) |
                           (static_cast<std::uint32_t>(header[3]) << 24U);
        if (magic != 20'000'630U) {
            return {false, output::FlatExrVerifyErrorCodeV1::HeaderParseFailed};
        }
        const auto versionField = static_cast<std::uint32_t>(header[4]) |
                                  (static_cast<std::uint32_t>(header[5]) << 8U) |
                                  (static_cast<std::uint32_t>(header[6]) << 16U) |
                                  (static_cast<std::uint32_t>(header[7]) << 24U);
        if ((versionField & 0xFFU) != 2U || (versionField & 0xFFFFFF00U) != 0U) {
            return {false, output::FlatExrVerifyErrorCodeV1::InvalidVersionField};
        }
        return {true, output::FlatExrVerifyErrorCodeV1::None};
    } catch (...) {
        return {false, output::FlatExrVerifyErrorCodeV1::SourceUnavailable};
    }
}

[[nodiscard]] std::optional<Imath::Box2i> inclusiveBox(const render::ImageWindow window) noexcept {
    if (!output::detail::outputAnalysisInclusiveAxisFitsSigned32V1(window.originX(),
                                                                   window.extent().width()) ||
        !output::detail::outputAnalysisInclusiveAxisFitsSigned32V1(window.originY(),
                                                                   window.extent().height())) {
        return std::nullopt;
    }
    return Imath::Box2i(
        Imath::V2i(static_cast<int>(window.originX()), static_cast<int>(window.originY())),
        Imath::V2i(static_cast<int>(window.originX() + window.extent().width() - 1),
                   static_cast<int>(window.originY() + window.extent().height() - 1)));
}

[[nodiscard]] std::optional<output::FlatExrVerifyDiagnosticV1>
checkAttributeAllowlist(const Imf::Header& header) noexcept {
    using output::FlatExrVerifyErrorCodeV1;
    std::array<bool, output::detail::kFlatExrHeaderAttributesV1.size()> seen{};
    for (auto it = header.begin(); it != header.end(); ++it) {
        const std::string_view name = it.name();
        const auto* const matchIt =
            std::ranges::find_if(output::detail::kFlatExrHeaderAttributesV1,
                                 [&](const auto& entry) { return entry.name == name; });
        if (matchIt == output::detail::kFlatExrHeaderAttributesV1.end()) {
            return ::diag(FlatExrVerifyErrorCodeV1::UnexpectedAttribute, std::string(name));
        }
        const auto index = static_cast<std::size_t>(
            std::distance(output::detail::kFlatExrHeaderAttributesV1.begin(), matchIt));
        seen[index] = true;
        const std::string_view typeName = it.attribute().typeName();
        if (typeName != matchIt->openExrTypeName) {
            return ::diag(FlatExrVerifyErrorCodeV1::AttributeTypeMismatch, std::string(name));
        }
    }
    for (std::size_t index = 0; index < seen.size(); ++index) {
        if (!seen[index]) {
            return ::diag(FlatExrVerifyErrorCodeV1::MissingAttribute,
                          std::string(output::detail::kFlatExrHeaderAttributesV1[index].name));
        }
    }
    return std::nullopt;
}

[[nodiscard]] std::optional<output::FlatExrVerifyDiagnosticV1>
checkAttributeValues(const Imf::Header& header, const Imath::Box2i& expectedDataWindow,
                     const Imath::Box2i& expectedDisplayWindow,
                     const std::uint32_t expectedPixelAspectBits) noexcept {
    using output::FlatExrVerifyErrorCodeV1;
    if (header.dataWindow() != expectedDataWindow) {
        return ::diag(FlatExrVerifyErrorCodeV1::WindowMismatch, "dataWindow");
    }
    if (header.displayWindow() != expectedDisplayWindow) {
        return ::diag(FlatExrVerifyErrorCodeV1::WindowMismatch, "displayWindow");
    }
    if (header.lineOrder() != Imf::INCREASING_Y) {
        return ::diag(FlatExrVerifyErrorCodeV1::AttributeValueMismatch, "lineOrder");
    }
    if (header.compression() != Imf::ZIP_COMPRESSION) {
        return ::diag(FlatExrVerifyErrorCodeV1::AttributeValueMismatch, "compression");
    }
    if (std::bit_cast<std::uint32_t>(header.pixelAspectRatio()) != expectedPixelAspectBits) {
        return ::diag(FlatExrVerifyErrorCodeV1::PixelAspectMismatch, "pixelAspectRatio");
    }
    const auto& center = header.screenWindowCenter();
    if (std::bit_cast<std::uint32_t>(center.x) != 0U ||
        std::bit_cast<std::uint32_t>(center.y) != 0U) {
        return ::diag(FlatExrVerifyErrorCodeV1::AttributeValueMismatch, "screenWindowCenter");
    }
    if (std::bit_cast<std::uint32_t>(header.screenWindowWidth()) != 0x3F800000U) {
        return ::diag(FlatExrVerifyErrorCodeV1::AttributeValueMismatch, "screenWindowWidth");
    }
    if (!Imf::hasChromaticities(header)) {
        return ::diag(FlatExrVerifyErrorCodeV1::MissingAttribute, "chromaticities");
    }
    const auto& chroma = Imf::chromaticities(header);
    const std::array<std::uint32_t, 8> chromaBits{
        std::bit_cast<std::uint32_t>(chroma.red.x),   std::bit_cast<std::uint32_t>(chroma.red.y),
        std::bit_cast<std::uint32_t>(chroma.green.x), std::bit_cast<std::uint32_t>(chroma.green.y),
        std::bit_cast<std::uint32_t>(chroma.blue.x),  std::bit_cast<std::uint32_t>(chroma.blue.y),
        std::bit_cast<std::uint32_t>(chroma.white.x), std::bit_cast<std::uint32_t>(chroma.white.y),
    };
    if (chromaBits != output::kFlatExrRec709D65ChromaticitiesBitsV1) {
        return ::diag(FlatExrVerifyErrorCodeV1::AttributeValueMismatch, "chromaticities");
    }
    const auto* colorInteropId = header.findTypedAttribute<Imf::StringAttribute>("colorInteropID");
    if (colorInteropId == nullptr ||
        std::string_view(colorInteropId->value()) != output::detail::kFlatExrColorInteropIdV1) {
        return ::diag(FlatExrVerifyErrorCodeV1::AttributeValueMismatch, "colorInteropID");
    }
    return std::nullopt;
}

[[nodiscard]] std::optional<output::FlatExrVerifyDiagnosticV1>
checkChannelList(const Imf::Header& header) noexcept {
    using output::FlatExrVerifyErrorCodeV1;
    const auto& channels = header.channels();
    std::size_t index = 0;
    for (auto it = channels.begin(); it != channels.end(); ++it, ++index) {
        if (index >= output::detail::kFlatExrChannelNamesLexicalV1.size() ||
            std::string_view(it.name()) != output::detail::kFlatExrChannelNamesLexicalV1[index]) {
            return ::diag(FlatExrVerifyErrorCodeV1::InvalidChannelList, "channels");
        }
        const auto& channel = it.channel();
        if (channel.type != Imf::FLOAT || channel.xSampling != 1 || channel.ySampling != 1 ||
            channel.pLinear) {
            return ::diag(FlatExrVerifyErrorCodeV1::InvalidChannelList, "channels");
        }
    }
    if (index != output::detail::kFlatExrChannelNamesLexicalV1.size()) {
        return ::diag(FlatExrVerifyErrorCodeV1::InvalidChannelList, "channels");
    }
    return std::nullopt;
}

} // namespace

namespace bloom::output::detail {

// The friend seam `output_semantic_identity.hpp` forward-declares
// (`friend class detail::FlatExrRgba32fLinRec709SceneSemanticPayloadVerifierV1;`) for exactly one
// purpose: only this reopen verifier -- never the writer, never a test -- may construct a
// `FlatExrRgba32fLinRec709SceneVerifiedSemanticProductV1` from decoded reopened values.
class FlatExrRgba32fLinRec709SceneSemanticPayloadVerifierV1 final {
  public:
    [[nodiscard]] static FlatExrRgba32fLinRec709SceneVerifiedSemanticProductV1 buildVerifiedProduct(
        const std::shared_ptr<const FlatExrRgba32fLinRec709SceneBoundOutputAnalysisV1>&
            boundAnalysis,
        const FlatExrRgba32fSemanticMetadataV1 metadata,
        std::vector<std::uint32_t>&& componentBits) {
        return FlatExrRgba32fLinRec709SceneVerifiedSemanticProductV1(boundAnalysis, metadata,
                                                                     std::move(componentBits));
    }
};

} // namespace bloom::output::detail

namespace bloom::output {

FlatExrVerifyResultV1::FlatExrVerifyResultV1(const FlatExrVerifyStatusV1 status,
                                             const core::Sha256Digest digest,
                                             FlatExrVerifyDiagnosticV1 diagnostic) noexcept
    : status_(status), digest_(digest), diagnostic_(std::move(diagnostic)) {}

FlatExrVerifyResultV1 FlatExrVerifyResultV1::verified(const core::Sha256Digest digest) noexcept {
    return {FlatExrVerifyStatusV1::Verified, digest, {}};
}

FlatExrVerifyResultV1 FlatExrVerifyResultV1::cancelled() noexcept {
    return {FlatExrVerifyStatusV1::Cancelled, {}, {}};
}

FlatExrVerifyResultV1 FlatExrVerifyResultV1::failed(FlatExrVerifyDiagnosticV1 diagnostic) noexcept {
    if (diagnostic.code == FlatExrVerifyErrorCodeV1::None) {
        diagnostic.code = FlatExrVerifyErrorCodeV1::InternalInvariant;
    }
    return {FlatExrVerifyStatusV1::Failed, {}, std::move(diagnostic)};
}

FlatExrVerifyResultV1 FlatExrRgba32fLinRec709SceneReopenVerifierV1::verify(
    const std::filesystem::path& stagedPath,
    const std::shared_ptr<const ProcessFrameSemanticIdentityV1>& processIdentity,
    const std::shared_ptr<const OutputAnalysisReportV1>& report,
    const runtime::CancellationToken& cancellation,
    const FlatExrVerifyScanProgressCallbackV1& scanProgress) const noexcept {
    if (processIdentity == nullptr || report == nullptr) {
        return FlatExrVerifyResultV1::failed(
            ::diag(FlatExrVerifyErrorCodeV1::IdentityIssuanceFailed));
    }
    // Binding recomputes the OutputAnalysisDigest from processIdentity/report -- the same
    // module-private seam the pre-approval analysis pipeline already uses -- and retains both
    // exact products so a later identity cannot be paired with substitute inputs.
    auto bound = bindFlatExrRgba32fLinRec709SceneOutputAnalysisV1(processIdentity, report);
    if (bound.analysis() == nullptr) {
        return FlatExrVerifyResultV1::failed(
            ::diag(FlatExrVerifyErrorCodeV1::IdentityIssuanceFailed));
    }
    const auto& boundAnalysis = bound.analysis();
    const auto& frame = processIdentity->processFrame();
    if (frame == nullptr || !frame->processImage().isValid()) {
        return FlatExrVerifyResultV1::failed(::diag(FlatExrVerifyErrorCodeV1::InternalInvariant));
    }
    const auto* descriptor = frame->processImage().descriptor();
    if (descriptor == nullptr) {
        return FlatExrVerifyResultV1::failed(::diag(FlatExrVerifyErrorCodeV1::InternalInvariant));
    }
    if (cancellation.isCancellationRequested()) {
        return FlatExrVerifyResultV1::cancelled();
    }

    const auto versionCheck = ::checkVersionField(stagedPath);
    if (!versionCheck.ok) {
        return FlatExrVerifyResultV1::failed(::diag(versionCheck.error));
    }

    try {
        Imf::InputFile input(stagedPath.string().c_str());
        const auto& header = input.header();

        if (const auto allowlistDiag = ::checkAttributeAllowlist(header)) {
            return FlatExrVerifyResultV1::failed(*allowlistDiag);
        }

        const auto expectedDataBox = ::inclusiveBox(descriptor->dataWindow());
        const auto expectedDisplayBox = ::inclusiveBox(descriptor->displayWindow());
        if (!expectedDataBox || !expectedDisplayBox) {
            return FlatExrVerifyResultV1::failed(
                ::diag(FlatExrVerifyErrorCodeV1::InternalInvariant));
        }
        const auto pixelAspect = descriptor->pixelAspect();
        const auto expectedRounded =
            output::detail::roundOutputAnalysisPositiveRationalToBinary32V1(
                {.numerator = pixelAspect.numerator(), .denominator = pixelAspect.denominator()});
        if (!expectedRounded) {
            return FlatExrVerifyResultV1::failed(
                ::diag(FlatExrVerifyErrorCodeV1::InternalInvariant));
        }
        if (const auto valueDiag = ::checkAttributeValues(
                header, *expectedDataBox, *expectedDisplayBox, expectedRounded->bits)) {
            return FlatExrVerifyResultV1::failed(*valueDiag);
        }
        if (const auto channelDiag = ::checkChannelList(header)) {
            return FlatExrVerifyResultV1::failed(*channelDiag);
        }
        if (cancellation.isCancellationRequested()) {
            return FlatExrVerifyResultV1::cancelled();
        }

        const auto dataBox = header.dataWindow();
        const auto width = static_cast<std::uint64_t>(static_cast<std::int64_t>(dataBox.max.x) -
                                                      static_cast<std::int64_t>(dataBox.min.x) + 1);
        const auto heightRows =
            static_cast<std::uint64_t>(static_cast<std::int64_t>(dataBox.max.y) -
                                       static_cast<std::int64_t>(dataBox.min.y) + 1);
        if (width > kOutputAnalysisMaximumDimensionV1 ||
            heightRows > kOutputAnalysisMaximumDimensionV1) {
            return FlatExrVerifyResultV1::failed(
                ::diag(FlatExrVerifyErrorCodeV1::ResourceLimitExceeded));
        }
        const auto pixelCount = ::multiplyChecked(width, heightRows);
        const auto componentCount = pixelCount ? ::multiplyChecked(*pixelCount, 4U) : std::nullopt;
        const auto byteCount = componentCount
                                   ? ::multiplyChecked(*componentCount, sizeof(std::uint32_t))
                                   : std::nullopt;
        if (!pixelCount || *pixelCount > kOutputAnalysisMaximumPixelCountV1 || !componentCount ||
            !byteCount || *byteCount > kOutputAnalysisMaximumProcessPixelBytesV1) {
            return FlatExrVerifyResultV1::failed(
                ::diag(FlatExrVerifyErrorCodeV1::ResourceLimitExceeded));
        }

        std::vector<std::uint32_t> componentBits;
        try {
            componentBits.resize(*componentCount);
        } catch (const std::bad_alloc&) {
            return FlatExrVerifyResultV1::failed(
                ::diag(FlatExrVerifyErrorCodeV1::AllocationFailure));
        } catch (const std::length_error&) {
            return FlatExrVerifyResultV1::failed(
                ::diag(FlatExrVerifyErrorCodeV1::AllocationFailure));
        }

        auto* bitsBase = reinterpret_cast<char*>(componentBits.data());
        constexpr std::size_t xStride = 4U * sizeof(std::uint32_t);
        const auto rowStrideBytes = xStride * static_cast<std::size_t>(width);
        Imf::FrameBuffer frameBuffer;
        frameBuffer.insert("R", Imf::Slice::Make(Imf::FLOAT, bitsBase + 0 * sizeof(std::uint32_t),
                                                 dataBox, xStride, rowStrideBytes));
        frameBuffer.insert("G", Imf::Slice::Make(Imf::FLOAT, bitsBase + 1 * sizeof(std::uint32_t),
                                                 dataBox, xStride, rowStrideBytes));
        frameBuffer.insert("B", Imf::Slice::Make(Imf::FLOAT, bitsBase + 2 * sizeof(std::uint32_t),
                                                 dataBox, xStride, rowStrideBytes));
        frameBuffer.insert("A", Imf::Slice::Make(Imf::FLOAT, bitsBase + 3 * sizeof(std::uint32_t),
                                                 dataBox, xStride, rowStrideBytes));
        input.setFrameBuffer(frameBuffer);

        const auto rowsPerChunk = static_cast<std::int64_t>(
            std::max<std::size_t>(1, kOutputAdapterMaximumStreamingChunkBytesV1 /
                                         std::max<std::size_t>(rowStrideBytes, 1)));
        const auto sourcePixels = frame->processImage().pixels();
        const auto sourceOriginY = descriptor->dataWindow().originY();
        std::int64_t y = dataBox.min.y;
        std::uint64_t completedScanlines = 0;
        ::reportScanProgress(scanProgress, {completedScanlines, heightRows});
        while (y <= static_cast<std::int64_t>(dataBox.max.y)) {
            if (cancellation.isCancellationRequested()) {
                return FlatExrVerifyResultV1::cancelled();
            }
            const auto chunkEnd = std::min<std::int64_t>(y + rowsPerChunk - 1, dataBox.max.y);
            try {
                input.readPixels(static_cast<int>(y), static_cast<int>(chunkEnd));
            } catch (...) {
                return FlatExrVerifyResultV1::failed(
                    ::diag(FlatExrVerifyErrorCodeV1::ScanlineReadFailed, {}, y));
            }
            for (auto rowY = y; rowY <= chunkEnd; ++rowY) {
                const auto sourceRowOffset = static_cast<std::size_t>(rowY - sourceOriginY) *
                                             static_cast<std::size_t>(width);
                const auto destRowOffset = static_cast<std::size_t>(rowY - dataBox.min.y) *
                                           static_cast<std::size_t>(width);
                for (std::uint64_t x = 0; x < width; ++x) {
                    const auto& sourcePixel =
                        sourcePixels[sourceRowOffset + static_cast<std::size_t>(x)];
                    const std::array<std::uint32_t, 4> expectedBits{
                        std::bit_cast<std::uint32_t>(sourcePixel.red()),
                        std::bit_cast<std::uint32_t>(sourcePixel.green()),
                        std::bit_cast<std::uint32_t>(sourcePixel.blue()),
                        std::bit_cast<std::uint32_t>(sourcePixel.alpha()),
                    };
                    const auto pixelIndex = destRowOffset + static_cast<std::size_t>(x);
                    for (std::uint8_t channel = 0; channel < 4U; ++channel) {
                        if (componentBits[pixelIndex * 4U + channel] != expectedBits[channel]) {
                            return FlatExrVerifyResultV1::failed(::diag(
                                FlatExrVerifyErrorCodeV1::SampleMismatch, {}, rowY, channel));
                        }
                    }
                }
            }
            completedScanlines += static_cast<std::uint64_t>(chunkEnd - y + 1);
            ::reportScanProgress(scanProgress, {completedScanlines, heightRows});
            y = chunkEnd + 1;
        }

        const FlatExrRgba32fSemanticMetadataV1 metadata{
            .dataWindow = {static_cast<std::int32_t>(dataBox.min.x),
                           static_cast<std::int32_t>(dataBox.min.y),
                           static_cast<std::int32_t>(dataBox.max.x),
                           static_cast<std::int32_t>(dataBox.max.y)},
            .displayWindow = {static_cast<std::int32_t>(header.displayWindow().min.x),
                              static_cast<std::int32_t>(header.displayWindow().min.y),
                              static_cast<std::int32_t>(header.displayWindow().max.x),
                              static_cast<std::int32_t>(header.displayWindow().max.y)},
            .pixelAspectRatioBits = std::bit_cast<std::uint32_t>(header.pixelAspectRatio()),
        };

        auto verifiedProduct =
            detail::FlatExrRgba32fLinRec709SceneSemanticPayloadVerifierV1::buildVerifiedProduct(
                boundAnalysis, metadata, std::move(componentBits));

        const OutputSemanticIdentityV1Preparer identityPreparer;
        const auto issuance = identityPreparer.prepareFlatExrRgba32fLinRec709SceneV1(
            FlatExrRgba32fLinRec709SceneOutputSemanticIdentityInputV1{std::move(verifiedProduct)},
            cancellation);

        switch (issuance.status()) {
        case OutputSemanticIdentityPreparationStatusV1::Prepared:
            if (issuance.identity() == nullptr) {
                return FlatExrVerifyResultV1::failed(
                    ::diag(FlatExrVerifyErrorCodeV1::InternalInvariant));
            }
            return FlatExrVerifyResultV1::verified(issuance.identity()->digest());
        case OutputSemanticIdentityPreparationStatusV1::Cancelled:
            return FlatExrVerifyResultV1::cancelled();
        case OutputSemanticIdentityPreparationStatusV1::Failed:
        default:
            return FlatExrVerifyResultV1::failed(
                ::diag(FlatExrVerifyErrorCodeV1::IdentityIssuanceFailed));
        }
    } catch (const std::bad_alloc&) {
        return FlatExrVerifyResultV1::failed(::diag(FlatExrVerifyErrorCodeV1::AllocationFailure));
    } catch (const std::exception&) {
        return FlatExrVerifyResultV1::failed(::diag(FlatExrVerifyErrorCodeV1::HeaderParseFailed));
    } catch (...) {
        return FlatExrVerifyResultV1::failed(::diag(FlatExrVerifyErrorCodeV1::InternalInvariant));
    }
}

} // namespace bloom::output
