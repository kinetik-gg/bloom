#pragma once

// A hand-rolled byte-level Constrained ZIP Profile archive builder shared by
// zip_container_preflight_tests.cpp and zip_container_tests.cpp. It knows nothing about libzip:
// every local file header, central directory header, and end-of-central-directory record is
// assembled field-by-field so a test can construct either a fully conforming archive or a single
// deliberate deviation from one. CRC and raw-deflate helpers use the qualified zlib the tests are
// already linked against (test fixtures only; production code never uses zlib outside
// zip_container.cpp).

#include <zlib.h>

#include <cstddef>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace bloom::project::test {

[[nodiscard]] inline std::uint32_t crc32Of(const std::span<const std::byte> payload) noexcept {
    const auto value = crc32_z(0L, reinterpret_cast<const Bytef*>(payload.data()), payload.size());
    return static_cast<std::uint32_t>(value);
}

// Raw deflate (no zlib/gzip wrapper) at level 6, matching what a ZIP method-8 entry stores.
[[nodiscard]] inline std::vector<std::byte> deflateRaw(const std::span<const std::byte> payload) {
    z_stream stream{};
    constexpr int kLevel = 6;
    constexpr int kWindowBits = -15; // negative: raw deflate, no zlib header/trailer.
    constexpr int kMemLevel = 8;
    if (deflateInit2(&stream, kLevel, Z_DEFLATED, kWindowBits, kMemLevel, Z_DEFAULT_STRATEGY) !=
        Z_OK) {
        throw std::runtime_error("deflateInit2 failed");
    }
    std::vector<std::byte> out(deflateBound(&stream, static_cast<uLong>(payload.size())));
    stream.next_in = reinterpret_cast<Bytef*>(const_cast<std::byte*>(payload.data()));
    stream.avail_in = static_cast<uInt>(payload.size());
    stream.next_out = reinterpret_cast<Bytef*>(out.data());
    stream.avail_out = static_cast<uInt>(out.size());
    const auto result = deflate(&stream, Z_FINISH);
    const auto producedBytes = out.size() - stream.avail_out;
    deflateEnd(&stream);
    if (result != Z_STREAM_END) {
        throw std::runtime_error("deflate did not finish in one call");
    }
    out.resize(producedBytes);
    return out;
}

[[nodiscard]] inline std::vector<std::byte> toBytes(const std::string_view text) {
    return {reinterpret_cast<const std::byte*>(text.data()),
            reinterpret_cast<const std::byte*>(text.data()) + text.size()};
}

// Every field a local or central header carries, defaulted to a conforming stored entry. Tests
// mutate exactly the field(s) they mean to violate.
struct EntrySpec final {
    std::string localName = "manifest.json";
    std::string centralName = "manifest.json";
    std::uint16_t localFlags = 0x0800;
    std::uint16_t centralFlags = 0x0800;
    std::uint16_t localMethod = 0;
    std::uint16_t centralMethod = 0;
    std::vector<std::byte> data; // bytes physically written after the local header.
    std::uint32_t localCompressedSize = 0;
    std::uint32_t centralCompressedSize = 0;
    std::uint32_t localUncompressedSize = 0;
    std::uint32_t centralUncompressedSize = 0;
    std::uint32_t localCrc = 0;
    std::uint32_t centralCrc = 0;
    std::vector<std::byte> localExtra;
    std::vector<std::byte> centralExtra;
    std::string centralComment;
    // version made by: high byte 3 (Unix host), low byte 20 (spec version 2.0); external
    // attributes: Unix mode 0644 (regular, non-executable) placed in the high 16 bits.
    std::uint16_t versionMadeBy = 0x0314;
    std::uint32_t externalAttrs = 0100644U << 16U;
    std::uint16_t diskNumberStart = 0;
};

[[nodiscard]] inline EntrySpec makeStoredEntry(const std::string& name,
                                               const std::span<const std::byte> payload) {
    EntrySpec spec;
    spec.localName = name;
    spec.centralName = name;
    spec.data.assign(payload.begin(), payload.end());
    spec.localCompressedSize = spec.centralCompressedSize =
        static_cast<std::uint32_t>(payload.size());
    spec.localUncompressedSize = spec.centralUncompressedSize =
        static_cast<std::uint32_t>(payload.size());
    spec.localCrc = spec.centralCrc = crc32Of(payload);
    return spec;
}

[[nodiscard]] inline EntrySpec makeDeflateEntry(const std::string& name,
                                                const std::span<const std::byte> payload) {
    EntrySpec spec = makeStoredEntry(name, payload);
    spec.data = deflateRaw(payload);
    spec.localCompressedSize = spec.centralCompressedSize =
        static_cast<std::uint32_t>(spec.data.size());
    spec.localMethod = spec.centralMethod = 8;
    return spec;
}

// Appends fixed-width little-endian fields and raw byte ranges to build one archive from
// individually-controlled local headers, central headers, and an EOCD record -- in whatever
// order and with whatever field values a test supplies, valid or not.
class ArchiveWriter final {
  public:
    // Returns the byte offset the local header was written at.
    std::uint64_t appendLocal(const EntrySpec& entry) {
        const auto offset = bytes_.size();
        appendU32(0x04034b50U);
        appendU16(20);
        appendU16(entry.localFlags);
        appendU16(entry.localMethod);
        appendU16(0);
        appendU16(0);
        appendU32(entry.localCrc);
        appendU32(entry.localCompressedSize);
        appendU32(entry.localUncompressedSize);
        appendU16(static_cast<std::uint16_t>(entry.localName.size()));
        appendU16(static_cast<std::uint16_t>(entry.localExtra.size()));
        appendText(entry.localName);
        appendRaw(entry.localExtra);
        appendRaw(entry.data);
        return offset;
    }

    void appendCentral(const EntrySpec& entry, const std::uint32_t localHeaderOffset) {
        appendU32(0x02014b50U);
        appendU16(entry.versionMadeBy);
        appendU16(20);
        appendU16(entry.centralFlags);
        appendU16(entry.centralMethod);
        appendU16(0);
        appendU16(0);
        appendU32(entry.centralCrc);
        appendU32(entry.centralCompressedSize);
        appendU32(entry.centralUncompressedSize);
        appendU16(static_cast<std::uint16_t>(entry.centralName.size()));
        appendU16(static_cast<std::uint16_t>(entry.centralExtra.size()));
        appendU16(static_cast<std::uint16_t>(entry.centralComment.size()));
        appendU16(entry.diskNumberStart);
        appendU16(0);
        appendU32(entry.externalAttrs);
        appendU32(localHeaderOffset);
        appendText(entry.centralName);
        appendRaw(entry.centralExtra);
        appendText(entry.centralComment);
    }

    void appendEocd(const std::uint16_t diskNumber, const std::uint16_t diskWithCd,
                    const std::uint16_t entriesThisDisk, const std::uint16_t entriesTotal,
                    const std::uint32_t cdSize, const std::uint32_t cdOffset,
                    const std::string& comment = {}) {
        appendU32(0x06054b50U);
        appendU16(diskNumber);
        appendU16(diskWithCd);
        appendU16(entriesThisDisk);
        appendU16(entriesTotal);
        appendU32(cdSize);
        appendU32(cdOffset);
        appendU16(static_cast<std::uint16_t>(comment.size()));
        appendText(comment);
    }

    void appendRawBytes(const std::span<const std::byte> raw) { appendRaw(raw); }

    [[nodiscard]] std::uint64_t size() const { return bytes_.size(); }
    [[nodiscard]] const std::vector<std::byte>& bytes() const { return bytes_; }

  private:
    void appendU16(const std::uint16_t value) {
        bytes_.push_back(static_cast<std::byte>(value & 0xFFU));
        bytes_.push_back(static_cast<std::byte>((value >> 8U) & 0xFFU));
    }
    void appendU32(const std::uint32_t value) {
        for (unsigned shift = 0; shift < 32U; shift += 8U) {
            bytes_.push_back(static_cast<std::byte>((value >> shift) & 0xFFU));
        }
    }
    void appendText(const std::string_view text) {
        for (const char character : text) {
            bytes_.push_back(static_cast<std::byte>(static_cast<unsigned char>(character)));
        }
    }
    void appendRaw(const std::span<const std::byte> raw) {
        bytes_.insert(bytes_.end(), raw.begin(), raw.end());
    }

    std::vector<std::byte> bytes_;
};

// Assembles a fully conforming two-entry archive: manifest local header at offset 0, its data
// abutting, the document local header abutting, its data abutting, the central directory
// abutting, and the EOCD abutting and ending the file.
[[nodiscard]] inline std::vector<std::byte> buildConformingArchive(const EntrySpec& manifest,
                                                                   const EntrySpec& document) {
    ArchiveWriter writer;
    const auto manifestOffset = writer.appendLocal(manifest);
    const auto documentOffset = writer.appendLocal(document);
    const auto centralStart = writer.size();
    writer.appendCentral(manifest, static_cast<std::uint32_t>(manifestOffset));
    writer.appendCentral(document, static_cast<std::uint32_t>(documentOffset));
    const auto centralSize = writer.size() - centralStart;
    writer.appendEocd(0, 0, 2, 2, static_cast<std::uint32_t>(centralSize),
                      static_cast<std::uint32_t>(centralStart));
    return writer.bytes();
}

} // namespace bloom::project::test
