#include <bloom/platform/source_identity.hpp>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <string_view>

namespace {
class Expectations final {
  public:
    void expect(const bool condition, const std::string_view message) {
        if (condition)
            return;
        ++failures_;
        std::cerr << "FAILED: " << message << '\n';
    }
    [[nodiscard]] int failures() const noexcept { return failures_; }

  private:
    int failures_ = 0;
};
void write(const std::filesystem::path& path, const std::string& bytes) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output << bytes;
}
bloom::core::Sha256Digest digestOf(const std::string& bytes) {
    return *bloom::core::Sha256Hasher::hash(std::as_bytes(std::span(bytes.data(), bytes.size())));
}
} // namespace

int main() {
    using namespace bloom::platform;
    Expectations expectations;
    const auto directory =
        std::filesystem::temp_directory_path() /
        ("bloom-source-identity-" +
         std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(directory);
    const auto source = directory / "clip.bin";
    const std::string original(200000, 'a');
    write(source, original);
    const auto expected = digestOf(original);

    const auto identity = observeFileIdentity(source);
    expectations.expect(identity && identity->size == original.size() && identity->fileIdLow != 0,
                        "a regular file has an identity");
    expectations.expect(!observeFileIdentity(directory), "a directory has no file identity");
    expectations.expect(!observeFileIdentity(directory / "missing"), "missing has no identity");

    // The first verification hashes once; repeated checks are identity-only.
    auto before = sourceReadWork();
    expectations.expect(verifySourceDigest(source, expected) == SourceCheck::Matches,
                        "first verification matches");
    auto after = sourceReadWork();
    expectations.expect(after.fullReads == before.fullReads + 1 &&
                            after.fullReadBytes == before.fullReadBytes + original.size(),
                        "first verification is one counted full read");
    before = after;
    for (int i = 0; i < 100; ++i)
        expectations.expect(verifySourceDigest(source, expected) == SourceCheck::Matches,
                            "repeated verification matches");
    after = sourceReadWork();
    expectations.expect(after.fullReads == before.fullReads, "repeated checks read nothing");
    expectations.expect(after.identityHits >= before.identityHits + 100,
                        "repeated checks are identity hits");

    // A same-size content rewrite changes the identity (mtime/ctime) and is detected by a rehash.
    std::string rewritten = original;
    rewritten[1000] = 'b';
    write(source, rewritten);
    before = sourceReadWork();
    expectations.expect(verifySourceDigest(source, expected) == SourceCheck::Changed,
                        "a same-size rewrite reports Changed");
    after = sourceReadWork();
    expectations.expect(after.fullReads == before.fullReads + 1, "a changed identity rehashes");
    expectations.expect(verifySourceDigest(source, digestOf(rewritten)) == SourceCheck::Matches,
                        "the new content verifies without another read");
    expectations.expect(sourceReadWork().fullReads == after.fullReads,
                        "the rehash recorded the new identity");

    // A pure timestamp touch rehashes once and stays verified.
    const auto touched = std::filesystem::last_write_time(source) + std::chrono::seconds(3);
    std::filesystem::last_write_time(source, touched);
    before = sourceReadWork();
    expectations.expect(verifySourceDigest(source, digestOf(rewritten)) == SourceCheck::Matches,
                        "a touched file still matches");
    expectations.expect(sourceReadWork().fullReads == before.fullReads + 1,
                        "a touched file is rehashed once");

    // Replacing the path with a different file (new inode) is never a memo hit.
    const auto replacement = directory / "replacement.bin";
    write(replacement, rewritten);
    std::filesystem::rename(replacement, source);
    expectations.expect(!observeSource(source).digest, "a replaced inode is not a memo hit");

    std::filesystem::remove(source);
    expectations.expect(verifySourceDigest(source, expected) == SourceCheck::Missing,
                        "a missing file reports Missing");
    expectations.expect(digestSourceFile(source).error == SourceHashError::Missing,
                        "digest of a missing file reports Missing");
    write(source, original);
    expectations.expect(hashSourceFile(source, {}, 10).error == SourceHashError::Oversized,
                        "the size limit refuses before reading");
    expectations.expect(hashSourceFile(source, [] { return true; }).error ==
                            SourceHashError::Cancelled,
                        "cancellation is reported");
    std::filesystem::remove_all(directory);
    return expectations.failures() == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
