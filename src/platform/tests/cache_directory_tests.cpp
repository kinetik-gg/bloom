#include <bloom/platform/cache_directory.hpp>

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

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

// setenv/unsetenv are POSIX-only; this test binary is built only on Linux (see
// src/platform/CMakeLists.txt's `if(BUILD_TESTING AND CMAKE_SYSTEM_NAME STREQUAL "Linux")`).
class EnvironmentVariable final {
  public:
    explicit EnvironmentVariable(std::string name) : name_(std::move(name)) {
        const auto* existing = std::getenv(name_.c_str()); // NOLINT(concurrency-mt-unsafe)
        if (existing != nullptr)
            saved_ = existing;
    }
    ~EnvironmentVariable() {
        if (saved_.has_value())
            static_cast<void>(::setenv(name_.c_str(), saved_->c_str(), 1));
        else
            static_cast<void>(::unsetenv(name_.c_str()));
    }
    EnvironmentVariable(const EnvironmentVariable&) = delete;
    EnvironmentVariable& operator=(const EnvironmentVariable&) = delete;
    void set(const std::string& value) const {
        static_cast<void>(::setenv(name_.c_str(), value.c_str(), 1));
    }
    void unset() const { static_cast<void>(::unsetenv(name_.c_str())); }

  private:
    std::string name_;
    std::optional<std::string> saved_;
};

} // namespace

int main() {
    Expectations check;
    namespace platform = bloom::platform;

    EnvironmentVariable xdg("XDG_CACHE_HOME");
    EnvironmentVariable home("HOME");

    xdg.set("/tmp/bloom-cache-directory-test-xdg");
    const auto fromXdg = platform::userCacheDirectory("bloom");
    check.expect(fromXdg.has_value() &&
                     *fromXdg == std::filesystem::path("/tmp/bloom-cache-directory-test-xdg/bloom"),
                 "an absolute XDG_CACHE_HOME wins and gets the app subdirectory appended");

    xdg.set("relative/not-absolute");
    home.set("/tmp/bloom-cache-directory-test-home");
    const auto relativeXdgIgnored = platform::userCacheDirectory("bloom");
    check.expect(relativeXdgIgnored.has_value() &&
                     *relativeXdgIgnored ==
                         std::filesystem::path("/tmp/bloom-cache-directory-test-home/.cache/bloom"),
                 "a relative XDG_CACHE_HOME is ignored in favor of $HOME/.cache");

    xdg.unset();
    const auto fromHome = platform::userCacheDirectory("bloom");
    check.expect(fromHome.has_value() &&
                     *fromHome ==
                         std::filesystem::path("/tmp/bloom-cache-directory-test-home/.cache/bloom"),
                 "no XDG_CACHE_HOME falls back to $HOME/.cache/<app>");

    home.unset();
    const auto neither = platform::userCacheDirectory("bloom");
    check.expect(!neither.has_value(),
                 "no resolvable environment variable returns nullopt rather than a relative path");

    home.set("/tmp/bloom-cache-directory-test-home");
    const auto emptySubdirectory = platform::userCacheDirectory("");
    check.expect(!emptySubdirectory.has_value(), "an empty app subdirectory is refused");

    return check.failures() == 0 ? 0 : 1;
}
