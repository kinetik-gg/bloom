#pragma once

#include <bloom/core/sha256.hpp>

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <variant>
#include <vector>

namespace bloom::media::provider {
using Digest = core::Sha256Digest;
using Bytes = std::vector<std::byte>;
// Frozen v1 values. Zero is deliberately invalid in every vocabulary except transport v0.
enum class Role : std::uint8_t {
    Probe = 1,
    DemuxIndex = 2,
    VideoDecode = 3,
    AudioDecode = 4,
    VideoEncode = 5,
    AudioEncode = 6,
    Mux = 7,
    ReopenDecode = 8,
    StructuralQc = 9,
    RecipientQc = 10
};
enum class Purpose : std::uint8_t { Preview = 1, Proxy = 2, Conform = 3, Export = 4, Delivery = 5 };
enum class MediaDeterminismV1 : std::uint8_t {
    ByteExact = 1,
    DecodedSemanticExact = 2,
    DecodedSemanticTolerance = 3,
    NoDeterminismClaim = 4
};
enum class Qualification : std::uint8_t {
    Development = 1,
    PreviewQualified = 2,
    ConformIngestQualified = 3,
    ExportQualified = 4,
    DeliveryQualified = 5
};
enum class Transport : std::uint8_t { PipeCopiesV0 = 0, SharedMemorySlabsReserved = 1 };
enum class Implementation : std::uint8_t { Software = 1, Hardware = 2 };
enum class Availability : std::uint8_t { Available = 1, Unavailable = 2 };
enum class QcResult : std::uint8_t { Pass = 1, Fail = 2, Incomplete = 3 };
enum class MediaKind : std::uint8_t { Video = 1, Audio = 2, Data = 3 };
enum class PixelFormat : std::uint8_t { Rgba8 = 1, Rgba32f = 2, Yuv420p8 = 3 };
enum class Error : std::uint8_t {
    InvalidValue = 1,
    Truncated,
    Oversized,
    BadEnum,
    BadLength,
    Replay,
    VersionMismatch,
    IdentityMismatch,
    UnexpectedMessage,
    DigestMismatch,
    Unavailable,
    Busy,
    Cancelled,
    Timeout,
    Crashed,
    Io,
    Shutdown
};
struct Unavailable {
    Error reason = Error::Unavailable;
    std::string detail;
    friend bool operator==(const Unavailable&, const Unavailable&) = default;
};
template <typename T> using Result = std::variant<T, Unavailable>;

struct Limits final {
    static constexpr std::uint32_t stringBytes = 4096;
    static constexpr std::uint32_t entries = 256;
    static constexpr std::uint32_t planes = 4;
    static constexpr std::uint32_t dimension = 16384;
    static constexpr std::uint64_t pixels = 16777216;
    static constexpr std::uint32_t productBytes = 256U * 1024U * 1024U;
    static constexpr std::uint32_t frameBytes = productBytes + 1024U * 1024U;
    static constexpr std::uint32_t channels = 64;
    static constexpr std::uint32_t audioSamples = 65536;
    static constexpr std::uint32_t sampleRate = 384000;
};
struct Rational {
    std::int64_t numerator = 0;
    std::int64_t denominator = 1;
    friend bool operator==(const Rational&, const Rational&) = default;
};
struct MediaCapabilityKeyV1 {
    Role role = Role::Probe;
    Purpose purpose = Purpose::Preview;
    std::string container, mapping, codec, profile, level, tier, sampleEntry;
    std::uint32_t bitDepth = 8;
    std::string range, chroma, chromaLocation, alpha, fieldMode;
    std::uint32_t maxWidth = 1, maxHeight = 1;
    Rational maxRate{1, 1};
    std::string audioFormat, channelLayout, colorFeatures, hdrFeatures, timecodeFeatures,
        metadataFeatures, timing, surfaceSemantics;
    friend bool operator==(const MediaCapabilityKeyV1&, const MediaCapabilityKeyV1&) = default;
};
struct ProviderExecutionKeyV1 {
    std::string provider, build;
    Digest dependencyLock;
    std::uint16_t protocol = 1;
    std::string os, architecture;
    Implementation implementation = Implementation::Software;
    std::string sdk, driver, device;
    std::uint64_t generation = 1;
    Transport transport = Transport::PipeCopiesV0;
    std::string synchronization, resourceProfile, entitlement, trustDomain;
    Availability availability = Availability::Available;
    friend bool operator==(const ProviderExecutionKeyV1&, const ProviderExecutionKeyV1&) = default;
};
struct AuthorityRecordV1 {
    // Empty issuer means absent; an attributed claim needs all of these fields.
    std::string issuer, reference, product, scope, verifiedDate, reviewDate;
    friend bool operator==(const AuthorityRecordV1&, const AuthorityRecordV1&) = default;
};
struct QualificationEvidenceV1 {
    Digest fixtures;
    Qualification qualification = Qualification::Development;
    QcResult result = QcResult::Incomplete;
    std::string reviewDate, reviewDeadline;
    AuthorityRecordV1 authority;
    friend bool operator==(const QualificationEvidenceV1&,
                           const QualificationEvidenceV1&) = default;
};
struct PipelineStepV1 {
    Digest capability, execution, evidence;
    friend bool operator==(const PipelineStepV1&, const PipelineStepV1&) = default;
};
struct PipelineQualificationV1 {
    std::vector<PipelineStepV1> steps;
    Purpose purpose = Purpose::Preview;
    std::string profile;
    Digest fixtures;
    std::vector<std::string> conversionVersions;
    MediaDeterminismV1 determinism = MediaDeterminismV1::NoDeterminismClaim;
    Digest toleranceProfile;
    std::string reopenPolicy, qcProfile;
    QcResult result = QcResult::Incomplete;
    friend bool operator==(const PipelineQualificationV1&,
                           const PipelineQualificationV1&) = default;
};
struct MediaQcEvidenceV1 {
    Digest artifact, snapshot, preset, pipeline, execution;
    std::string tool, version, profile, coverage;
    bool independentReader = false;
    AuthorityRecordV1 externalQc;
    QcResult result = QcResult::Incomplete;
    friend bool operator==(const MediaQcEvidenceV1&, const MediaQcEvidenceV1&) = default;
};
struct ColourTags {
    // Original H.273 code points; -1 means absent. No inferred color interpretation.
    std::int32_t primaries = -1, transfer = -1, matrix = -1, range = -1;
    friend bool operator==(const ColourTags&, const ColourTags&) = default;
};
struct StreamDescriptor {
    std::uint32_t id = 0;
    MediaKind kind = MediaKind::Video;
    std::string codec, profile;
    Rational timebase{1, 1}, rate{1, 1};
    std::uint32_t width = 1, height = 1;
    PixelFormat format = PixelFormat::Rgba8;
    ColourTags colour;
    std::uint32_t sampleRate = 0;
    std::vector<std::string> channelLayout;
    friend bool operator==(const StreamDescriptor&, const StreamDescriptor&) = default;
};
struct ProbeResult {
    std::string container, version;
    std::uint64_t sourceBytes = 0;
    Digest sourceDigest;
    std::vector<StreamDescriptor> streams;
    friend bool operator==(const ProbeResult&, const ProbeResult&) = default;
};
struct CpuPlane {
    std::uint32_t width = 0, height = 0, stride = 0;
    Bytes bytes;
    Digest digest;
    friend bool operator==(const CpuPlane&, const CpuPlane&) = default;
};
struct FrameProduct {
    PixelFormat format = PixelFormat::Rgba8;
    Rational pts;
    ColourTags colour;
    std::vector<CpuPlane> planes;
    friend bool operator==(const FrameProduct&, const FrameProduct&) = default;
};
struct AudioBlock {
    Rational pts;
    std::uint32_t sampleRate = 0;
    std::vector<std::string> channelLayout;
    std::vector<std::vector<float>> channels;
};
[[nodiscard]] bool valid(const Rational& value);
[[nodiscard]] bool valid(const ProbeResult& value);
[[nodiscard]] bool valid(const FrameProduct& value);
[[nodiscard]] bool valid(const AudioBlock& value);
[[nodiscard]] Digest digestBytes(std::span<const std::byte> bytes);
// Domain string (u32 length + bytes), u16 schema=1, then declaration-order fields.
// Integers are little-endian; enums/bools u8, counts/string lengths u32, digests 32 raw bytes.
#define BLOOM_MEDIA_RECORD(Type)                                                                   \
    [[nodiscard]] Result<Bytes> canonicalBytes(const Type& value);                                 \
    [[nodiscard]] Result<Digest> digest(const Type& value);
BLOOM_MEDIA_RECORD(MediaCapabilityKeyV1)
BLOOM_MEDIA_RECORD(ProviderExecutionKeyV1)
BLOOM_MEDIA_RECORD(QualificationEvidenceV1)
BLOOM_MEDIA_RECORD(PipelineQualificationV1)
BLOOM_MEDIA_RECORD(MediaQcEvidenceV1)
#undef BLOOM_MEDIA_RECORD
} // namespace bloom::media::provider
