#pragma once
#include <array>
#include <bloom/color/ocio_cpu_file_transform_processor.hpp>
#include <cstdint>
#include <span>
#ifdef __linux__
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace bloom::color::detail {
inline constexpr std::uint64_t kLutProtocolMagic = 0x424c4f4f4d4c5554ULL;
inline constexpr std::uint64_t kLutProtocolVersion = 1;
inline constexpr std::size_t kLutSlabBytes = std::size_t{16} * 1024U * 1024U;
// The isolated helper's serialized GPU-program reply is bounded by the same 64 MiB external-LUT
// ceiling; a larger reflected OCIO program is a typed refusal, never a truncated transport.
inline constexpr std::size_t kMaximumGpuProgramBytes = std::size_t{64} * 1024U * 1024U;
// One helper serves one command at a time. Command 0 is the handshake, 1 builds a CPU processor,
// 2 applies a pixel slab, and 3 reflects the executable GPU program for a FileTransform.
inline constexpr std::uint64_t kLutCommandHello = 0;
inline constexpr std::uint64_t kLutCommandBuildCpu = 1;
inline constexpr std::uint64_t kLutCommandApplySlab = 2;
inline constexpr std::uint64_t kLutCommandExtractGpuProgram = 3;
// Same-machine private IPC: fixed-width fields, no pointers or dependency objects. A generation
// owns one helper; every reply echoes its command nonce and exact byte count before publication.
using LutPacket = std::array<std::uint64_t, 12>;
inline LutPacket packet(const std::uint64_t command, const std::uint64_t nonce) {
    return {kLutProtocolMagic, kLutProtocolVersion, command, nonce, 0, 0, 0, 0, 0, 0, 0, 0};
}
inline bool validPacket(const LutPacket& value, const std::uint64_t command,
                        const std::uint64_t nonce) {
    return value[0] == kLutProtocolMagic && value[1] == kLutProtocolVersion &&
           value[2] == command && value[3] == nonce;
}

// Typed, versioned request for command 3. The fixed packet is the wire form; these named fields are
// the checked view both sides use instead of raw indices. `programExtent` is the pre-sized sealed
// output file the helper writes the serialized program into; `programInode` binds that descriptor.
struct LutGpuProgramRequest final {
    std::uint64_t lutBytes = 0;
    std::uint64_t lutFormat = 0;
    std::uint64_t interpolation = 0;
    std::uint64_t direction = 0;
    std::uint64_t lutInode = 0;
    std::uint64_t programExtent = 0;
    std::uint64_t programInode = 0;
    [[nodiscard]] LutPacket encode(const std::uint64_t nonce) const {
        auto value = packet(kLutCommandExtractGpuProgram, nonce);
        value[4] = lutBytes;
        value[5] = lutFormat;
        value[6] = interpolation;
        value[7] = direction;
        value[8] = lutInode;
        value[9] = programExtent;
        value[10] = programInode;
        return value;
    }
    [[nodiscard]] static LutGpuProgramRequest decode(const LutPacket& value) {
        return {value[4], value[5], value[6], value[7], value[8], value[9], value[10]};
    }
};
// Typed reply for command 3. `programBytes` is meaningful only when `error` is None.
struct LutGpuProgramReply final {
    LutError error = LutError::HelperProtocolViolation;
    std::uint64_t programBytes = 0;
};

#ifdef __linux__
class LutFd final {
  public:
    explicit LutFd(int value = -1) : value_(value) {}
    ~LutFd() {
        if (value_ >= 0)
            ::close(value_);
    }
    LutFd(const LutFd&) = delete;
    LutFd& operator=(const LutFd&) = delete;
    [[nodiscard]] int get() const { return value_; }

  private:
    int value_;
};
inline bool writeFd(const int fd, std::span<const std::byte> bytes) {
    while (!bytes.empty()) {
        const auto count = ::write(fd, bytes.data(), bytes.size());
        if (count <= 0)
            return false;
        bytes = bytes.subspan(static_cast<std::size_t>(count));
    }
    return true;
}
inline bool readFd(const int fd, std::span<std::byte> bytes) {
    while (!bytes.empty()) {
        const auto count = ::read(fd, bytes.data(), bytes.size());
        if (count <= 0)
            return false;
        bytes = bytes.subspan(static_cast<std::size_t>(count));
    }
    return true;
}
inline std::uint64_t inode(const int fd) {
    struct stat value{};
    return ::fstat(fd, &value) == 0 ? static_cast<std::uint64_t>(value.st_ino) : 0;
}
inline int sharedFile() { return ::memfd_create("bloom-lut", MFD_CLOEXEC | MFD_ALLOW_SEALING); }
inline bool sealExtent(const int fd) {
    return ::fcntl(fd, F_ADD_SEALS, F_SEAL_GROW | F_SEAL_SHRINK) == 0;
}
inline bool sealInput(const int fd) {
    return ::fcntl(fd, F_ADD_SEALS, F_SEAL_WRITE | F_SEAL_GROW | F_SEAL_SHRINK | F_SEAL_SEAL) == 0;
}
inline bool validSharedFile(const int fd, const std::uint64_t expectedInode,
                            const std::uint64_t bytes, const bool sealed) {
    struct stat info{};
    const auto seals = ::fcntl(fd, F_GET_SEALS);
    return ::fstat(fd, &info) == 0 && S_ISREG(info.st_mode) && info.st_size >= 0 &&
           static_cast<std::uint64_t>(info.st_size) == bytes &&
           static_cast<std::uint64_t>(info.st_ino) == expectedInode && seals >= 0 &&
           (seals & (F_SEAL_GROW | F_SEAL_SHRINK)) == (F_SEAL_GROW | F_SEAL_SHRINK) &&
           (!sealed || (seals & (F_SEAL_WRITE | F_SEAL_GROW | F_SEAL_SHRINK)) ==
                           (F_SEAL_WRITE | F_SEAL_GROW | F_SEAL_SHRINK));
}
#endif
} // namespace bloom::color::detail
