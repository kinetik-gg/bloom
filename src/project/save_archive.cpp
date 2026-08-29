#include <bloom/project/save_archive.hpp>

#include "save_archive_internal.hpp"

#include <bloom/document/document.hpp>
#include <bloom/project/canonical_base64.hpp>
#include <bloom/project/canonical_decimal.hpp>
#include <bloom/project/project_io_memory_resource.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <exception>
#include <limits>
#include <memory>
#include <memory_resource>
#include <new>
#include <optional>
#include <span>
#include <utility>
#include <variant>
#include <vector>

namespace {

using bloom::project::CanonicalDocumentError;
using bloom::project::CanonicalDocumentV1;
using bloom::project::CanonicalManifestV1;
using bloom::project::JsonValue;
using bloom::project::JsonValueKind;
using bloom::project::ProjectIoMemoryReservation;
using bloom::project::ProjectIoOperationMemory;
using bloom::project::SaveArchiveDocumentEncodingFailure;
using bloom::project::SaveArchiveEntry;
using bloom::project::SaveArchiveFailure;
using bloom::project::SaveArchiveLimits;
using bloom::project::SaveArchiveManifestEncodingFailure;
using bloom::project::SaveArchiveResourceExhausted;
using bloom::project::SaveArchiveStage;

constexpr std::uint64_t kSemanticBytesPerJsonValue = 128;

[[nodiscard]] std::span<const std::byte> asBytes(const std::span<const char> bytes) noexcept {
    return {reinterpret_cast<const std::byte*>(bytes.data()), bytes.size()};
}

[[nodiscard]] bool checkedAdd(const std::size_t left, const std::size_t right,
                              std::size_t& result) noexcept {
    if (right > std::numeric_limits<std::size_t>::max() - left) {
        return false;
    }
    result = left + right;
    return true;
}

[[nodiscard]] std::optional<std::uint64_t>
semanticRepresentationCharge(const std::size_t sourceBytes,
                             const std::uint64_t valueCount) noexcept {
    if (valueCount >
        (std::numeric_limits<std::uint64_t>::max() - sourceBytes) / kSemanticBytesPerJsonValue) {
        return std::nullopt;
    }
    return static_cast<std::uint64_t>(sourceBytes) + valueCount * kSemanticBytesPerJsonValue;
}

[[nodiscard]] std::optional<ProjectIoMemoryReservation>
reserveRepresentation(const ProjectIoOperationMemory& operation, const std::size_t sourceBytes,
                      const std::uint64_t valueCount) {
    const auto charge = semanticRepresentationCharge(sourceBytes, valueCount);
    if (!charge.has_value()) {
        return std::nullopt;
    }
    auto result = operation.reserve(*charge);
    if (!result) {
        return std::nullopt;
    }
    return std::move(result).takeReservation();
}

struct ScratchRequirements final {
    std::size_t sortEntries = 0;
    std::size_t payloadBytes = 0;
    bool overflow = false;
};

[[nodiscard]] ScratchRequirements
measureScratch(const bloom::document::Snapshot& snapshot) noexcept {
    ScratchRequirements requirements;
    std::size_t firstWindow = snapshot.project().compositions().size();
    std::size_t secondWindow = snapshot.project().extensionRecords().size();
    std::size_t thirdWindow = 0;

    for (const auto& composition : snapshot.project().compositions()) {
        secondWindow = std::max(secondWindow, composition.parameters().records().size());
        secondWindow = std::max(secondWindow, composition.animationCurves().records().size());
        secondWindow = std::max(secondWindow, composition.graph().nodes().size());
        secondWindow = std::max(secondWindow, composition.graph().edges().size());
        secondWindow = std::max(secondWindow, composition.graph().layerOutputs().size());
        for (const auto& node : composition.graph().nodes()) {
            thirdWindow = std::max(thirdWindow, node.parameters.size());
        }
        for (const auto& curve : composition.animationCurves().records()) {
            if (const auto* scalar = std::get_if<bloom::document::ScalarAnimationCurve>(&curve)) {
                thirdWindow = std::max(thirdWindow, scalar->keyframes.size());
            } else if (const auto* vector =
                           std::get_if<bloom::document::Vec2AnimationCurve>(&curve)) {
                thirdWindow = std::max(thirdWindow, vector->keyframes.size());
            }
        }
    }

    if (!checkedAdd(firstWindow, secondWindow, requirements.sortEntries) ||
        !checkedAdd(requirements.sortEntries, thirdWindow, requirements.sortEntries)) {
        requirements.overflow = true;
        return requirements;
    }
    for (const auto& record : snapshot.project().extensionRecords()) {
        const auto encoded = bloom::project::canonicalBase64EncodedSize(record.payload.size());
        if (!encoded.hasValue()) {
            requirements.overflow = true;
            return requirements;
        }
        requirements.payloadBytes = std::max(requirements.payloadBytes, *encoded.value());
    }
    return requirements;
}

[[nodiscard]] std::optional<SaveArchiveFailure>
encodeManifest(const CanonicalManifestV1& manifest,
               const bloom::project::CanonicalManifestLimits& limits,
               std::pmr::vector<char>& output, const SaveArchiveStage stage) {
    const auto size = bloom::project::canonicalManifestSize(manifest, limits);
    if (!size) {
        return SaveArchiveFailure(stage, SaveArchiveManifestEncodingFailure{size.error(),
                                                                            size.requirementIndex(),
                                                                            size.nodeTypeIndex()});
    }
    output.resize(*size.value());
    const auto written = bloom::project::encodeCanonicalManifest(manifest, output, limits);
    if (!written) {
        return SaveArchiveFailure(
            stage, SaveArchiveManifestEncodingFailure{written.error(), written.requirementIndex(),
                                                      written.nodeTypeIndex()});
    }
    return std::nullopt;
}

[[nodiscard]] std::optional<SaveArchiveFailure>
encodeDocument(const CanonicalDocumentV1& input,
               const bloom::project::CanonicalDocumentLimits& limits,
               std::pmr::memory_resource* resource, std::pmr::vector<char>& output,
               const SaveArchiveStage stage) {
    if (input.snapshot == nullptr || input.colorSettings == nullptr) {
        return SaveArchiveFailure(
            stage, SaveArchiveDocumentEncodingFailure{CanonicalDocumentError::MissingInput});
    }
    const auto scratchRequirements = measureScratch(*input.snapshot);
    if (scratchRequirements.overflow) {
        return SaveArchiveFailure(stage, SaveArchiveResourceExhausted{});
    }

    std::pmr::vector<char> payloadScratch(scratchRequirements.payloadBytes, resource);
    std::pmr::vector<std::size_t> sortScratch(scratchRequirements.sortEntries, resource);
    auto request = input;
    request.payloadScratch = payloadScratch;
    request.sortScratch = sortScratch;

    const auto size = bloom::project::canonicalDocumentSize(request, limits);
    if (!size) {
        return SaveArchiveFailure(stage, SaveArchiveDocumentEncodingFailure{size.error(),
                                                                            size.compositionIndex(),
                                                                            size.elementIndex()});
    }
    output.resize(*size.value());
    const auto written = bloom::project::encodeCanonicalDocument(request, output, limits);
    if (!written) {
        return SaveArchiveFailure(
            stage, SaveArchiveDocumentEncodingFailure{written.error(), written.compositionIndex(),
                                                      written.elementIndex()});
    }
    return std::nullopt;
}

[[nodiscard]] std::uint64_t countJsonValues(const JsonValue& value) noexcept {
    std::uint64_t total = 1;
    if (value.kind() == JsonValueKind::Array) {
        for (const auto& element : value.arrayElements()) {
            total += countJsonValues(element);
        }
    } else if (value.kind() == JsonValueKind::Object) {
        for (const auto& member : value.objectMembers()) {
            total += countJsonValues(member.value());
        }
    }
    return total;
}

[[nodiscard]] bloom::project::StrictJsonDomLimits
manifestJsonLimits(const SaveArchiveLimits& limits) noexcept {
    auto result = limits.json;
    result.maximumInputBytes =
        std::min({result.maximumInputBytes, limits.manifest.maximumOutputBytes,
                  static_cast<std::size_t>(limits.container.maxManifestBytes)});
    result.maximumDecodedStringBytes =
        std::min<std::uint64_t>(result.maximumDecodedStringBytes, result.maximumInputBytes);
    return result;
}

[[nodiscard]] bloom::project::StrictJsonDomLimits
documentJsonLimits(const SaveArchiveLimits& limits, const std::uint64_t remainingValues) noexcept {
    auto result = limits.json;
    result.maximumInputBytes =
        std::min({result.maximumInputBytes, limits.document.maximumOutputBytes,
                  static_cast<std::size_t>(limits.container.maxDocumentBytes)});
    result.maximumValues = remainingValues;
    return result;
}

[[nodiscard]] std::optional<bloom::document::SchemaVersion>
documentRootVersion(const JsonValue& root) noexcept {
    const auto* version = root.findMember("schemaVersion");
    if (version == nullptr || version->kind() != JsonValueKind::Object) {
        return std::nullopt;
    }
    const auto* majorValue = version->findMember("major");
    const auto* minorValue = version->findMember("minor");
    if (majorValue == nullptr || minorValue == nullptr) {
        return std::nullopt;
    }
    const auto majorToken = majorValue->asNumberToken();
    const auto minorToken = minorValue->asNumberToken();
    if (!majorToken.has_value() || !minorToken.has_value()) {
        return std::nullopt;
    }
    const auto major = bloom::project::parseCanonicalJsonUInt32(
        *majorToken, std::numeric_limits<std::uint32_t>::max());
    const auto minor = bloom::project::parseCanonicalJsonUInt32(
        *minorToken, std::numeric_limits<std::uint32_t>::max());
    if (!major || !minor) {
        return std::nullopt;
    }
    return bloom::document::SchemaVersion{*major.value(), *minor.value()};
}

[[nodiscard]] bool byteEqual(const std::span<const std::byte> left,
                             const std::span<const std::byte> right) noexcept {
    return left.size() == right.size() && std::ranges::equal(left, right);
}

} // namespace

namespace bloom::project {

SaveArchiveErrorPath SaveArchiveErrorPath::from(const std::string_view path) noexcept {
    SaveArchiveErrorPath result;
    const auto count = std::min(path.size(), result.chars_.size());
    std::memcpy(result.chars_.data(), path.data(), count);
    result.size_ = static_cast<std::uint16_t>(count);
    result.truncated_ = count != path.size();
    return result;
}

SaveArchiveVerificationResult SaveArchiveVerificationResult::success() noexcept { return {}; }

SaveArchiveVerificationResult
SaveArchiveVerificationResult::failure(SaveArchiveFailure failureValue) {
    SaveArchiveVerificationResult result;
    result.failure_.emplace(std::move(failureValue));
    return result;
}

SaveArchiveFailure SaveArchiveVerificationResult::takeFailure() && {
    if (!failure_.has_value()) {
        return {};
    }
    return std::move(*failure_);
}

SaveArchiveResult SaveArchiveResult::success(ZipContainerWriteResult archive) {
    SaveArchiveResult result;
    result.archive_.emplace(std::move(archive));
    return result;
}

SaveArchiveResult SaveArchiveResult::failure(SaveArchiveFailure failureValue) {
    SaveArchiveResult result;
    result.failure_.emplace(std::move(failureValue));
    return result;
}

SaveArchiveVerificationResult verifySaveArchive(const std::span<const std::byte> archive,
                                                const SaveArchiveExpectedContent& expected,
                                                const SaveArchiveLimits& limits,
                                                ProjectIoOperationMemory operation) noexcept {
    auto stage = SaveArchiveStage::ContainerRead;
    try {
        auto reopened = readZipContainer(archive, limits.container, operation);
        if (!reopened) {
            return SaveArchiveVerificationResult::failure(SaveArchiveFailure(
                stage, SaveArchiveContainerReadFailure{reopened.error(), reopened.byteOffset()}));
        }
        const auto* entries = reopened.document();

        stage = SaveArchiveStage::ManifestParse;
        auto manifestDom =
            parseStrictJsonDom(entries->manifestBytes(), manifestJsonLimits(limits), operation);
        if (!manifestDom) {
            return SaveArchiveVerificationResult::failure(SaveArchiveFailure(
                stage,
                SaveArchiveJsonParseFailure{manifestDom.error(), manifestDom.byteOffset(),
                                            SaveArchiveErrorPath::from(manifestDom.memberPath())}));
        }
        const auto manifestValueCount = countJsonValues(manifestDom.document()->root());

        stage = SaveArchiveStage::DocumentParse;
        if (manifestValueCount >= limits.json.maximumValues) {
            return SaveArchiveVerificationResult::failure(SaveArchiveFailure(
                stage, SaveArchiveJsonParseFailure{.error = StrictJsonDomError::ValueLimitExceeded,
                                                   .byteOffset = 0,
                                                   .path = SaveArchiveErrorPath{}}));
        }
        const auto remainingValues = limits.json.maximumValues - manifestValueCount;
        auto documentDom = parseStrictJsonDom(
            entries->documentBytes(), documentJsonLimits(limits, remainingValues), operation);
        if (!documentDom) {
            return SaveArchiveVerificationResult::failure(SaveArchiveFailure(
                stage,
                SaveArchiveJsonParseFailure{documentDom.error(), documentDom.byteOffset(),
                                            SaveArchiveErrorPath::from(documentDom.memberPath())}));
        }
        const auto documentValueCount = countJsonValues(documentDom.document()->root());

        stage = SaveArchiveStage::ManifestDecode;
        auto manifestReservation =
            reserveRepresentation(operation, entries->manifestBytes().size(), manifestValueCount);
        if (!manifestReservation.has_value()) {
            return SaveArchiveVerificationResult::failure(
                SaveArchiveFailure(stage, SaveArchiveResourceExhausted{}));
        }
        auto decodedManifest = decodeManifestEnvelope(manifestDom.document()->root());
        if (!decodedManifest) {
            return SaveArchiveVerificationResult::failure(
                SaveArchiveFailure(stage, SaveArchiveManifestDecodeFailure{
                                              decodedManifest.outcome(), decodedManifest.error(),
                                              SaveArchiveErrorPath::from(decodedManifest.path())}));
        }

        stage = SaveArchiveStage::VersionAgreement;
        const auto decodedDocumentVersion = documentRootVersion(documentDom.document()->root());
        const auto& manifestValue = *decodedManifest.value();
        if (manifestValue.documentSchemaVersion != expected.documentSchemaVersion ||
            (decodedDocumentVersion.has_value() &&
             manifestValue.documentSchemaVersion != *decodedDocumentVersion)) {
            return SaveArchiveVerificationResult::failure(SaveArchiveFailure(
                stage, SaveArchiveVersionAgreementFailure{
                           manifestValue.documentSchemaVersion,
                           decodedDocumentVersion.value_or(manifestValue.documentSchemaVersion),
                           expected.documentSchemaVersion}));
        }

        stage = SaveArchiveStage::DocumentDecode;
        auto decodeReservation =
            reserveRepresentation(operation, entries->documentBytes().size(), documentValueCount);
        if (!decodeReservation.has_value()) {
            return SaveArchiveVerificationResult::failure(
                SaveArchiveFailure(stage, SaveArchiveResourceExhausted{}));
        }
        auto decodedDocument = decodeDocumentEnvelope(documentDom.document()->root());
        if (!decodedDocument) {
            return SaveArchiveVerificationResult::failure(
                SaveArchiveFailure(stage, SaveArchiveDocumentDecodeFailure{
                                              decodedDocument.outcome(), decodedDocument.error(),
                                              decodedDocument.preservationReason(),
                                              SaveArchiveErrorPath::from(decodedDocument.path())}));
        }

        stage = SaveArchiveStage::Reconstruction;
        auto reconstructionReservation =
            reserveRepresentation(operation, entries->documentBytes().size(), documentValueCount);
        if (!reconstructionReservation.has_value()) {
            return SaveArchiveVerificationResult::failure(
                SaveArchiveFailure(stage, SaveArchiveResourceExhausted{}));
        }
        auto reconstructed = reconstructDocument(*decodedDocument.value());
        if (!reconstructed) {
            return SaveArchiveVerificationResult::failure(
                SaveArchiveFailure(stage, reconstructed.rejection()));
        }

        // validateManifestRequirements() currently accepts only live Project truth, not the
        // DecodedDocumentEnvelope. Reconstruction therefore immediately precedes this required
        // validation while both decoded and reconstructed representations remain charged.
        stage = SaveArchiveStage::RequirementsValidation;
        auto requirementsReservation = reserveRepresentation(
            operation, entries->manifestBytes().size() + entries->documentBytes().size(),
            manifestValueCount);
        if (!requirementsReservation.has_value()) {
            return SaveArchiveVerificationResult::failure(
                SaveArchiveFailure(stage, SaveArchiveResourceExhausted{}));
        }
        auto reconstructedSnapshot = reconstructed.value()->document->snapshot();
        auto requirementValidation = validateManifestRequirements(reconstructedSnapshot.project(),
                                                                  manifestValue.requirements);
        if (!requirementValidation.ok()) {
            return SaveArchiveVerificationResult::failure(SaveArchiveFailure(
                stage, SaveArchiveRequirementsFailure{std::move(requirementValidation)}));
        }
        requirementsReservation.reset();

        // Final use of `operation` in this function: every earlier stage above needed the shared
        // handle again for a later call, so those uses stayed copies (ProjectIoOperationMemory is a
        // cheap shared_ptr-backed handle); this is the one place it can genuinely sink.
        ProjectIoMemoryResource reencodeResource(std::move(operation));
        std::pmr::vector<char> manifestReencoded(&reencodeResource);
        std::pmr::vector<char> documentReencoded(&reencodeResource);

        stage = SaveArchiveStage::ManifestReencode;
        const CanonicalManifestV1 manifestRequest{
            .format = kCanonicalManifestFormat,
            .containerVersion = manifestValue.containerVersion,
            .documentPath = manifestValue.documentPath,
            .documentSchemaVersion = manifestValue.documentSchemaVersion,
            .requirements = manifestValue.requirements,
        };
        if (auto failure =
                encodeManifest(manifestRequest, limits.manifest, manifestReencoded, stage);
            failure.has_value()) {
            return SaveArchiveVerificationResult::failure(std::move(*failure));
        }

        stage = SaveArchiveStage::DocumentReencode;
        const CanonicalDocumentV1 documentRequest{
            .snapshot = &reconstructedSnapshot,
            .colorSettings = &reconstructed.value()->colorSettings,
            .roundTrip = decodedDocument.roundTrip(),
            .schemaMinor = decodedDocumentVersion.has_value() ? decodedDocumentVersion->minor : 0,
        };
        if (auto failure = encodeDocument(documentRequest, limits.document, &reencodeResource,
                                          documentReencoded, stage);
            failure.has_value()) {
            return SaveArchiveVerificationResult::failure(std::move(*failure));
        }

        stage = SaveArchiveStage::ManifestByteComparison;
        const auto manifestReencodedBytes = asBytes(manifestReencoded);
        if (!byteEqual(entries->manifestBytes(), expected.manifestBytes) ||
            !byteEqual(manifestReencodedBytes, expected.manifestBytes)) {
            return SaveArchiveVerificationResult::failure(SaveArchiveFailure(
                stage, SaveArchiveVerificationMismatch{SaveArchiveEntry::Manifest}));
        }

        stage = SaveArchiveStage::DocumentByteComparison;
        const auto documentReencodedBytes = asBytes(documentReencoded);
        if (!byteEqual(entries->documentBytes(), expected.documentBytes) ||
            !byteEqual(documentReencodedBytes, expected.documentBytes)) {
            return SaveArchiveVerificationResult::failure(SaveArchiveFailure(
                stage, SaveArchiveVerificationMismatch{SaveArchiveEntry::Document}));
        }
        return SaveArchiveVerificationResult::success();
    } catch (const std::bad_alloc&) {
        return SaveArchiveVerificationResult::failure(
            SaveArchiveFailure(stage, SaveArchiveResourceExhausted{}));
    } catch (...) {
        return SaveArchiveVerificationResult::failure(
            SaveArchiveFailure(stage, SaveArchiveUnexpectedFailure{}));
    }
}

SaveArchiveResult buildSaveArchive(const CanonicalManifestV1& manifest,
                                   const CanonicalDocumentV1& document,
                                   const SaveArchiveLimits& limits,
                                   ProjectIoOperationMemory operation) noexcept {
    auto outcome =
        detail::buildSaveArchiveEntries(manifest, document, limits, std::move(operation));
    if (!outcome) {
        return SaveArchiveResult::failure(std::move(outcome).takeFailure());
    }
    return SaveArchiveResult::success(std::move(outcome).takeArchive());
}

SaveArchiveResult buildVerifiedSaveArchive(const CanonicalManifestV1& manifest,
                                           const CanonicalDocumentV1& document,
                                           const SaveArchiveLimits& limits,
                                           ProjectIoOperationMemory operation) noexcept {
    // `operation` is a cheap shared_ptr-backed handle (see verifySaveArchive()'s own final-use
    // comment above), so the copy passed to the shared build routine and the moved-from final use
    // in the verifySaveArchive() call below are both safe.
    auto outcome = detail::buildSaveArchiveEntries(manifest, document, limits, operation);
    if (!outcome) {
        return SaveArchiveResult::failure(std::move(outcome).takeFailure());
    }

    const auto expected = outcome.expectedContent(manifest.documentSchemaVersion);
    auto verified =
        verifySaveArchive(outcome.archiveBytes(), expected, limits, std::move(operation));
    if (!verified) {
        return SaveArchiveResult::failure(std::move(verified).takeFailure());
    }
    return SaveArchiveResult::success(std::move(outcome).takeArchive());
}

namespace detail {

std::span<const std::byte> SaveArchiveBuiltEntries::manifestByteSpan() const noexcept {
    return asBytes(manifestBytes);
}

std::span<const std::byte> SaveArchiveBuiltEntries::documentByteSpan() const noexcept {
    return asBytes(documentBytes);
}

SaveArchiveFailure SaveArchiveBuildOutcome::takeFailure() && noexcept {
    if (!failure.has_value()) {
        std::terminate();
    }
    // operator*() (unlike value()) cannot throw, which is why it -- not value() -- is used on
    // every already-checked access below: value()'s own always-noexcept-incompatible throwing
    // contract trips bugprone-exception-escape even when, as here, the has_value() guard above
    // makes the empty case provably unreachable.
    return std::move(*failure);
}

ZipContainerWriteResult SaveArchiveBuildOutcome::takeArchive() && noexcept {
    if (!archive.has_value()) {
        std::terminate();
    }
    return std::move(*archive);
}

std::span<const std::byte> SaveArchiveBuildOutcome::archiveBytes() const& noexcept {
    if (!archive.has_value()) {
        std::terminate();
    }
    return archive->archive()->bytes();
}

SaveArchiveExpectedContent SaveArchiveBuildOutcome::expectedContent(
    const document::SchemaVersion documentSchemaVersion) const& noexcept {
    if (!entries.has_value()) {
        std::terminate();
    }
    return SaveArchiveExpectedContent{
        .manifestBytes = entries->manifestByteSpan(),
        .documentBytes = entries->documentByteSpan(),
        .documentSchemaVersion = documentSchemaVersion,
    };
}

SaveArchiveBuildOutcome buildSaveArchiveEntries(const CanonicalManifestV1& manifest,
                                                const CanonicalDocumentV1& document,
                                                const SaveArchiveLimits& limits,
                                                ProjectIoOperationMemory operation) noexcept {
    auto stage = SaveArchiveStage::ManifestEncode;
    try {
        // Heap-allocated so its address stays stable once SaveArchiveBuiltEntries is returned and
        // moved by the caller (see save_archive_internal.hpp's file comment).
        auto resource = std::make_unique<ProjectIoMemoryResource>(operation);
        std::pmr::vector<char> manifestBytes(resource.get());
        std::pmr::vector<char> documentBytes(resource.get());

        if (auto failure = encodeManifest(manifest, limits.manifest, manifestBytes, stage);
            failure.has_value()) {
            SaveArchiveBuildOutcome outcome;
            outcome.failure = std::move(failure);
            return outcome;
        }

        stage = SaveArchiveStage::DocumentEncode;
        if (auto failure =
                encodeDocument(document, limits.document, resource.get(), documentBytes, stage);
            failure.has_value()) {
            SaveArchiveBuildOutcome outcome;
            outcome.failure = std::move(failure);
            return outcome;
        }

        stage = SaveArchiveStage::ContainerWrite;
        // Final use of `operation` in this function: `resource` above already holds its own
        // independent copy of the shared handle (constructed before this point), so moving the
        // local parameter away here is safe.
        auto written = writeZipContainer(asBytes(manifestBytes), asBytes(documentBytes),
                                         limits.container, std::move(operation));
        if (!written) {
            SaveArchiveBuildOutcome outcome;
            outcome.failure = SaveArchiveFailure(
                stage, SaveArchiveContainerWriteFailure{written.error(), written.entryInError()});
            return outcome;
        }

        SaveArchiveBuildOutcome outcome;
        outcome.entries = SaveArchiveBuiltEntries{
            .resource = std::move(resource),
            .manifestBytes = std::move(manifestBytes),
            .documentBytes = std::move(documentBytes),
        };
        outcome.archive = std::move(written);
        return outcome;
    } catch (const std::bad_alloc&) {
        SaveArchiveBuildOutcome outcome;
        outcome.failure = SaveArchiveFailure(stage, SaveArchiveResourceExhausted{});
        return outcome;
    } catch (...) {
        SaveArchiveBuildOutcome outcome;
        outcome.failure = SaveArchiveFailure(stage, SaveArchiveUnexpectedFailure{});
        return outcome;
    }
}

} // namespace detail

} // namespace bloom::project
