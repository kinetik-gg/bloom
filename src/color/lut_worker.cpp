#include "lut_preflight.hpp"
#include "lut_worker_ipc.hpp"
#include "lut_worker_protocol.hpp"
#include <OpenColorIO/OpenColorIO.h>
#include <algorithm>
#include <bloom/platform/process_supervisor.hpp>
#include <cmath>
#include <iostream>
#include <limits>
#include <string>
#ifdef __linux__
#include <cerrno>
#include <cstddef>
#include <linux/audit.h>
#include <linux/filter.h>
#include <linux/landlock.h>
#include <linux/seccomp.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#endif

namespace {
using namespace bloom::color;
namespace OCIO = OCIO_NAMESPACE;
#ifdef __linux__
// The helper receives only anonymous sealed files. Landlock denies access to the filesystem;
// seccomp denies networking, child creation, execution, and process-memory/signal access.
bool confine() {
    if (::prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) != 0)
        return false;
    landlock_ruleset_attr attributes{};
    attributes.handled_access_fs = (1ULL << 13U) - 1;
    detail::LutFd rules(static_cast<int>(::syscall(SYS_landlock_create_ruleset, &attributes,
                                                   sizeof(attributes.handled_access_fs), 0)));
    if (rules.get() < 0 || ::syscall(SYS_landlock_restrict_self, rules.get(), 0) != 0)
        return false;
#if defined(__x86_64__)
    constexpr auto architecture = AUDIT_ARCH_X86_64;
#elif defined(__aarch64__)
    constexpr auto architecture = AUDIT_ARCH_AARCH64;
#else
    return false;
#endif
#if defined(__x86_64__) || defined(__aarch64__)
    std::vector<sock_filter> filters{
        BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(seccomp_data, arch)),
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, architecture, 1, 0),
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_KILL_PROCESS),
        BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(seccomp_data, nr))};
    const auto deny = [&](const std::uint32_t number) {
        filters.push_back(BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, number, 0, 1));
        filters.push_back(BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ERRNO | EPERM));
    };
    for (const auto call :
         {SYS_socket, SYS_socketpair, SYS_connect, SYS_bind, SYS_listen, SYS_clone, SYS_clone3,
          SYS_execve, SYS_execveat, SYS_ptrace, SYS_process_vm_readv, SYS_process_vm_writev,
          SYS_kill, SYS_tkill, SYS_tgkill, SYS_io_uring_setup})
        deny(static_cast<std::uint32_t>(call));
#ifdef SYS_fork
    deny(SYS_fork);
    deny(SYS_vfork);
#endif
    // x32 uses a second syscall namespace on x86-64.
#if defined(__x86_64__)
    filters.push_back(BPF_JUMP(BPF_JMP | BPF_JGE | BPF_K, 0x40000000U, 0, 1));
    filters.push_back(BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_KILL_PROCESS));
#endif
    filters.push_back(BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW));
    sock_fprog program{static_cast<unsigned short>(filters.size()), filters.data()};
    return ::prctl(PR_SET_SECCOMP, SECCOMP_MODE_FILTER, &program) == 0;
#endif
}

LutError build(const detail::LutPacket& message, OCIO::ConstCPUProcessorRcPtr& cpu,
               const int descriptorSocket, bool& identity) {
    if (!message[4] || message[4] > kMaximumLutBytes || message[8] < 1 || message[8] > 4 ||
        message[9] > 2 || message[10] > 1)
        return LutError::HelperProtocolViolation;
    const auto descriptors = detail::receiveDescriptors(descriptorSocket, 1);
    detail::LutFd file(descriptors[0]);
    if (file.get() < 0 || !detail::validSharedFile(file.get(), message[7], message[4], true))
        return LutError::HelperProtocolViolation;
    if (!confine())
        return LutError::HelperUnavailable;
    try {
        std::vector<std::byte> bytes(static_cast<std::size_t>(message[4]));
        if (::lseek(file.get(), 0, SEEK_SET) != 0 || !detail::readFd(file.get(), bytes))
            return LutError::HelperProtocolViolation;
        const auto checked = detail::preflightLut(
            std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size()),
            static_cast<std::uint32_t>(message[8]), {}, &identity);
        if (checked != LutError::None)
            return checked;
        auto transform = OCIO::FileTransform::Create();
        const auto path = "/proc/self/fd/" + std::to_string(file.get());
        transform->setSrc(path.c_str());
        constexpr std::array interpolation{OCIO::INTERP_LINEAR, OCIO::INTERP_TETRAHEDRAL,
                                           OCIO::INTERP_BEST};
        transform->setInterpolation(interpolation[message[9]]);
        transform->setDirection(message[10] == 0 ? OCIO::TRANSFORM_DIR_FORWARD
                                                 : OCIO::TRANSFORM_DIR_INVERSE);
        cpu = OCIO::Config::CreateRaw()->getProcessor(transform)->getDefaultCPUProcessor();
        return cpu ? LutError::None : LutError::TransformBuildFailed;
    } catch (const std::bad_alloc&) {
        return LutError::HelperMemoryLimit;
    } catch (const std::exception&) {
        return LutError::MalformedFile;
    }
}

class Mapping final {
  public:
    Mapping(const int descriptor, const std::size_t size, const int protection)
        : data_(::mmap(nullptr, size, protection, MAP_SHARED, descriptor, 0)), size_(size) {}
    ~Mapping() {
        if (data_ != MAP_FAILED)
            ::munmap(data_, size_);
    }
    Mapping(const Mapping&) = delete;
    Mapping& operator=(const Mapping&) = delete;
    [[nodiscard]] bool valid() const { return data_ != MAP_FAILED; }
    [[nodiscard]] std::span<std::array<float, 4>> pixels() const {
        return {static_cast<std::array<float, 4>*>(data_), size_ / sizeof(std::array<float, 4>)};
    }

  private:
    void* data_;
    std::size_t size_;
};

LutError applySlab(const detail::LutPacket& message, const OCIO::ConstCPUProcessorRcPtr& cpu,
                   const int descriptorSocket, const bool identity) {
    if (!cpu || !message[4] || message[4] > detail::kLutSlabBytes || message[4] % 16 != 0)
        return LutError::HelperProtocolViolation;
    const auto descriptors = detail::receiveDescriptors(descriptorSocket, 2);
    detail::LutFd input(descriptors[0]), output(descriptors[1]);
    if (input.get() < 0 || output.get() < 0 ||
        !detail::validSharedFile(input.get(), message[7], message[4], true) ||
        !detail::validSharedFile(output.get(), message[9], message[4], false))
        return LutError::HelperProtocolViolation;
    Mapping in(input.get(), message[4], PROT_READ),
        out(output.get(), message[4], PROT_READ | PROT_WRITE);
    if (!in.valid() || !out.valid())
        return LutError::HelperMemoryLimit;
    try {
        const auto source = in.pixels(), destination = out.pixels();
        std::ranges::copy(source, destination.begin());
        for (auto& pixel : destination) {
            if (!std::ranges::all_of(pixel, [](const float f) { return std::isfinite(f); }))
                return LutError::InvalidPixel;
            if (!identity)
                cpu->applyRGB(pixel.data());
            if (!std::ranges::all_of(pixel, [](const float f) { return std::isfinite(f); }))
                return LutError::InvalidPixel;
        }
    } catch (const std::exception&) {
        return LutError::TransformBuildFailed;
    }
    return LutError::None;
}
#endif
} // namespace

int main() {
#ifndef __linux__
    return 1;
#else
    try {
        if (!bloom::platform::processWorkerBootstrap())
            return 1;
        detail::LutFd descriptorSocket(::socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0));
        if (descriptorSocket.get() < 0)
            return 1;
        OCIO::ConstCPUProcessorRcPtr cpu;
        bool identity = false;
        std::uint64_t nonce = 0;
        while (true) {
            detail::LutPacket message{};
            if (!detail::readFd(STDIN_FILENO, std::as_writable_bytes(std::span(message))))
                return 0;
            if (message[0] != detail::kLutProtocolMagic ||
                message[1] != detail::kLutProtocolVersion || message[3] != ++nonce)
                return 1;
            LutError error = LutError::HelperProtocolViolation;
            if (message[2] == 0 && nonce == 1) {
                const auto address = detail::lutSocketAddress(message[5], message[6]);
                error = ::connect(descriptorSocket.get(),
                                  reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == 0
                            ? LutError::None
                            : LutError::HelperUnavailable;
            } else if (message[2] == 1 && nonce == 2)
                error = build(message, cpu, descriptorSocket.get(), identity);
            else if (message[2] == 2 && nonce > 2)
                error = applySlab(message, cpu, descriptorSocket.get(), identity);
            message[5] = static_cast<std::uint64_t>(error);
            message[6] = cpu && (identity || cpu->isIdentity()) ? 1 : 0;
            if (!detail::writeFd(STDOUT_FILENO, std::as_bytes(std::span(message))))
                return 1;
            if (error != LutError::None)
                return 1;
        }
    } catch (const std::exception&) {
        return 1;
    }
#endif
}
