// Owner-only secret files and the system CSPRNG on Linux and macOS (ADR 0034). See
// bloom/platform/owner_only_secret.hpp.
#include <bloom/platform/owner_only_secret.hpp>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstring>
#include <string>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#if defined(__linux__)
#include <sys/random.h>
#endif

namespace bloom::platform {
namespace {

constexpr mode_t kFileMode = S_IRUSR | S_IWUSR;
constexpr mode_t kDirectoryMode = S_IRWXU;

struct Descriptor final {
    int fd = -1;
    Descriptor() = default;
    explicit Descriptor(const int value) noexcept : fd(value) {}
    Descriptor(const Descriptor&) = delete;
    Descriptor& operator=(const Descriptor&) = delete;
    ~Descriptor() {
        if (fd >= 0)
            ::close(fd);
    }
};

bool printable(const std::string_view value) noexcept {
    return !value.empty() && std::ranges::all_of(value, [](const char c) {
        return static_cast<unsigned char>(c) >= 0x21U && static_cast<unsigned char>(c) <= 0x7eU;
    });
}

// Creates every missing directory of `folder` owner-only and tightens the final one to 0700 when
// this user owns it.
bool ensureOwnerOnlyDirectory(const std::filesystem::path& folder) {
    std::error_code error;
    if (!std::filesystem::exists(folder, error)) {
        const auto parent = folder.parent_path();
        if (!parent.empty() && parent != folder && !ensureOwnerOnlyDirectory(parent))
            return false;
        if (::mkdir(folder.c_str(), kDirectoryMode) != 0 && errno != EEXIST)
            return false;
    }
    struct stat status{};
    if (::stat(folder.c_str(), &status) != 0 || !S_ISDIR(status.st_mode))
        return false;
    return true;
}

bool tightenDirectory(const std::filesystem::path& folder) {
    struct stat status{};
    if (::lstat(folder.c_str(), &status) != 0 || !S_ISDIR(status.st_mode) ||
        status.st_uid != ::geteuid())
        return false;
    if ((status.st_mode & 077U) != 0 && ::chmod(folder.c_str(), kDirectoryMode) != 0)
        return false;
    return true;
}

std::string hexName(const std::span<const std::byte> bytes) {
    constexpr std::string_view digits = "0123456789abcdef";
    std::string text;
    for (const auto byte : bytes) {
        const auto value = std::to_integer<unsigned>(byte);
        text.push_back(digits[value >> 4U]);
        text.push_back(digits[value & 0xFU]);
    }
    return text;
}

} // namespace

bool fillSecureRandom(const std::span<std::byte> bytes) noexcept {
    std::size_t filled = 0;
    while (filled < bytes.size()) {
        const auto chunk = std::min<std::size_t>(bytes.size() - filled, 256);
#if defined(__linux__)
        const auto got = ::getrandom(bytes.data() + filled, chunk, 0);
        if (got < 0) {
            if (errno == EINTR)
                continue;
            std::memset(bytes.data(), 0, bytes.size());
            return false;
        }
        filled += static_cast<std::size_t>(got);
#else
        if (::getentropy(bytes.data() + filled, chunk) != 0) {
            std::memset(bytes.data(), 0, bytes.size());
            return false;
        }
        filled += chunk;
#endif
    }
    return true;
}

OwnerOnlySecretRead readOwnerOnlySecret(const std::filesystem::path& file,
                                        const std::size_t maximumBytes) {
    OwnerOnlySecretRead result;
    const Descriptor descriptor(::open(file.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW));
    if (descriptor.fd < 0) {
        result.status =
            errno == ENOENT ? OwnerOnlySecretStatus::Missing : OwnerOnlySecretStatus::Failed;
        result.message =
            errno == ENOENT ? "The secret does not exist yet." : "The secret could not be opened.";
        return result;
    }
    struct stat status{};
    if (::fstat(descriptor.fd, &status) != 0 || !S_ISREG(status.st_mode)) {
        result.status = OwnerOnlySecretStatus::Failed;
        result.message = "The secret is not a regular file.";
        return result;
    }
    if (status.st_uid != ::geteuid() || (status.st_mode & 077U) != 0) {
        result.status = OwnerOnlySecretStatus::ExposedToOtherUsers;
        result.message = "Other users can access the secret.";
        return result;
    }
    if (status.st_size <= 0 || static_cast<std::uint64_t>(status.st_size) > maximumBytes) {
        result.status = OwnerOnlySecretStatus::Malformed;
        result.message = "The secret has an unexpected size.";
        return result;
    }
    std::string value(static_cast<std::size_t>(status.st_size), '\0');
    std::size_t read = 0;
    while (read < value.size()) {
        const auto got = ::read(descriptor.fd, value.data() + read, value.size() - read);
        if (got < 0 && errno == EINTR)
            continue;
        if (got <= 0)
            break;
        read += static_cast<std::size_t>(got);
    }
    value.resize(read);
    if (!printable(value)) {
        result.status = OwnerOnlySecretStatus::Malformed;
        result.message = "The secret is not printable text.";
        return result;
    }
    result.status = OwnerOnlySecretStatus::Ok;
    result.value = std::move(value);
    return result;
}

OwnerOnlySecretWrite writeOwnerOnlySecret(const std::filesystem::path& file,
                                          const std::string_view value) {
    OwnerOnlySecretWrite result;
    const auto fail = [&](std::string message) {
        result.status = OwnerOnlySecretStatus::Failed;
        result.message = std::move(message);
        return result;
    };
    if (!printable(value)) {
        result.status = OwnerOnlySecretStatus::Malformed;
        result.message = "The secret is not printable text.";
        return result;
    }
    const auto folder = file.parent_path();
    if (folder.empty() || !ensureOwnerOnlyDirectory(folder) || !tightenDirectory(folder))
        return fail("The secret's folder could not be created owner-only.");
    std::array<std::byte, 8> random{};
    if (!fillSecureRandom(random))
        return fail("The system random source is unavailable.");
    const auto temporary = folder / ("." + file.filename().string() + "." + hexName(random));
    {
        const Descriptor descriptor(::open(
            temporary.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, kFileMode));
        if (descriptor.fd < 0)
            return fail("The secret could not be written.");
        bool written = ::fchmod(descriptor.fd, kFileMode) == 0;
        std::size_t done = 0;
        while (written && done < value.size()) {
            const auto wrote = ::write(descriptor.fd, value.data() + done, value.size() - done);
            if (wrote < 0 && errno == EINTR)
                continue;
            written = wrote > 0;
            if (written)
                done += static_cast<std::size_t>(wrote);
        }
        if (!written || ::fsync(descriptor.fd) != 0) {
            ::unlink(temporary.c_str());
            return fail("The secret could not be written.");
        }
    }
    if (::rename(temporary.c_str(), file.c_str()) != 0) {
        ::unlink(temporary.c_str());
        return fail("The secret could not be replaced.");
    }
    const Descriptor directory(::open(folder.c_str(), O_RDONLY | O_CLOEXEC | O_DIRECTORY));
    if (directory.fd >= 0)
        static_cast<void>(::fsync(directory.fd));
    result.status = OwnerOnlySecretStatus::Ok;
    return result;
}

} // namespace bloom::platform
