#pragma once

#include "dependency_artifact_checks.hpp"

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace bloom::quality::dependencies::tests {

class Expectations final {
  public:
    void expect(const bool condition, const std::string_view message) {
        if (!condition) {
            ++failures_;
            std::cerr << "FAIL: " << message << '\n';
        }
    }

    template <typename Operation>
    void rejects(const std::string_view code, Operation&& operation,
                 const std::string_view message) {
        try {
            std::invoke(std::forward<Operation>(operation));
            expect(false, message);
        } catch (const CheckError& error) {
            if (!code.empty()) {
                const auto expected = std::string(code) + ':';
                if (!std::string_view(error.what()).starts_with(expected)) {
                    std::cerr << "Expected " << code << ", received " << error.what() << '\n';
                    expect(false, message);
                }
            }
        } catch (const std::exception& error) {
            std::cerr << "Unexpected exception for " << message << ": " << error.what() << '\n';
            expect(false, message);
        }
    }

    [[nodiscard]] auto failures() const noexcept -> int { return failures_; }

  private:
    int failures_{0};
};

class TemporaryDirectory final {
  public:
    TemporaryDirectory() {
        static std::atomic_uint64_t sequence{0};
        const auto now = std::chrono::steady_clock::now().time_since_epoch().count();
        for (std::uint64_t attempt = 0; attempt < 100; ++attempt) {
            root_ = std::filesystem::temp_directory_path() /
                    ("bloom-dependency-checks-" + std::to_string(now) + '-' +
                     std::to_string(sequence.fetch_add(1)) + '-' + std::to_string(attempt));
            std::error_code error;
            if (std::filesystem::create_directory(root_, error)) {
                return;
            }
        }
        throw std::runtime_error("could not create temporary directory");
    }

    TemporaryDirectory(const TemporaryDirectory&) = delete;
    auto operator=(const TemporaryDirectory&) -> TemporaryDirectory& = delete;
    ~TemporaryDirectory() {
        std::error_code error;
        std::filesystem::remove_all(root_, error);
    }

    [[nodiscard]] auto root() const -> const std::filesystem::path& { return root_; }

    void write(const std::filesystem::path& relative, const std::string_view content) const {
        const auto path = root_ / relative;
        std::filesystem::create_directories(path.parent_path());
        std::ofstream stream(path, std::ios::binary);
        stream.write(content.data(), static_cast<std::streamsize>(content.size()));
        if (!stream) {
            throw std::runtime_error("could not write temporary fixture");
        }
    }

  private:
    std::filesystem::path root_;
};

[[nodiscard]] auto runBoundaryTests(const std::filesystem::path& repositoryRoot) -> int;
[[nodiscard]] auto runProductionLockTests(const std::filesystem::path& repositoryRoot) -> int;

} // namespace bloom::quality::dependencies::tests
