#include "ocio_gpu_program_worker.hpp"

#include "lut_resource_budget.hpp"
#include "lut_worker_ipc.hpp"
#include "lut_worker_protocol.hpp"

#include <bloom/platform/process_supervisor.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <memory>
#include <string>
#include <system_error>
#include <utility>

namespace bloom::color::detail {
namespace {
using Clock = std::chrono::steady_clock;

#ifdef __linux__
[[nodiscard]] LutError processError(const platform::ProcessFailure failure) {
    switch (failure.code) {
    case platform::ProcessError::Unavailable:
    case platform::ProcessError::Spawn:
        return LutError::HelperUnavailable;
    case platform::ProcessError::ResourceLimit:
        return LutError::HelperMemoryLimit;
    case platform::ProcessError::Timeout:
        return LutError::HelperDeadline;
    case platform::ProcessError::Cancelled:
        return LutError::HelperCancelled;
    case platform::ProcessError::Crashed:
        return LutError::HelperTerminated;
    case platform::ProcessError::Io:
        return LutError::HelperProtocolViolation;
    }
    return LutError::HelperProtocolViolation;
}

[[nodiscard]] std::string workerPath() {
    std::error_code error;
    const auto executable = std::filesystem::read_symlink("/proc/self/exe", error);
    if (!error) {
        const auto installed = executable.parent_path() / "../libexec/bloom/bloom-color-worker";
        if (std::filesystem::is_regular_file(installed, error))
            return installed.lexically_normal().string();
    }
#ifdef BLOOM_COLOR_WORKER
    return BLOOM_COLOR_WORKER;
#else
    return {};
#endif
}

// Tracks the helper for the duration of one extraction and always reaps it, including every early
// return and any throw.
class WorkerGuard final {
  public:
    explicit WorkerGuard(std::unique_ptr<platform::ProcessSupervisor> worker)
        : worker_(std::move(worker)) {}
    ~WorkerGuard() {
        if (worker_) {
            worker_->stop();
        }
    }
    WorkerGuard(const WorkerGuard&) = delete;
    WorkerGuard& operator=(const WorkerGuard&) = delete;
    [[nodiscard]] platform::ProcessSupervisor* operator->() const noexcept { return worker_.get(); }

  private:
    std::unique_ptr<platform::ProcessSupervisor> worker_;
};
#endif
} // namespace

FileTransformGpuExtraction extractFileTransformGpuProgram(
    const std::span<const std::byte> lutBytes, const core::Sha256Digest& expectedDigest,
    const std::uint32_t lutFormat, const LutInterpolation interpolation,
    const LutDirection direction, const std::function<bool()>& cancellation) {
    FileTransformGpuExtraction result;
    if (lutBytes.empty() || lutBytes.size() > kMaximumLutBytes || lutFormat < 1 || lutFormat > 4 ||
        static_cast<std::uint32_t>(interpolation) >
            static_cast<std::uint32_t>(LutInterpolation::Best) ||
        static_cast<std::uint32_t>(direction) > static_cast<std::uint32_t>(LutDirection::Inverse) ||
        expectedDigest == core::Sha256Digest{}) {
        result.error = LutError::MalformedFile;
        return result;
    }
    if (cancellation && cancellation()) {
        result.error = LutError::HelperCancelled;
        return result;
    }
#ifndef __linux__
    result.error = LutError::HelperUnavailable;
    return result;
#else
    const auto outputReservation = LutReservation::acquire(kMaximumGpuProgramBytes);
    if (!outputReservation) {
        result.error = LutError::HelperMemoryLimit;
        return result;
    }
    const auto digest = core::Sha256Hasher::hash(lutBytes);
    if (!digest || *digest != expectedDigest) {
        result.error = LutError::ChangedFile;
        return result;
    }
    const auto generation = nextLutGeneration();
    LutFd listener(::socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC | SOCK_NONBLOCK, 0));
    const auto address = lutSocketAddress(static_cast<std::uint64_t>(::getpid()), generation);
    if (listener.get() < 0 ||
        ::bind(listener.get(), reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0 ||
        ::listen(listener.get(), 1) != 0) {
        result.error = LutError::HelperUnavailable;
        return result;
    }
    platform::ProcessOptions options;
    options.executable = workerPath();
    options.addressSpaceBytes = std::uint64_t{512} * 1024U * 1024U;
    options.messageGrace = std::chrono::milliseconds(50);
    options.termGrace = std::chrono::milliseconds(100);
    auto launched = platform::ProcessSupervisor::launch(options);
    if (const auto* failure = std::get_if<platform::ProcessFailure>(&launched)) {
        result.error = processError(*failure);
        return result;
    }
    WorkerGuard worker(std::move(std::get<std::unique_ptr<platform::ProcessSupervisor>>(launched)));
    std::uint64_t nonce = 1;
    const auto exchange = [&](LutPacket& message, const std::chrono::seconds deadline) {
        const auto expiry = Clock::now() + deadline;
        const auto command = message[2];
        const auto expectedNonce = message[3];
        const auto sent = message;
        const auto written = worker->write(std::as_bytes(std::span(message)), expiry, cancellation);
        if (const auto* error = std::get_if<platform::ProcessFailure>(&written)) {
            return processError(*error);
        }
        const auto read =
            worker->read(std::as_writable_bytes(std::span(message)), expiry, cancellation);
        if (const auto* error = std::get_if<platform::ProcessFailure>(&read)) {
            return processError(*error);
        }
        if (!validPacket(message, command, expectedNonce) ||
            message[5] > static_cast<std::uint64_t>(LutError::InvalidPixel) ||
            !std::equal(message.begin() + 7, message.end(), sent.begin() + 7)) {
            return LutError::HelperProtocolViolation;
        }
        return static_cast<LutError>(message[5]);
    };
    auto hello = packet(kLutCommandHello, nonce++);
    hello[5] = static_cast<std::uint64_t>(::getpid());
    hello[6] = generation;
    if (const auto error = exchange(hello, std::chrono::seconds(5)); error != LutError::None) {
        result.error = error;
        return result;
    }
    LutFd descriptorSocket(
        ::accept4(listener.get(), nullptr, nullptr, SOCK_CLOEXEC | SOCK_NONBLOCK));
    ucred peer{};
    socklen_t peerSize = sizeof(peer);
    if (descriptorSocket.get() < 0 ||
        ::getsockopt(descriptorSocket.get(), SOL_SOCKET, SO_PEERCRED, &peer, &peerSize) != 0 ||
        peer.pid != worker->processId()) {
        result.error = LutError::HelperProtocolViolation;
        return result;
    }
    LutFd resource(sharedFile());
    if (resource.get() < 0 || !writeFd(resource.get(), lutBytes) || !sealInput(resource.get())) {
        result.error = LutError::HelperMemoryLimit;
        return result;
    }
    LutFd programFile(sharedFile());
    if (programFile.get() < 0 ||
        ::ftruncate(programFile.get(), static_cast<off_t>(kMaximumGpuProgramBytes)) != 0 ||
        !sealExtent(programFile.get())) {
        result.error = LutError::HelperMemoryLimit;
        return result;
    }
    const std::array descriptors{resource.get(), programFile.get()};
    if (!sendDescriptors(descriptorSocket.get(), descriptors)) {
        result.error = LutError::HelperProtocolViolation;
        return result;
    }
    const LutGpuProgramRequest request{
        .lutBytes = lutBytes.size(),
        .lutFormat = lutFormat,
        .interpolation = static_cast<std::uint64_t>(interpolation),
        .direction = static_cast<std::uint64_t>(direction),
        .lutInode = inode(resource.get()),
        .programExtent = kMaximumGpuProgramBytes,
        .programInode = inode(programFile.get()),
    };
    auto message = request.encode(nonce++);
    if (const auto error = exchange(message, std::chrono::seconds(30)); error != LutError::None) {
        result.error = error;
        return result;
    }
    const LutGpuProgramReply reply{static_cast<LutError>(message[5]), message[6]};
    if (reply.programBytes == 0 || reply.programBytes > kMaximumGpuProgramBytes) {
        result.error = LutError::HelperProtocolViolation;
        return result;
    }
    if (::lseek(programFile.get(), 0, SEEK_SET) != 0) {
        result.error = LutError::HelperProtocolViolation;
        return result;
    }
    try {
        result.bytes.resize(static_cast<std::size_t>(reply.programBytes));
    } catch (const std::bad_alloc&) {
        result.error = LutError::HelperMemoryLimit;
        return result;
    }
    if (!readFd(programFile.get(), std::as_writable_bytes(std::span(result.bytes)))) {
        result.bytes.clear();
        result.error = LutError::HelperProtocolViolation;
        return result;
    }
    result.error = LutError::None;
    return result;
#endif
}

} // namespace bloom::color::detail
