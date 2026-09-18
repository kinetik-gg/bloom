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
