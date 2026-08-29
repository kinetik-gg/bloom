#pragma once

#include <bloom/document/schema_version.hpp>
#include <bloom/document/validation.hpp>
#include <bloom/project/canonical_document.hpp>
#include <bloom/project/canonical_manifest.hpp>
#include <bloom/project/document_decode.hpp>
#include <bloom/project/document_reconstruct.hpp>
#include <bloom/project/manifest_decode.hpp>
#include <bloom/project/project_io_memory.hpp>
#include <bloom/project/strict_json_dom.hpp>
#include <bloom/project/zip_container.hpp>
#include <bloom/project/zip_container_writer.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <utility>
#include <variant>

namespace bloom::project {

// Every limit used by the composed save/reopen chain. The JSON value ceiling is shared across
// manifest.json and document.json; the verifier subtracts the manifest DOM's exact value count
// before parsing the document DOM.
struct SaveArchiveLimits final {
    CanonicalManifestLimits manifest{};
    CanonicalDocumentLimits document{};
    ZipContainerLimits container{};
    StrictJsonDomLimits json{};
};

enum class SaveArchiveStage : std::uint8_t {
    None,
    ManifestEncode,
    DocumentEncode,
    ContainerWrite,
    ContainerRead,
    ManifestParse,
    DocumentParse,
    ManifestDecode,
    VersionAgreement,
    DocumentDecode,
    Reconstruction,
    RequirementsValidation,
    ManifestReencode,
    DocumentReencode,
    ManifestByteComparison,
    DocumentByteComparison,
};

enum class SaveArchiveEntry : std::uint8_t {
    None,
    Manifest,
    Document,
};

// Fixed-capacity copy of an underlying module's bounded diagnostic path. Keeping the path in the
// failure value avoids dangling string_views without introducing another allocation on a failure
// path.
class SaveArchiveErrorPath final {
  public:
    [[nodiscard]] static SaveArchiveErrorPath from(std::string_view path) noexcept;

    [[nodiscard]] std::string_view view() const& noexcept { return {chars_.data(), size_}; }
    [[nodiscard]] std::string_view view() const&& = delete;
    [[nodiscard]] bool truncated() const noexcept { return truncated_; }

  private:
    std::array<char, 512> chars_{};
    std::uint16_t size_ = 0;
    bool truncated_ = false;
};

struct SaveArchiveManifestEncodingFailure final {
    CanonicalManifestError error = CanonicalManifestError::None;
    std::size_t requirementIndex = kCanonicalManifestNoIndex;
    std::size_t nodeTypeIndex = kCanonicalManifestNoIndex;
};

struct SaveArchiveDocumentEncodingFailure final {
    CanonicalDocumentError error = CanonicalDocumentError::None;
    std::size_t compositionIndex = kCanonicalDocumentNoIndex;
    std::size_t elementIndex = kCanonicalDocumentNoIndex;
};

struct SaveArchiveContainerWriteFailure final {
    ZipContainerWriteError error = ZipContainerWriteError::None;
    ZipContainerWriteEntry entry = ZipContainerWriteEntry::None;
};

struct SaveArchiveContainerReadFailure final {
    ZipContainerError error = ZipContainerError::None;
    std::size_t byteOffset = 0;
};

struct SaveArchiveJsonParseFailure final {
    StrictJsonDomError error = StrictJsonDomError::None;
    std::size_t byteOffset = 0;
    SaveArchiveErrorPath path;
};

struct SaveArchiveManifestDecodeFailure final {
    ManifestDecodeOutcome outcome = ManifestDecodeOutcome::Failed;
    ManifestDecodeError error = ManifestDecodeError::None;
    SaveArchiveErrorPath path;
};

struct SaveArchiveVersionAgreementFailure final {
    document::SchemaVersion manifestVersion;
    document::SchemaVersion documentVersion;
    document::SchemaVersion capturedInputVersion;
};

struct SaveArchiveDocumentDecodeFailure final {
    DocumentDecodeOutcome outcome = DocumentDecodeOutcome::Failed;
    DocumentDecodeError error = DocumentDecodeError::None;
    RoundTripPreservationReason preservationReason = RoundTripPreservationReason::None;
    SaveArchiveErrorPath path;
};

struct SaveArchiveRequirementsFailure final {
    document::ValidationResult validation;
};

struct SaveArchiveVerificationMismatch final {
    SaveArchiveEntry entry = SaveArchiveEntry::None;
};

// Used only for allocations/reservations introduced by this composition layer. Allocating module
// calls retain their own ResourceExhausted enum in their stage-specific payload above.
struct SaveArchiveResourceExhausted final {};

// A termination-free boundary must also fail closed if a composed throwing surface emits an
// exception other than std::bad_alloc despite its documented contract.
struct SaveArchiveUnexpectedFailure final {};

using SaveArchiveFailurePayload = std::variant<
    std::monostate, SaveArchiveManifestEncodingFailure, SaveArchiveDocumentEncodingFailure,
    SaveArchiveContainerWriteFailure, SaveArchiveContainerReadFailure, SaveArchiveJsonParseFailure,
    SaveArchiveManifestDecodeFailure, SaveArchiveVersionAgreementFailure,
    SaveArchiveDocumentDecodeFailure, SaveArchiveRequirementsFailure, ReconstructionRejected,
    SaveArchiveVerificationMismatch, SaveArchiveResourceExhausted, SaveArchiveUnexpectedFailure>;

class SaveArchiveFailure final {
  public:
    SaveArchiveFailure() = default;

    template <typename Payload>
    SaveArchiveFailure(const SaveArchiveStage stage, Payload payload)
        : stage_(stage), payload_(std::move(payload)) {}

    [[nodiscard]] SaveArchiveStage stage() const noexcept { return stage_; }
    [[nodiscard]] const SaveArchiveFailurePayload& payload() const noexcept { return payload_; }

    template <typename Payload> [[nodiscard]] const Payload* payloadAs() const noexcept {
        return std::get_if<Payload>(&payload_);
    }

  private:
    SaveArchiveStage stage_ = SaveArchiveStage::None;
    SaveArchiveFailurePayload payload_;
};

// Captured stage-1/stage-2 bytes and the input's declared document version. The bytes must outlive
// verifySaveArchive(). They are not retained by its result.
struct SaveArchiveExpectedContent final {
    std::span<const std::byte> manifestBytes;
    std::span<const std::byte> documentBytes;
    document::SchemaVersion documentSchemaVersion;
};

class [[nodiscard]] SaveArchiveVerificationResult final {
  public:
    [[nodiscard]] static SaveArchiveVerificationResult success() noexcept;
    [[nodiscard]] static SaveArchiveVerificationResult failure(SaveArchiveFailure failure);

    [[nodiscard]] explicit operator bool() const noexcept { return !failure_.has_value(); }
    [[nodiscard]] const SaveArchiveFailure* failure() const& noexcept {
        return failure_.has_value() ? &*failure_ : nullptr;
    }
    [[nodiscard]] const SaveArchiveFailure* failure() const&& = delete;
    [[nodiscard]] SaveArchiveFailure takeFailure() &&;

  private:
    std::optional<SaveArchiveFailure> failure_;
};

class [[nodiscard]] SaveArchiveResult final {
  public:
    SaveArchiveResult(SaveArchiveResult&&) noexcept = default;
    SaveArchiveResult& operator=(SaveArchiveResult&&) noexcept = default;
    SaveArchiveResult(const SaveArchiveResult&) = delete;
    SaveArchiveResult& operator=(const SaveArchiveResult&) = delete;
    ~SaveArchiveResult() = default;

    [[nodiscard]] static SaveArchiveResult success(ZipContainerWriteResult archive);
    [[nodiscard]] static SaveArchiveResult failure(SaveArchiveFailure failure);

    [[nodiscard]] explicit operator bool() const noexcept { return archive_.has_value(); }
    [[nodiscard]] const ZipContainerArchive* archive() const& noexcept {
        return archive_.has_value() ? archive_->archive() : nullptr;
    }
    [[nodiscard]] const ZipContainerArchive* archive() const&& = delete;
    [[nodiscard]] const SaveArchiveFailure* failure() const& noexcept {
        return failure_.has_value() ? &*failure_ : nullptr;
    }
    [[nodiscard]] const SaveArchiveFailure* failure() const&& = delete;

  private:
    SaveArchiveResult() = default;

    std::optional<ZipContainerWriteResult> archive_;
    std::optional<SaveArchiveFailure> failure_;
};

// Runs the complete bounded reopen/decode/reconstruct/re-encode verification against arbitrary
// archive bytes. This is public so corruption tests and the future Open composition can exercise
// the exact same validation chain as Save. No archive or partial decoded state is returned.
[[nodiscard]] SaveArchiveVerificationResult
verifySaveArchive(std::span<const std::byte> archive, const SaveArchiveExpectedContent& expected,
                  const SaveArchiveLimits& limits, ProjectIoOperationMemory operation) noexcept;

// Encodes the captured values, writes the archive, reopens it through verifySaveArchive(), and on
// success transfers ownership of the still-charged archive buffer into the result. The input's
// scratch spans are not used: this composition allocates and charges exact scratch requirements
// through `operation` for both the captured and reconstructed writes.
[[nodiscard]] SaveArchiveResult
buildVerifiedSaveArchive(const CanonicalManifestV1& manifest, const CanonicalDocumentV1& document,
                         const SaveArchiveLimits& limits,
                         ProjectIoOperationMemory operation) noexcept;

} // namespace bloom::project
