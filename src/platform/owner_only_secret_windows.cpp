// Owner-only secret files and the system CSPRNG on Windows (ADR 0034). See
// bloom/platform/owner_only_secret.hpp. Files and their folder carry a protected DACL that grants
// only the current user's SID.
#include <bloom/platform/owner_only_secret.hpp>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <aclapi.h>
#include <bcrypt.h>
#include <sddl.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

namespace bloom::platform {
namespace {

struct Handle final {
    HANDLE value = INVALID_HANDLE_VALUE;
    explicit Handle(const HANDLE handle) noexcept : value(handle) {}
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    ~Handle() {
        if (value != INVALID_HANDLE_VALUE && value != nullptr)
            ::CloseHandle(value);
    }
    [[nodiscard]] bool valid() const noexcept {
        return value != INVALID_HANDLE_VALUE && value != nullptr;
    }
};

struct LocalMemory final {
    void* value = nullptr;
    LocalMemory() = default;
    LocalMemory(const LocalMemory&) = delete;
    LocalMemory& operator=(const LocalMemory&) = delete;
    ~LocalMemory() {
        if (value != nullptr)
            ::LocalFree(value);
    }
};

// The current user's SID as raw bytes.
std::vector<std::byte> currentUserSid() {
    HANDLE raw = nullptr;
    if (!::OpenProcessToken(::GetCurrentProcess(), TOKEN_QUERY, &raw))
        return {};
    const Handle token(raw);
    DWORD size = 0;
    ::GetTokenInformation(token.value, TokenUser, nullptr, 0, &size);
    std::vector<std::byte> buffer(size);
    if (size == 0 || !::GetTokenInformation(token.value, TokenUser, buffer.data(), size, &size))
        return {};
    const auto* user = reinterpret_cast<const TOKEN_USER*>(buffer.data());
    const DWORD length = ::GetLengthSid(user->User.Sid);
    std::vector<std::byte> sid(length);
    if (!::CopySid(length, sid.data(), user->User.Sid))
        return {};
    return sid;
}

// "D:P(A;OICI;FA;;;<user>)": protected (no inherited ACEs), full access for the user only.
bool ownerOnlyDescriptor(LocalMemory& memory) {
    auto sid = currentUserSid();
    if (sid.empty())
        return false;
    LPWSTR text = nullptr;
    if (!::ConvertSidToStringSidW(reinterpret_cast<PSID>(sid.data()), &text))
        return false;
    const std::wstring sddl = std::wstring(L"D:P(A;OICI;FA;;;") + text + L")";
    ::LocalFree(text);
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    if (!::ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(), SDDL_REVISION_1,
                                                                &descriptor, nullptr))
        return false;
    memory.value = descriptor;
    return true;
}

// True when every allow entry of the file's DACL names the current user (no DACL at all means
// everyone has access).
bool ownerOnly(const std::filesystem::path& file) {
    PACL dacl = nullptr;
    LocalMemory descriptor;
    PSECURITY_DESCRIPTOR raw = nullptr;
    if (::GetNamedSecurityInfoW(file.c_str(), SE_FILE_OBJECT, DACL_SECURITY_INFORMATION, nullptr,
                                nullptr, &dacl, nullptr, &raw) != ERROR_SUCCESS)
        return false;
    descriptor.value = raw;
    if (dacl == nullptr)
        return false;
    const auto sid = currentUserSid();
    if (sid.empty())
        return false;
    for (DWORD index = 0; index < dacl->AceCount; ++index) {
        void* entry = nullptr;
        if (!::GetAce(dacl, index, &entry))
            return false;
        const auto* header = static_cast<const ACE_HEADER*>(entry);
        if (header->AceType == ACCESS_DENIED_ACE_TYPE)
            continue;
        if (header->AceType != ACCESS_ALLOWED_ACE_TYPE)
            return false;
        const auto* allowed = static_cast<const ACCESS_ALLOWED_ACE*>(entry);
        auto* aceSid = const_cast<DWORD*>(&allowed->SidStart);
        if (!::EqualSid(reinterpret_cast<PSID>(aceSid),
                        reinterpret_cast<PSID>(const_cast<std::byte*>(sid.data()))))
            return false;
    }
    return true;
}

bool ensureDirectory(const std::filesystem::path& folder, SECURITY_ATTRIBUTES& attributes) {
    if (std::error_code error; std::filesystem::is_directory(folder, error))
        return true;
    const auto parent = folder.parent_path();
    if (!parent.empty() && parent != folder) {
        std::error_code error;
        if (!std::filesystem::is_directory(parent, error))
            std::filesystem::create_directories(parent, error);
    }
    return ::CreateDirectoryW(folder.c_str(), &attributes) != 0 ||
           ::GetLastError() == ERROR_ALREADY_EXISTS;
}

bool printable(const std::string_view value) noexcept {
    return !value.empty() && std::ranges::all_of(value, [](const char c) {
        return static_cast<unsigned char>(c) >= 0x21U && static_cast<unsigned char>(c) <= 0x7eU;
    });
}

std::wstring hexName(const std::span<const std::byte> bytes) {
    constexpr std::wstring_view digits = L"0123456789abcdef";
    std::wstring text;
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
        const auto chunk =
            static_cast<ULONG>(std::min<std::size_t>(bytes.size() - filled, 1U << 20U));
        if (::BCryptGenRandom(nullptr, reinterpret_cast<PUCHAR>(bytes.data() + filled), chunk,
                              BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0) {
            std::memset(bytes.data(), 0, bytes.size());
            return false;
        }
        filled += chunk;
    }
    return true;
}

OwnerOnlySecretRead readOwnerOnlySecret(const std::filesystem::path& file,
                                        const std::size_t maximumBytes) {
    OwnerOnlySecretRead result;
    const Handle handle(
        ::CreateFileW(file.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                      FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
    if (!handle.valid()) {
        const auto error = ::GetLastError();
        const bool missing = error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND;
        result.status = missing ? OwnerOnlySecretStatus::Missing : OwnerOnlySecretStatus::Failed;
        result.message =
            missing ? "The secret does not exist yet." : "The secret could not be opened.";
        return result;
    }
    if (!ownerOnly(file)) {
        result.status = OwnerOnlySecretStatus::ExposedToOtherUsers;
        result.message = "Other users can access the secret.";
        return result;
    }
    LARGE_INTEGER size{};
    if (!::GetFileSizeEx(handle.value, &size) || size.QuadPart <= 0 ||
        static_cast<std::uint64_t>(size.QuadPart) > maximumBytes) {
        result.status = OwnerOnlySecretStatus::Malformed;
        result.message = "The secret has an unexpected size.";
        return result;
    }
    std::string value(static_cast<std::size_t>(size.QuadPart), '\0');
    DWORD read = 0;
    if (!::ReadFile(handle.value, value.data(), static_cast<DWORD>(value.size()), &read, nullptr)) {
        result.status = OwnerOnlySecretStatus::Failed;
        result.message = "The secret could not be read.";
        return result;
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
    LocalMemory descriptor;
    if (!ownerOnlyDescriptor(descriptor))
        return fail("The secret's access rules could not be prepared.");
    SECURITY_ATTRIBUTES attributes{sizeof(SECURITY_ATTRIBUTES), descriptor.value, FALSE};
    const auto folder = file.parent_path();
    if (folder.empty() || !ensureDirectory(folder, attributes))
        return fail("The secret's folder could not be created owner-only.");
    std::array<std::byte, 8> random{};
    if (!fillSecureRandom(random))
        return fail("The system random source is unavailable.");
    const auto temporary = folder / (L"." + file.filename().wstring() + L"." + hexName(random));
    {
        const Handle handle(::CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, &attributes,
                                          CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr));
        if (!handle.valid())
            return fail("The secret could not be written.");
        DWORD written = 0;
        const bool ok = ::WriteFile(handle.value, value.data(), static_cast<DWORD>(value.size()),
                                    &written, nullptr) &&
                        written == value.size() && ::FlushFileBuffers(handle.value);
        if (!ok) {
            ::DeleteFileW(temporary.c_str());
            return fail("The secret could not be written.");
        }
    }
    if (!::MoveFileExW(temporary.c_str(), file.c_str(),
                       MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        ::DeleteFileW(temporary.c_str());
        return fail("The secret could not be replaced.");
    }
    result.status = OwnerOnlySecretStatus::Ok;
    return result;
}

} // namespace bloom::platform
